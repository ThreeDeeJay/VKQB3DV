/*
 * vulkan_stereo_3dvision.cpp  v6
 *
 * Stereoscopic 3D – NVIDIA 3D Vision beta driver 426.06, GeForce/consumer GPU.
 *
 * ── ROOT CAUSE HISTORY ───────────────────────────────────────────────────────
 *
 *  v5: stereo swapchain CI had pNext=null. The NVIDIA driver automatically
 *  acquires FSE (fullscreen exclusive) for any WS_POPUP window covering the
 *  full display ("fullscreen optimizations"). With pNext=null, the driver
 *  attempts auto-FSE AND stereo path setup simultaneously – two exclusive
 *  display ownership mechanisms collide internally → -3.
 *
 *  v6 fix: chain VkSurfaceFullScreenExclusiveInfoEXT with
 *  VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT into the STEREO swapchain CI.
 *  This explicitly tells the driver "do not use FSE for this surface; let
 *  your internal stereo mechanism own display exclusivity instead."
 *  VkSurfaceFullScreenExclusiveWin32InfoEXT is NOT needed for DISALLOWED.
 *
 *  This differs from all previous attempts:
 *    v2: APPLICATION_CONTROLLED chained  → -3  (FSE conflicts with stereo)
 *    v3: FSE on device, pNext=null       → -3  (auto-FSE conflicts)
 *    v4: FSE not on device, IMMEDIATE    → -3  (no vsync for shutter timing)
 *    v5: FSE not on device, pNext=null   → -3  (auto-FSE still conflicts)
 *    v6: FSE on device, DISALLOWED chain → ?   (prevent auto-FSE, use stereo path)
 *
 * ── SWAPCHAIN PATHS ──────────────────────────────────────────────────────────
 *
 *  STEREO  (3D Vision ON  →  maxImageArrayLayers == 2):
 *    - VK_EXT_full_screen_exclusive loaded on device.
 *    - imageArrayLayers = 2, FIFO, preTransform=IDENTITY.
 *    - pNext → VkSurfaceFullScreenExclusiveInfoEXT { DISALLOWED }.
 *      Prevents driver auto-FSE so its stereo path gets exclusive display
 *      ownership through its own internal mechanism.
 *    - imageCount = caps.minImageCount (minimum, typically 2).
 *
 *  NON-STEREO  (3D Vision OFF  →  maxImageArrayLayers == 1):
 *    - No FSE at all. WS_POPUP suffices; driver auto-FSE is harmless here.
 *    - imageArrayLayers = 1, FIFO.
 *    - Only left-eye (cyan) pass executes.
 *
 * ── DISPLAY REQUIREMENT ──────────────────────────────────────────────────────
 *  The monitor MUST be running at 120 Hz in NVIDIA's stereoscopic mode.
 *  If EnumDisplaySettings reports < 120 Hz, the driver will refuse the stereo
 *  swapchain regardless of any Vulkan parameters.  Set this in:
 *    NVIDIA Control Panel → "Set up stereoscopic 3D" → enable, then
 *    confirm the display mode is 1920×1080 @ 120 Hz (or your panel's stereo Hz).
 *
 * ── BUILD ────────────────────────────────────────────────────────────────────
 *   cl /std:c++17 /W3 /O2 main.cpp /I%VULKAN_SDK%\Include ^
 *      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib
 *
 *   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
 *   cmake --build build --config Release
 *
 * ── OUTPUT ───────────────────────────────────────────────────────────────────
 *   Left eye  →  CYAN  (layer 0)     Right eye  →  RED  (layer 1)
 *   Log: vulkan_stereo.log beside the exe, flushed per line.
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

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static FILE *s_logFile = nullptr;

static void logInit() {
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *sl = strrchr(exe, '\\');
    if (sl) sl[1] = '\0'; else exe[0] = '\0';
    char path[MAX_PATH]{};
    snprintf(path, MAX_PATH, "%svulkan_stereo.log", exe);
    s_logFile = fopen(path, "w");
    printf("Log: %s\n", path); fflush(stdout);
}

static void log(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    puts(buf);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
    if (s_logFile) { fputs(buf, s_logFile); fputc('\n', s_logFile); fflush(s_logFile); }
}

// ---------------------------------------------------------------------------
// Win32 display diagnostics
// ---------------------------------------------------------------------------

static void logDisplayMode() {
    log("--- display diagnostics ---");
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);

    // Current active mode
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        log("  Current display mode: %ux%u @ %u Hz  bpp=%u  flags=0x%x",
            dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
            dm.dmBitsPerPel, dm.dmDisplayFlags);
        if (dm.dmDisplayFrequency < 120)
            log("  WARNING: refresh rate is %u Hz.  3D Vision requires 120 Hz stereo mode.",
                dm.dmDisplayFrequency);
        else
            log("  Refresh rate OK (%u Hz >= 120 Hz)", dm.dmDisplayFrequency);
    } else {
        log("  EnumDisplaySettings(CURRENT) failed");
    }

    // Walk all modes and flag any 120 Hz stereo-capable entries
    log("  Enumerating all display modes for 120 Hz stereo candidates:");
    bool found120 = false;
    for (DWORD idx = 0; ; ++idx) {
        DEVMODEA m{};
        m.dmSize = sizeof(m);
        if (!EnumDisplaySettingsA(nullptr, idx, &m)) break;
        if (m.dmDisplayFrequency >= 120 && m.dmPelsWidth == dm.dmPelsWidth) {
            log("    mode[%u]: %ux%u @ %u Hz  bpp=%u  flags=0x%x",
                idx, m.dmPelsWidth, m.dmPelsHeight,
                m.dmDisplayFrequency, m.dmBitsPerPel, m.dmDisplayFlags);
            found120 = true;
        }
    }
    if (!found120)
        log("  WARNING: no 120 Hz mode found for current resolution – "
            "3D Vision may not be available on this display.");
}

// ---------------------------------------------------------------------------
// VK helpers
// ---------------------------------------------------------------------------

static const char *vkResultStr(VkResult r) {
    switch (r) {
    case VK_SUCCESS:                     return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR:              return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:       return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_DEVICE_LOST:           return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_SURFACE_LOST_KHR:      return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
                                         return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    default:                             return "(unknown)";
    }
}

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        log("  CALL  " #expr);                                                 \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            char _b[512];                                                      \
            snprintf(_b, sizeof(_b), "FATAL %s (%d) at line %d\n" #expr,      \
                     vkResultStr(_r), (int)_r, __LINE__);                      \
            log(_b); throw std::runtime_error(_b);                             \
        }                                                                      \
        log("  OK    " #expr);                                                 \
    } while(0)

#define VK_CHECK_SWAPCHAIN(expr, out)                                          \
    do {                                                                       \
        (out) = (expr);                                                        \
        if ((out) != VK_SUCCESS && (out) != VK_SUBOPTIMAL_KHR &&              \
            (out) != VK_ERROR_OUT_OF_DATE_KHR &&                              \
            (out) != VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {          \
            char _b[512];                                                      \
            snprintf(_b, sizeof(_b), "FATAL %s (%d) at line %d\n" #expr,      \
                     vkResultStr((VkResult)(out)), (int)(out), __LINE__);      \
            log(_b); throw std::runtime_error(_b);                             \
        }                                                                      \
        if ((out) != VK_SUCCESS)                                               \
            log("  WARN  " #expr " -> %s", vkResultStr((VkResult)(out)));      \
    } while(0)

template<class T> static T vk_clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static bool hasInstanceExt(const char *n) {
    uint32_t c=0; vkEnumerateInstanceExtensionProperties(nullptr,&c,nullptr);
    std::vector<VkExtensionProperties> v(c);
    vkEnumerateInstanceExtensionProperties(nullptr,&c,v.data());
    for(auto &e:v) if(!strcmp(e.extensionName,n)) return true;
    return false;
}
static bool hasDeviceExt(VkPhysicalDevice pd, const char *n) {
    uint32_t c=0; vkEnumerateDeviceExtensionProperties(pd,nullptr,&c,nullptr);
    std::vector<VkExtensionProperties> v(c);
    vkEnumerateDeviceExtensionProperties(pd,nullptr,&c,v.data());
    for(auto &e:v) if(!strcmp(e.extensionName,n)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Win32 window
// ---------------------------------------------------------------------------

static HWND s_hwnd = nullptr;
static bool s_quit = false;

static LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_DESTROY||msg==WM_CLOSE){s_quit=true;PostQuitMessage(0);return 0;}
    return DefWindowProcA(hwnd,msg,wp,lp);
}

static HWND createFullscreenWindow(HINSTANCE hInst) {
    WNDCLASSEXA wc{};
    wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.hCursor=LoadCursorA(nullptr,IDC_ARROW);
    wc.lpszClassName="VkStereoWnd";
    RegisterClassExA(&wc);

    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    log("Primary monitor: %dx%d", sw, sh);

    HWND hwnd=CreateWindowExA(0,"VkStereoWnd",
        "Vulkan 3D Vision Stereo [Left=Cyan | Right=Red]  ESC=quit",
        WS_POPUP, 0,0,sw,sh, nullptr,nullptr,hInst,nullptr);
    ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd);
    log("Window: WS_POPUP %dx%d", sw, sh);
    return hwnd;
}

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------

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
    bool                     stereo     = false;

    std::vector<VkImage>        swapImages;
    std::vector<VkImageView>    viewLeft, viewRight;
    std::vector<VkFramebuffer>  fbLeft,   fbRight;

    VkRenderPass renderPassLeft  = VK_NULL_HANDLE;
    VkRenderPass renderPassRight = VK_NULL_HANDLE;

    VkCommandPool                cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmdBufs;

    static const uint32_t MAX_FRAMES = 2;
    VkSemaphore imageReady[MAX_FRAMES]{};
    VkSemaphore renderDone[MAX_FRAMES]{};
    VkFence     inFlight  [MAX_FRAMES]{};
    uint32_t    frameIndex = 0;

    // FSE extension – used ONLY for stereo path, with DISALLOWED flag.
    // Stereo needs FSE loaded so it can chain DISALLOWED into the swapchain CI,
    // preventing the driver's auto-FSE from conflicting with its stereo path.
    // Non-stereo: no FSE (driver auto-FSE on WS_POPUP is harmless).
    bool hasSurfaceCaps2 = false;
    bool hasFSE          = false;

    // -----------------------------------------------------------------------
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
        log("=== init complete: %ux%u  %u images  stereo=%s ===",
            swapExtent.width, swapExtent.height, swapCount,
            stereo ? "YES" : "NO");
    }

    // -----------------------------------------------------------------------
    void createInstance() {
        log("--- createInstance ---");

        // VK_KHR_get_surface_capabilities2 is required by VK_EXT_full_screen_exclusive.
        // We need FSE to chain DISALLOWED into the stereo swapchain CI.
        hasSurfaceCaps2 = hasInstanceExt(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        log("  VK_KHR_get_surface_capabilities2: %s",
            hasSurfaceCaps2 ? "available" : "NOT available");

        std::vector<const char *> exts = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        if (hasSurfaceCaps2)
            exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

        const char *valLayer = "VK_LAYER_KHRONOS_validation";
        uint32_t lc=0; vkEnumerateInstanceLayerProperties(&lc,nullptr);
        std::vector<VkLayerProperties> lp(lc);
        vkEnumerateInstanceLayerProperties(&lc,lp.data());
        bool hasVal=false;
        for(auto &l:lp) if(!strcmp(l.layerName,valLayer)){hasVal=true;break;}
        log("  Validation layer: %s", hasVal?"available":"NOT available");

        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName="VkStereo3DVision"; ai.apiVersion=VK_API_VERSION_1_1;

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo=&ai;
        ci.enabledExtensionCount=(uint32_t)exts.size();
        ci.ppEnabledExtensionNames=exts.data();
        if(hasVal){ci.enabledLayerCount=1;ci.ppEnabledLayerNames=&valLayer;}

        VK_CHECK(vkCreateInstance(&ci,nullptr,&instance));
        log("  Instance created (validation %s)", hasVal?"ON":"OFF");
    }

    // -----------------------------------------------------------------------
    void createSurface(HINSTANCE hInst, HWND hwnd) {
        log("--- createSurface ---");
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance=hInst; ci.hwnd=hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance,&ci,nullptr,&surface));
    }

    // -----------------------------------------------------------------------
    void pickPhysicalDevice() {
        log("--- pickPhysicalDevice ---");
        uint32_t cnt=0;
        vkEnumeratePhysicalDevices(instance,&cnt,nullptr);
        log("  Physical devices: %u", cnt);
        if(!cnt) throw std::runtime_error("No Vulkan GPUs");
        std::vector<VkPhysicalDevice> devs(cnt);
        vkEnumeratePhysicalDevices(instance,&cnt,devs.data());

        for(auto pd:devs) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pd,&props);
            uint32_t maj=props.driverVersion>>22, min=(props.driverVersion>>14)&0xFF;
            log("  GPU: %s  driver %u.%u (raw 0x%x)",
                props.deviceName, maj, min, props.driverVersion);

            uint32_t qfc=0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd,&qfc,nullptr);
            std::vector<VkQueueFamilyProperties> qf(qfc);
            vkGetPhysicalDeviceQueueFamilyProperties(pd,&qfc,qf.data());

            for(uint32_t i=0;i<qfc;++i) {
                VkBool32 present=VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd,i,surface,&present);
                if(!(qf[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)||!present) continue;
                physDev=pd; queueFamily=i;

                VkSurfaceCapabilitiesKHR caps{};
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd,surface,&caps);
                log("  Surface caps: minImg=%u maxImg=%u maxArrayLayers=%u "
                    "preTransform=0x%x",
                    caps.minImageCount, caps.maxImageCount,
                    caps.maxImageArrayLayers, (unsigned)caps.currentTransform);

                // Log all supported surface formats
                uint32_t fc=0;
                vkGetPhysicalDeviceSurfaceFormatsKHR(pd,surface,&fc,nullptr);
                std::vector<VkSurfaceFormatKHR> fmts(fc);
                vkGetPhysicalDeviceSurfaceFormatsKHR(pd,surface,&fc,fmts.data());
                log("  Surface formats (%u):", fc);
                for(auto &f:fmts)
                    log("    format=%d  colorSpace=%d", (int)f.format,(int)f.colorSpace);

                // Log present modes
                uint32_t pmCount=0;
                vkGetPhysicalDeviceSurfacePresentModesKHR(pd,surface,&pmCount,nullptr);
                std::vector<VkPresentModeKHR> pms(pmCount);
                vkGetPhysicalDeviceSurfacePresentModesKHR(pd,surface,&pmCount,pms.data());
                char pmBuf[128]{}; int off=0;
                for(auto pm:pms) off+=snprintf(pmBuf+off,sizeof(pmBuf)-off,"%d ",pm);
                log("  Present modes: [%s]", pmBuf);

                stereo = (caps.maxImageArrayLayers >= 2);
                log("  Stereo path: %s (maxImageArrayLayers=%u)",
                    stereo?"YES":"NO", caps.maxImageArrayLayers);

                // FSE availability – needed for stereo DISALLOWED chain
                bool fseAvail = hasSurfaceCaps2 &&
                    hasDeviceExt(pd, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
                hasFSE = fseAvail;
                log("  VK_EXT_full_screen_exclusive: %s", hasFSE?"available":"NOT available");

                if (stereo && !hasFSE)
                    log("  WARNING: FSE unavailable – cannot chain DISALLOWED; "
                        "driver auto-FSE may conflict with stereo path.");
                break;
            }
            if(physDev) break;
        }
        if(!physDev) throw std::runtime_error("No suitable GPU");
    }

    // -----------------------------------------------------------------------
    void createDevice() {
        log("--- createDevice ---");
        float prio=1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex=queueFamily; qci.queueCount=1; qci.pQueuePriorities=&prio;

        // Load FSE device extension when:
        //   - stereo path: need it to chain DISALLOWED into swapchain CI
        //   - non-stereo:  not loaded (driver auto-FSE is harmless there)
        std::vector<const char*> exts={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if (stereo && hasFSE) {
            exts.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
            log("  Loading VK_EXT_full_screen_exclusive (stereo DISALLOWED path)");
        } else {
            log("  VK_EXT_full_screen_exclusive NOT loaded (%s)",
                stereo ? "not available" : "non-stereo path – not needed");
        }

        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount=1; ci.pQueueCreateInfos=&qci;
        ci.enabledExtensionCount=(uint32_t)exts.size();
        ci.ppEnabledExtensionNames=exts.data();
        VK_CHECK(vkCreateDevice(physDev,&ci,nullptr,&device));
        vkGetDeviceQueue(device,queueFamily,0,&queue);
    }

    // -----------------------------------------------------------------------
    void createSwapchain(HWND hwnd) {
        log("--- createSwapchain ---");

        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev,surface,&caps);

        // Format
        uint32_t fc=0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev,surface,&fc,nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fc);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev,surface,&fc,fmts.data());
        VkSurfaceFormatKHR fmt=fmts[0];
        for(auto &f:fmts)
            if(f.format==VK_FORMAT_B8G8R8A8_UNORM &&
               f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt=f;
        swapFmt=fmt.format;
        log("  Format=%d  colorSpace=%d", (int)swapFmt,(int)fmt.colorSpace);

        // Extent
        RECT rc{}; GetClientRect(hwnd,&rc);
        uint32_t ww=(uint32_t)(rc.right-rc.left); if(!ww)ww=1;
        uint32_t wh=(uint32_t)(rc.bottom-rc.top);  if(!wh)wh=1;
        swapExtent.width =vk_clamp(ww,caps.minImageExtent.width, caps.maxImageExtent.width);
        swapExtent.height=vk_clamp(wh,caps.minImageExtent.height,caps.maxImageExtent.height);
        log("  Extent: %ux%u", swapExtent.width, swapExtent.height);

        // Image count
        // Stereo: minImageCount only (driver allocates stereo companion buffers
        // per image; extra images can exceed an internal 426.06 allocation limit).
        uint32_t imgCount = stereo ? caps.minImageCount : caps.minImageCount + 1;
        if(caps.maxImageCount && imgCount>caps.maxImageCount) imgCount=caps.maxImageCount;
        log("  imageCount: %u (min=%u)", imgCount, caps.minImageCount);

        uint32_t layers = stereo ? 2u : 1u;

        // ── Full Screen Exclusive DISALLOWED for stereo ────────────────────
        //
        // The NVIDIA driver automatically acquires FSE ("fullscreen
        // optimizations") for any WS_POPUP window covering the full display.
        // With the stereo swapchain path (imageArrayLayers=2), the driver also
        // tries to initialise its internal stereo flip engine.  These two
        // exclusive display ownership mechanisms conflict → VK_ERROR_INITIALIZATION_FAILED.
        //
        // Chaining VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT tells the driver
        // explicitly: "do not use FSE for this swapchain."  The driver then
        // routes through its stereo-only path which owns display exclusivity
        // via its own internal mechanism (the 3D Vision shutter driver), rather
        // than through the generic FSE path.
        //
        // VkSurfaceFullScreenExclusiveWin32InfoEXT is NOT required for DISALLOWED
        // (the spec only mandates it for APPLICATION_CONTROLLED).
        //
        VkSurfaceFullScreenExclusiveInfoEXT fseDisallow{
            VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
        fseDisallow.pNext            = nullptr;
        fseDisallow.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;

        const bool chainFseDisallow = stereo && hasFSE;

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.pNext            = chainFseDisallow ? &fseDisallow : nullptr;
        ci.surface          = surface;
        ci.minImageCount    = imgCount;
        ci.imageFormat      = swapFmt;
        ci.imageColorSpace  = fmt.colorSpace;
        ci.imageExtent      = swapExtent;
        ci.imageArrayLayers = layers;   // KEY: 2=stereo
        ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped          = VK_TRUE;

        log("  vkCreateSwapchainKHR: layers=%u  FIFO  IDENTITY  pNext=%s",
            layers,
            chainFseDisallow ? "FSE_DISALLOWED" : "null");

        VK_CHECK(vkCreateSwapchainKHR(device,&ci,nullptr,&swapchain));

        // Images + per-layer views
        vkGetSwapchainImagesKHR(device,swapchain,&swapCount,nullptr);
        swapImages.resize(swapCount);
        vkGetSwapchainImagesKHR(device,swapchain,&swapCount,swapImages.data());
        log("  swapCount: %u", swapCount);

        viewLeft .resize(swapCount,VK_NULL_HANDLE);
        viewRight.resize(swapCount,VK_NULL_HANDLE);
        for(uint32_t i=0;i<swapCount;++i){
            auto makeView=[&](uint32_t layer)->VkImageView{
                VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vi.image=swapImages[i]; vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
                vi.format=swapFmt;
                vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,layer,1};
                VkImageView v=VK_NULL_HANDLE;
                VK_CHECK(vkCreateImageView(device,&vi,nullptr,&v));
                return v;
            };
            viewLeft[i]=makeView(0);
            if(stereo) viewRight[i]=makeView(1);
            log("  image[%u]: L=%p  R=%p",i,(void*)viewLeft[i],(void*)viewRight[i]);
        }
    }

    // -----------------------------------------------------------------------
    VkRenderPass makeRenderPass() {
        VkAttachmentDescription att{};
        att.format=swapFmt; att.samples=VK_SAMPLE_COUNT_1_BIT;
        att.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;

        VkSubpassDependency dep{};
        dep.srcSubpass=VK_SUBPASS_EXTERNAL; dep.dstSubpass=0;
        dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask=0;
        dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rci.attachmentCount=1; rci.pAttachments=&att;
        rci.subpassCount=1;    rci.pSubpasses=&sub;
        rci.dependencyCount=1; rci.pDependencies=&dep;

        VkRenderPass rp=VK_NULL_HANDLE;
        VK_CHECK(vkCreateRenderPass(device,&rci,nullptr,&rp));
        return rp;
    }

    void createRenderPasses() {
        log("--- createRenderPasses ---");
        renderPassLeft=makeRenderPass();
        if(stereo) renderPassRight=makeRenderPass();
    }

    // -----------------------------------------------------------------------
    void createFramebuffers() {
        log("--- createFramebuffers ---");
        fbLeft .resize(swapCount,VK_NULL_HANDLE);
        fbRight.resize(swapCount,VK_NULL_HANDLE);
        for(uint32_t i=0;i<swapCount;++i){
            auto makeFB=[&](VkRenderPass rp,VkImageView v)->VkFramebuffer{
                VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                fi.renderPass=rp; fi.attachmentCount=1; fi.pAttachments=&v;
                fi.width=swapExtent.width; fi.height=swapExtent.height; fi.layers=1;
                VkFramebuffer fb=VK_NULL_HANDLE;
                VK_CHECK(vkCreateFramebuffer(device,&fi,nullptr,&fb));
                return fb;
            };
            fbLeft[i]=makeFB(renderPassLeft,viewLeft[i]);
            if(stereo) fbRight[i]=makeFB(renderPassRight,viewRight[i]);
        }
    }

    // -----------------------------------------------------------------------
    void createCommandPool() {
        log("--- createCommandPool ---");
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex=queueFamily;
        ci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device,&ci,nullptr,&cmdPool));
    }

    void allocateCommandBuffers() {
        log("--- allocateCommandBuffers ---");
        cmdBufs.resize(swapCount);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool=cmdPool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount=swapCount;
        VK_CHECK(vkAllocateCommandBuffers(device,&ai,cmdBufs.data()));
    }

    // -----------------------------------------------------------------------
    void createSyncObjects() {
        log("--- createSyncObjects ---");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        for(uint32_t i=0;i<MAX_FRAMES;++i){
            VK_CHECK(vkCreateSemaphore(device,&si,nullptr,&imageReady[i]));
            VK_CHECK(vkCreateSemaphore(device,&si,nullptr,&renderDone[i]));
            VK_CHECK(vkCreateFence    (device,&fi,nullptr,&inFlight  [i]));
        }
    }

    // -----------------------------------------------------------------------
    void recordCommandBuffer(uint32_t idx) {
        VkCommandBuffer cb=cmdBufs[idx];
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb,&bi));

        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderArea={{0,0},swapExtent};
        rpbi.clearValueCount=1;

        // Left eye: CYAN  (layer 0)
        {
            VkClearValue cv{};
            cv.color.float32[0]=0.f; cv.color.float32[1]=1.f;
            cv.color.float32[2]=1.f; cv.color.float32[3]=1.f;
            rpbi.renderPass=renderPassLeft; rpbi.framebuffer=fbLeft[idx];
            rpbi.pClearValues=&cv;
            vkCmdBeginRenderPass(cb,&rpbi,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdEndRenderPass(cb);
        }

        // Right eye: RED  (layer 1) – stereo only
        if(stereo){
            VkClearValue cv{};
            cv.color.float32[0]=1.f; cv.color.float32[1]=0.f;
            cv.color.float32[2]=0.f; cv.color.float32[3]=1.f;
            rpbi.renderPass=renderPassRight; rpbi.framebuffer=fbRight[idx];
            rpbi.pClearValues=&cv;
            vkCmdBeginRenderPass(cb,&rpbi,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdEndRenderPass(cb);
        }

        VK_CHECK(vkEndCommandBuffer(cb));
    }

    // -----------------------------------------------------------------------
    void drawFrame() {
        uint32_t fi=frameIndex%MAX_FRAMES;
        vkWaitForFences(device,1,&inFlight[fi],VK_TRUE,UINT64_MAX);

        uint32_t imgIdx=0; VkResult r;
        VK_CHECK_SWAPCHAIN(
            vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,
                                  imageReady[fi],VK_NULL_HANDLE,&imgIdx),r);
        if(r==VK_ERROR_OUT_OF_DATE_KHR||r==VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
            return;

        vkResetFences(device,1,&inFlight[fi]);
        vkResetCommandBuffer(cmdBufs[imgIdx],0);
        recordCommandBuffer(imgIdx);

        VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount=1;   si.pWaitSemaphores=&imageReady[fi];
        si.pWaitDstStageMask=&waitStage;
        si.commandBufferCount=1;   si.pCommandBuffers=&cmdBufs[imgIdx];
        si.signalSemaphoreCount=1; si.pSignalSemaphores=&renderDone[fi];
        VK_CHECK(vkQueueSubmit(queue,1,&si,inFlight[fi]));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&renderDone[fi];
        pi.swapchainCount=1;     pi.pSwapchains=&swapchain;
        pi.pImageIndices=&imgIdx;
        VK_CHECK_SWAPCHAIN(vkQueuePresentKHR(queue,&pi),r);

        ++frameIndex;
    }

    // -----------------------------------------------------------------------
    void cleanup() {
        log("--- cleanup ---");
        if(device) vkDeviceWaitIdle(device);
        for(uint32_t i=0;i<MAX_FRAMES;++i){
            if(imageReady[i]) vkDestroySemaphore(device,imageReady[i],nullptr);
            if(renderDone[i]) vkDestroySemaphore(device,renderDone[i],nullptr);
            if(inFlight  [i]) vkDestroyFence    (device,inFlight  [i],nullptr);
        }
        if(cmdPool) vkDestroyCommandPool(device,cmdPool,nullptr);
        for(uint32_t i=0;i<swapCount;++i){
            if(fbLeft [i]) vkDestroyFramebuffer(device,fbLeft [i],nullptr);
            if(fbRight[i]) vkDestroyFramebuffer(device,fbRight[i],nullptr);
            if(viewLeft [i]) vkDestroyImageView(device,viewLeft [i],nullptr);
            if(viewRight[i]) vkDestroyImageView(device,viewRight[i],nullptr);
        }
        if(renderPassLeft)  vkDestroyRenderPass  (device,renderPassLeft, nullptr);
        if(renderPassRight) vkDestroyRenderPass  (device,renderPassRight,nullptr);
        if(swapchain)       vkDestroySwapchainKHR(device,swapchain,      nullptr);
        if(device)          vkDestroyDevice      (device,                nullptr);
        if(surface)         vkDestroySurfaceKHR  (instance,surface,      nullptr);
        if(instance)        vkDestroyInstance    (instance,              nullptr);
        log("--- cleanup done ---");
    }
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    AllocConsole();
    FILE *f=nullptr; freopen_s(&f,"CONOUT$","w",stdout);
    logInit();
    log("=== VkStereo3DVision v6 startup ===");

    // Win32 display diagnostics before Vulkan init
    logDisplayMode();

    StereoApp app{};
    try {
        s_hwnd=createFullscreenWindow(hInst);
        app.init(hInst,s_hwnd);

        log("--- render loop (ESC to quit) ---");
        uint32_t frameCount=0;
        MSG msg{};
        while(!s_quit){
            while(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){
                if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE) s_quit=true;
                TranslateMessage(&msg); DispatchMessageA(&msg);
            }
            if(!s_quit){
                app.drawFrame();
                if(++frameCount==5)
                    log("  (first 5 frames OK – call-level logging suppressed)");
            }
        }
        log("--- render loop exited after %u frames ---", frameCount);
    } catch(const std::exception &e){
        log("EXCEPTION: %s", e.what());
        MessageBoxA(nullptr,e.what(),"Fatal Error",MB_OK|MB_ICONERROR);
    }
    app.cleanup();
    if(s_logFile) fclose(s_logFile);
    return 0;
}
