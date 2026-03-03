/*
 * vulkan_stereo_3dvision.cpp
 *
 * Stereoscopic 3D – NVIDIA 3D Vision beta driver 426.06, GeForce / consumer GPU.
 *
 * ── SWAPCHAIN PATHS ──────────────────────────────────────────────────────────
 *
 *  STEREO  (3D Vision ON, maxImageArrayLayers == 2):
 *    imageArrayLayers = 2, NO FSE extension.
 *    The 3D Vision driver handles exclusive display ownership internally via
 *    its own stereo mode-switch; chaining VkSurfaceFullScreenExclusiveInfoEXT
 *    into the same CreateInfo crashes the driver (tested on 426.06 / RTX 2080Ti).
 *    The window must still be WS_POPUP covering the full display so the driver
 *    can flip without DWM interference.
 *
 *  NON-STEREO  (3D Vision OFF, maxImageArrayLayers == 1):
 *    imageArrayLayers = 1, VK_EXT_full_screen_exclusive APPLICATION_CONTROLLED.
 *    Explicit vkAcquireFullScreenExclusiveModeEXT for crisp, tearing-free output.
 *    Only the LEFT eye (cyan) render pass executes; the right-eye pass is skipped
 *    entirely so the second clear does not overwrite the first on the same layer.
 *
 * ── BUILD ────────────────────────────────────────────────────────────────────
 *   cl /std:c++17 /W3 /O2 main.cpp /I%VULKAN_SDK%\Include ^
 *      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib
 *
 *   or: cmake -S . -B build -G "Visual Studio 17 2022" -A x64
 *       cmake --build build --config Release
 *
 * ── PREREQUISITES ────────────────────────────────────────────────────────────
 *   • NVIDIA GeForce + 3D Vision kit or 3D Vision Ready monitor
 *   • Driver <= 426.06   (the 3D Vision beta branch)
 *   • 3D Vision enabled in NVIDIA Control Panel -> "Set up stereoscopic 3D"
 *   • Vulkan SDK 1.1.114+
 *
 * ── OUTPUT ───────────────────────────────────────────────────────────────────
 *   Left  eye -> CYAN  (layer 0)   Right eye -> RED  (layer 1)
 *   Log written to  vulkan_stereo.log  beside the executable (flushed per line).
 *   Press ESC to quit.
 */

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

// ── Logging ──────────────────────────────────────────────────────────────────
// Every line is flushed immediately so a hard crash still leaves a complete log.

static FILE *s_logFile = nullptr;

static void logInit() {
    // Place log next to the executable
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    // Replace filename with "vulkan_stereo.log"
    char *slash = strrchr(exePath, '\\');
    if (slash) *(slash + 1) = '\0'; else exePath[0] = '\0';
    char logPath[MAX_PATH]{};
    snprintf(logPath, MAX_PATH, "%svulkan_stereo.log", exePath);
    s_logFile = fopen(logPath, "w");
    // Also print log path to console
    printf("Log file: %s\n", logPath);
    fflush(stdout);
}

static void log(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);

    // Console + debugger
    puts(buf);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    // File – flush every line so a crash leaves a complete log
    if (s_logFile) {
        fputs(buf,  s_logFile);
        fputc('\n', s_logFile);
        fflush(s_logFile);
    }
}

// ── VK_CHECK ─────────────────────────────────────────────────────────────────

#define VK_CHECK(expr)                                                              \
    do {                                                                            \
        log("  CALL  " #expr);                                                      \
        VkResult _r = (expr);                                                       \
        if (_r != VK_SUCCESS) {                                                     \
            char _b[512];                                                           \
            snprintf(_b, sizeof(_b),                                                \
                     "FATAL Vulkan error %d at line %d\n" #expr, (int)_r, __LINE__);\
            log(_b);                                                                \
            throw std::runtime_error(_b);                                           \
        }                                                                           \
        log("  OK    " #expr);                                                      \
    } while(0)

// Same but tolerates VK_SUBOPTIMAL / VK_ERROR_OUT_OF_DATE without throwing
#define VK_CHECK_PRESENT(expr, result_out)                                          \
    do {                                                                            \
        (result_out) = (expr);                                                      \
        if ((result_out) != VK_SUCCESS &&                                           \
            (result_out) != VK_SUBOPTIMAL_KHR &&                                   \
            (result_out) != VK_ERROR_OUT_OF_DATE_KHR) {                            \
            char _b[512];                                                           \
            snprintf(_b, sizeof(_b),                                                \
                     "FATAL Vulkan error %d at line %d\n" #expr,                   \
                     (int)(result_out), __LINE__);                                  \
            log(_b); throw std::runtime_error(_b);                                 \
        }                                                                           \
    } while(0)

// ── Utilities ─────────────────────────────────────────────────────────────────

template<class T> static T vk_clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
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

// ── Win32 window ──────────────────────────────────────────────────────────────

static HWND s_hwnd = nullptr;
static bool s_quit = false;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY || msg == WM_CLOSE) {
        s_quit = true; PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// Borderless WS_POPUP covering the full primary monitor.
// Required for BOTH paths:
//  - Stereo:     driver needs to flip without DWM compositing.
//  - Non-stereo: vkAcquireFullScreenExclusiveModeEXT requires full-display coverage.
static HWND createFullscreenWindow(HINSTANCE hInst) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = "VkStereoWnd";
    RegisterClassExA(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    log("Primary monitor: %dx%d", sw, sh);

    HWND hwnd = CreateWindowExA(
        0, "VkStereoWnd",
        "Vulkan 3D Vision Stereo  [Left=Cyan | Right=Red]  (ESC to quit)",
        WS_POPUP,
        0, 0, sw, sh,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    log("Window created: WS_POPUP %dx%d", sw, sh);
    return hwnd;
}

// ── Application ───────────────────────────────────────────────────────────────

struct StereoApp {
    VkInstance       instance    = VK_NULL_HANDLE;
    VkSurfaceKHR     surface     = VK_NULL_HANDLE;
    VkPhysicalDevice physDev     = VK_NULL_HANDLE;
    VkDevice         device      = VK_NULL_HANDLE;
    uint32_t         queueFamily = UINT32_MAX;
    VkQueue          queue       = VK_NULL_HANDLE;

    VkSwapchainKHR           swapchain  = VK_NULL_HANDLE;
    VkFormat                 swapFmt    = VK_FORMAT_UNDEFINED;
    VkExtent2D               swapExtent{};
    uint32_t                 swapCount  = 0;
    bool                     stereo     = false;  // true = imageArrayLayers==2

    std::vector<VkImage>        swapImages;
    std::vector<VkImageView>    viewLeft, viewRight;
    std::vector<VkFramebuffer>  fbLeft,   fbRight;

    // Two separate render pass objects (both initialLayout=UNDEFINED).
    // Having two distinct objects is cleaner and avoids any ambiguity about
    // which pass applies to which layer's layout state.
    VkRenderPass renderPassLeft  = VK_NULL_HANDLE;
    VkRenderPass renderPassRight = VK_NULL_HANDLE;

    VkCommandPool                cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmdBufs;

    static const uint32_t MAX_FRAMES = 2;
    VkSemaphore imageReady[MAX_FRAMES]{};
    VkSemaphore renderDone[MAX_FRAMES]{};
    VkFence     inFlight  [MAX_FRAMES]{};
    uint32_t    frameIndex = 0;

    // Extension availability
    bool hasSurfaceCaps2 = false;
    bool hasFSE          = false;

    // FSE function pointer (non-stereo path only)
    PFN_vkAcquireFullScreenExclusiveModeEXT pfnAcquireFSE = nullptr;

    // ── init ──────────────────────────────────────────────────────────────

    void init(HINSTANCE hInst, HWND hwnd) {
        log("=== init begin ===");
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
        log("=== init complete: %ux%u, %u images, stereo=%s ===",
            swapExtent.width, swapExtent.height, swapCount,
            stereo ? "YES" : "NO");
    }

    // ── instance ──────────────────────────────────────────────────────────

    void createInstance() {
        log("--- createInstance ---");

        hasSurfaceCaps2 = hasInstanceExt(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        log("VK_KHR_get_surface_capabilities2: %s",
            hasSurfaceCaps2 ? "available" : "NOT available");

        std::vector<const char *> exts = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        if (hasSurfaceCaps2)
            exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

        const char *valLayer = "VK_LAYER_KHRONOS_validation";
        uint32_t lc = 0; vkEnumerateInstanceLayerProperties(&lc, nullptr);
        std::vector<VkLayerProperties> lp(lc);
        vkEnumerateInstanceLayerProperties(&lc, lp.data());
        bool hasVal = false;
        for (auto &l : lp) if (strcmp(l.layerName, valLayer) == 0) { hasVal = true; break; }
        log("Validation layer: %s", hasVal ? "available" : "NOT available");

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

    // ── surface ───────────────────────────────────────────────────────────

    void createSurface(HINSTANCE hInst, HWND hwnd) {
        log("--- createSurface ---");
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance = hInst; ci.hwnd = hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface));
    }

    // ── physical device ───────────────────────────────────────────────────

    void pickPhysicalDevice() {
        log("--- pickPhysicalDevice ---");
        uint32_t cnt = 0;
        vkEnumeratePhysicalDevices(instance, &cnt, nullptr);
        log("Physical devices found: %u", cnt);
        if (!cnt) throw std::runtime_error("No Vulkan GPUs");
        std::vector<VkPhysicalDevice> devs(cnt);
        vkEnumeratePhysicalDevices(instance, &cnt, devs.data());

        for (auto pd : devs) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pd, &props);
            log("  Device: %s (driver 0x%x)", props.deviceName, props.driverVersion);

            uint32_t qfc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qfc);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qf.data());

            for (uint32_t i = 0; i < qfc; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
                if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;

                physDev = pd; queueFamily = i;
                log("  Selected queue family: %u", i);

                VkSurfaceCapabilitiesKHR caps{};
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
                log("  Surface caps: minImageCount=%u maxImageCount=%u "
                    "maxImageArrayLayers=%u",
                    caps.minImageCount, caps.maxImageCount, caps.maxImageArrayLayers);

                if (caps.maxImageArrayLayers < 2)
                    log("  WARNING: maxImageArrayLayers < 2 -- 3D Vision not active "
                        "or driver > 426.06");

                hasFSE = hasSurfaceCaps2 &&
                         hasDeviceExt(pd, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
                log("  VK_EXT_full_screen_exclusive: %s",
                    hasFSE ? "available" : "NOT available");
                break;
            }
            if (physDev) break;
        }
        if (!physDev) throw std::runtime_error("No suitable GPU found");
    }

    // ── device ────────────────────────────────────────────────────────────

    void createDevice() {
        log("--- createDevice ---");
        float prio = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily; qci.queueCount = 1; qci.pQueuePriorities = &prio;

        // FSE is only added to the device for the NON-STEREO path.
        // When stereo is active the 3D Vision driver manages exclusive display
        // ownership itself; combining FSE with imageArrayLayers=2 crashes 426.06.
        // We don't yet know whether stereo will be active (that requires the
        // surface caps check done in pickPhysicalDevice), so we load FSE here
        // if available but only use it in createSwapchain() for the non-stereo path.
        std::vector<const char *> exts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        if (hasFSE) {
            exts.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
            log("  Enabling VK_EXT_full_screen_exclusive on device");
        }

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
            log("  vkAcquireFullScreenExclusiveModeEXT: %s",
                pfnAcquireFSE ? "loaded" : "NOT loaded");
            if (!pfnAcquireFSE) hasFSE = false;
        }
    }

    // ── swapchain ─────────────────────────────────────────────────────────
    //
    // STEREO PATH (maxImageArrayLayers == 2):
    //   imageArrayLayers=2, NO FSE extension chain.
    //   The NVIDIA 3D Vision driver handles the exclusive display flip itself.
    //   Chaining VkSurfaceFullScreenExclusiveInfoEXT into this swapchain
    //   CreateInfo crashes driver 426.06 on a stereo-mode surface.
    //
    // NON-STEREO PATH (maxImageArrayLayers == 1):
    //   imageArrayLayers=1, FSE APPLICATION_CONTROLLED.
    //   Explicit vkAcquireFullScreenExclusiveModeEXT for exclusive output.
    //
    void createSwapchain(HWND hwnd) {
        log("--- createSwapchain ---");

        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev, surface, &caps);

        // Format
        uint32_t fc = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, surface, &fc, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fc);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, surface, &fc, fmts.data());
        VkSurfaceFormatKHR fmt = fmts[0];
        for (auto &f : fmts)
            if (f.format     == VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt = f;
        swapFmt = fmt.format;
        log("  swapFmt=%d colorSpace=%d", (int)swapFmt, (int)fmt.colorSpace);

        // Extent
        RECT rc{}; GetClientRect(hwnd, &rc);
        uint32_t ww = (uint32_t)(rc.right  - rc.left); if (!ww) ww = 1;
        uint32_t wh = (uint32_t)(rc.bottom - rc.top);  if (!wh) wh = 1;
        swapExtent.width  = vk_clamp(ww, caps.minImageExtent.width,  caps.maxImageExtent.width);
        swapExtent.height = vk_clamp(wh, caps.minImageExtent.height, caps.maxImageExtent.height);
        log("  swapExtent: %ux%u (window %ux%u)", swapExtent.width, swapExtent.height, ww, wh);

        uint32_t imgCount = caps.minImageCount + 1;
        if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;
        log("  imgCount: %u", imgCount);

        // Decide stereo vs non-stereo
        stereo = (caps.maxImageArrayLayers >= 2);
        uint32_t layers = stereo ? 2u : 1u;
        log(stereo
            ? "STEREO path: imageArrayLayers=2, NO FSE (3D Vision driver manages display)"
            : "NON-STEREO path: imageArrayLayers=1, FSE APPLICATION_CONTROLLED");

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface          = surface;
        ci.minImageCount    = imgCount;
        ci.imageFormat      = swapFmt;
        ci.imageColorSpace  = fmt.colorSpace;
        ci.imageExtent      = swapExtent;
        ci.imageArrayLayers = layers;             // KEY: 2=stereo, 1=mono
        ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform     = caps.currentTransform;
        ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped          = VK_TRUE;
        // ci.pNext intentionally left null for stereo path (see note above)

        // NON-STEREO ONLY: chain FSE into swapchain create info
        VkSurfaceFullScreenExclusiveWin32InfoEXT fseWin32{
            VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
        VkSurfaceFullScreenExclusiveInfoEXT fseInfo{
            VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};

        if (!stereo && hasFSE) {
            fseWin32.hmonitor         = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
            fseInfo.pNext             = &fseWin32;
            fseInfo.fullScreenExclusive =
                VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
            ci.pNext = &fseInfo;
            log("  FSE chain attached to swapchain CreateInfo");
        }

        log("  Calling vkCreateSwapchainKHR (imageArrayLayers=%u, pNext=%s)...",
            layers, ci.pNext ? "FSE chain" : "null");
        VK_CHECK(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain));

        // NON-STEREO ONLY: acquire exclusive display ownership
        if (!stereo && hasFSE && pfnAcquireFSE) {
            log("  Calling vkAcquireFullScreenExclusiveModeEXT...");
            VkResult r = pfnAcquireFSE(device, swapchain);
            log("  vkAcquireFullScreenExclusiveModeEXT returned %d (%s)",
                (int)r, r == VK_SUCCESS ? "OK" : "FAILED");
        }

        // Images + per-layer views
        vkGetSwapchainImagesKHR(device, swapchain, &swapCount, nullptr);
        swapImages.resize(swapCount);
        vkGetSwapchainImagesKHR(device, swapchain, &swapCount, swapImages.data());
        log("  swapCount: %u", swapCount);

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
            viewLeft[i] = makeView(0);
            // Right-eye view only makes sense for the stereo path;
            // in non-stereo mode we never use viewRight / fbRight.
            viewRight[i] = stereo ? makeView(1) : VK_NULL_HANDLE;
            log("  image[%u]: viewLeft=%p viewRight=%p",
                i, (void*)viewLeft[i], (void*)viewRight[i]);
        }
    }

    // ── render passes ─────────────────────────────────────────────────────
    // Both use initialLayout=UNDEFINED (discard prior content – always valid,
    // no barrier needed) and finalLayout=PRESENT_SRC_KHR.

    VkRenderPass makeRenderPass() {
        VkAttachmentDescription att{};
        att.format         = swapFmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;       // discard – always legal
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

        VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rci.attachmentCount = 1; rci.pAttachments  = &att;
        rci.subpassCount    = 1; rci.pSubpasses    = &sub;
        rci.dependencyCount = 1; rci.pDependencies = &dep;
        VkRenderPass rp = VK_NULL_HANDLE;
        VK_CHECK(vkCreateRenderPass(device, &rci, nullptr, &rp));
        return rp;
    }

    void createRenderPasses() {
        log("--- createRenderPasses ---");
        renderPassLeft  = makeRenderPass();
        if (stereo)
            renderPassRight = makeRenderPass();
        // Non-stereo: renderPassRight stays VK_NULL_HANDLE; it is never used.
    }

    // ── framebuffers ──────────────────────────────────────────────────────

    void createFramebuffers() {
        log("--- createFramebuffers ---");
        fbLeft .resize(swapCount, VK_NULL_HANDLE);
        fbRight.resize(swapCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < swapCount; ++i) {
            auto makeFB = [&](VkRenderPass rp, VkImageView view) -> VkFramebuffer {
                VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                fi.renderPass      = rp;
                fi.attachmentCount = 1; fi.pAttachments = &view;
                fi.width           = swapExtent.width;
                fi.height          = swapExtent.height;
                fi.layers          = 1;
                VkFramebuffer fb = VK_NULL_HANDLE;
                VK_CHECK(vkCreateFramebuffer(device, &fi, nullptr, &fb));
                return fb;
            };
            fbLeft [i] = makeFB(renderPassLeft, viewLeft[i]);
            if (stereo)
                fbRight[i] = makeFB(renderPassRight, viewRight[i]);
        }
    }

    // ── command pool + buffers ────────────────────────────────────────────

    void createCommandPool() {
        log("--- createCommandPool ---");
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = queueFamily;
        ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device, &ci, nullptr, &cmdPool));
    }

    void allocateCommandBuffers() {
        log("--- allocateCommandBuffers ---");
        cmdBufs.resize(swapCount);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool        = cmdPool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = swapCount;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, cmdBufs.data()));
    }

    // ── sync objects ──────────────────────────────────────────────────────

    void createSyncObjects() {
        log("--- createSyncObjects ---");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo     fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            VK_CHECK(vkCreateSemaphore(device, &si, nullptr, &imageReady[i]));
            VK_CHECK(vkCreateSemaphore(device, &si, nullptr, &renderDone[i]));
            VK_CHECK(vkCreateFence    (device, &fi, nullptr, &inFlight  [i]));
        }
    }

    // ── record ────────────────────────────────────────────────────────────

    void recordCommandBuffer(uint32_t idx) {
        VkCommandBuffer cb = cmdBufs[idx];
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));

        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderArea      = {{0,0}, swapExtent};
        rpbi.clearValueCount = 1;

        // ── LEFT eye: CYAN (R=0, G=1, B=1) – always rendered ─────────────
        {
            VkClearValue cv{};
            cv.color.float32[0] = 0.f;
            cv.color.float32[1] = 1.f;
            cv.color.float32[2] = 1.f;
            cv.color.float32[3] = 1.f;
            rpbi.renderPass  = renderPassLeft;
            rpbi.framebuffer = fbLeft[idx];
            rpbi.pClearValues = &cv;
            vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            // Clear colour IS the full frame content; no draw calls needed.
            vkCmdEndRenderPass(cb);
        }

        // ── RIGHT eye: RED (R=1, G=0, B=0) – STEREO ONLY ─────────────────
        // Skipped in non-stereo mode. Previously this pass ran on layer 0
        // (same as left eye in fallback) and overwrote cyan with red, causing
        // only the red frame to be visible. Now we guard with `stereo`.
        if (stereo) {
            VkClearValue cv{};
            cv.color.float32[0] = 1.f;
            cv.color.float32[1] = 0.f;
            cv.color.float32[2] = 0.f;
            cv.color.float32[3] = 1.f;
            rpbi.renderPass  = renderPassRight;
            rpbi.framebuffer = fbRight[idx];
            rpbi.pClearValues = &cv;
            vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            // initialLayout=UNDEFINED on renderPassRight: no barrier needed.
            vkCmdEndRenderPass(cb);
        }

        VK_CHECK(vkEndCommandBuffer(cb));
    }

    // ── draw frame ────────────────────────────────────────────────────────

    void drawFrame() {
        uint32_t fi = frameIndex % MAX_FRAMES;
        vkWaitForFences(device, 1, &inFlight[fi], VK_TRUE, UINT64_MAX);

        uint32_t imgIdx = 0;
        VkResult r;
        VK_CHECK_PRESENT(vkAcquireNextImageKHR(
            device, swapchain, UINT64_MAX, imageReady[fi], VK_NULL_HANDLE, &imgIdx), r);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return;

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
        VK_CHECK_PRESENT(vkQueuePresentKHR(queue, &pi), r);

        ++frameIndex;
    }

    // ── cleanup ───────────────────────────────────────────────────────────

    void cleanup() {
        log("--- cleanup ---");
        if (device) vkDeviceWaitIdle(device);
        for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
            if (imageReady[i]) vkDestroySemaphore(device, imageReady[i], nullptr);
            if (renderDone[i]) vkDestroySemaphore(device, renderDone[i], nullptr);
            if (inFlight  [i]) vkDestroyFence    (device, inFlight  [i], nullptr);
        }
        if (cmdPool) vkDestroyCommandPool(device, cmdPool, nullptr);
        for (uint32_t i = 0; i < swapCount; ++i) {
            if (fbLeft [i]) vkDestroyFramebuffer(device, fbLeft [i], nullptr);
            if (fbRight[i]) vkDestroyFramebuffer(device, fbRight[i], nullptr);
            if (viewLeft [i]) vkDestroyImageView(device, viewLeft [i], nullptr);
            if (viewRight[i]) vkDestroyImageView(device, viewRight[i], nullptr);
        }
        if (renderPassLeft)  vkDestroyRenderPass  (device, renderPassLeft,  nullptr);
        if (renderPassRight) vkDestroyRenderPass  (device, renderPassRight, nullptr);
        if (swapchain)       vkDestroySwapchainKHR(device, swapchain,       nullptr);
        if (device)          vkDestroyDevice      (device,                  nullptr);
        if (surface)         vkDestroySurfaceKHR  (instance, surface,       nullptr);
        if (instance)        vkDestroyInstance    (instance,                nullptr);
        log("--- cleanup done ---");
    }
};

// ── Entry point ───────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    AllocConsole();
    FILE *f = nullptr; freopen_s(&f, "CONOUT$", "w", stdout);
    logInit();
    log("=== VkStereo3DVision startup ===");

    StereoApp app{};
    try {
        s_hwnd = createFullscreenWindow(hInst);
        app.init(hInst, s_hwnd);

        log("Entering render loop (ESC to quit)");
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
        log("Render loop exited");
    } catch (const std::exception &e) {
        log("EXCEPTION: %s", e.what());
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_OK | MB_ICONERROR);
    }
    app.cleanup();
    if (s_logFile) fclose(s_logFile);
    return 0;
}
