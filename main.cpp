/*
 * vulkan_stereo_3dvision.cpp  v8
 *
 * Stereoscopic 3D – NVIDIA 3D Vision beta driver 426.06, GeForce/consumer GPU.
 *
 * ── ROOT CAUSE ANALYSIS (accumulated across v1–v7) ───────────────────────────
 *
 *  v7 NVAPI logs revealed two problems:
 *
 *  A) NvAPI_Stereo_Enable() returns OK but is insufficient.
 *     It sets a global flag but does NOT create a per-process stereo handle.
 *     The stereo ICD checks for a live handle bound to an active device.
 *     Evidence: with ALLOWED/DISALLOWED, still get -3 after Enable().
 *     With APPLICATION_CONTROLLED, the driver *partially* enters the stereo
 *     path (different from -3 – it hard-crashes instead), confirming that
 *     Enable() opened a door but something else is still missing.
 *
 *  B) The D3D11 stereo observation on 452.06 is the key:
 *     D3D11 CreateDevice → NvAPI_Stereo_CreateHandleFromIUnknown(pDevice, &hStereo)
 *     That call creates a per-device stereo handle that registers the *process*
 *     with the 3D Vision stereo subsystem. This is what actually allows the
 *     driver to accept an imageArrayLayers=2 Vulkan swapchain for this process.
 *
 * ── FIX ──────────────────────────────────────────────────────────────────────
 *
 *  1. Dynamically load d3d11.dll (no extra link deps) and create a minimal
 *     D3D11 device (just enough to get an IUnknown pointer).
 *  2. Call NvAPI_Stereo_Enable() then
 *     NvAPI_Stereo_CreateHandleFromIUnknown(pD3D11Device, &hStereo).
 *     This creates the per-process stereo handle the Vulkan ICD checks for.
 *  3. Keep the Vulkan stereo swapchain creation with all FSE modes probed,
 *     but now with the process properly registered.
 *  4. Validation layer is disabled for the stereo Vulkan instance.
 *     The layer wraps ICD entry points; on 426.06 the stereo check appears
 *     to use a back-channel that bypasses the layer, but excluding it removes
 *     any possibility of layer interference.
 *
 * ── SWAPCHAIN PATHS ──────────────────────────────────────────────────────────
 *
 *  STEREO  (maxImageArrayLayers == 2):
 *    D3D11 bootstrap + NVAPI handle creation → Vulkan swapchain probe
 *    across all FSE modes.  FIFO, IDENTITY, min image count.
 *
 *  NON-STEREO  (maxImageArrayLayers == 1):
 *    No D3D11 bootstrap. Simple FIFO swapchain, validation layer ON.
 *
 * ── BUILD ────────────────────────────────────────────────────────────────────
 *   cl /std:c++17 /W3 /O2 main.cpp /I%VULKAN_SDK%\Include ^
 *      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib
 *   (d3d11 loaded dynamically – no d3d11.lib needed)
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
#include <unknwn.h>     // IUnknown

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

// ============================================================================
// Display diagnostics
// ============================================================================

static void logDisplayMode() {
    log("--- display diagnostics ---");
    DEVMODEA dm{}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        log("  Current: %ux%u @ %u Hz  bpp=%u  flags=0x%x",
            dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
            dm.dmBitsPerPel, dm.dmDisplayFlags);
    for (DWORD i = 0; ; ++i) {
        DEVMODEA m{}; m.dmSize = sizeof(m);
        if (!EnumDisplaySettingsA(nullptr, i, &m)) break;
        if (m.dmDisplayFrequency >= 120 && m.dmPelsWidth == dm.dmPelsWidth)
            log("  mode[%u]: %ux%u @ %u Hz  bpp=%u",
                i, m.dmPelsWidth, m.dmPelsHeight,
                m.dmDisplayFrequency, m.dmBitsPerPel);
    }
}

// ============================================================================
// D3D11 bootstrap – loaded dynamically, no d3d11.lib required
// ============================================================================

// Minimal D3D11 types needed to call CreateDevice
// (avoids needing d3d11.h / dxgi.h)
typedef enum D3D_DRIVER_TYPE_ {
    D3D_DRIVER_TYPE_HARDWARE = 1
} D3D_DRIVER_TYPE_;
typedef enum D3D_FEATURE_LEVEL_ {
    D3D_FEATURE_LEVEL_11_0 = 0xb000
} D3D_FEATURE_LEVEL_;
// D3D11_SDK_VERSION is 7; passed directly as a literal below

typedef HRESULT (WINAPI *PfnD3D11CreateDevice)(
    void*,              // pAdapter
    D3D_DRIVER_TYPE_,   // DriverType
    HMODULE,            // Software
    UINT,               // Flags
    const D3D_FEATURE_LEVEL_*, // pFeatureLevels
    UINT,               // FeatureLevels
    UINT,               // SDKVersion
    IUnknown**,         // ppDevice  (ID3D11Device*)
    D3D_FEATURE_LEVEL_*, // pFeatureLevel
    IUnknown**          // ppImmediateContext (ID3D11DeviceContext*)
);

static HMODULE   s_hD3D11  = nullptr;
static IUnknown *s_pD3DDevice = nullptr;  // kept alive while stereo handle exists

static bool d3d11Bootstrap() {
    log("--- D3D11 bootstrap ---");

    s_hD3D11 = LoadLibraryA("d3d11.dll");
    if (!s_hD3D11) { log("  d3d11.dll: not found"); return false; }
    log("  d3d11.dll loaded");

    auto fnCreate = (PfnD3D11CreateDevice)
        GetProcAddress(s_hD3D11, "D3D11CreateDevice");
    if (!fnCreate) { log("  D3D11CreateDevice: not found"); return false; }
    log("  D3D11CreateDevice: found");

    D3D_FEATURE_LEVEL_ fl = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL_ flOut{};
    IUnknown *pCtx = nullptr;

    HRESULT hr = fnCreate(
        nullptr,                 // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                 // no software rasterizer
        0,                       // no debug flags
        &fl, 1,                  // feature levels
        7,                       // D3D11_SDK_VERSION
        &s_pD3DDevice,           // out: device
        &flOut,
        &pCtx                    // out: immediate context (we don't need it)
    );
    log("  D3D11CreateDevice: hr=0x%x (%s)",
        (unsigned)hr, SUCCEEDED(hr)?"OK":"FAILED");
    if (pCtx) { pCtx->Release(); pCtx = nullptr; }
    if (FAILED(hr)) { s_pD3DDevice = nullptr; return false; }

    log("  D3D11 device created: %p", (void*)s_pD3DDevice);
    return true;
}


// ============================================================================
// NVAPI – loaded dynamically, no SDK required
// ============================================================================

#define NVAPI_OK   0
#define NVAPI_ERROR (-1)

typedef void *(*PfnNvApiQueryInterface)(unsigned int id);
typedef int   (*PfnNvApiVoid)();
typedef int   (*PfnNvApiFromIUnknown)(IUnknown *pDev, void **ppHandle);
typedef int   (*PfnNvApiDestroyHandle)(void *handle);

static HMODULE               s_hNvapi      = nullptr;
static PfnNvApiQueryInterface s_nvQI        = nullptr;
static void                 *s_stereoHandle = nullptr;

static void *nvQ(unsigned int id) {
    return s_nvQI ? s_nvQI(id) : nullptr;
}

// Documented stable NVAPI function IDs
// (verified against public NVAPI SDK headers nvapi.h / nvapi_lite_stereo.h)
#define NVAPI_ID_Initialize                    0x0150E828u
#define NVAPI_ID_Unload                        0xD22BDD7Eu
#define NVAPI_ID_Stereo_Enable                 0x239C4545u
#define NVAPI_ID_Stereo_SetDriverMode          0x5E8F0BECu  // 0=automatic, 1=manual
#define NVAPI_ID_Stereo_CreateHandleFromIUnknown 0xAC7E37F4u
#define NVAPI_ID_Stereo_DestroyHandle          0x3A153134u
#define NVAPI_ID_Stereo_Activate               0xF6A1AD68u
#define NVAPI_ID_Stereo_IsActivated            0x1FB0BC30u  // (StereoHandle, NvU8*)

static bool nvapiInit(IUnknown *pD3DDevice) {
    log("--- NVAPI init ---");

    s_hNvapi = LoadLibraryA("nvapi64.dll");
    if (!s_hNvapi) { log("  nvapi64.dll: not found"); return false; }
    log("  nvapi64.dll loaded");

    s_nvQI = (PfnNvApiQueryInterface)GetProcAddress(s_hNvapi,"nvapi_QueryInterface");
    if (!s_nvQI) { log("  nvapi_QueryInterface: not found"); return false; }
    log("  nvapi_QueryInterface: OK");

    // NvAPI_Initialize
    auto fnInit = (PfnNvApiVoid)nvQ(NVAPI_ID_Initialize);
    if (!fnInit) { log("  NvAPI_Initialize: fn not found"); return false; }
    int r = fnInit();
    log("  NvAPI_Initialize() = %d (%s)", r, r==NVAPI_OK?"OK":"FAILED");
    if (r != NVAPI_OK) return false;

    // NvAPI_Stereo_Enable – global enable
    {
        auto fn = (PfnNvApiVoid)nvQ(NVAPI_ID_Stereo_Enable);
        log("  NvAPI_Stereo_Enable fn: %s", fn?"found":"NOT found");
        if (fn) {
            int sr = fn();
            log("  NvAPI_Stereo_Enable() = %d (%s)", sr, sr==NVAPI_OK?"OK":"FAILED");
        }
    }

    // NvAPI_Stereo_SetDriverMode(0 = AUTOMATIC)
    // Ensures the driver uses automatic stereo detection rather than
    // requiring explicit per-eye rendering commands.
    {
        typedef int (*PfnSetMode)(unsigned int mode);
        auto fn = (PfnSetMode)nvQ(NVAPI_ID_Stereo_SetDriverMode);
        log("  NvAPI_Stereo_SetDriverMode fn: %s", fn?"found":"NOT found");
        if (fn) {
            int sr = fn(0); // 0 = NVAPI_STEREO_DRIVER_MODE_AUTOMATIC
            log("  NvAPI_Stereo_SetDriverMode(AUTOMATIC) = %d (%s)",
                sr, sr==NVAPI_OK?"OK":"FAILED");
        }
    }

    // NvAPI_Stereo_CreateHandleFromIUnknown – THE critical call.
    // Creates a per-process stereo handle bound to pD3DDevice.
    // This is what D3D11 applications do internally; it registers the
    // current process with the 3D Vision stereo subsystem and enables
    // the driver to accept imageArrayLayers=2 Vulkan swapchains.
    if (pD3DDevice) {
        auto fn = (PfnNvApiFromIUnknown)nvQ(NVAPI_ID_Stereo_CreateHandleFromIUnknown);
        log("  NvAPI_Stereo_CreateHandleFromIUnknown fn: %s",fn?"found":"NOT found");
        if (fn) {
            int sr = fn(pD3DDevice, &s_stereoHandle);
            log("  NvAPI_Stereo_CreateHandleFromIUnknown(d3dDev) = %d (%s)  handle=%p",
                sr, sr==NVAPI_OK?"OK":"FAILED", s_stereoHandle);
            if (sr != NVAPI_OK || !s_stereoHandle) {
                log("  WARNING: stereo handle creation failed – "
                    "Vulkan stereo swapchain will likely still fail");
            }
        }
    } else {
        log("  WARNING: no D3D11 device – skipping CreateHandleFromIUnknown");
    }

    // NvAPI_Stereo_Activate – explicitly activate stereo for this handle
    if (s_stereoHandle) {
        typedef int (*PfnActivate)(void *handle);
        auto fn = (PfnActivate)nvQ(NVAPI_ID_Stereo_Activate);
        log("  NvAPI_Stereo_Activate fn: %s", fn?"found":"NOT found");
        if (fn) {
            int sr = fn(s_stereoHandle);
            log("  NvAPI_Stereo_Activate(handle) = %d (%s)",
                sr, sr==NVAPI_OK?"OK":"FAILED");
        }
    }

    // NvAPI_Stereo_IsActivated – confirm active (needs handle + bool*)
    if (s_stereoHandle) {
        typedef int (*PfnIsActivated)(void *handle, unsigned char *pActive);
        auto fn = (PfnIsActivated)nvQ(NVAPI_ID_Stereo_IsActivated);
        log("  NvAPI_Stereo_IsActivated fn: %s", fn?"found":"NOT found");
        if (fn) {
            unsigned char active = 0;
            int sr = fn(s_stereoHandle, &active);
            log("  NvAPI_Stereo_IsActivated(handle) = %d  active=%d", sr, (int)active);
        }
    }

    log("  NVAPI init complete");
    return true;
}

static void nvapiShutdown() {
    if (s_stereoHandle) {
        auto fn = (PfnNvApiDestroyHandle)nvQ(NVAPI_ID_Stereo_DestroyHandle);
        if (fn) fn(s_stereoHandle);
        s_stereoHandle = nullptr;
    }
    if (s_hNvapi) {
        auto fn = (PfnNvApiVoid)nvQ(NVAPI_ID_Unload);
        if (fn) fn();
        FreeLibrary(s_hNvapi);
        s_hNvapi = nullptr;
    }
}

static void d3d11Shutdown() {
    if (s_pD3DDevice) { s_pD3DDevice->Release(); s_pD3DDevice = nullptr; }
    if (s_hD3D11)     { FreeLibrary(s_hD3D11); s_hD3D11 = nullptr; }
}

// ============================================================================
// VK helpers
// ============================================================================

static const char *vkResultStr(VkResult r) {
    switch(r){
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
        VkResult _r=(expr);                                                    \
        if(_r!=VK_SUCCESS){                                                    \
            char _b[512];                                                      \
            snprintf(_b,sizeof(_b),"FATAL %s (%d) at line %d\n" #expr,        \
                     vkResultStr(_r),(int)_r,__LINE__);                        \
            log(_b); throw std::runtime_error(_b);                             \
        }                                                                      \
        log("  OK    " #expr);                                                 \
    }while(0)

#define VK_CHECK_SWAPCHAIN(expr,out)                                           \
    do{                                                                        \
        (out)=(expr);                                                          \
        if((out)!=VK_SUCCESS&&(out)!=VK_SUBOPTIMAL_KHR&&                      \
           (out)!=VK_ERROR_OUT_OF_DATE_KHR&&                                  \
           (out)!=VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT){               \
            char _b[512];                                                      \
            snprintf(_b,sizeof(_b),"FATAL %s (%d) at line %d\n" #expr,        \
                     vkResultStr((VkResult)(out)),(int)(out),__LINE__);        \
            log(_b); throw std::runtime_error(_b);                             \
        }                                                                      \
        if((out)!=VK_SUCCESS)                                                  \
            log("  WARN  " #expr " -> %s",vkResultStr((VkResult)(out)));       \
    }while(0)

template<class T> static T vk_clamp(T v,T lo,T hi){return v<lo?lo:(v>hi?hi:v);}

static bool hasInstanceExt(const char *n){
    uint32_t c=0; vkEnumerateInstanceExtensionProperties(nullptr,&c,nullptr);
    std::vector<VkExtensionProperties> v(c);
    vkEnumerateInstanceExtensionProperties(nullptr,&c,v.data());
    for(auto &e:v) if(!strcmp(e.extensionName,n)) return true;
    return false;
}
static bool hasDeviceExt(VkPhysicalDevice pd,const char *n){
    uint32_t c=0; vkEnumerateDeviceExtensionProperties(pd,nullptr,&c,nullptr);
    std::vector<VkExtensionProperties> v(c);
    vkEnumerateDeviceExtensionProperties(pd,nullptr,&c,v.data());
    for(auto &e:v) if(!strcmp(e.extensionName,n)) return true;
    return false;
}

// ============================================================================
// Win32 window
// ============================================================================

static HWND s_hwnd = nullptr;
static bool s_quit = false;

static LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_DESTROY||msg==WM_CLOSE){s_quit=true;PostQuitMessage(0);return 0;}
    return DefWindowProcA(hwnd,msg,wp,lp);
}

static HWND createFullscreenWindow(HINSTANCE hInst){
    WNDCLASSEXA wc{};
    wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.hCursor=LoadCursorA(nullptr,IDC_ARROW);
    wc.lpszClassName="VkStereoWnd";
    RegisterClassExA(&wc);
    int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);
    log("Primary monitor: %dx%d",sw,sh);
    HWND hwnd=CreateWindowExA(0,"VkStereoWnd",
        "Vulkan 3D Vision Stereo [Left=Cyan | Right=Red]  ESC=quit",
        WS_POPUP,0,0,sw,sh,nullptr,nullptr,hInst,nullptr);
    ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd);
    SetForegroundWindow(hwnd); BringWindowToTop(hwnd);
    // Brief message pump to fully realise the window
    MSG msg{};
    for(int i=0;i<10;++i)
        while(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
    log("Window: WS_POPUP %dx%d",sw,sh);
    return hwnd;
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

    bool hasSurfaceCaps2 = false;
    bool hasFSE          = false;

    // -----------------------------------------------------------------------
    void init(HINSTANCE hInst, HWND hwnd) {
        log("=== init begin ===");
        createInstance();
        createSurface(hInst,hwnd);
        pickPhysicalDevice();
        createDevice();
        createSwapchain(hwnd);
        createRenderPasses();
        createFramebuffers();
        createCommandPool();
        allocateCommandBuffers();
        createSyncObjects();
        log("=== init complete: %ux%u  %u images  stereo=%s ===",
            swapExtent.width,swapExtent.height,swapCount,stereo?"YES":"NO");
    }

    // -----------------------------------------------------------------------
    void createInstance() {
        log("--- createInstance ---");

        hasSurfaceCaps2=hasInstanceExt(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        log("  VK_KHR_get_surface_capabilities2: %s",
            hasSurfaceCaps2?"available":"NOT available");

        std::vector<const char*> exts={
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        if(hasSurfaceCaps2)
            exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

        // Disable the validation layer when an NVAPI stereo handle was created
        // (i.e. when we're attempting the stereo path).
        // The Khronos validation layer wraps every ICD entry point; on 426.06
        // the driver's stereo path check uses a back-channel that may not
        // function correctly when ICD calls are indirected through the layer.
        // Disabling it removes this variable entirely for the stereo case.
        bool wantVal = (s_stereoHandle == nullptr);

        const char *valLayer = "VK_LAYER_KHRONOS_validation";
        uint32_t lc=0; vkEnumerateInstanceLayerProperties(&lc,nullptr);
        std::vector<VkLayerProperties> lp(lc);
        vkEnumerateInstanceLayerProperties(&lc,lp.data());
        bool hasVal=false;
        for(auto &l:lp) if(!strcmp(l.layerName,valLayer)){hasVal=true;break;}
        log("  Validation layer: %s",
            (!wantVal)?"DISABLED (stereo path)":
            hasVal?"available":"NOT available");

        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName="VkStereo3DVision"; ai.apiVersion=VK_API_VERSION_1_1;

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo=&ai;
        ci.enabledExtensionCount=(uint32_t)exts.size();
        ci.ppEnabledExtensionNames=exts.data();
        if(wantVal&&hasVal){ci.enabledLayerCount=1;ci.ppEnabledLayerNames=&valLayer;}

        VK_CHECK(vkCreateInstance(&ci,nullptr,&instance));
        log("  Instance created (validation %s)",
            (wantVal&&hasVal)?"ON":"OFF");
    }

    // -----------------------------------------------------------------------
    void createSurface(HINSTANCE hInst,HWND hwnd){
        log("--- createSurface ---");
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance=hInst; ci.hwnd=hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance,&ci,nullptr,&surface));
    }

    // -----------------------------------------------------------------------
    void pickPhysicalDevice(){
        log("--- pickPhysicalDevice ---");
        uint32_t cnt=0;
        vkEnumeratePhysicalDevices(instance,&cnt,nullptr);
        log("  Physical devices: %u",cnt);
        if(!cnt) throw std::runtime_error("No Vulkan GPUs");
        std::vector<VkPhysicalDevice> devs(cnt);
        vkEnumeratePhysicalDevices(instance,&cnt,devs.data());

        for(auto pd:devs){
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pd,&props);
            uint32_t maj=props.driverVersion>>22,min=(props.driverVersion>>14)&0xFF;
            log("  GPU: %s  driver %u.%u (0x%x)",
                props.deviceName,maj,min,props.driverVersion);

            uint32_t qfc=0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd,&qfc,nullptr);
            std::vector<VkQueueFamilyProperties> qf(qfc);
            vkGetPhysicalDeviceQueueFamilyProperties(pd,&qfc,qf.data());

            for(uint32_t i=0;i<qfc;++i){
                VkBool32 present=VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd,i,surface,&present);
                if(!(qf[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)||!present) continue;
                physDev=pd; queueFamily=i;

                VkSurfaceCapabilitiesKHR caps{};
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd,surface,&caps);
                log("  Surface: min=%u max=%u maxLayers=%u xfm=0x%x",
                    caps.minImageCount,caps.maxImageCount,
                    caps.maxImageArrayLayers,(unsigned)caps.currentTransform);

                uint32_t fc=0;
                vkGetPhysicalDeviceSurfaceFormatsKHR(pd,surface,&fc,nullptr);
                std::vector<VkSurfaceFormatKHR> fmts(fc);
                vkGetPhysicalDeviceSurfaceFormatsKHR(pd,surface,&fc,fmts.data());
                for(auto &f:fmts)
                    log("  fmt=%d cs=%d",(int)f.format,(int)f.colorSpace);

                uint32_t pmc=0;
                vkGetPhysicalDeviceSurfacePresentModesKHR(pd,surface,&pmc,nullptr);
                std::vector<VkPresentModeKHR> pms(pmc);
                vkGetPhysicalDeviceSurfacePresentModesKHR(pd,surface,&pmc,pms.data());
                char pmb[128]{}; int off=0;
                for(auto pm:pms) off+=snprintf(pmb+off,sizeof(pmb)-off,"%d ",pm);
                log("  PresentModes: [%s]",pmb);

                stereo=(caps.maxImageArrayLayers>=2);
                log("  Stereo: %s (maxLayers=%u)",
                    stereo?"YES":"NO",caps.maxImageArrayLayers);

                hasFSE=hasSurfaceCaps2&&
                       hasDeviceExt(pd,VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
                log("  VK_EXT_full_screen_exclusive: %s",hasFSE?"available":"NOT available");
                break;
            }
            if(physDev) break;
        }
        if(!physDev) throw std::runtime_error("No suitable GPU");
    }

    // -----------------------------------------------------------------------
    void createDevice(){
        log("--- createDevice ---");
        float prio=1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex=queueFamily; qci.queueCount=1; qci.pQueuePriorities=&prio;

        std::vector<const char*> exts={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        if(hasFSE){
            exts.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
            log("  VK_EXT_full_screen_exclusive: loaded");
        }

        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount=1; ci.pQueueCreateInfos=&qci;
        ci.enabledExtensionCount=(uint32_t)exts.size();
        ci.ppEnabledExtensionNames=exts.data();
        VK_CHECK(vkCreateDevice(physDev,&ci,nullptr,&device));
        vkGetDeviceQueue(device,queueFamily,0,&queue);
    }

    // -----------------------------------------------------------------------
    // Probe all FSE modes for the stereo swapchain.
    // Attempts in order, returns first success or VK_NULL_HANDLE.
    VkSwapchainKHR tryCreateStereoSwapchain(
            VkExtent2D extent, VkSurfaceFormatKHR fmt,
            uint32_t imgCount, HWND hwnd)
    {
        struct Attempt {
            const char               *label;
            VkFullScreenExclusiveEXT  fseMode;
            bool                      chainFse;
            bool                      chainWin32; // include Win32 HMONITOR info
        };
        // Probe order: ALLOWED first (driver uses its preferred internal path),
        // then variants without the Win32 chain (simpler), then null pNext.
        Attempt attempts[] = {
            { "ALLOWED+Win32",     VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT,                true,  true  },
            { "ALLOWED",           VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT,                true,  false },
            { "DISALLOWED",        VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT,              true,  false },
            { "APP_CTRL+Win32",    VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT, true,  true  },
            { "APP_CTRL",          VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT, true,  false },
            { "null",              VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT,                false, false },
        };

        for(auto &a:attempts){
            if(a.chainFse && !hasFSE){
                log("    [%s] skipped (FSE not on device)",a.label);
                continue;
            }
            log("  --- attempt: %s ---",a.label);

            VkSurfaceFullScreenExclusiveWin32InfoEXT fseWin32{
                VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
            fseWin32.hmonitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTOPRIMARY);

            VkSurfaceFullScreenExclusiveInfoEXT fseInfo{
                VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
            fseInfo.fullScreenExclusive=a.fseMode;
            fseInfo.pNext=(a.chainFse&&a.chainWin32)?(void*)&fseWin32:nullptr;

            VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            ci.pNext           =a.chainFse?(void*)&fseInfo:nullptr;
            ci.surface          =surface;
            ci.minImageCount    =imgCount;
            ci.imageFormat      =fmt.format;
            ci.imageColorSpace  =fmt.colorSpace;
            ci.imageExtent      =extent;
            ci.imageArrayLayers =2;
            ci.imageUsage       =VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            ci.imageSharingMode =VK_SHARING_MODE_EXCLUSIVE;
            ci.preTransform     =VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            ci.compositeAlpha   =VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            ci.presentMode      =VK_PRESENT_MODE_FIFO_KHR;
            ci.clipped          =VK_TRUE;

            log("    Creating swapchain (layers=2 FIFO IDENTITY pNext=%s)...",
                a.chainFse?a.label:"null");

            VkSwapchainKHR sc=VK_NULL_HANDLE;
            // Wrap in SEH to catch hard driver crashes (access violations etc.)
            // that some FSE+stereo combos trigger on 426.06.
            VkResult r=VK_ERROR_INITIALIZATION_FAILED;
            __try {
                r=vkCreateSwapchainKHR(device,&ci,nullptr,&sc);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                log("    EXCEPTION 0x%08x during vkCreateSwapchainKHR – "
                    "driver crash for this FSE mode",GetExceptionCode());
                sc=VK_NULL_HANDLE;
                continue;
            }
            log("    -> %s (%d)",vkResultStr(r),(int)r);

            if(r==VK_SUCCESS){
                log("    SUCCESS: FSE mode = %s",a.label);
                return sc;
            }
        }
        return VK_NULL_HANDLE;
    }

    // -----------------------------------------------------------------------
    void createSwapchain(HWND hwnd){
        log("--- createSwapchain ---");

        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev,surface,&caps);

        uint32_t fc=0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev,surface,&fc,nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fc);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physDev,surface,&fc,fmts.data());
        VkSurfaceFormatKHR fmt=fmts[0];
        for(auto &f:fmts)
            if(f.format==VK_FORMAT_B8G8R8A8_UNORM&&
               f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) fmt=f;
        swapFmt=fmt.format;
        log("  Format=%d  colorSpace=%d",(int)swapFmt,(int)fmt.colorSpace);

        RECT rc{}; GetClientRect(hwnd,&rc);
        uint32_t ww=(uint32_t)(rc.right-rc.left); if(!ww)ww=1;
        uint32_t wh=(uint32_t)(rc.bottom-rc.top);  if(!wh)wh=1;
        swapExtent.width =vk_clamp(ww,caps.minImageExtent.width, caps.maxImageExtent.width);
        swapExtent.height=vk_clamp(wh,caps.minImageExtent.height,caps.maxImageExtent.height);
        log("  Extent: %ux%u",swapExtent.width,swapExtent.height);

        if(stereo){
            uint32_t imgCount=caps.minImageCount;
            log("  STEREO probe: imageCount=%u  stereoHandle=%p",
                imgCount,(void*)s_stereoHandle);
            swapchain=tryCreateStereoSwapchain(swapExtent,fmt,imgCount,hwnd);
            if(swapchain==VK_NULL_HANDLE){
                log("  ALL STEREO ATTEMPTS FAILED.");
                log("  Remaining hypotheses:");
                log("    1. This driver requires the app be whitelisted in an");
                log("       NVIDIA application profile (check NVIDIA Inspector).");
                log("    2. The stereo ICD path requires a Vulkan device handle");
                log("       passed to NVAPI (NvAPI_Vulkan_* functions if they exist).");
                log("    3. Vulkan imageArrayLayers=2 stereo is not functional on");
                log("       426.06 without an undocumented registry key or NVAPI flag.");
                log("    4. Try enabling 'Allow application to control' stereo mode");
                log("       in NVIDIA Control Panel -> Manage 3D Settings.");
                throw std::runtime_error(
                    "Stereo swapchain creation failed (all modes).\n"
                    "See vulkan_stereo.log for details.");
            }
        } else {
            uint32_t imgCount=caps.minImageCount+1;
            if(caps.maxImageCount&&imgCount>caps.maxImageCount) imgCount=caps.maxImageCount;
            log("  NON-STEREO: imageCount=%u",imgCount);
            VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            ci.surface=surface; ci.minImageCount=imgCount;
            ci.imageFormat=swapFmt; ci.imageColorSpace=fmt.colorSpace;
            ci.imageExtent=swapExtent; ci.imageArrayLayers=1;
            ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
            ci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;
            ci.clipped=VK_TRUE; ci.pNext=nullptr;
            VK_CHECK(vkCreateSwapchainKHR(device,&ci,nullptr,&swapchain));
        }

        vkGetSwapchainImagesKHR(device,swapchain,&swapCount,nullptr);
        swapImages.resize(swapCount);
        vkGetSwapchainImagesKHR(device,swapchain,&swapCount,swapImages.data());
        log("  swapCount: %u",swapCount);

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
            log("  image[%u]: L=%p R=%p",i,(void*)viewLeft[i],(void*)viewRight[i]);
        }
    }

    // -----------------------------------------------------------------------
    VkRenderPass makeRenderPass(){
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

    void createRenderPasses(){
        log("--- createRenderPasses ---");
        renderPassLeft=makeRenderPass();
        if(stereo) renderPassRight=makeRenderPass();
    }

    void createFramebuffers(){
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

    void createCommandPool(){
        log("--- createCommandPool ---");
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex=queueFamily;
        ci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device,&ci,nullptr,&cmdPool));
    }

    void allocateCommandBuffers(){
        log("--- allocateCommandBuffers ---");
        cmdBufs.resize(swapCount);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool=cmdPool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount=swapCount;
        VK_CHECK(vkAllocateCommandBuffers(device,&ai,cmdBufs.data()));
    }

    void createSyncObjects(){
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

    void recordCommandBuffer(uint32_t idx){
        VkCommandBuffer cb=cmdBufs[idx];
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb,&bi));
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderArea={{0,0},swapExtent}; rpbi.clearValueCount=1;
        // Left eye: CYAN
        {
            VkClearValue cv{};
            cv.color.float32[0]=0.f; cv.color.float32[1]=1.f;
            cv.color.float32[2]=1.f; cv.color.float32[3]=1.f;
            rpbi.renderPass=renderPassLeft; rpbi.framebuffer=fbLeft[idx];
            rpbi.pClearValues=&cv;
            vkCmdBeginRenderPass(cb,&rpbi,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdEndRenderPass(cb);
        }
        // Right eye: RED (stereo only)
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

    void drawFrame(){
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
        VkPipelineStageFlags ws=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount=1;   si.pWaitSemaphores=&imageReady[fi];
        si.pWaitDstStageMask=&ws;
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

    void cleanup(){
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

// ============================================================================
// Entry point
// ============================================================================

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    AllocConsole();
    FILE *f=nullptr; freopen_s(&f,"CONOUT$","w",stdout);
    logInit();
    log("=== VkStereo3DVision v8 startup ===");

    logDisplayMode();

    // Step 1: Create a D3D11 device (dynamically – no d3d11.lib needed).
    // This is needed to create an NVAPI stereo handle bound to a real device,
    // which is what registers this process with the 3D Vision stereo subsystem.
    bool d3dOk = d3d11Bootstrap();

    // Step 2: NVAPI init + NvAPI_Stereo_CreateHandleFromIUnknown.
    bool nvapiOk = nvapiInit(s_pD3DDevice);
    log("  D3D11: %s  NVAPI: %s",
        d3dOk?"OK":"FAILED", nvapiOk?"OK":"FAILED");

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
                    log("  (first 5 frames OK – logging suppressed)");
            }
        }
        log("--- exited after %u frames ---",frameCount);
    } catch(const std::exception &e){
        log("EXCEPTION: %s",e.what());
        MessageBoxA(nullptr,e.what(),"Fatal Error",MB_OK|MB_ICONERROR);
    }
    app.cleanup();
    nvapiShutdown();
    d3d11Shutdown();
    if(s_logFile) fclose(s_logFile);
    return 0;
}
