/*
 * vulkan_stereo_3dvision.cpp  v9
 *
 * Stereoscopic 3D – NVIDIA 3D Vision beta driver 426.06, GeForce/consumer GPU.
 *
 * ── HISTORY ──────────────────────────────────────────────────────────────────
 *
 *  v8 produced:
 *    NvAPI_Stereo_CreateHandleFromIUnknown = 0 (OK)  handle=<valid>
 *    NvAPI_Stereo_Activate = 0 (OK)
 *    NvAPI_Stereo_IsActivated → active=0   ← stereo NOT live
 *    APP_CTRL → driver crash (uncatchable – driver thread)
 *    ALLOWED / DISALLOWED → -3
 *
 * ── ROOT CAUSE (v8 → v9) ─────────────────────────────────────────────────────
 *
 *  NvAPI_Stereo_IsActivated returning active=0 despite Activate() succeeding
 *  means the stereo subsystem accepts the activation request but cannot
 *  commit until the process has a live DXGI swap chain on the target monitor.
 *  The driver's stereo mode is "pending" — the shutter emitter is not yet
 *  synchronized and the ICD's stereo initialisation gate is not cleared.
 *
 *  The APP_CTRL crash is the ICD's stereo swapchain setup code walking the
 *  DXGI surface chain for the HWND, finding no DXGI swap chain, and faulting
 *  inside the driver thread (which SEH in the application thread cannot catch).
 *
 *  v8 created the window AFTER D3D11 / NVAPI init and used D3D11CreateDevice
 *  (no swap chain). Both mistakes left the HWND without any DXGI context.
 *
 * ── FIX (v9) ─────────────────────────────────────────────────────────────────
 *
 *  Correct sequencing:
 *    1. Create the WS_POPUP window first.
 *    2. D3D11CreateDeviceAndSwapChain on that HWND — creates a DXGI swap
 *       chain bound to the presentation window, giving the stereo driver the
 *       DXGI surface context it needs.
 *    3. NvAPI_Stereo_CreateHandleFromIUnknown(d3dDevice) — handle creation
 *       now sees an active DXGI context.
 *    4. Enumerate ALL Vulkan device extensions (logged) — look for any NV_*
 *       stereo extension we may have missed.
 *    5. Probe the Vulkan stereo swapchain:
 *       APP_CTRL is now expected to survive (DXGI context present).
 *       ALLOWED / DISALLOWED included as before.
 *       imageCount=1 variants included (driver may require single-buffer).
 *       TRANSFER_DST usage bit included.
 *
 * ── BUILD ────────────────────────────────────────────────────────────────────
 *   cl /std:c++17 /W3 /O2 main.cpp /I%VULKAN_SDK%\Include ^
 *      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib
 *   (d3d11 loaded dynamically — no d3d11.lib / dxgi.lib needed)
 *
 * ── OUTPUT ───────────────────────────────────────────────────────────────────
 *   Left eye → CYAN (layer 0)    Right eye → RED (layer 1)
 *   Log: vulkan_stereo.log beside the exe.   ESC to quit.
 */

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <unknwn.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

// ============================================================================
// Logging
// ============================================================================

static FILE *s_logFile = nullptr;

static void logInit() {
    char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *sl = strrchr(exe, '\\'); if (sl) sl[1] = '\0'; else exe[0] = '\0';
    char path[MAX_PATH]{}; snprintf(path, MAX_PATH, "%svulkan_stereo.log", exe);
    s_logFile = fopen(path, "w");
    printf("Log: %s\n", path); fflush(stdout);
}
static void log(const char *fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    puts(buf); OutputDebugStringA(buf); OutputDebugStringA("\n");
    if (s_logFile) { fputs(buf, s_logFile); fputc('\n', s_logFile); fflush(s_logFile); }
}

// ============================================================================
// Display diagnostics
// ============================================================================

static void logDisplayMode() {
    log("--- display diagnostics ---");
    DEVMODEA dm{}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        log("  Current: %ux%u @ %u Hz  bpp=%u",
            dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmBitsPerPel);
    for (DWORD i = 0; ; ++i) {
        DEVMODEA m{}; m.dmSize = sizeof(m);
        if (!EnumDisplaySettingsA(nullptr, i, &m)) break;
        if (m.dmDisplayFrequency >= 120 && m.dmPelsWidth == dm.dmPelsWidth)
            log("  mode[%u]: %ux%u @ %u Hz", i, m.dmPelsWidth, m.dmPelsHeight, m.dmDisplayFrequency);
    }
}

// ============================================================================
// Win32 window  — must be created FIRST so DXGI can bind to it
// ============================================================================

static HWND s_hwnd = nullptr;
static bool s_quit = false;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY || msg == WM_CLOSE) { s_quit = true; PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
static HWND createFullscreenWindow(HINSTANCE hInst) {
    WNDCLASSEXA wc{}; wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW); wc.lpszClassName = "VkStereoWnd";
    RegisterClassExA(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    log("Primary monitor: %dx%d", sw, sh);
    HWND hwnd = CreateWindowExA(0, "VkStereoWnd", "Vulkan 3D Vision  ESC=quit",
        WS_POPUP, 0, 0, sw, sh, nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
    SetForegroundWindow(hwnd); BringWindowToTop(hwnd);
    MSG msg{}; for (int i = 0; i < 10; ++i)
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
    log("Window: WS_POPUP %dx%d", sw, sh);
    return hwnd;
}

// ============================================================================
// D3D11 + DXGI bootstrap — loaded dynamically, no d3d11.lib / dxgi.lib needed
//
// Creates a DXGI swap chain ON the presentation HWND.
// This is what the NVIDIA stereo driver requires to:
//   (a) clear the "stereo pending" state after NvAPI_Stereo_Activate()
//   (b) provide the DXGI surface context that the Vulkan ICD stereo path
//       looks up when setting up the stereo flip engine for APP_CTRL mode.
// ============================================================================

// Minimal type definitions (avoids d3d11.h / dxgi.h entirely)
typedef enum { DDTH_ = 1  } D3D_DRIVER_TYPE_;
typedef enum { DFL_ = 0xb000 } D3D_FEATURE_LEVEL_;
typedef enum { DFMT_B8G8R8A8_ = 87 } DXGI_FORMAT_;
typedef enum { DSLO_ = 0 } DXGI_SCANLINE_ORDER_;
typedef enum { DSCA_ = 0 } DXGI_SCALING_;
typedef enum { DSE_DISCARD_ = 0 } DXGI_SWAP_EFFECT_;
typedef UINT DXGI_USAGE_;
#define DXGI_USAGE_RTO_ 0x20u

typedef struct { UINT N, D; } DXGI_RATIONAL_;
typedef struct {
    UINT W, H; DXGI_RATIONAL_ Refresh; DXGI_FORMAT_ Fmt;
    DXGI_SCANLINE_ORDER_ SLO; DXGI_SCALING_ Scale;
} DXGI_MODE_DESC_;
typedef struct { UINT Count, Quality; } DXGI_SAMPLE_DESC_;
typedef struct {
    DXGI_MODE_DESC_   BufferDesc;
    DXGI_SAMPLE_DESC_ SampleDesc;
    DXGI_USAGE_       BufferUsage;
    UINT              BufferCount;
    HWND              OutputWindow;
    BOOL              Windowed;
    DXGI_SWAP_EFFECT_ SwapEffect;
    UINT              Flags;
} DXGI_SWAP_CHAIN_DESC_;

typedef HRESULT (WINAPI *PfnCreateDevAndSC)(
    void*, D3D_DRIVER_TYPE_, HMODULE, UINT,
    const D3D_FEATURE_LEVEL_*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC_*,
    IUnknown**, IUnknown**, D3D_FEATURE_LEVEL_*, IUnknown**);
typedef HRESULT (WINAPI *PfnCreateDev)(
    void*, D3D_DRIVER_TYPE_, HMODULE, UINT,
    const D3D_FEATURE_LEVEL_*, UINT, UINT,
    IUnknown**, D3D_FEATURE_LEVEL_*, IUnknown**);

static HMODULE   s_hD3D11         = nullptr;
static IUnknown *s_pD3DDevice     = nullptr;
static IUnknown *s_pDXGISwapChain = nullptr;

static bool d3d11Bootstrap(HWND hwnd) {
    log("--- D3D11 + DXGI bootstrap ---");
    s_hD3D11 = LoadLibraryA("d3d11.dll");
    if (!s_hD3D11) { log("  d3d11.dll: not found"); return false; }
    log("  d3d11.dll loaded");

    D3D_FEATURE_LEVEL_ fl = DFL_, flOut{};

    // Preferred path: CreateDeviceAndSwapChain — binds a DXGI swap chain to
    // the HWND. This is the exact call sequence used by D3D11+3DV apps, and
    // is what the stereo driver looks for when setting up the stereo flip path.
    auto fnDAS = (PfnCreateDevAndSC)GetProcAddress(s_hD3D11, "D3D11CreateDeviceAndSwapChain");
    if (fnDAS) {
        DXGI_SWAP_CHAIN_DESC_ scd{};
        scd.BufferDesc.W   = 0; // use window size
        scd.BufferDesc.H   = 0;
        scd.BufferDesc.Refresh = { 120, 1 };
        scd.BufferDesc.Fmt = DFMT_B8G8R8A8_;
        scd.SampleDesc     = { 1, 0 };
        scd.BufferUsage    = DXGI_USAGE_RTO_;
        scd.BufferCount    = 2;
        scd.OutputWindow   = hwnd;
        scd.Windowed       = TRUE;
        scd.SwapEffect     = DSE_DISCARD_;
        IUnknown *pCtx = nullptr;
        HRESULT hr = fnDAS(
            nullptr, DDTH_, nullptr, 0, &fl, 1, 7, &scd,
            &s_pDXGISwapChain, &s_pD3DDevice, &flOut, &pCtx);
        if (pCtx) { pCtx->Release(); pCtx = nullptr; }
        log("  D3D11CreateDeviceAndSwapChain: hr=0x%x (%s)",
            (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAILED");
        if (SUCCEEDED(hr)) {
            log("  D3D11 device:     %p", (void*)s_pD3DDevice);
            log("  DXGI swap chain:  %p", (void*)s_pDXGISwapChain);
            return true;
        }
        if (s_pD3DDevice) { s_pD3DDevice->Release(); s_pD3DDevice = nullptr; }
    }

    // Fallback: device only (no swap chain — APP_CTRL will be skipped)
    auto fnDev = (PfnCreateDev)GetProcAddress(s_hD3D11, "D3D11CreateDevice");
    if (!fnDev) { log("  D3D11CreateDevice: not found"); return false; }
    IUnknown *pCtx = nullptr;
    HRESULT hr = fnDev(nullptr, DDTH_, nullptr, 0, &fl, 1, 7, &s_pD3DDevice, &flOut, &pCtx);
    if (pCtx) { pCtx->Release(); }
    log("  D3D11CreateDevice (fallback): hr=0x%x (%s)", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) { s_pD3DDevice = nullptr; return false; }
    log("  D3D11 device (no DXGI SC): %p", (void*)s_pD3DDevice);
    return true;
}

static void d3d11Shutdown() {
    if (s_pDXGISwapChain) { s_pDXGISwapChain->Release(); s_pDXGISwapChain = nullptr; }
    if (s_pD3DDevice)     { s_pD3DDevice->Release();     s_pD3DDevice     = nullptr; }
    if (s_hD3D11)         { FreeLibrary(s_hD3D11);        s_hD3D11         = nullptr; }
}

// ============================================================================
// NVAPI
// ============================================================================

#define NVAPI_OK 0
typedef void *(*PfnNvQI)(unsigned int);
typedef int   (*PfnNvV)();
typedef int   (*PfnNvFromIUnk)(IUnknown*, void**);
typedef int   (*PfnNvDH)(void*);

static HMODULE  s_hNvapi  = nullptr;
static PfnNvQI  s_nvQI    = nullptr;
static void    *s_hStereo = nullptr;

static void *nvQ(unsigned id) { return s_nvQI ? s_nvQI(id) : nullptr; }

#define NvID_Initialize           0x0150E828u
#define NvID_Unload               0xD22BDD7Eu
#define NvID_Stereo_Enable        0x239C4545u
#define NvID_Stereo_SetMode       0x5E8F0BECu
#define NvID_Stereo_CreateFromIUnk 0xAC7E37F4u
#define NvID_Stereo_DestroyHandle 0x3A153134u
#define NvID_Stereo_Activate      0xF6A1AD68u
#define NvID_Stereo_IsActivated   0x1FB0BC30u
#define NvID_Stereo_SetSeparation 0x5C069FA3u

static bool nvapiInit(IUnknown *pDev) {
    log("--- NVAPI init ---");
    s_hNvapi = LoadLibraryA("nvapi64.dll");
    if (!s_hNvapi) { log("  nvapi64.dll: not found"); return false; }
    s_nvQI = (PfnNvQI)GetProcAddress(s_hNvapi, "nvapi_QueryInterface");
    if (!s_nvQI) { log("  nvapi_QueryInterface: not found"); return false; }
    log("  nvapi64.dll OK");

    auto fnInit = (PfnNvV)nvQ(NvID_Initialize);
    if (!fnInit) { log("  NvAPI_Initialize fn: not found"); return false; }
    int r = fnInit(); log("  NvAPI_Initialize = %d", r); if (r) return false;

    { auto fn = (PfnNvV)nvQ(NvID_Stereo_Enable);
      if (fn) { r = fn(); log("  NvAPI_Stereo_Enable = %d", r); } }

    { typedef int(*Pfn)(unsigned); auto fn = (Pfn)nvQ(NvID_Stereo_SetMode);
      if (fn) { r = fn(0); log("  NvAPI_Stereo_SetDriverMode(AUTOMATIC) = %d", r); } }

    { auto fn = (PfnNvFromIUnk)nvQ(NvID_Stereo_CreateFromIUnk);
      if (fn && pDev) {
          r = fn(pDev, &s_hStereo);
          log("  NvAPI_Stereo_CreateHandleFromIUnknown = %d (%s)  handle=%p",
              r, r == NVAPI_OK ? "OK" : "FAILED", (void*)s_hStereo);
      } }

    if (s_hStereo) {
        { typedef int(*Pfn)(void*); auto fn = (Pfn)nvQ(NvID_Stereo_Activate);
          if (fn) { r = fn(s_hStereo); log("  NvAPI_Stereo_Activate = %d", r); } }

        { typedef int(*Pfn)(void*, unsigned char*); auto fn = (Pfn)nvQ(NvID_Stereo_IsActivated);
          if (fn) { unsigned char a = 0; r = fn(s_hStereo, &a);
                    log("  NvAPI_Stereo_IsActivated = %d  active=%d", r, (int)a); } }

        { typedef int(*Pfn)(void*, float); auto fn = (Pfn)nvQ(NvID_Stereo_SetSeparation);
          if (fn) { r = fn(s_hStereo, 50.f); log("  NvAPI_Stereo_SetSeparation(50%%) = %d", r); } }
    }
    log("  NVAPI complete  handle=%p", (void*)s_hStereo);
    return true;
}
static void nvapiShutdown() {
    if (s_hStereo) { auto fn = (PfnNvDH)nvQ(NvID_Stereo_DestroyHandle); if (fn) fn(s_hStereo); s_hStereo = nullptr; }
    if (s_hNvapi)  { auto fn = (PfnNvV) nvQ(NvID_Unload);               if (fn) fn();           FreeLibrary(s_hNvapi); s_hNvapi = nullptr; }
}

// ============================================================================
// VK helpers
// ============================================================================

static const char *vkResultStr(VkResult r) {
    switch (r) {
    case VK_SUCCESS:                     return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR:              return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:       return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_DEVICE_LOST:           return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_SURFACE_LOST_KHR:      return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FSE_LOST";
    default:                             return "(unknown)";
    }
}
#define VK_CHECK(expr) do { \
    log("  CALL  " #expr); VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { char _b[512]; \
        snprintf(_b, sizeof(_b), "FATAL %s (%d) at line %d\n" #expr, vkResultStr(_r),(int)_r,__LINE__); \
        log(_b); throw std::runtime_error(_b); } \
    log("  OK    " #expr); } while(0)
#define VK_CHECK_SC(expr,out) do { (out)=(expr); \
    if ((out)!=VK_SUCCESS && (out)!=VK_SUBOPTIMAL_KHR && (out)!=VK_ERROR_OUT_OF_DATE_KHR && \
        (out)!=(VkResult)VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) { \
        char _b[512]; snprintf(_b,sizeof(_b),"FATAL %s (%d) at line %d\n" #expr, \
            vkResultStr((VkResult)(out)),(int)(out),__LINE__); log(_b); throw std::runtime_error(_b); } \
    if ((out)!=VK_SUCCESS) log("  WARN  " #expr " -> %s",vkResultStr((VkResult)(out))); } while(0)

template<class T> static T vk_clamp(T v,T lo,T hi){return v<lo?lo:(v>hi?hi:v);}
static bool hasInstanceExt(const char *n) {
    uint32_t c=0; vkEnumerateInstanceExtensionProperties(nullptr,&c,nullptr);
    std::vector<VkExtensionProperties> v(c);
    vkEnumerateInstanceExtensionProperties(nullptr,&c,v.data());
    for (auto &e:v) if (!strcmp(e.extensionName,n)) return true; return false;
}

// ============================================================================
// Application
// ============================================================================

struct StereoApp {
    VkInstance       instance    = VK_NULL_HANDLE;
    VkSurfaceKHR     surface     = VK_NULL_HANDLE;
    VkPhysicalDevice physDev     = VK_NULL_HANDLE;
    VkDevice         device      = VK_NULL_HANDLE;
    uint32_t         queueFamily = UINT32_MAX;
    VkQueue          queue       = VK_NULL_HANDLE;
    VkSwapchainKHR   swapchain   = VK_NULL_HANDLE;
    VkFormat         swapFmt     = VK_FORMAT_UNDEFINED;
    VkExtent2D       swapExtent{};
    uint32_t         swapCount   = 0;
    bool             stereo      = false;
    std::vector<VkImage>       swapImages;
    std::vector<VkImageView>   viewLeft, viewRight;
    std::vector<VkFramebuffer> fbLeft, fbRight;
    VkRenderPass renderPassLeft=VK_NULL_HANDLE, renderPassRight=VK_NULL_HANDLE;
    VkCommandPool cmdPool=VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmdBufs;
    static const uint32_t MAX_FRAMES=2;
    VkSemaphore imageReady[MAX_FRAMES]{}, renderDone[MAX_FRAMES]{};
    VkFence     inFlight  [MAX_FRAMES]{};
    uint32_t    frameIndex=0;
    bool hasSurfaceCaps2=false, hasFSE=false;

    // -----------------------------------------------------------------------
    void init(HINSTANCE hInst, HWND hwnd) {
        log("=== init begin ===");
        createInstance(); createSurface(hInst, hwnd);
        pickPhysicalDevice(); createDevice();
        createSwapchain(hwnd); createRenderPasses();
        createFramebuffers(); createCommandPool();
        allocateCommandBuffers(); createSyncObjects();
        log("=== init complete: %ux%u  %u images  stereo=%s ===",
            swapExtent.width, swapExtent.height, swapCount, stereo?"YES":"NO");
    }

    // -----------------------------------------------------------------------
    void createInstance() {
        log("--- createInstance ---");
        hasSurfaceCaps2 = hasInstanceExt(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        log("  get_surface_caps2: %s", hasSurfaceCaps2?"available":"NOT available");

        std::vector<const char*> exts = {
            VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME
        };
        if (hasSurfaceCaps2)
            exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

        // Disable validation layer for stereo (NVAPI handle present).
        // The layer wraps every ICD entry point; on 426.06 the stereo
        // path check uses a driver back-channel that bypasses the layer.
        bool wantVal = (s_hStereo == nullptr);
        const char *valLayer = "VK_LAYER_KHRONOS_validation";
        uint32_t lc=0; vkEnumerateInstanceLayerProperties(&lc, nullptr);
        std::vector<VkLayerProperties> lp(lc);
        vkEnumerateInstanceLayerProperties(&lc, lp.data());
        bool hasVal=false;
        for (auto &l:lp) if (!strcmp(l.layerName,valLayer)){hasVal=true;break;}
        log("  Validation: %s", !wantVal?"OFF (stereo)":hasVal?"available":"unavailable");

        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName="VkStereo3DVision"; ai.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo=&ai;
        ci.enabledExtensionCount=(uint32_t)exts.size();
        ci.ppEnabledExtensionNames=exts.data();
        if (wantVal&&hasVal) { ci.enabledLayerCount=1; ci.ppEnabledLayerNames=&valLayer; }
        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
        log("  Instance: validation %s", (wantVal&&hasVal)?"ON":"OFF");
    }

    void createSurface(HINSTANCE hInst, HWND hwnd) {
        log("--- createSurface ---");
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance=hInst; ci.hwnd=hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface));
    }

    // -----------------------------------------------------------------------
    // Log ALL device extensions — critical for finding any NV_* stereo ext
    void logAllDeviceExtensions(VkPhysicalDevice pd) {
        uint32_t c=0; vkEnumerateDeviceExtensionProperties(pd, nullptr, &c, nullptr);
        std::vector<VkExtensionProperties> v(c);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &c, v.data());
        std::sort(v.begin(), v.end(), [](const VkExtensionProperties &a,
                                        const VkExtensionProperties &b) {
            return strcmp(a.extensionName, b.extensionName) < 0;
        });
        log("--- device extensions (%u total) ---", c);
        for (auto &e:v) log("  [%5u] %s", e.specVersion, e.extensionName);
    }

    // -----------------------------------------------------------------------
    void pickPhysicalDevice() {
        log("--- pickPhysicalDevice ---");
        uint32_t cnt=0; vkEnumeratePhysicalDevices(instance, &cnt, nullptr);
        if (!cnt) throw std::runtime_error("No Vulkan GPUs");
        std::vector<VkPhysicalDevice> devs(cnt);
        vkEnumeratePhysicalDevices(instance, &cnt, devs.data());

        for (auto pd:devs) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pd, &props);
            log("  GPU: %s  driver %u.%u (0x%x)",
                props.deviceName, props.driverVersion>>22,
                (props.driverVersion>>14)&0xFF, props.driverVersion);

            uint32_t qfc=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qfc,nullptr);
            std::vector<VkQueueFamilyProperties> qf(qfc);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qf.data());
            for (uint32_t i=0; i<qfc; ++i) {
                VkBool32 present=VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
                if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
                physDev=pd; queueFamily=i;

                VkSurfaceCapabilitiesKHR caps{};
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
                log("  Surface: min=%u max=%u maxLayers=%u usage=0x%x xfm=0x%x",
                    caps.minImageCount, caps.maxImageCount, caps.maxImageArrayLayers,
                    caps.supportedUsageFlags, (unsigned)caps.currentTransform);

                // Also query via caps2 path
                if (hasSurfaceCaps2) {
                    auto pfn = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
                        vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceSurfaceCapabilities2KHR");
                    if (pfn) {
                        VkPhysicalDeviceSurfaceInfo2KHR si2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
                        si2.surface=surface;
                        VkSurfaceCapabilities2KHR c2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
                        pfn(pd, &si2, &c2);
                        log("  Caps2: min=%u max=%u maxLayers=%u usage=0x%x",
                            c2.surfaceCapabilities.minImageCount,
                            c2.surfaceCapabilities.maxImageCount,
                            c2.surfaceCapabilities.maxImageArrayLayers,
                            c2.surfaceCapabilities.supportedUsageFlags);
                    }
                }

                uint32_t fc=0;
                vkGetPhysicalDeviceSurfaceFormatsKHR(pd,surface,&fc,nullptr);
                std::vector<VkSurfaceFormatKHR> fmts(fc);
                vkGetPhysicalDeviceSurfaceFormatsKHR(pd,surface,&fc,fmts.data());
                for (auto &f:fmts) log("  fmt=%d cs=%d",(int)f.format,(int)f.colorSpace);

                uint32_t pmc=0;
                vkGetPhysicalDeviceSurfacePresentModesKHR(pd,surface,&pmc,nullptr);
                std::vector<VkPresentModeKHR> pms(pmc);
                vkGetPhysicalDeviceSurfacePresentModesKHR(pd,surface,&pmc,pms.data());
                char pmb[128]{}; int off=0;
                for (auto pm:pms) off+=snprintf(pmb+off,sizeof(pmb)-off,"%d ",pm);
                log("  PresentModes: [%s]", pmb);

                stereo = (caps.maxImageArrayLayers >= 2);
                log("  Stereo: %s  maxLayers=%u", stereo?"YES":"NO", caps.maxImageArrayLayers);

                // Log ALL extensions before checking FSE
                logAllDeviceExtensions(pd);

                uint32_t ec=0;
                vkEnumerateDeviceExtensionProperties(pd, nullptr, &ec, nullptr);
                std::vector<VkExtensionProperties> ev(ec);
                vkEnumerateDeviceExtensionProperties(pd, nullptr, &ec, ev.data());
                hasFSE=false;
                for (auto &e:ev)
                    if (!strcmp(e.extensionName,VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME))
                        hasFSE=true;
                log("  VK_EXT_full_screen_exclusive: %s", hasFSE?"available":"NOT available");
                break;
            }
            if (physDev) break;
        }
        if (!physDev) throw std::runtime_error("No suitable GPU");
    }

    // -----------------------------------------------------------------------
    void createDevice() {
        log("--- createDevice ---");
        float prio=1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex=queueFamily; qci.queueCount=1; qci.pQueuePriorities=&prio;
        std::vector<const char*> exts={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if (hasFSE) { exts.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
                      log("  FSE: loaded"); }
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount=1; ci.pQueueCreateInfos=&qci;
        ci.enabledExtensionCount=(uint32_t)exts.size();
        ci.ppEnabledExtensionNames=exts.data();
        VK_CHECK(vkCreateDevice(physDev, &ci, nullptr, &device));
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
    }

    // -----------------------------------------------------------------------
    // Probe all meaningful parameter combinations for the stereo swapchain.
    struct Probe {
        const char              *label;
        VkFullScreenExclusiveEXT fseMode;
        bool                     chainFse;
        bool                     chainWin32;
        uint32_t                 imageCount; // 0 = use min
        VkImageUsageFlags        usage;
        VkPresentModeKHR         presentMode;
    };

    VkSwapchainKHR tryProbe(const Probe &a, VkExtent2D ext,
                             VkSurfaceFormatKHR fmt, uint32_t minImg, HWND hwnd)
    {
        // APP_CTRL without DXGI swap chain has crashed the driver every time.
        // Skip it unless the DXGI swap chain was successfully created on the HWND.
        if (a.chainFse &&
            a.fseMode == VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT &&
            !s_pDXGISwapChain) {
            log("    [%s] SKIPPED – APP_CTRL requires DXGI swap chain on HWND (not present)", a.label);
            return VK_NULL_HANDLE;
        }
        if (a.chainFse && !hasFSE) {
            log("    [%s] SKIPPED – FSE not on device", a.label);
            return VK_NULL_HANDLE;
        }

        uint32_t imgCount = a.imageCount ? a.imageCount : minImg;
        log("  [%s] imageCount=%u usage=0x%x pm=%d pNext=%s",
            a.label, imgCount, (unsigned)a.usage, (int)a.presentMode,
            a.chainFse ? a.label : "null");

        VkSurfaceFullScreenExclusiveWin32InfoEXT fw{
            VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
        fw.hmonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);

        VkSurfaceFullScreenExclusiveInfoEXT fi{
            VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
        fi.fullScreenExclusive = a.fseMode;
        fi.pNext = (a.chainFse && a.chainWin32) ? (void*)&fw : nullptr;

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.pNext            = a.chainFse ? (void*)&fi : nullptr;
        ci.surface          = surface;
        ci.minImageCount    = imgCount;
        ci.imageFormat      = fmt.format;
        ci.imageColorSpace  = fmt.colorSpace;
        ci.imageExtent      = ext;
        ci.imageArrayLayers = 2;
        ci.imageUsage       = a.usage;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode      = a.presentMode;
        ci.clipped          = VK_TRUE;

        VkSwapchainKHR sc = VK_NULL_HANDLE;
        VkResult r = vkCreateSwapchainKHR(device, &ci, nullptr, &sc);
        log("    -> %s (%d)", vkResultStr(r), (int)r);
        if (r == VK_SUCCESS) { log("    SUCCESS: %s", a.label); return sc; }
        return VK_NULL_HANDLE;
    }

    VkSwapchainKHR tryCreateStereoSwapchain(VkExtent2D ext, VkSurfaceFormatKHR fmt,
                                             uint32_t minImg, HWND hwnd)
    {
        using E = VkFullScreenExclusiveEXT;
        const E ALLOW = VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT;
        const E DISALLOW = VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
        const E APPCTRL = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
        const E DEFLT = VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;
        const VkImageUsageFlags COLOR = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const VkImageUsageFlags DST   = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const VkPresentModeKHR FIFO = VK_PRESENT_MODE_FIFO_KHR;
        const VkPresentModeKHR MAILB = VK_PRESENT_MODE_MAILBOX_KHR;
        const VkPresentModeKHR IMMED = VK_PRESENT_MODE_IMMEDIATE_KHR;

        log("  Stereo probe: NVAPI handle=%p  DXGI SC=%p",
            (void*)s_hStereo, (void*)s_pDXGISwapChain);

        Probe probes[] = {
            // FSE ALLOWED variants (most likely to succeed with DXGI context)
            {"ALLOWED+Win32 FIFO",      ALLOW,   true,  true,  0, COLOR,     FIFO  },
            {"ALLOWED       FIFO",      ALLOW,   true,  false, 0, COLOR,     FIFO  },
            {"ALLOWED+Win32 FIFO+DST",  ALLOW,   true,  true,  0, COLOR|DST, FIFO  },
            {"ALLOWED       FIFO+DST",  ALLOW,   true,  false, 0, COLOR|DST, FIFO  },
            {"ALLOWED+Win32 img=1",     ALLOW,   true,  true,  1, COLOR,     FIFO  },
            {"ALLOWED+Win32 MAILBOX",   ALLOW,   true,  true,  0, COLOR,     MAILB },
            {"ALLOWED+Win32 IMMED",     ALLOW,   true,  true,  0, COLOR,     IMMED },
            // APP_CTRL (requires DXGI SC on HWND — safe with v9 DXGI bootstrap)
            {"APPCTRL+Win32 FIFO",      APPCTRL, true,  true,  0, COLOR,     FIFO  },
            {"APPCTRL+Win32 FIFO+DST",  APPCTRL, true,  true,  0, COLOR|DST, FIFO  },
            {"APPCTRL       FIFO",      APPCTRL, true,  false, 0, COLOR,     FIFO  },
            // DISALLOWED
            {"DISALLOWED    FIFO",      DISALLOW,true,  false, 0, COLOR,     FIFO  },
            // No FSE chain (null pNext) — baseline
            {"null          FIFO",      DEFLT,   false, false, 0, COLOR,     FIFO  },
            {"null          FIFO+DST",  DEFLT,   false, false, 0, COLOR|DST, FIFO  },
            {"null          img=1",     DEFLT,   false, false, 1, COLOR,     FIFO  },
            {"null          MAILBOX",   DEFLT,   false, false, 0, COLOR,     MAILB },
            {"null          IMMED",     DEFLT,   false, false, 0, COLOR,     IMMED },
        };

        for (auto &p:probes) {
            VkSwapchainKHR sc = tryProbe(p, ext, fmt, minImg, hwnd);
            if (sc != VK_NULL_HANDLE) return sc;
        }
        return VK_NULL_HANDLE;
    }

    // -----------------------------------------------------------------------
    void createSwapchain(HWND hwnd) {
        log("--- createSwapchain ---");
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev, surface, &caps);

        uint32_t fc=0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev,surface,&fc,nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fc);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev,surface,&fc,fmts.data());
        VkSurfaceFormatKHR fmt=fmts[0];
        for (auto &f:fmts)
            if (f.format==VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt=f;
        swapFmt = fmt.format;
        log("  Format=%d  colorSpace=%d", (int)swapFmt, (int)fmt.colorSpace);

        RECT rc{}; GetClientRect(hwnd, &rc);
        uint32_t ww=(uint32_t)(rc.right-rc.left); if(!ww)ww=1;
        uint32_t wh=(uint32_t)(rc.bottom-rc.top);  if(!wh)wh=1;
        swapExtent.width  = vk_clamp(ww,caps.minImageExtent.width, caps.maxImageExtent.width);
        swapExtent.height = vk_clamp(wh,caps.minImageExtent.height,caps.maxImageExtent.height);
        log("  Extent: %ux%u", swapExtent.width, swapExtent.height);

        if (stereo) {
            swapchain = tryCreateStereoSwapchain(swapExtent, fmt, caps.minImageCount, hwnd);
            if (swapchain == VK_NULL_HANDLE) {
                log("  ALL STEREO PROBES FAILED.");
                log("  Check the device extension list above for any NV_* stereo extension.");
                log("  The next step if all probes fail is to examine the driver with");
                log("  a Vulkan API capture tool (RenderDoc / VkConfig) to see what");
                log("  parameters a D3D11+NVAPI app successfully passes under the hood.");
                throw std::runtime_error("All stereo swapchain probes failed. See log.");
            }
        } else {
            uint32_t imgCount = caps.minImageCount + 1;
            if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;
            VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            ci.surface=surface; ci.minImageCount=imgCount;
            ci.imageFormat=swapFmt; ci.imageColorSpace=fmt.colorSpace;
            ci.imageExtent=swapExtent; ci.imageArrayLayers=1;
            ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
            ci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            ci.presentMode=VK_PRESENT_MODE_FIFO_KHR; ci.clipped=VK_TRUE;
            VK_CHECK(vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain));
        }

        vkGetSwapchainImagesKHR(device, swapchain, &swapCount, nullptr);
        swapImages.resize(swapCount);
        vkGetSwapchainImagesKHR(device, swapchain, &swapCount, swapImages.data());
        log("  swapCount: %u", swapCount);

        viewLeft.resize(swapCount, VK_NULL_HANDLE);
        viewRight.resize(swapCount, VK_NULL_HANDLE);
        for (uint32_t i=0; i<swapCount; ++i) {
            auto makeView = [&](uint32_t layer) -> VkImageView {
                VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vi.image=swapImages[i]; vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
                vi.format=swapFmt;
                vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,layer,1};
                VkImageView v=VK_NULL_HANDLE;
                VK_CHECK(vkCreateImageView(device, &vi, nullptr, &v));
                return v;
            };
            viewLeft[i] = makeView(0);
            if (stereo) viewRight[i] = makeView(1);
            log("  image[%u]: L=%p  R=%p", i, (void*)viewLeft[i], (void*)viewRight[i]);
        }
    }

    // -----------------------------------------------------------------------
    VkRenderPass makeRenderPass() {
        VkAttachmentDescription att{};
        att.format=swapFmt; att.samples=VK_SAMPLE_COUNT_1_BIT;
        att.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
        VkSubpassDependency dep{};
        dep.srcSubpass=VK_SUBPASS_EXTERNAL; dep.dstSubpass=0;
        dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.srcAccessMask=0;
        dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rci.attachmentCount=1; rci.pAttachments=&att;
        rci.subpassCount=1; rci.pSubpasses=&sub;
        rci.dependencyCount=1; rci.pDependencies=&dep;
        VkRenderPass rp=VK_NULL_HANDLE;
        VK_CHECK(vkCreateRenderPass(device, &rci, nullptr, &rp));
        return rp;
    }

    void createRenderPasses() {
        log("--- createRenderPasses ---");
        renderPassLeft = makeRenderPass();
        if (stereo) renderPassRight = makeRenderPass();
    }
    void createFramebuffers() {
        log("--- createFramebuffers ---");
        fbLeft.resize(swapCount,VK_NULL_HANDLE); fbRight.resize(swapCount,VK_NULL_HANDLE);
        for (uint32_t i=0; i<swapCount; ++i) {
            auto makeFB = [&](VkRenderPass rp, VkImageView v) -> VkFramebuffer {
                VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                fi.renderPass=rp; fi.attachmentCount=1; fi.pAttachments=&v;
                fi.width=swapExtent.width; fi.height=swapExtent.height; fi.layers=1;
                VkFramebuffer fb=VK_NULL_HANDLE;
                VK_CHECK(vkCreateFramebuffer(device, &fi, nullptr, &fb));
                return fb;
            };
            fbLeft[i] = makeFB(renderPassLeft, viewLeft[i]);
            if (stereo) fbRight[i] = makeFB(renderPassRight, viewRight[i]);
        }
    }
    void createCommandPool() {
        log("--- createCommandPool ---");
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex=queueFamily;
        ci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device, &ci, nullptr, &cmdPool));
    }
    void allocateCommandBuffers() {
        log("--- allocateCommandBuffers ---");
        cmdBufs.resize(swapCount);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool=cmdPool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount=swapCount;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, cmdBufs.data()));
    }
    void createSyncObjects() {
        log("--- createSyncObjects ---");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i=0; i<MAX_FRAMES; ++i) {
            VK_CHECK(vkCreateSemaphore(device,&si,nullptr,&imageReady[i]));
            VK_CHECK(vkCreateSemaphore(device,&si,nullptr,&renderDone[i]));
            VK_CHECK(vkCreateFence    (device,&fi,nullptr,&inFlight  [i]));
        }
    }

    void recordCommandBuffer(uint32_t idx) {
        VkCommandBuffer cb=cmdBufs[idx];
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderArea={{0,0},swapExtent}; rpbi.clearValueCount=1;
        // Left eye: CYAN
        { VkClearValue cv{}; cv.color.float32[1]=1.f; cv.color.float32[2]=1.f; cv.color.float32[3]=1.f;
          rpbi.renderPass=renderPassLeft; rpbi.framebuffer=fbLeft[idx]; rpbi.pClearValues=&cv;
          vkCmdBeginRenderPass(cb,&rpbi,VK_SUBPASS_CONTENTS_INLINE); vkCmdEndRenderPass(cb); }
        // Right eye: RED
        if (stereo) {
            VkClearValue cv{}; cv.color.float32[0]=1.f; cv.color.float32[3]=1.f;
            rpbi.renderPass=renderPassRight; rpbi.framebuffer=fbRight[idx]; rpbi.pClearValues=&cv;
            vkCmdBeginRenderPass(cb,&rpbi,VK_SUBPASS_CONTENTS_INLINE); vkCmdEndRenderPass(cb);
        }
        VK_CHECK(vkEndCommandBuffer(cb));
    }

    void drawFrame() {
        uint32_t fi=frameIndex%MAX_FRAMES;
        vkWaitForFences(device,1,&inFlight[fi],VK_TRUE,UINT64_MAX);
        uint32_t imgIdx=0; VkResult r;
        VK_CHECK_SC(vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,imageReady[fi],VK_NULL_HANDLE,&imgIdx),r);
        if (r==VK_ERROR_OUT_OF_DATE_KHR) return;
        vkResetFences(device,1,&inFlight[fi]);
        vkResetCommandBuffer(cmdBufs[imgIdx],0);
        recordCommandBuffer(imgIdx);
        VkPipelineStageFlags ws=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount=1; si.pWaitSemaphores=&imageReady[fi]; si.pWaitDstStageMask=&ws;
        si.commandBufferCount=1; si.pCommandBuffers=&cmdBufs[imgIdx];
        si.signalSemaphoreCount=1; si.pSignalSemaphores=&renderDone[fi];
        VK_CHECK(vkQueueSubmit(queue,1,&si,inFlight[fi]));
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&renderDone[fi];
        pi.swapchainCount=1; pi.pSwapchains=&swapchain; pi.pImageIndices=&imgIdx;
        VK_CHECK_SC(vkQueuePresentKHR(queue,&pi),r);
        ++frameIndex;
    }

    void cleanup() {
        log("--- cleanup ---");
        if (device) vkDeviceWaitIdle(device);
        for (uint32_t i=0; i<MAX_FRAMES; ++i) {
            if (imageReady[i]) vkDestroySemaphore(device,imageReady[i],nullptr);
            if (renderDone[i]) vkDestroySemaphore(device,renderDone[i],nullptr);
            if (inFlight  [i]) vkDestroyFence    (device,inFlight  [i],nullptr);
        }
        if (cmdPool) vkDestroyCommandPool(device,cmdPool,nullptr);
        for (uint32_t i=0; i<swapCount; ++i) {
            if (fbLeft [i]) vkDestroyFramebuffer(device,fbLeft [i],nullptr);
            if (fbRight[i]) vkDestroyFramebuffer(device,fbRight[i],nullptr);
            if (viewLeft [i]) vkDestroyImageView(device,viewLeft [i],nullptr);
            if (viewRight[i]) vkDestroyImageView(device,viewRight[i],nullptr);
        }
        if (renderPassLeft)  vkDestroyRenderPass  (device,renderPassLeft, nullptr);
        if (renderPassRight) vkDestroyRenderPass  (device,renderPassRight,nullptr);
        if (swapchain)       vkDestroySwapchainKHR(device,swapchain,      nullptr);
        if (device)          vkDestroyDevice      (device,                nullptr);
        if (surface)         vkDestroySurfaceKHR  (instance,surface,      nullptr);
        if (instance)        vkDestroyInstance    (instance,              nullptr);
        log("--- cleanup done ---");
    }
};

// ============================================================================
// Entry point
// ============================================================================

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    AllocConsole();
    FILE *f=nullptr; freopen_s(&f,"CONOUT$","w",stdout);
    logInit();
    log("=== VkStereo3DVision v9 startup ===");
    logDisplayMode();

    // Step 1: Create window FIRST — DXGI must bind to an existing HWND.
    s_hwnd = createFullscreenWindow(hInst);

    // Step 2: Create D3D11 device + DXGI swap chain bound to the window.
    //   - Provides the DXGI surface context the stereo ICD looks up on the HWND.
    //   - Must precede NVAPI init so the handle sees an active DXGI context.
    bool d3dOk = d3d11Bootstrap(s_hwnd);

    // Step 3: NVAPI stereo handle registration.
    //   NvAPI_Stereo_CreateHandleFromIUnknown registers this process with the
    //   3D Vision stereo subsystem. With DXGI swap chain now present on the
    //   HWND, NvAPI_Stereo_IsActivated should return active=1 after Activate().
    bool nvapiOk = nvapiInit(s_pD3DDevice);

    log("  Window: OK  D3D11: %s  DXGI SC: %s  NVAPI: %s",
        d3dOk ? "OK" : "FAILED",
        s_pDXGISwapChain ? "OK" : "FAILED",
        nvapiOk ? "OK" : "FAILED");

    StereoApp app{};
    try {
        app.init(hInst, s_hwnd);
        log("--- render loop (ESC to quit) ---");
        uint32_t fc=0; MSG msg{};
        while (!s_quit) {
            while (PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)) {
                if (msg.message==WM_KEYDOWN && msg.wParam==VK_ESCAPE) s_quit=true;
                TranslateMessage(&msg); DispatchMessageA(&msg);
            }
            if (!s_quit) {
                app.drawFrame();
                if (++fc == 5) log("  (5 frames OK — logging suppressed)");
            }
        }
        log("--- exited after %u frames ---", fc);
    } catch (const std::exception &e) {
        log("EXCEPTION: %s", e.what());
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_OK|MB_ICONERROR);
    }
    app.cleanup();
    nvapiShutdown();
    d3d11Shutdown();
    if (s_logFile) fclose(s_logFile);
    return 0;
}
