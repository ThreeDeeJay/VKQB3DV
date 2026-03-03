/*
 * vulkan_stereo_3dvision.cpp
 *
 * Stereoscopic 3D demo using Vulkan 1.1.114 + NVIDIA 3D Vision beta driver 426.06
 * on GeForce / consumer GPUs.
 *
 * STEREO MECHANISM
 * ────────────────
 * NVIDIA's 3D Vision beta driver (426.06 and below) exposes stereoscopic quad-
 * buffer support through a VkSwapchain whose imageArrayLayers == 2.  Layer 0
 * is the LEFT-eye image; layer 1 is the RIGHT-eye image.  No proprietary
 * extension is required beyond the standard WSI stack:
 *
 *   VK_KHR_surface
 *   VK_KHR_win32_surface
 *   VK_KHR_swapchain
 *
 * The driver signals support by reporting maxImageArrayLayers >= 2 on the
 * surface capabilities when 3D Vision is active in the NVIDIA Control Panel.
 *
 * PREREQUISITES
 * ─────────────
 *  • Windows 10 (or 7/8 with appropriate Vulkan runtime)
 *  • NVIDIA GeForce GPU with 3D Vision kit *or* 3D Vision Ready monitor
 *  • NVIDIA driver  ≤ 426.06  (the 3D Vision beta branch)
 *  • 3D Vision enabled in NVIDIA Control Panel → "Set up stereoscopic 3D"
 *  • Vulkan SDK 1.1.114 or compatible (only standard headers / vulkan-1.lib)
 *
 * BUILD  (Visual Studio, from a Developer Command Prompt)
 * ────────────────────────────────────────────────────────
 *   cl /std:c++17 /W3 /O2 /D_CRT_SECURE_NO_WARNINGS main.cpp ^
 *      /I%VULKAN_SDK%\Include ^
 *      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib
 *
 * BUILD  (CMake – see CMakeLists.txt)
 *
 * WHAT YOU SEE
 * ────────────
 *  Left  eye → solid CYAN  frame  (R=0, G=255, B=255)
 *  Right eye → solid RED   frame  (R=255, G=0, B=0)
 *  With 3D Vision active the glasses alternate so each eye sees its colour.
 */

// ── MUST come before any Windows headers to suppress min/max macros ──────────
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// ─── helpers ────────────────────────────────────────────────────────────────

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            char _buf[256];                                                    \
            snprintf(_buf, sizeof(_buf), "Vulkan error %d at line %d: %s",    \
                     (int)_r, __LINE__, #expr);                                \
            throw std::runtime_error(_buf);                                    \
        }                                                                      \
    } while (0)

// Safe clamp that avoids the std::clamp / Windows macro conflict
template<typename T>
static T vk_clamp(T val, T lo, T hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static void log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    printf("%s\n", buf);
}

// ─── Win32 window ───────────────────────────────────────────────────────────

static HWND s_hwnd  = nullptr;
static bool s_quit  = false;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY:
    case WM_CLOSE:
        s_quit = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND createWindow(HINSTANCE hInst, int w, int h) {
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "VkStereoWnd";
    RegisterClassExA(&wc);

    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(
        0, "VkStereoWnd",
        "Vulkan 3D Vision Stereo  (Left=Cyan | Right=Red)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

// ─── StereoApp ───────────────────────────────────────────────────────────────

struct StereoApp {
    // ── Vulkan handles ──
    VkInstance               instance       = VK_NULL_HANDLE;
    VkSurfaceKHR             surface        = VK_NULL_HANDLE;
    VkPhysicalDevice         physDev        = VK_NULL_HANDLE;
    VkDevice                 device         = VK_NULL_HANDLE;
    uint32_t                 queueFamily    = UINT32_MAX;
    VkQueue                  queue          = VK_NULL_HANDLE;

    VkSwapchainKHR           swapchain      = VK_NULL_HANDLE;
    VkFormat                 swapFmt        = VK_FORMAT_UNDEFINED;
    VkExtent2D               swapExtent{};
    uint32_t                 swapImageCount = 0;

    // Per swapchain image: two layer views (left=0, right=1) + two framebuffers
    std::vector<VkImage>        swapImages;
    std::vector<VkImageView>    viewLeft;    // layer 0 – cyan
    std::vector<VkImageView>    viewRight;   // layer 1 – red
    std::vector<VkFramebuffer>  fbLeft;
    std::vector<VkFramebuffer>  fbRight;

    VkRenderPass             renderPass   = VK_NULL_HANDLE;

    VkCommandPool            cmdPool      = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmdBufs;

    static const uint32_t    MAX_FRAMES   = 2;
    VkSemaphore              imageReady[MAX_FRAMES]{};
    VkSemaphore              renderDone[MAX_FRAMES]{};
    VkFence                  inFlight  [MAX_FRAMES]{};
    uint32_t                 frameIndex   = 0;

    // ── init ──────────────────────────────────────────────────────────────

    void init(HINSTANCE hInst, HWND hwnd) {
        createInstance();
        createSurface(hInst, hwnd);
        pickPhysicalDevice();
        createDevice();
        createSwapchain(hwnd);
        createRenderPass();
        createFramebuffers();
        createCommandPool();
        allocateCommandBuffers();
        createSyncObjects();
        log("Init complete. Swapchain %ux%u, %u images, imageArrayLayers=2",
            swapExtent.width, swapExtent.height, swapImageCount);
    }

    // ── instance ──────────────────────────────────────────────────────────

    void createInstance() {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName   = "VkStereo3DVision";
        ai.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        ai.apiVersion         = VK_API_VERSION_1_1;

        const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
        const char *exts[]   = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME
        };

        // Check validation layer availability (optional)
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> avail(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, avail.data());
        bool hasValidation = false;
        for (auto &lp : avail)
            if (strcmp(lp.layerName, layers[0]) == 0) { hasValidation = true; break; }

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo        = &ai;
        ci.enabledExtensionCount   = 2;
        ci.ppEnabledExtensionNames = exts;
        if (hasValidation) {
            ci.enabledLayerCount   = 1;
            ci.ppEnabledLayerNames = layers;
        }

        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
        log("Vulkan instance created (validation %s)", hasValidation ? "ON" : "OFF");
    }

    // ── surface ───────────────────────────────────────────────────────────

    void createSurface(HINSTANCE hInst, HWND hwnd) {
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance = hInst;
        ci.hwnd      = hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface));
    }

    // ── physical device ───────────────────────────────────────────────────

    void pickPhysicalDevice() {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) throw std::runtime_error("No Vulkan physical devices found");
        std::vector<VkPhysicalDevice> devs(count);
        vkEnumeratePhysicalDevices(instance, &count, devs.data());

        for (auto pd : devs) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pd, &props);

            uint32_t qfCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qfCount);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qf.data());

            for (uint32_t i = 0; i < qfCount; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
                if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    physDev     = pd;
                    queueFamily = i;

                    // ──────────────────────────────────────────────────────
                    // STEREO CHECK
                    // maxImageArrayLayers >= 2 is the necessary condition for
                    // the 3D Vision quad-buffer stereo path.  The NVIDIA 3D
                    // Vision beta driver sets this to 2 when stereoscopic 3D
                    // is enabled in the control panel.
                    // ──────────────────────────────────────────────────────
                    VkSurfaceCapabilitiesKHR caps{};
                    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
                    log("GPU: %s | surface maxImageArrayLayers = %u",
                        props.deviceName, caps.maxImageArrayLayers);
                    if (caps.maxImageArrayLayers < 2) {
                        log("WARNING: maxImageArrayLayers < 2 on this surface.");
                        log("  -> Ensure 3D Vision is ENABLED in NVIDIA Control Panel");
                        log("     and you are running driver <= 426.06.");
                        log("  -> Falling back to single-layer swapchain (no stereo).");
                    }
                    break;
                }
            }
            if (physDev != VK_NULL_HANDLE) break;
        }
        if (physDev == VK_NULL_HANDLE)
            throw std::runtime_error("No suitable GPU found");
    }

    // ── logical device ────────────────────────────────────────────────────

    void createDevice() {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &prio;

        const char *devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount    = 1;
        ci.pQueueCreateInfos       = &qci;
        ci.enabledExtensionCount   = 1;
        ci.ppEnabledExtensionNames = devExts;

        VK_CHECK(vkCreateDevice(physDev, &ci, nullptr, &device));
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
    }

    // ── swapchain ─────────────────────────────────────────────────────────

    void createSwapchain(HWND hwnd) {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev, surface, &caps);

        // Format selection: prefer B8G8R8A8_UNORM / SRGB_NONLINEAR
        uint32_t fmtCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, surface, &fmtCount, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, surface, &fmtCount, fmts.data());
        VkSurfaceFormatKHR chosen = fmts[0];
        for (auto &f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                chosen = f;
        swapFmt = chosen.format;

        // Present mode: FIFO (vsync) for stable stereo timing
        VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;

        // Extent – use vk_clamp to avoid the Windows min/max macro conflict
        RECT rc; GetClientRect(hwnd, &rc);
        uint32_t winW = (rc.right  > rc.left) ? (uint32_t)(rc.right  - rc.left) : 1u;
        uint32_t winH = (rc.bottom > rc.top)  ? (uint32_t)(rc.bottom - rc.top)  : 1u;

        swapExtent.width  = vk_clamp(winW, caps.minImageExtent.width,  caps.maxImageExtent.width);
        swapExtent.height = vk_clamp(winH, caps.minImageExtent.height, caps.maxImageExtent.height);

        uint32_t imgCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount)
            imgCount = caps.maxImageCount;

        // ──────────────────────────────────────────────────────────────────
        // KEY: imageArrayLayers = 2
        //
        // This is the entire "trick" for NVIDIA 3D Vision stereo on consumer
        // GPUs via the 426.06 beta driver.  Layer 0 → left eye (cyan),
        // layer 1 → right eye (red).  The NVIDIA driver intercepts Present()
        // and drives the LCD shutter glasses accordingly.
        //
        // The surface must report maxImageArrayLayers >= 2 (it will when
        // 3D Vision is active).  No other proprietary extension needed.
        // ──────────────────────────────────────────────────────────────────
        uint32_t layers = (caps.maxImageArrayLayers >= 2) ? 2u : 1u;
        if (layers == 1)
            log("STEREO DISABLED – rendering single layer only");
        else
            log("STEREO ENABLED  – imageArrayLayers = 2");

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface               = surface;
        ci.minImageCount         = imgCount;
        ci.imageFormat           = swapFmt;
        ci.imageColorSpace       = chosen.colorSpace;
        ci.imageExtent           = swapExtent;
        ci.imageArrayLayers      = layers;           // <─── STEREO KEY
        ci.imageUsage            = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform          = caps.currentTransform;
        ci.compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode           = pm;
        ci.clipped               = VK_TRUE;

        VK_CHECK(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain));

        vkGetSwapchainImagesKHR(device, swapchain, &swapImageCount, nullptr);
        swapImages.resize(swapImageCount);
        vkGetSwapchainImagesKHR(device, swapchain, &swapImageCount, swapImages.data());

        viewLeft .resize(swapImageCount, VK_NULL_HANDLE);
        viewRight.resize(swapImageCount, VK_NULL_HANDLE);

        for (uint32_t i = 0; i < swapImageCount; ++i) {
            auto makeView = [&](uint32_t layer) -> VkImageView {
                VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vi.image    = swapImages[i];
                vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vi.format   = swapFmt;
                vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                vi.subresourceRange.baseMipLevel   = 0;
                vi.subresourceRange.levelCount     = 1;
                vi.subresourceRange.baseArrayLayer = layer; // 0=left, 1=right
                vi.subresourceRange.layerCount     = 1;
                VkImageView v = VK_NULL_HANDLE;
                VK_CHECK(vkCreateImageView(device, &vi, nullptr, &v));
                return v;
            };
            viewLeft [i] = makeView(0);
            viewRight[i] = (layers == 2) ? makeView(1) : makeView(0);
        }
    }

    // ── render pass ───────────────────────────────────────────────────────

    void createRenderPass() {
        VkAttachmentDescription att{};
        att.format         = swapFmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1;
        ci.pAttachments    = &att;
        ci.subpassCount    = 1;
        ci.pSubpasses      = &sub;
        ci.dependencyCount = 1;
        ci.pDependencies   = &dep;

        VK_CHECK(vkCreateRenderPass(device, &ci, nullptr, &renderPass));
    }

    // ── framebuffers ──────────────────────────────────────────────────────

    void createFramebuffers() {
        fbLeft .resize(swapImageCount, VK_NULL_HANDLE);
        fbRight.resize(swapImageCount, VK_NULL_HANDLE);

        for (uint32_t i = 0; i < swapImageCount; ++i) {
            auto makeFB = [&](VkImageView view) -> VkFramebuffer {
                VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                fi.renderPass      = renderPass;
                fi.attachmentCount = 1;
                fi.pAttachments    = &view;
                fi.width           = swapExtent.width;
                fi.height          = swapExtent.height;
                fi.layers          = 1;
                VkFramebuffer fb = VK_NULL_HANDLE;
                VK_CHECK(vkCreateFramebuffer(device, &fi, nullptr, &fb));
                return fb;
            };
            fbLeft [i] = makeFB(viewLeft [i]);
            fbRight[i] = makeFB(viewRight[i]);
        }
    }

    // ── command pool / buffers ────────────────────────────────────────────

    void createCommandPool() {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = queueFamily;
        ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device, &ci, nullptr, &cmdPool));
    }

    void allocateCommandBuffers() {
        cmdBufs.resize(swapImageCount);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool        = cmdPool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = swapImageCount;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, cmdBufs.data()));
    }

    // ── sync objects ──────────────────────────────────────────────────────

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

    // ── render ────────────────────────────────────────────────────────────

    void recordCommandBuffer(uint32_t imageIndex) {
        VkCommandBuffer cb = cmdBufs[imageIndex];
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));

        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass  = renderPass;
        rpbi.renderArea  = {{0, 0}, swapExtent};

        // ── Left eye: CYAN  (R=0, G=1, B=1) ──────────────────────────────
        {
            VkClearValue cv{};
            cv.color.float32[0] = 0.0f;  // R
            cv.color.float32[1] = 1.0f;  // G
            cv.color.float32[2] = 1.0f;  // B
            cv.color.float32[3] = 1.0f;  // A
            rpbi.framebuffer     = fbLeft[imageIndex];
            rpbi.clearValueCount = 1;
            rpbi.pClearValues    = &cv;
            vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            // No draw calls – the clear colour IS the entire content
            vkCmdEndRenderPass(cb);
        }

        // ── Right eye: RED  (R=1, G=0, B=0) ─────────────────────────────
        //
        // The right-eye view addresses a different baseArrayLayer of the same
        // VkImage.  We transition that layer back to UNDEFINED before the
        // second render-pass so that loadOp=CLEAR works correctly.
        {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.newLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image               = swapImages[imageIndex];
            barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 1};
            vkCmdPipelineBarrier(
                cb,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);

            VkClearValue cv{};
            cv.color.float32[0] = 1.0f;  // R
            cv.color.float32[1] = 0.0f;  // G
            cv.color.float32[2] = 0.0f;  // B
            cv.color.float32[3] = 1.0f;  // A
            rpbi.framebuffer     = fbRight[imageIndex];
            rpbi.clearValueCount = 1;
            rpbi.pClearValues    = &cv;
            vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdEndRenderPass(cb);
        }

        VK_CHECK(vkEndCommandBuffer(cb));
    }

    void drawFrame() {
        uint32_t fi = frameIndex % MAX_FRAMES;

        vkWaitForFences(device, 1, &inFlight[fi], VK_TRUE, UINT64_MAX);

        uint32_t imgIdx = 0;
        VkResult res = vkAcquireNextImageKHR(
            device, swapchain, UINT64_MAX, imageReady[fi], VK_NULL_HANDLE, &imgIdx);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
            return; // skip minimised / resized frame
        VK_CHECK(res);

        vkResetFences(device, 1, &inFlight[fi]);
        vkResetCommandBuffer(cmdBufs[imgIdx], 0);
        recordCommandBuffer(imgIdx);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &imageReady[fi];
        si.pWaitDstStageMask    = &waitStage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmdBufs[imgIdx];
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &renderDone[fi];
        VK_CHECK(vkQueueSubmit(queue, 1, &si, inFlight[fi]));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &renderDone[fi];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &swapchain;
        pi.pImageIndices      = &imgIdx;
        vkQueuePresentKHR(queue, &pi);

        ++frameIndex;
    }

    // ── cleanup ───────────────────────────────────────────────────────────

    void cleanup() {
        if (device) vkDeviceWaitIdle(device);

        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            if (imageReady[i]) vkDestroySemaphore(device, imageReady[i], nullptr);
            if (renderDone[i]) vkDestroySemaphore(device, renderDone[i], nullptr);
            if (inFlight  [i]) vkDestroyFence    (device, inFlight  [i], nullptr);
        }
        if (cmdPool) vkDestroyCommandPool(device, cmdPool, nullptr);
        for (auto fb : fbLeft)   vkDestroyFramebuffer(device, fb, nullptr);
        for (auto fb : fbRight)  vkDestroyFramebuffer(device, fb, nullptr);
        for (auto v  : viewLeft) vkDestroyImageView  (device, v,  nullptr);
        for (auto v  : viewRight)vkDestroyImageView  (device, v,  nullptr);
        if (renderPass) vkDestroyRenderPass  (device, renderPass, nullptr);
        if (swapchain)  vkDestroySwapchainKHR(device, swapchain,  nullptr);
        if (device)     vkDestroyDevice      (device, nullptr);
        if (surface)    vkDestroySurfaceKHR  (instance, surface,  nullptr);
        if (instance)   vkDestroyInstance    (instance, nullptr);
    }
};

// ─── entry point ─────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Attach a console for log output (freopen_s avoids the C4996 deprecation)
    AllocConsole();
    FILE *dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);

    StereoApp app{};
    try {
        s_hwnd = createWindow(hInst, 1280, 720);
        app.init(hInst, s_hwnd);

        MSG msg{};
        while (!s_quit) {
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
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
