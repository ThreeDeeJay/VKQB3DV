/*
 * vulkan_stereo_3dvision.cpp  v10
 *
 * Stereoscopic 3D – NVIDIA 3D Vision, driver 426.06, GeForce/consumer GPU.
 *
 * ── CONCLUSION FROM v1–v9 ────────────────────────────────────────────────────
 *
 *  vkCreateSwapchainKHR with imageArrayLayers=2 returned -3 (VK_ERROR_
 *  INITIALIZATION_FAILED) for every possible parameter combination across
 *  9 versions:  every FSE mode, every image count, every present mode,
 *  with/without NVAPI stereo registration, with/without DXGI swap chain,
 *  with/without validation layer.
 *
 *  The full device extension list (v9 log) contains no VK_NV_stereo or any
 *  undocumented stereo Vulkan extension.  VK_NV_display_stereo exists in the
 *  spec but is Linux/KMS only (requires VK_KHR_display, not on Windows) and
 *  is for DP in-band stereo signals — irrelevant for 3D Vision shutter glasses.
 *
 *  Conclusion: the Vulkan ICD on 426.06 advertises maxImageArrayLayers=2 as
 *  a surface capability but does not implement the stereo swapchain creation
 *  path.  The feature was never completed.  3D Vision stereo requires the
 *  D3D11+NVAPI path, which this version implements.
 *
 * ── v10 ARCHITECTURE ─────────────────────────────────────────────────────────
 *
 *  STEREO path (NvAPI_Stereo_CreateHandleFromIUnknown succeeds):
 *    D3D11 device + DXGI windowed swap chain on the WS_POPUP HWND.
 *    NVAPI stereo handle registered to the D3D11 device.
 *    Per frame:
 *      NvAPI_Stereo_SetActiveEye(LEFT)  → ClearRenderTargetView(CYAN)
 *      NvAPI_Stereo_SetActiveEye(RIGHT) → ClearRenderTargetView(RED)
 *      IDXGISwapChain::Present(1, 0)
 *    The 3D Vision driver composites left+right and synchronises with the
 *    IR emitter during Present.  No Vulkan used for stereo rendering.
 *
 *  NON-STEREO path (3D Vision off / stereo handle not created):
 *    Vulkan single-layer swapchain as in v9.  D3D11 device released before
 *    Vulkan surface creation to avoid HWND contention.
 *
 * ── NVAPI IDs ────────────────────────────────────────────────────────────────
 *  All IDs are stable across NVAPI versions (function-code dispatch table).
 *  NvAPI_Stereo_SetActiveEye: left=1, right=0 (NV_STEREO_ACTIVE_EYE enum).
 *
 * ── BUILD ────────────────────────────────────────────────────────────────────
 *   cl /std:c++17 /W3 /O2 main.cpp /I%VULKAN_SDK%\Include ^
 *      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib
 *   (d3d11 and nvapi loaded at runtime – no extra .lib needed)
 *
 * ── OUTPUT ───────────────────────────────────────────────────────────────────
 *   Stereo:     left eye CYAN,  right eye RED   (via 3D Vision shutter glasses)
 *   Non-stereo: CYAN fullscreen (Vulkan)
 *   Log: vulkan_stereo.log beside the exe.   ESC to quit.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

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
    char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr,exe,MAX_PATH);
    char *sl=strrchr(exe,'\\'); if(sl)sl[1]='\0'; else exe[0]='\0';
    char path[MAX_PATH]{}; snprintf(path,MAX_PATH,"%svulkan_stereo.log",exe);
    s_logFile=fopen(path,"w"); printf("Log: %s\n",path); fflush(stdout);
}
static void log(const char *fmt,...) {
    char buf[2048]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    puts(buf); OutputDebugStringA(buf); OutputDebugStringA("\n");
    if(s_logFile){fputs(buf,s_logFile);fputc('\n',s_logFile);fflush(s_logFile);}
}

// ============================================================================
// Display diagnostics
// ============================================================================
static void logDisplayMode() {
    log("--- display diagnostics ---");
    DEVMODEA dm{}; dm.dmSize=sizeof(dm);
    if(EnumDisplaySettingsA(nullptr,ENUM_CURRENT_SETTINGS,&dm))
        log("  Current: %ux%u @ %u Hz  bpp=%u",
            dm.dmPelsWidth,dm.dmPelsHeight,dm.dmDisplayFrequency,dm.dmBitsPerPel);
    for(DWORD i=0;;++i){
        DEVMODEA m{}; m.dmSize=sizeof(m);
        if(!EnumDisplaySettingsA(nullptr,i,&m))break;
        if(m.dmDisplayFrequency>=120&&m.dmPelsWidth==dm.dmPelsWidth)
            log("  mode[%u]: %ux%u @ %u Hz",i,m.dmPelsWidth,m.dmPelsHeight,m.dmDisplayFrequency);
    }
}

// ============================================================================
// Win32 window
// ============================================================================
static HWND s_hwnd=nullptr; static bool s_quit=false;
static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_DESTROY||m==WM_CLOSE){s_quit=true;PostQuitMessage(0);return 0;}
    return DefWindowProcA(h,m,w,l);
}
static HWND createFullscreenWindow(HINSTANCE hInst){
    WNDCLASSEXA wc{}; wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.hCursor=LoadCursorA(nullptr,IDC_ARROW); wc.lpszClassName="VkStereoWnd";
    RegisterClassExA(&wc);
    int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);
    log("Primary monitor: %dx%d",sw,sh);
    HWND hwnd=CreateWindowExA(0,"VkStereoWnd","3D Vision Stereo  ESC=quit",
        WS_POPUP,0,0,sw,sh,nullptr,nullptr,hInst,nullptr);
    ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd);
    SetForegroundWindow(hwnd); BringWindowToTop(hwnd);
    MSG msg{}; for(int i=0;i<10;++i)
        while(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageA(&msg);}
    log("Window: WS_POPUP %dx%d",sw,sh);
    return hwnd;
}

// ============================================================================
// Minimal D3D11 / DXGI types (no headers needed)
// ============================================================================
typedef enum { DDTH_HW=1 }           D3D_DRIVER_TYPE_;
typedef enum { DFL_11_0=0xb000 }     D3D_FEATURE_LEVEL_;
typedef enum { DFMT_B8G8R8A8=87 }    DXGI_FORMAT_;
typedef enum { DSE_DISCARD=0 }        DXGI_SWAP_EFFECT_;
typedef UINT DXGI_USAGE_;
#define DXGI_USAGE_RTO 0x20u
typedef struct { UINT N,D; }          DXGI_RATIONAL_;
typedef struct { UINT W,H; DXGI_RATIONAL_ Refresh; DXGI_FORMAT_ Fmt; UINT SLO,Scale; } DXGI_MODE_DESC_;
typedef struct { UINT Count,Quality; } DXGI_SAMPLE_DESC_;
typedef struct {
    DXGI_MODE_DESC_ BD; DXGI_SAMPLE_DESC_ SD;
    DXGI_USAGE_ BU; UINT BC; HWND OW; BOOL Win; DXGI_SWAP_EFFECT_ SE; UINT Fl;
} DXGI_SWAP_CHAIN_DESC_;

// D3D11_RENDER_TARGET_VIEW_DESC – just enough for Texture2D
// D3D11_RTV_DIMENSION_TEXTURE2D = 4
typedef struct { UINT MipSlice; } D3D11_TEX2D_RTV_;
typedef struct { DXGI_FORMAT_ Fmt; UINT Dim; D3D11_TEX2D_RTV_ Tex2D; char pad[60]; } D3D11_RTV_DESC_;

typedef struct { FLOAT X,Y,W,H,MinZ,MaxZ; } D3D11_VIEWPORT_;

// IID_ID3D11Texture2D = {6F15AAF2-D208-4E89-9AB4-489535D34F9C}
static const GUID s_iidTex2D = {0x6F15AAF2,0xD208,0x4E89,{0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C}};

typedef HRESULT(WINAPI *PfnD3D11CreateDAS)(
    void*,D3D_DRIVER_TYPE_,HMODULE,UINT,
    const D3D_FEATURE_LEVEL_*,UINT,UINT,
    const DXGI_SWAP_CHAIN_DESC_*,
    IUnknown**,IUnknown**,D3D_FEATURE_LEVEL_*,IUnknown**);
typedef HRESULT(WINAPI *PfnD3D11CreateD)(
    void*,D3D_DRIVER_TYPE_,HMODULE,UINT,
    const D3D_FEATURE_LEVEL_*,UINT,UINT,
    IUnknown**,D3D_FEATURE_LEVEL_*,IUnknown**);

// COM vtable helper – calls virtual method at index N
template<int N,typename R=HRESULT,typename...A>
static R vcall(void *o, A...args){
    auto vt=*(void***)o;
    return ((R(__stdcall*)(void*,A...))vt[N])(o,args...);
}

// IDXGISwapChain vtable indices (IUnknown:0-2, IDXGIObject:3-6, IDXGIDeviceSubObject:7)
//   8=Present  9=GetBuffer  10=SetFullscreenState
// ID3D11Device vtable (IUnknown:0-2, then methods 3+)
//   9=CreateRenderTargetView  40=GetImmediateContext
// ID3D11DeviceContext (IUnknown:0-2, ID3D11DeviceChild:3-6, then 7+)
//   33=OMSetRenderTargets  44=RSSetViewports  50=ClearRenderTargetView

static HMODULE   s_hD3D11       = nullptr;
static IUnknown *s_pD3DDevice   = nullptr;  // ID3D11Device*
static IUnknown *s_pDXGISC      = nullptr;  // IDXGISwapChain*
static IUnknown *s_pD3DCtx      = nullptr;  // ID3D11DeviceContext*
static IUnknown *s_pBackBufTex  = nullptr;  // ID3D11Texture2D* (backbuffer)
static IUnknown *s_pRTV         = nullptr;  // ID3D11RenderTargetView*
static int       s_swW=0, s_swH=0;

// Release a COM object safely
static void safeRelease(IUnknown *&p){if(p){p->Release();p=nullptr;}}

static bool d3d11Init(HWND hwnd, int w, int h) {
    log("--- D3D11 + DXGI init ---");
    s_hD3D11=LoadLibraryA("d3d11.dll");
    if(!s_hD3D11){log("  d3d11.dll: not found"); return false;}
    log("  d3d11.dll loaded");

    D3D_FEATURE_LEVEL_ fl=DFL_11_0, flOut{};
    DXGI_SWAP_CHAIN_DESC_ scd{};
    scd.BD.W=0; scd.BD.H=0; scd.BD.Refresh={120,1}; scd.BD.Fmt=DFMT_B8G8R8A8;
    scd.SD={1,0};
    scd.BU=DXGI_USAGE_RTO; scd.BC=2; scd.OW=hwnd;
    scd.Win=TRUE; scd.SE=DSE_DISCARD;

    // Attempt 1: device + swap chain together
    auto fnDAS=(PfnD3D11CreateDAS)GetProcAddress(s_hD3D11,"D3D11CreateDeviceAndSwapChain");
    if(fnDAS){
        IUnknown *pCtx=nullptr;
        HRESULT hr=fnDAS(nullptr,DDTH_HW,nullptr,0,&fl,1,7,&scd,
                         &s_pDXGISC,&s_pD3DDevice,&flOut,&pCtx);
        log("  D3D11CreateDeviceAndSwapChain: hr=0x%x",(unsigned)hr);
        if(pCtx){pCtx->Release();pCtx=nullptr;}
        if(SUCCEEDED(hr)){
            log("  Device=%p  SwapChain=%p",(void*)s_pD3DDevice,(void*)s_pDXGISC);
            s_swW=w; s_swH=h;
            return true;
        }
        safeRelease(s_pD3DDevice);
    }

    // Attempt 2: device only
    auto fnD=(PfnD3D11CreateD)GetProcAddress(s_hD3D11,"D3D11CreateDevice");
    if(!fnD){log("  D3D11CreateDevice: not found"); return false;}
    IUnknown *pCtx=nullptr;
    HRESULT hr=fnD(nullptr,DDTH_HW,nullptr,0,&fl,1,7,&s_pD3DDevice,&flOut,&pCtx);
    if(pCtx){pCtx->Release();}
    log("  D3D11CreateDevice (fallback): hr=0x%x",(unsigned)hr);
    if(FAILED(hr)){s_pD3DDevice=nullptr; return false;}
    log("  Device (no DXGI SC)=%p",(void*)s_pD3DDevice);
    return true;
}

// Create render target view on the DXGI back buffer
static bool d3d11SetupRTV(){
    if(!s_pD3DDevice||!s_pDXGISC) return false;
    log("--- D3D11 RTV setup ---");

    // GetImmediateContext (vtable 40 on ID3D11Device)
    vcall<40,void>(s_pD3DDevice,(IUnknown**)&s_pD3DCtx);
    log("  ImmediateContext=%p",(void*)s_pD3DCtx);
    if(!s_pD3DCtx) return false;

    // IDXGISwapChain::GetBuffer(0, IID_ID3D11Texture2D, &tex) – vtable 9
    HRESULT hr=vcall<9>(s_pDXGISC,(UINT)0,&s_iidTex2D,(void**)&s_pBackBufTex);
    log("  GetBuffer: hr=0x%x  tex=%p",(unsigned)hr,(void*)s_pBackBufTex);
    if(FAILED(hr)||!s_pBackBufTex) return false;

    // ID3D11Device::CreateRenderTargetView (vtable 9) – pass nullptr for desc (full texture)
    hr=vcall<9>(s_pD3DDevice,s_pBackBufTex,(void*)nullptr,(IUnknown**)&s_pRTV);
    log("  CreateRenderTargetView: hr=0x%x  rtv=%p",(unsigned)hr,(void*)s_pRTV);
    return SUCCEEDED(hr)&&s_pRTV;
}

static void d3d11Shutdown(){
    safeRelease(s_pRTV); safeRelease(s_pBackBufTex);
    safeRelease(s_pD3DCtx); safeRelease(s_pDXGISC); safeRelease(s_pD3DDevice);
    if(s_hD3D11){FreeLibrary(s_hD3D11);s_hD3D11=nullptr;}
}

// ============================================================================
// NVAPI
// ============================================================================
#define NVAPI_OK 0
typedef void *(*PfnNvQI)(unsigned);
typedef int   (*PfnNvV)();
typedef int   (*PfnNvFromIUnk)(IUnknown*,void**);
typedef int   (*PfnNvDH)(void*);

static HMODULE  s_hNvapi = nullptr;
static PfnNvQI  s_nvQI   = nullptr;
static void    *s_hStereo= nullptr;

static void *nvQ(unsigned id){return s_nvQI?s_nvQI(id):nullptr;}

#define NvID_Initialize           0x0150E828u
#define NvID_Unload               0xD22BDD7Eu
#define NvID_Stereo_Enable        0x239C4545u
#define NvID_Stereo_SetMode       0x5E8F0BECu
#define NvID_Stereo_CreateFromIUnk 0xAC7E37F4u
#define NvID_Stereo_DestroyHandle 0x3A153134u
#define NvID_Stereo_Activate      0xF6A1AD68u
#define NvID_Stereo_IsActivated   0x1FB0BC30u   // (handle, *byte)
#define NvID_Stereo_SetSeparation 0x5C069FA3u   // (handle, float)
#define NvID_Stereo_SetActiveEye  0x96EEA9D9u   // (handle, NvU32 eye)
// NV_STEREO_ACTIVE_EYE: RIGHT=0, LEFT=1, MONO=2  (per NVAPI SDK)
#define NV_EYE_RIGHT 0u
#define NV_EYE_LEFT  1u
#define NV_EYE_MONO  2u
#define NvID_Stereo_GetSeparation 0xCE5729DFu   // (handle, *float)

static bool nvapiInit(IUnknown *pDev){
    log("--- NVAPI init ---");
    s_hNvapi=LoadLibraryA("nvapi64.dll");
    if(!s_hNvapi){log("  nvapi64.dll: not found"); return false;}
    s_nvQI=(PfnNvQI)GetProcAddress(s_hNvapi,"nvapi_QueryInterface");
    if(!s_nvQI){log("  nvapi_QueryInterface: not found"); return false;}
    log("  nvapi64.dll OK");

    {auto fn=(PfnNvV)nvQ(NvID_Initialize);
     if(!fn){log("  NvAPI_Initialize: fn not found"); return false;}
     int r=fn(); log("  NvAPI_Initialize = %d",r); if(r)return false;}

    {auto fn=(PfnNvV)nvQ(NvID_Stereo_Enable);
     if(fn){int r=fn(); log("  NvAPI_Stereo_Enable = %d",r);}}

    {typedef int(*Pfn)(unsigned); auto fn=(Pfn)nvQ(NvID_Stereo_SetMode);
     if(fn){int r=fn(0); log("  NvAPI_Stereo_SetDriverMode(AUTOMATIC) = %d",r);}}

    {auto fn=(PfnNvFromIUnk)nvQ(NvID_Stereo_CreateFromIUnk);
     if(fn&&pDev){
         int r=fn(pDev,&s_hStereo);
         log("  NvAPI_Stereo_CreateHandleFromIUnknown = %d (%s)  handle=%p",
             r,r==NVAPI_OK?"OK":"FAILED",(void*)s_hStereo);
         if(r!=NVAPI_OK) s_hStereo=nullptr;
     }}

    if(s_hStereo){
        {typedef int(*Pfn)(void*); auto fn=(Pfn)nvQ(NvID_Stereo_Activate);
         if(fn){int r=fn(s_hStereo); log("  NvAPI_Stereo_Activate = %d",r);}}

        {typedef int(*Pfn)(void*,unsigned char*); auto fn=(Pfn)nvQ(NvID_Stereo_IsActivated);
         if(fn){unsigned char a=0; fn(s_hStereo,&a); log("  NvAPI_Stereo_IsActivated: active=%d",(int)a);}}

        {typedef int(*Pfn)(void*,float); auto fn=(Pfn)nvQ(NvID_Stereo_SetSeparation);
         if(fn){int r=fn(s_hStereo,50.f); log("  NvAPI_Stereo_SetSeparation(50%%) = %d",r);}}

        // Verify SetActiveEye function is present (don't call yet, just probe)
        {auto fn=nvQ(NvID_Stereo_SetActiveEye);
         log("  NvAPI_Stereo_SetActiveEye fn: %s",fn?"FOUND":"NOT FOUND");}
    }
    log("  NVAPI complete  handle=%p",(void*)s_hStereo);
    return true;
}

static void nvapiShutdown(){
    if(s_hStereo){auto f=(PfnNvDH)nvQ(NvID_Stereo_DestroyHandle);if(f)f(s_hStereo);s_hStereo=nullptr;}
    if(s_hNvapi){auto f=(PfnNvV)nvQ(NvID_Unload);if(f)f();FreeLibrary(s_hNvapi);s_hNvapi=nullptr;}
}

// ============================================================================
// Stereo render loop (D3D11 + NVAPI) – no Vulkan
// ============================================================================

// Set active eye; returns NVAPI result code
static int setEye(unsigned eye){
    typedef int(*Pfn)(void*,unsigned); auto fn=(Pfn)nvQ(NvID_Stereo_SetActiveEye);
    if(!fn) return -1;
    return fn(s_hStereo,eye);
}

// Clear the current RTV to a solid colour via D3D11 context.
// OMSetRenderTargets = vtable 33,  RSSetViewports = 44, ClearRenderTargetView = 50
static void clearRTV(float r,float g,float b){
    if(!s_pD3DCtx||!s_pRTV) return;
    float c[4]={r,g,b,1.f};
    // Set viewport
    D3D11_VIEWPORT_ vp{0.f,0.f,(float)s_swW,(float)s_swH,0.f,1.f};
    vcall<44,void>(s_pD3DCtx,(UINT)1,&vp);
    // Set render target
    vcall<33,void>(s_pD3DCtx,(UINT)1,&s_pRTV,(void*)nullptr);
    // Clear
    vcall<50,void>(s_pD3DCtx,s_pRTV,c);
}

// Present via DXGI (vtable 8: Present(SyncInterval, Flags))
static HRESULT dxgiPresent(UINT sync){
    if(!s_pDXGISC) return E_FAIL;
    return vcall<8>(s_pDXGISC,sync,(UINT)0);
}

static void runStereoLoop(){
    log("--- D3D11 stereo render loop (ESC to quit) ---");
    log("  Left eye = CYAN,  Right eye = RED");
    log("  NV_EYE_LEFT=%u  NV_EYE_RIGHT=%u",NV_EYE_LEFT,NV_EYE_RIGHT);

    typedef int(*PfnIA)(void*,unsigned char*); auto fnIA=(PfnIA)nvQ(NvID_Stereo_IsActivated);
    typedef int(*PfnGS)(void*,float*);          auto fnGS=(PfnGS)nvQ(NvID_Stereo_GetSeparation);

    uint32_t frame=0;
    MSG msg{};
    while(!s_quit){
        while(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE) s_quit=true;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        if(s_quit) break;

        // Left eye
        int rL=setEye(NV_EYE_LEFT);
        clearRTV(0.f,1.f,1.f); // CYAN

        // Right eye
        int rR=setEye(NV_EYE_RIGHT);
        clearRTV(1.f,0.f,0.f); // RED

        // Present (sync=1 for vsync – required for shutter timing)
        HRESULT hr=dxgiPresent(1);

        ++frame;

        // Diagnostic every 60 frames
        if(frame==1||frame%60==0){
            unsigned char active=0;
            float sep=0.f;
            if(fnIA) fnIA(s_hStereo,&active);
            if(fnGS) fnGS(s_hStereo,&sep);
            log("  frame=%-6u  setEye(L)=%d setEye(R)=%d  present=0x%x  "
                "IsActivated=%d  separation=%.1f%%",
                frame,rL,rR,(unsigned)hr,(int)active,sep);
        }
    }
    log("--- stereo loop exited after %u frames ---",frame);
}

// ============================================================================
// Vulkan non-stereo path (unchanged from v9)
// ============================================================================

static const char *vkResultStr(VkResult r){
    switch(r){
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FSE_LOST";
    default: return "(unknown)";
    }
}
#define VK_CHECK(expr) do{ log("  CALL  " #expr); VkResult _r=(expr); \
    if(_r!=VK_SUCCESS){char _b[512]; snprintf(_b,sizeof(_b),"FATAL %s (%d) at line %d\n" #expr, \
    vkResultStr(_r),(int)_r,__LINE__); log(_b); throw std::runtime_error(_b);} \
    log("  OK    " #expr); }while(0)
#define VK_CHECK_SC(expr,out) do{(out)=(expr); \
    if((out)!=VK_SUCCESS&&(out)!=VK_SUBOPTIMAL_KHR&&(out)!=VK_ERROR_OUT_OF_DATE_KHR){ \
    char _b[512]; snprintf(_b,sizeof(_b),"FATAL %s (%d) at line %d\n" #expr, \
    vkResultStr((VkResult)(out)),(int)(out),__LINE__); log(_b); throw std::runtime_error(_b);} \
    if((out)!=VK_SUCCESS)log("  WARN  " #expr " -> %s",vkResultStr((VkResult)(out)));} while(0)

template<class T> static T vk_clamp(T v,T lo,T hi){return v<lo?lo:(v>hi?hi:v);}
static bool hasInstExt(const char *n){
    uint32_t c=0; vkEnumerateInstanceExtensionProperties(nullptr,&c,nullptr);
    std::vector<VkExtensionProperties> v(c); vkEnumerateInstanceExtensionProperties(nullptr,&c,v.data());
    for(auto &e:v)if(!strcmp(e.extensionName,n))return true; return false;
}

struct VkApp {
    VkInstance       instance=VK_NULL_HANDLE; VkSurfaceKHR surface=VK_NULL_HANDLE;
    VkPhysicalDevice physDev=VK_NULL_HANDLE;  VkDevice     device =VK_NULL_HANDLE;
    uint32_t         queueFamily=UINT32_MAX;  VkQueue      queue  =VK_NULL_HANDLE;
    VkSwapchainKHR   swapchain=VK_NULL_HANDLE;
    VkFormat         swapFmt=VK_FORMAT_UNDEFINED; VkExtent2D swapExtent{};
    uint32_t         swapCount=0;
    std::vector<VkImage>       imgs;
    std::vector<VkImageView>   views;
    std::vector<VkFramebuffer> fbs;
    VkRenderPass     rp=VK_NULL_HANDLE;
    VkCommandPool    pool=VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cbs;
    static const uint32_t MF=2;
    VkSemaphore imgRdy[MF]{},renDone[MF]{};
    VkFence     inFlt[MF]{};
    uint32_t    fi=0;

    void init(HINSTANCE hInst,HWND hwnd){
        log("=== Vulkan (non-stereo) init ===");
        // Instance
        bool sc2=hasInstExt(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        std::vector<const char*> ie={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        if(sc2)ie.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        const char *val="VK_LAYER_KHRONOS_validation";
        uint32_t lc=0; vkEnumerateInstanceLayerProperties(&lc,nullptr);
        std::vector<VkLayerProperties> lp(lc); vkEnumerateInstanceLayerProperties(&lc,lp.data());
        bool hv=false; for(auto &l:lp)if(!strcmp(l.layerName,val)){hv=true;break;}
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName="VkStereoMono"; ai.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo=&ai;
        ici.enabledExtensionCount=(uint32_t)ie.size(); ici.ppEnabledExtensionNames=ie.data();
        if(hv){ici.enabledLayerCount=1;ici.ppEnabledLayerNames=&val;}
        VK_CHECK(vkCreateInstance(&ici,nullptr,&instance));
        log("  Instance: validation %s",hv?"ON":"OFF");
        // Surface
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance=hInst; sci.hwnd=hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance,&sci,nullptr,&surface));
        // Physical device + queue
        uint32_t cnt=0; vkEnumeratePhysicalDevices(instance,&cnt,nullptr);
        std::vector<VkPhysicalDevice> devs(cnt); vkEnumeratePhysicalDevices(instance,&cnt,devs.data());
        for(auto pd:devs){
            VkPhysicalDeviceProperties pr{}; vkGetPhysicalDeviceProperties(pd,&pr);
            log("  GPU: %s",pr.deviceName);
            uint32_t qfc=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qfc,nullptr);
            std::vector<VkQueueFamilyProperties> qf(qfc);
            vkGetPhysicalDeviceQueueFamilyProperties(pd,&qfc,qf.data());
            for(uint32_t i=0;i<qfc;++i){
                VkBool32 pres=VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(pd,i,surface,&pres);
                if(!(qf[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)||!pres)continue;
                physDev=pd; queueFamily=i; break;
            }
            if(physDev)break;
        }
        if(!physDev)throw std::runtime_error("No Vulkan GPU");
        // Device
        float p=1.f;
        VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        dq.queueFamilyIndex=queueFamily;dq.queueCount=1;dq.pQueuePriorities=&p;
        const char *de=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&dq;dc.enabledExtensionCount=1;dc.ppEnabledExtensionNames=&de;
        VK_CHECK(vkCreateDevice(physDev,&dc,nullptr,&device));
        vkGetDeviceQueue(device,queueFamily,0,&queue);
        // Swapchain
        VkSurfaceCapabilitiesKHR caps{}; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev,surface,&caps);
        uint32_t fc=0; vkGetPhysicalDeviceSurfaceFormatsKHR(physDev,surface,&fc,nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fc); vkGetPhysicalDeviceSurfaceFormatsKHR(physDev,surface,&fc,fmts.data());
        VkSurfaceFormatKHR fmt=fmts[0];
        for(auto &f:fmts)if(f.format==VK_FORMAT_B8G8R8A8_UNORM&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)fmt=f;
        swapFmt=fmt.format;
        RECT rc{}; GetClientRect(hwnd,&rc);
        uint32_t ww=(uint32_t)(rc.right-rc.left); if(!ww)ww=1;
        uint32_t wh=(uint32_t)(rc.bottom-rc.top);  if(!wh)wh=1;
        swapExtent.width=vk_clamp(ww,caps.minImageExtent.width,caps.maxImageExtent.width);
        swapExtent.height=vk_clamp(wh,caps.minImageExtent.height,caps.maxImageExtent.height);
        uint32_t imgc=caps.minImageCount+1;
        if(caps.maxImageCount&&imgc>caps.maxImageCount)imgc=caps.maxImageCount;
        VkSwapchainCreateInfoKHR scc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        scc.surface=surface;scc.minImageCount=imgc;scc.imageFormat=swapFmt;
        scc.imageColorSpace=fmt.colorSpace;scc.imageExtent=swapExtent;scc.imageArrayLayers=1;
        scc.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        scc.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
        scc.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        scc.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        scc.presentMode=VK_PRESENT_MODE_FIFO_KHR;scc.clipped=VK_TRUE;
        VK_CHECK(vkCreateSwapchainKHR(device,&scc,nullptr,&swapchain));
        vkGetSwapchainImagesKHR(device,swapchain,&swapCount,nullptr);
        imgs.resize(swapCount); vkGetSwapchainImagesKHR(device,swapchain,&swapCount,imgs.data());
        views.resize(swapCount,VK_NULL_HANDLE);
        for(uint32_t i=0;i<swapCount;++i){
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image=imgs[i];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=swapFmt;
            vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            VK_CHECK(vkCreateImageView(device,&vi,nullptr,&views[i]));
        }
        // Render pass
        VkAttachmentDescription att{};att.format=swapFmt;att.samples=VK_SAMPLE_COUNT_1_BIT;
        att.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;att.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        VkSubpassDependency dep{};dep.srcSubpass=VK_SUBPASS_EXTERNAL;dep.dstSubpass=0;
        dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rci.attachmentCount=1;rci.pAttachments=&att;rci.subpassCount=1;rci.pSubpasses=&sub;
        rci.dependencyCount=1;rci.pDependencies=&dep;
        VK_CHECK(vkCreateRenderPass(device,&rci,nullptr,&rp));
        // Framebuffers
        fbs.resize(swapCount,VK_NULL_HANDLE);
        for(uint32_t i=0;i<swapCount;++i){
            VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbi.renderPass=rp;fbi.attachmentCount=1;fbi.pAttachments=&views[i];
            fbi.width=swapExtent.width;fbi.height=swapExtent.height;fbi.layers=1;
            VK_CHECK(vkCreateFramebuffer(device,&fbi,nullptr,&fbs[i]));
        }
        // Command pool + buffers
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex=queueFamily;cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device,&cpi,nullptr,&pool));
        cbs.resize(swapCount);
        VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cba.commandPool=pool;cba.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cba.commandBufferCount=swapCount;
        VK_CHECK(vkAllocateCommandBuffers(device,&cba,cbs.data()));
        // Sync
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fci.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        for(uint32_t i=0;i<MF;++i){
            VK_CHECK(vkCreateSemaphore(device,&si,nullptr,&imgRdy[i]));
            VK_CHECK(vkCreateSemaphore(device,&si,nullptr,&renDone[i]));
            VK_CHECK(vkCreateFence    (device,&fci,nullptr,&inFlt  [i]));
        }
        log("=== Vulkan init complete: %ux%u  %u images ===",swapExtent.width,swapExtent.height,swapCount);
    }

    void drawFrame(){
        vkWaitForFences(device,1,&inFlt[fi],VK_TRUE,UINT64_MAX);
        uint32_t idx=0; VkResult r;
        VK_CHECK_SC(vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,imgRdy[fi],VK_NULL_HANDLE,&idx),r);
        if(r==VK_ERROR_OUT_OF_DATE_KHR)return;
        vkResetFences(device,1,&inFlt[fi]);
        vkResetCommandBuffer(cbs[idx],0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cbs[idx],&bi));
        VkClearValue cv{}; cv.color.float32[1]=1.f;cv.color.float32[2]=1.f;cv.color.float32[3]=1.f;
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass=rp;rpbi.framebuffer=fbs[idx];
        rpbi.renderArea={{0,0},swapExtent};rpbi.clearValueCount=1;rpbi.pClearValues=&cv;
        vkCmdBeginRenderPass(cbs[idx],&rpbi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cbs[idx]);
        VK_CHECK(vkEndCommandBuffer(cbs[idx]));
        VkPipelineStageFlags ws=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo subi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        subi.waitSemaphoreCount=1;subi.pWaitSemaphores=&imgRdy[fi];subi.pWaitDstStageMask=&ws;
        subi.commandBufferCount=1;subi.pCommandBuffers=&cbs[idx];
        subi.signalSemaphoreCount=1;subi.pSignalSemaphores=&renDone[fi];
        VK_CHECK(vkQueueSubmit(queue,1,&subi,inFlt[fi]));
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&renDone[fi];
        pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&idx;
        VK_CHECK_SC(vkQueuePresentKHR(queue,&pi),r);
        fi=(fi+1)%MF;
    }

    void cleanup(){
        log("--- Vulkan cleanup ---");
        if(device)vkDeviceWaitIdle(device);
        for(uint32_t i=0;i<MF;++i){
            if(imgRdy[i])vkDestroySemaphore(device,imgRdy[i],nullptr);
            if(renDone[i])vkDestroySemaphore(device,renDone[i],nullptr);
            if(inFlt[i])vkDestroyFence(device,inFlt[i],nullptr);
        }
        if(pool)vkDestroyCommandPool(device,pool,nullptr);
        for(uint32_t i=0;i<swapCount;++i){
            if(fbs[i])vkDestroyFramebuffer(device,fbs[i],nullptr);
            if(views[i])vkDestroyImageView(device,views[i],nullptr);
        }
        if(rp)vkDestroyRenderPass(device,rp,nullptr);
        if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);
        if(device)vkDestroyDevice(device,nullptr);
        if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);
        if(instance)vkDestroyInstance(instance,nullptr);
        log("--- Vulkan cleanup done ---");
    }
};

// ============================================================================
// Entry point
// ============================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int){
    AllocConsole();
    FILE *f=nullptr; freopen_s(&f,"CONOUT$","w",stdout);
    logInit();
    log("=== VkStereo3DVision v10 startup ===");
    logDisplayMode();

    // Window first – DXGI needs an existing HWND
    s_hwnd = createFullscreenWindow(hInst);
    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);

    // D3D11 device + DXGI swap chain on the window
    bool d3dOk = d3d11Init(s_hwnd, sw, sh);

    // NVAPI: try to get stereo handle (returns error if 3DV is off)
    bool nvapiOk = nvapiInit(s_pD3DDevice);

    bool stereoMode = (s_hStereo != nullptr);
    log("  D3D11=%s  DXGI_SC=%s  NVAPI=%s  stereoMode=%s",
        d3dOk?"OK":"FAIL", s_pDXGISC?"OK":"FAIL",
        nvapiOk?"OK":"FAIL", stereoMode?"YES":"NO");

    if(stereoMode){
        // D3D11 stereo path
        if(!d3d11SetupRTV()){
            log("ERROR: D3D11 RTV setup failed – cannot proceed with stereo path");
            MessageBoxA(nullptr,"D3D11 RTV setup failed","Error",MB_OK|MB_ICONERROR);
        } else {
            runStereoLoop();
        }
        d3d11Shutdown();
        nvapiShutdown();
    } else {
        // Release D3D11 before creating Vulkan surface on the same HWND
        d3d11Shutdown();
        nvapiShutdown();

        // Vulkan non-stereo path
        VkApp app{};
        try{
            app.init(hInst, s_hwnd);
            log("--- Vulkan render loop (ESC to quit) ---");
            uint32_t fc=0; MSG msg{};
            while(!s_quit){
                while(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){
                    if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE) s_quit=true;
                    TranslateMessage(&msg); DispatchMessageA(&msg);
                }
                if(!s_quit){ app.drawFrame(); if(++fc==5)log("  (5 frames OK)"); }
            }
            log("--- Vulkan exited after %u frames ---",fc);
        }catch(const std::exception &e){
            log("EXCEPTION: %s",e.what());
            MessageBoxA(nullptr,e.what(),"Fatal Error",MB_OK|MB_ICONERROR);
        }
        app.cleanup();
    }

    if(s_logFile) fclose(s_logFile);
    return 0;
}
