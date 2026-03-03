/*
 * vulkan_stereo_3dvision.cpp
 *
 * Stereoscopic 3D demo – NVIDIA 3D Vision beta driver 426.06, GeForce/consumer.
 *
 * KEY REQUIREMENTS (both must be true simultaneously):
 *  1. imageArrayLayers = 2  in the swapchain  (layer 0 = left, layer 1 = right)
 *  2. VK_EXT_full_screen_exclusive with APPLICATION_CONTROLLED mode
 *     → vkAcquireFullScreenExclusiveModeEXT must succeed before rendering.
 *     The DWM compositor blocks stereo in windowed/overlay mode; the driver
 *     returns VK_ERROR_INITIALIZATION_FAILED (-3) from vkCreateSwapchainKHR
 *     if exclusive ownership is not established.
 *
 * BUILD (Developer Command Prompt):
 *   cl /std:c++17 /W3 /O2 main.cpp /I%VULKAN_SDK%\Include ^
 *      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib
 *
 * PREREQUISITES:
 *   - NVIDIA GeForce GPU + 3D Vision kit or 3D Vision Ready monitor
 *   - Driver <= 426.06  (3D Vision beta branch)
 *   - 3D Vision enabled in NVIDIA Control Panel -> "Set up stereoscopic 3D"
 *   - Vulkan SDK 1.1.114+
 *
 * WHAT YOU SEE:
 *   Left  eye -> solid CYAN   (R=0,   G=255, B=255)
 *   Right eye -> solid RED    (R=255, G=0,   B=0  )
 *
 * Press ESC to quit.
 */

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            char _b[256];                                                      \
            snprintf(_b, sizeof(_b), "Vulkan error %d at line %d:\n%s",       \
                     (int)_r, __LINE__, #expr);                                \
            throw std::runtime_error(_b);                                      \
        }                                                                      \
    } while(0)

template<class T> static T vk_clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void log(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
    puts(buf);
}

static bool hasInstanceExt(const char *name) {
    uint32_t n = 0; vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> v(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, v.data());
    for (auto &e : v) if (strcmp(e.extensionName, name) == 0) return true;
    return false;
}

static bool hasDeviceExt(VkPhysicalDevice pd, const char *name) {
    uint32_t n = 0; vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> v(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, v.data());
    for (auto &e : v) if (strcmp(e.extensionName, name) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Win32 window
// ---------------------------------------------------------------------------

static HWND s_hwnd = nullptr;
static bool s_quit = false;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY || msg == WM_CLOSE) {
        s_quit = true; PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// Borderless fullscreen window over the primary monitor.
// Full-display coverage is required so Windows grants exclusive ownership
// when vkAcquireFullScreenExclusiveModeEXT is called.
static HWND createFullscreenWindow(HINSTANCE hInst) {
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "VkStereoWnd";
    RegisterClassExA(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    // WS_POPUP with no border = borderless fullscreen required for FSE
    HWND hwnd = CreateWindowExA(
        0, "VkStereoWnd",
        "Vulkan 3D Vision Stereo  [Left=Cyan | Right=Red]  (ESC to quit)",
        WS_POPUP,
        0, 0, sw, sh,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------

struct StereoApp {
    // Core Vulkan objects
    VkInstance       instance    = VK_NULL_HANDLE;
    VkSurfaceKHR     surface     = VK_NULL_HANDLE;
    VkPhysicalDevice physDev     = VK_NULL_HANDLE;
    VkDevice         device      = VK_NULL_HANDLE;
    uint32_t         queueFamily = UINT32_MAX;
    VkQueue          queue       = VK_NULL_HANDLE;

    // Swapchain
    VkSwapchainKHR        swapchain  = VK_NULL_HANDLE;
    VkFormat              swapFmt    = VK_FORMAT_UNDEFINED;
    VkExtent2D            swapExtent{};
    uint32_t              swapCount  = 0;
    bool                  stereo     = false;

    std::vector<VkImage>        swapImages;
    std::vector<VkImageView>    viewLeft, viewRight;
    std::vector<VkFramebuffer>  fbLeft,   fbRight;

    // Render passes – one per eye to avoid any inter-pass barrier.
    // Both use initialLayout = UNDEFINED (= discard, always legal) so no
    // VkImageMemoryBarrier is needed between them.  The old code incorrectly
    // used a barrier with newLayout = UNDEFINED which is illegal per
    // VUID-VkImageMemoryBarrier-newLayout-01198.
    VkRenderPass renderPassLeft  = VK_NULL_HANDLE;
    VkRenderPass renderPassRight = VK_NULL_HANDLE;

    VkCommandPool                cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmdBufs;

    static const uint32_t MAX_FRAMES = 2;
    VkSemaphore imageReady[MAX_FRAMES]{};
    VkSemaphore renderDone[MAX_FRAMES]{};
    VkFence     inFlight  [MAX_FRAMES]{};
    uint32_t    frameIndex = 0;

    // Extension support flags
    bool hasSurfaceCaps2 = false;  // VK_KHR_get_surface_capabilities2
    bool hasFSE          = false;  // VK_EXT_full_screen_exclusive

    // Extension function pointer
    PFN_vkAcquireFullScreenExclusiveModeEXT pfnAcquireFSE = nullptr;

    // -------------------------------------------------------------------
    // Init
    // -------------------------------------------------------------------

    void init(HINSTANCE hInst, HWND hwnd) {
        createInstance();
        createSurface(hInst, hwnd);
        pickPhysicalDevice();
        createDevice();
        createSwapchain(hwnd);
        createRenderPasses();
        createFramebuffers();
        createCommandPool();
        allocateCommandBuffers();
        createSyncObjects();
        log("Init complete. %ux%u, %u images, stereo=%s",
            swapExtent.width, swapExtent.height, swapCount,
            stereo ? "YES" : "NO");
    }

    // -------------------------------------------------------------------
    // Instance – must include VK_KHR_get_surface_capabilities2 because
    // VK_EXT_full_screen_exclusive depends on it.
    // -------------------------------------------------------------------

    void createInstance() {
        hasSurfaceCaps2 = hasInstanceExt(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

        std::vector<const char *> exts = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        if (hasSurfaceCaps2) {
            exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            log("VK_KHR_get_surface_capabilities2 available");
        } else {
            log("WARNING: VK_KHR_get_surface_capabilities2 not available -> no FSE");
        }

        const char *valLayer = "VK_LAYER_KHRONOS_validation";
        uint32_t lc = 0; vkEnumerateInstanceLayerProperties(&lc, nullptr);
        std::vector<VkLayerProperties> lp(lc);
        vkEnumerateInstanceLayerProperties(&lc, lp.data());
        bool hasVal = false;
        for (auto &l : lp) if (strcmp(l.layerName, valLayer) == 0) { hasVal = true; break; }

        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "VkStereo3DVision";
        ai.apiVersion       = VK_API_VERSION_1_1;

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo        = &ai;
        ci.enabledExtensionCount   = (uint32_t)exts.size();
        ci.ppEnabledExtensionNames = exts.data();
        if (hasVal) { ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = &valLayer; }

        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
        log("Instance created (validation %s)", hasVal ? "ON" : "OFF");
    }

    // -------------------------------------------------------------------
    // Surface
    // -------------------------------------------------------------------

    void createSurface(HINSTANCE hInst, HWND hwnd) {
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance = hInst; ci.hwnd = hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface));
    }

    // -------------------------------------------------------------------
    // Physical device
    // -------------------------------------------------------------------

    void pickPhysicalDevice() {
        uint32_t cnt = 0;
        vkEnumeratePhysicalDevices(instance, &cnt, nullptr);
        if (!cnt) throw std::runtime_error("No Vulkan GPUs found");
        std::vector<VkPhysicalDevice> devs(cnt);
        vkEnumeratePhysicalDevices(instance, &cnt, devs.data());

        for (auto pd : devs) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pd, &props);

            uint32_t qfc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qfc);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qf.data());

            for (uint32_t i = 0; i < qfc; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
                if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;

                physDev = pd; queueFamily = i;

                VkSurfaceCapabilitiesKHR caps{};
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
                log("GPU: %s | maxImageArrayLayers=%u",
                    props.deviceName, caps.maxImageArrayLayers);
                if (caps.maxImageArrayLayers < 2)
                    log("WARNING: maxImageArrayLayers < 2 -- is 3D Vision enabled "
                        "in NVIDIA Control Panel and driver <= 426.06?");

                hasFSE = hasSurfaceCaps2 &&
                         hasDeviceExt(pd, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
                log("VK_EXT_full_screen_exclusive: %s", hasFSE ? "available" : "NOT available");
                if (!hasFSE)
                    log("WARNING: FSE unavailable -- driver will likely reject "
                        "stereo swapchain with VK_ERROR_INITIALIZATION_FAILED");
                break;
            }
            if (physDev) break;
        }
        if (!physDev) throw std::runtime_error("No suitable GPU found");
    }

    // -------------------------------------------------------------------
    // Logical device
    // -------------------------------------------------------------------

    void createDevice() {
        float prio = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily; qci.queueCount = 1; qci.pQueuePriorities = &prio;

        std::vector<const char *> exts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        if (hasFSE) exts.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);

        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount    = 1;
        ci.pQueueCreateInfos       = &qci;
        ci.enabledExtensionCount   = (uint32_t)exts.size();
        ci.ppEnabledExtensionNames = exts.data();
        VK_CHECK(vkCreateDevice(physDev, &ci, nullptr, &device));
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        if (hasFSE) {
            pfnAcquireFSE = (PFN_vkAcquireFullScreenExclusiveModeEXT)
                vkGetDeviceProcAddr(device, "vkAcquireFullScreenExclusiveModeEXT");
            if (!pfnAcquireFSE) {
                log("ERROR: could not load vkAcquireFullScreenExclusiveModeEXT");
                hasFSE = false;
            }
        }
    }

    // -------------------------------------------------------------------
    // Swapchain
    //
    // THE TWO REQUIREMENTS FOR NVIDIA 3D VISION STEREO:
    //
    //  A) imageArrayLayers = 2
    //     Layer 0 = left eye (cyan), layer 1 = right eye (red).
    //     The surface reports maxImageArrayLayers=2 when 3D Vision is active.
    //
    //  B) VK_EXT_full_screen_exclusive APPLICATION_CONTROLLED
    //     Chain VkSurfaceFullScreenExclusiveInfoEXT into the swapchain CI.
    //     Then call vkAcquireFullScreenExclusiveModeEXT after creation.
    //     Without this the driver returns VK_ERROR_INITIALIZATION_FAILED (-3)
    //     because DWM compositing is incompatible with the stereo present path.
    // -------------------------------------------------------------------

    void createSwapchain(HWND hwnd) {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev, surface, &caps);

        // Format: prefer B8G8R8A8_UNORM
        uint32_t fc = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, surface, &fc, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fc);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, surface, &fc, fmts.data());
        VkSurfaceFormatKHR fmt = fmts[0];
        for (auto &f : fmts)
            if (f.format     == VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt = f;
        swapFmt = fmt.format;

        // Extent
        RECT rc{}; GetClientRect(hwnd, &rc);
        uint32_t ww = (uint32_t)(rc.right  - rc.left); if (!ww) ww = 1;
        uint32_t wh = (uint32_t)(rc.bottom - rc.top);  if (!wh) wh = 1;
        swapExtent.width  = vk_clamp(ww, caps.minImageExtent.width,  caps.maxImageExtent.width);
        swapExtent.height = vk_clamp(wh, caps.minImageExtent.height, caps.maxImageExtent.height);

        uint32_t imgCount = caps.minImageCount + 1;
        if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

        // Stereo layers (requirement A)
        uint32_t layers = (caps.maxImageArrayLayers >= 2) ? 2u : 1u;
        stereo = (layers == 2);
        log(stereo ? "STEREO ENABLED  -- imageArrayLayers=2"
                   : "STEREO DISABLED -- imageArrayLayers=1 (no stereo)");

        // Full Screen Exclusive chain (requirement B)
        // VkSurfaceFullScreenExclusiveWin32InfoEXT.hmonitor must be the monitor
        // the window is displayed on; must match what the surface was created on.
        VkSurfaceFullScreenExclusiveWin32InfoEXT fseWin32{
            VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
        fseWin32.hmonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);

        VkSurfaceFullScreenExclusiveInfoEXT fseInfo{
            VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
        fseInfo.pNext = &fseWin32;
        // APPLICATION_CONTROLLED = we explicitly call vkAcquireFullScreenExclusiveModeEXT
        fseInfo.fullScreenExclusive =
            hasFSE ? VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT
                   : VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.pNext            = hasFSE ? &fseInfo : nullptr;  // FSE chain
        ci.surface          = surface;
        ci.minImageCount    = imgCount;
        ci.imageFormat      = swapFmt;
        ci.imageColorSpace  = fmt.colorSpace;
        ci.imageExtent      = swapExtent;
        ci.imageArrayLayers = layers;             // <-- STEREO KEY
        ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform     = caps.currentTransform;
        ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;  // vsync, stable stereo timing
        ci.clipped          = VK_TRUE;

        VK_CHECK(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain));

        // Acquire fullscreen exclusive ownership.
        // Must be called before the first vkQueuePresentKHR.
        if (hasFSE && pfnAcquireFSE) {
            VkResult r = pfnAcquireFSE(device, swapchain);
            if (r == VK_SUCCESS)
                log("Fullscreen exclusive mode ACQUIRED successfully");
            else
                log("WARNING: vkAcquireFullScreenExclusiveModeEXT returned %d "
                    "-- stereo may not work", (int)r);
        }

        // Retrieve images and create per-layer views
        vkGetSwapchainImagesKHR(device, swapchain, &swapCount, nullptr);
        swapImages.resize(swapCount);
        vkGetSwapchainImagesKHR(device, swapchain, &swapCount, swapImages.data());

        viewLeft .resize(swapCount, VK_NULL_HANDLE);
        viewRight.resize(swapCount, VK_NULL_HANDLE);

        for (uint32_t i = 0; i < swapCount; ++i) {
            auto makeView = [&](uint32_t layer) -> VkImageView {
                VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vi.image    = swapImages[i];
                vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vi.format   = swapFmt;
                vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
                VkImageView v = VK_NULL_HANDLE;
                VK_CHECK(vkCreateImageView(device, &vi, nullptr, &v));
                return v;
            };
            viewLeft [i] = makeView(0);
            viewRight[i] = stereo ? makeView(1) : makeView(0);
        }
    }

    // -------------------------------------------------------------------
    // Render passes
    //
    // We use two separate render pass objects (one per eye) with
    // initialLayout = UNDEFINED for both.
    //
    // UNDEFINED initialLayout tells the driver "discard any prior content"
    // which is always valid regardless of the image's actual current layout.
    // This means NO explicit VkImageMemoryBarrier is needed between the two
    // passes -- and more importantly it avoids the previous code's bug of
    // issuing a barrier with newLayout = UNDEFINED, which is illegal:
    //   VUID-VkImageMemoryBarrier-newLayout-01198:
    //   "newLayout must not be VK_IMAGE_LAYOUT_UNDEFINED or PREINITIALIZED"
    // -------------------------------------------------------------------

    VkRenderPass makeRenderPass() {
        VkAttachmentDescription att{};
        att.format         = swapFmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;      // discard -- always legal
        att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1; ci.pAttachments  = &att;
        ci.subpassCount    = 1; ci.pSubpasses    = &sub;
        ci.dependencyCount = 1; ci.pDependencies = &dep;

        VkRenderPass rp = VK_NULL_HANDLE;
        VK_CHECK(vkCreateRenderPass(device, &ci, nullptr, &rp));
        return rp;
    }

    void createRenderPasses() {
        renderPassLeft  = makeRenderPass();
        renderPassRight = makeRenderPass();
    }

    // -------------------------------------------------------------------
    // Framebuffers
    // -------------------------------------------------------------------

    void createFramebuffers() {
        fbLeft .resize(swapCount, VK_NULL_HANDLE);
        fbRight.resize(swapCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < swapCount; ++i) {
            auto makeFB = [&](VkRenderPass rp, VkImageView view) -> VkFramebuffer {
                VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                fi.renderPass      = rp;
                fi.attachmentCount = 1;
                fi.pAttachments    = &view;
                fi.width           = swapExtent.width;
                fi.height          = swapExtent.height;
                fi.layers          = 1;
                VkFramebuffer fb = VK_NULL_HANDLE;
                VK_CHECK(vkCreateFramebuffer(device, &fi, nullptr, &fb));
                return fb;
            };
            fbLeft [i] = makeFB(renderPassLeft,  viewLeft [i]);
            fbRight[i] = makeFB(renderPassRight, viewRight[i]);
        }
    }

    // -------------------------------------------------------------------
    // Command pool + buffers
    // -------------------------------------------------------------------

    void createCommandPool() {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = queueFamily;
        ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device, &ci, nullptr, &cmdPool));
    }

    void allocateCommandBuffers() {
        cmdBufs.resize(swapCount);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool        = cmdPool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = swapCount;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, cmdBufs.data()));
    }

    // -------------------------------------------------------------------
    // Sync objects
    // -------------------------------------------------------------------

    void createSyncObjects() {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo     fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            VK_CHECK(vkCreateSemaphore(device, &si, nullptr, &imageReady[i]));
            VK_CHECK(vkCreateSemaphore(device, &si, nullptr, &renderDone[i]));
            VK_CHECK(vkCreateFence    (device, &fi, nullptr, &inFlight  [i]));
        }
    }

    // -------------------------------------------------------------------
    // Record command buffer
    // -------------------------------------------------------------------

    void recordCommandBuffer(uint32_t idx) {
        VkCommandBuffer cb = cmdBufs[idx];
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));

        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderArea      = {{0,0}, swapExtent};
        rpbi.clearValueCount = 1;

        // -- LEFT eye: CYAN (R=0, G=1, B=1) ---------------------------------
        {
            VkClearValue cv{};
            cv.color.float32[0] = 0.f;  // R
            cv.color.float32[1] = 1.f;  // G
            cv.color.float32[2] = 1.f;  // B
            cv.color.float32[3] = 1.f;  // A
            rpbi.renderPass  = renderPassLeft;
            rpbi.framebuffer = fbLeft[idx];
            rpbi.pClearValues = &cv;
            vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            // No draw calls -- the clear colour IS the entire frame.
            vkCmdEndRenderPass(cb);
            // After: layer 0 is in PRESENT_SRC_KHR layout.
        }

        // -- RIGHT eye: RED (R=1, G=0, B=0) ----------------------------------
        //
        // Layer 1 has not been written this frame.  renderPassRight has
        // initialLayout=UNDEFINED (discard) which is always legal regardless
        // of layer 1's actual layout -- so NO barrier is required here.
        //
        // The old code issued a barrier with newLayout=UNDEFINED which is
        // explicitly forbidden by VUID-VkImageMemoryBarrier-newLayout-01198.
        {
            VkClearValue cv{};
            cv.color.float32[0] = 1.f;  // R
            cv.color.float32[1] = 0.f;  // G
            cv.color.float32[2] = 0.f;  // B
            cv.color.float32[3] = 1.f;  // A
            rpbi.renderPass  = renderPassRight;
            rpbi.framebuffer = fbRight[idx];
            rpbi.pClearValues = &cv;
            vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdEndRenderPass(cb);
            // After: layer 1 is in PRESENT_SRC_KHR layout.
        }

        VK_CHECK(vkEndCommandBuffer(cb));
    }

    // -------------------------------------------------------------------
    // Draw frame
    // -------------------------------------------------------------------

    void drawFrame() {
        uint32_t fi = frameIndex % MAX_FRAMES;
        vkWaitForFences(device, 1, &inFlight[fi], VK_TRUE, UINT64_MAX);

        uint32_t imgIdx = 0;
        VkResult r = vkAcquireNextImageKHR(
            device, swapchain, UINT64_MAX, imageReady[fi], VK_NULL_HANDLE, &imgIdx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return;
        VK_CHECK(r);

        vkResetFences(device, 1, &inFlight[fi]);
        vkResetCommandBuffer(cmdBufs[imgIdx], 0);
        recordCommandBuffer(imgIdx);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount   = 1; si.pWaitSemaphores   = &imageReady[fi];
        si.pWaitDstStageMask    = &waitStage;
        si.commandBufferCount   = 1; si.pCommandBuffers   = &cmdBufs[imgIdx];
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &renderDone[fi];
        VK_CHECK(vkQueueSubmit(queue, 1, &si, inFlight[fi]));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &renderDone[fi];
        pi.swapchainCount     = 1; pi.pSwapchains     = &swapchain;
        pi.pImageIndices      = &imgIdx;
        vkQueuePresentKHR(queue, &pi);

        ++frameIndex;
    }

    // -------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------

    void cleanup() {
        if (device) vkDeviceWaitIdle(device);
        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            if (imageReady[i]) vkDestroySemaphore(device, imageReady[i], nullptr);
            if (renderDone[i]) vkDestroySemaphore(device, renderDone[i], nullptr);
            if (inFlight  [i]) vkDestroyFence    (device, inFlight  [i], nullptr);
        }
        if (cmdPool)         vkDestroyCommandPool (device, cmdPool,          nullptr);
        for (auto fb : fbLeft)   vkDestroyFramebuffer(device, fb, nullptr);
        for (auto fb : fbRight)  vkDestroyFramebuffer(device, fb, nullptr);
        for (auto v  : viewLeft) vkDestroyImageView  (device, v,  nullptr);
        for (auto v  : viewRight)vkDestroyImageView  (device, v,  nullptr);
        if (renderPassLeft)  vkDestroyRenderPass  (device, renderPassLeft,   nullptr);
        if (renderPassRight) vkDestroyRenderPass  (device, renderPassRight,  nullptr);
        if (swapchain)       vkDestroySwapchainKHR(device, swapchain,        nullptr);
        if (device)          vkDestroyDevice      (device,                   nullptr);
        if (surface)         vkDestroySurfaceKHR  (instance, surface,        nullptr);
        if (instance)        vkDestroyInstance    (instance,                 nullptr);
    }
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    AllocConsole();
    FILE *f = nullptr; freopen_s(&f, "CONOUT$", "w", stdout);

    StereoApp app{};
    try {
        s_hwnd = createFullscreenWindow(hInst);
        app.init(hInst, s_hwnd);

        MSG msg{};
        while (!s_quit) {
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
                    s_quit = true;
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (!s_quit) app.drawFrame();
        }
    } catch (const std::exception &e) {
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_OK | MB_ICONERROR);
    }
    app.cleanup();
    return 0;
}
