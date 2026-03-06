/*
 * vulkan_stereo_3dvision.cpp  v24
 *
 * Stereoscopic 3D – NVIDIA 3D Vision, driver 426.06 / 452.06.
 *
 * ── HISTORY ──────────────────────────────────────────────────────────────────
 *
 *  v13-v21: Tried NVAPI companion surface path (SetSurfaceCreationMode,
 *           SetActiveEye, DIRECT mode, RSBC, combined D3D11 call).
 *           GetSurfaceCreationMode = 0 (AUTO) at every probe point across all
 *           versions.  SetSurfaceCreationMode not in dispatch table.
 *           Companion surface never allocated.  RSBC permanently a no-op.
 *
 * ── ROOT CAUSE ───────────────────────────────────────────────────────────────
 *
 *  On driver 426.06 + RTX 2080 Ti, the NVAPI functions required to allocate
 *  a stereo companion surface for explicit per-eye rendering are absent from
 *  the dispatch table.  No NVAPI path can produce per-eye content.
 *
 * ── v22 APPROACH: DXGI 1.2 STEREO SWAP CHAIN ─────────────────────────────────
 *
 *  Windows 8+ exposes stereo swap chains through DXGI 1.2.
 *  When 3D Vision is enabled in the NVIDIA control panel, the driver reports
 *  the display as stereo-capable to DXGI.
 *
 *  IDXGIFactory2::CreateSwapChainForHwnd with Stereo=TRUE creates a swap chain
 *  whose back buffer is a Texture2DArray[2]:
 *    slice 0 → left  eye
 *    slice 1 → right eye
 *
 *  No NVAPI companion allocation needed — DXGI owns the eye buffers.
 *  RTVs are created on each slice; each is cleared to its eye colour.
 *
 *  Sequence:
 *    1. NvAPI_Stereo_Enable (hint to driver before D3D11 init)
 *    2. D3D11CreateDevice (device only – no swap chain)
 *    3. IDXGIFactory2 via QI chain
 *    4. IDXGIFactory2::IsWindowedStereoEnabled() → log result
 *    5. CreateSwapChainForHwnd(Stereo=TRUE, windowed)
 *    6. SetFullscreenState(TRUE)
 *    7. GetBuffer → Texture2DArray; create slice RTVs
 *    8. Per frame: ClearRTV(leftRTV, CYAN), ClearRTV(rightRTV, RED), Present
 *
 *  NVAPI is still used for IsActivated monitoring and Activate() calls,
 *  but NOT for per-eye rendering.
 *
 * ── BUILD ────────────────────────────────────────────────────────────────────
 *   cl /std:c++17 /W3 /O2 main.cpp /I%VULKAN_SDK%\Include ^
 *      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib
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
static FILE *s_logFile=nullptr;
static void logInit(){
    char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr,exe,MAX_PATH);
    char *sl=strrchr(exe,'\\'); if(sl)sl[1]='\0'; else exe[0]='\0';
    char path[MAX_PATH]{}; snprintf(path,MAX_PATH,"%svulkan_stereo.log",exe);
    s_logFile=fopen(path,"w"); printf("Log: %s\n",path); fflush(stdout);
}
static void log(const char *fmt,...){
    char buf[2048]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    puts(buf); OutputDebugStringA(buf); OutputDebugStringA("\n");
    if(s_logFile){fputs(buf,s_logFile);fputc('\n',s_logFile);fflush(s_logFile);}
}

// ============================================================================
// Win32
// ============================================================================
static HWND s_hwnd=nullptr;
static bool s_quit=false;
static bool s_restore=false;

static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_DESTROY: case WM_CLOSE: s_quit=true; PostQuitMessage(0); return 0;
    case WM_ACTIVATE:    if(LOWORD(w)!=WA_INACTIVE){s_restore=true; log("  WM_ACTIVATE → restore");}    break;
    case WM_ACTIVATEAPP: if(w){s_restore=true; log("  WM_ACTIVATEAPP → restore");}                       break;
    case WM_SETFOCUS:    s_restore=true; break;
    }
    return DefWindowProcA(h,m,w,l);
}
static HWND createWindow(HINSTANCE hInst,int w,int h){
    WNDCLASSEXA wc{}; wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.hCursor=LoadCursorA(nullptr,IDC_ARROW); wc.lpszClassName="VkStereoWnd";
    RegisterClassExA(&wc);
    HWND hwnd=CreateWindowExA(0,"VkStereoWnd","3D Vision Stereo v24  ESC=quit",
        WS_POPUP,0,0,w,h,nullptr,nullptr,hInst,nullptr);
    ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd);
    SetForegroundWindow(hwnd); BringWindowToTop(hwnd);
    MSG msg{}; for(int i=0;i<10;++i)
        while(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageA(&msg);}
    return hwnd;
}

// ============================================================================
// Minimal D3D11 / DXGI types (no SDK headers required)
// ============================================================================
typedef enum { DDTH_HW=1          } D3D_DRIVER_TYPE_;
typedef enum { DFL_11_0=0xb000    } D3D_FEATURE_LEVEL_;
typedef enum { DFMT_B8G8R8A8=87, DFMT_UNKNOWN=0 } DXGI_FORMAT_;
typedef enum { DSE_FLIP_DISCARD=4, DSE_DISCARD=0 } DXGI_SWAP_EFFECT_;
typedef enum { DSCALE_NONE=1      } DXGI_SCALING_;
typedef enum { DALPHA_UNSPEC=0    } DXGI_ALPHA_MODE_;
#define DXGI_USAGE_RTO 0x20u
#define DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH 2u

typedef struct { UINT N,D; } DXGI_RATIONAL_;
typedef struct { UINT Count,Quality; } DXGI_SAMPLE_DESC_;

// DXGI_SWAP_CHAIN_DESC1  (IDXGIFactory2::CreateSwapChainForHwnd)
typedef struct {
    UINT Width, Height;
    DXGI_FORMAT_     Format;
    BOOL             Stereo;       // ← TRUE for stereo Texture2DArray[2] back buffer
    DXGI_SAMPLE_DESC_ SampleDesc;
    UINT             BufferUsage;
    UINT             BufferCount;
    DXGI_SCALING_    Scaling;
    DXGI_SWAP_EFFECT_ SwapEffect;
    DXGI_ALPHA_MODE_ AlphaMode;
    UINT             Flags;
} DXGI_SWAP_CHAIN_DESC1_;

// DXGI_SWAP_CHAIN_FULLSCREEN_DESC (optional pFullscreenDesc to CreateSwapChainForHwnd)
typedef struct {
    DXGI_RATIONAL_ RefreshRate;
    UINT           ScanlineOrdering;  // DXGI_MODE_SCANLINE_ORDER
    UINT           Rotation;          // DXGI_MODE_ROTATION
    BOOL           Windowed;
} DXGI_SC_FULLSCREEN_DESC_;

// D3D11_RENDER_TARGET_VIEW_DESC for Texture2DArray
// ViewDimension: TEXTURE2D=4  TEXTURE2DARRAY=5
typedef struct {
    DXGI_FORMAT_ Format;
    UINT         ViewDimension;
    UINT         _pad;           // union padding to reach Texture2DArray fields
    UINT         MipSlice;
    UINT         FirstArraySlice;
    UINT         ArraySize;
} D3D11_RTV_DESC_Tex2DArray_;

typedef struct {
    UINT Width,Height,MipLevels,ArraySize;
    DXGI_FORMAT_ Format; DXGI_SAMPLE_DESC_ SampleDesc;
    UINT Usage,BindFlags,CPUAccessFlags,MiscFlags;
} D3D11_TEX2D_DESC_;

// IIDs
static const GUID s_iidTex2D    ={0x6F15AAF2,0xD208,0x4E89,{0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C}};
static const GUID s_iidDXGIDev  ={0x54EC77FA,0x1377,0x44E6,{0x8C,0x32,0x88,0xFD,0x5F,0x44,0xC8,0x4C}};
// IDXGIFactory2
static const GUID s_iidDXGIFact2={0x50C83A1C,0xE072,0x4C48,{0x87,0xB0,0x36,0x30,0xFA,0x36,0xA6,0xD0}};

typedef HRESULT(WINAPI *PfnD3D11CD)(
    void*,D3D_DRIVER_TYPE_,HMODULE,UINT,
    const D3D_FEATURE_LEVEL_*,UINT,UINT,
    IUnknown**,D3D_FEATURE_LEVEL_*,IUnknown**);

template<int N,typename R=HRESULT,typename...A>
static R vcall(void *o,A...args){
    return ((R(__stdcall*)(void*,A...))(*(void***)o)[N])(o,args...);
}

// Vtable indices:
//   IUnknown:              0=QI 1=AddRef 2=Release
//   IDXGIObject:           3..6  (6=GetParent)
//   IDXGIDeviceSubObject:  7=GetDevice
//   IDXGIDevice:           7=GetAdapter
//   IDXGIAdapter:          (inherits IDXGIObject, GetParent=6)
//   IDXGIFactory:          10=CreateSwapChain
//   IDXGIFactory1:         13=EnumAdapters1  14=IsCurrent
//   IDXGIFactory2:         14=IsWindowedStereoEnabled
//                          15=CreateSwapChainForHwnd
//   IDXGISwapChain:        8=Present 9=GetBuffer 10=SetFullscreenState
//                          11=GetFullscreenState 12=GetDesc 13=ResizeBuffers
//   ID3D11Device:          5=CreateTexture2D 9=CreateRenderTargetView
//                          40=GetImmediateContext
//   ID3D11DeviceContext:   33=OMSetRenderTargets 50=ClearRenderTargetView

static HMODULE   s_hD3D11    = nullptr;
static IUnknown *s_pD3DDev   = nullptr;  // ID3D11Device
static IUnknown *s_pDXGISC   = nullptr;  // IDXGISwapChain (stereo)
static IUnknown *s_pD3DCtx   = nullptr;  // ID3D11DeviceContext
static IUnknown *s_pBBTex    = nullptr;  // back buffer Texture2DArray[2]
static IUnknown *s_pEyeRTV[2]= {};       // RTV slice 0 (left) and slice 1 (right)
static int s_W=0,s_H=0;

static void safeRelease(IUnknown *&p){if(p){p->Release();p=nullptr;}}

static bool d3d11CreateDevice(){
    log("--- D3D11 device (no swap chain) ---");
    s_hD3D11=LoadLibraryA("d3d11.dll");
    if(!s_hD3D11){log("  d3d11.dll not found"); return false;}
    auto fn=(PfnD3D11CD)GetProcAddress(s_hD3D11,"D3D11CreateDevice");
    if(!fn){log("  D3D11CreateDevice not found"); return false;}
    D3D_FEATURE_LEVEL_ fl=DFL_11_0,flOut{};
    IUnknown *pCtx=nullptr;
    HRESULT hr=fn(nullptr,DDTH_HW,nullptr,0,&fl,1,7,&s_pD3DDev,&flOut,&pCtx);
    log("  D3D11CreateDevice: hr=0x%x (%s)",(unsigned)hr,SUCCEEDED(hr)?"OK":"FAILED");
    if(pCtx) pCtx->Release();
    if(FAILED(hr)){s_pD3DDev=nullptr; return false;}
    log("  Device=%p",(void*)s_pD3DDev);
    return true;
}

// Build RTVs on both slices of the Texture2DArray back buffer.
static bool d3d11BuildSliceRTVs(){
    safeRelease(s_pEyeRTV[0]); safeRelease(s_pEyeRTV[1]); safeRelease(s_pBBTex);
    if(!s_pD3DDev||!s_pDXGISC) return false;
    if(!s_pD3DCtx) vcall<40,void>(s_pD3DDev,(IUnknown**)&s_pD3DCtx);

    HRESULT hr=vcall<9>(s_pDXGISC,(UINT)0,&s_iidTex2D,(void**)&s_pBBTex);
    log("  GetBuffer: hr=0x%x  tex=%p",(unsigned)hr,(void*)s_pBBTex);
    if(FAILED(hr)||!s_pBBTex) return false;

    for(UINT slice=0;slice<2;++slice){
        D3D11_RTV_DESC_Tex2DArray_ desc{};
        desc.Format        = DFMT_B8G8R8A8;
        desc.ViewDimension = 5;          // D3D11_RTV_DIMENSION_TEXTURE2DARRAY
        desc._pad          = 0;
        desc.MipSlice      = 0;
        desc.FirstArraySlice = slice;
        desc.ArraySize       = 1;
        hr=vcall<9>(s_pD3DDev,s_pBBTex,&desc,(IUnknown**)&s_pEyeRTV[slice]);
        log("  CreateRTV(slice=%u): hr=0x%x  rtv=%p",slice,(unsigned)hr,(void*)s_pEyeRTV[slice]);
        if(FAILED(hr)||!s_pEyeRTV[slice]){
            if(slice==0) log("  FATAL: slice 0 RTV failed");
            else         log("  WARNING: slice 1 RTV failed – back buffer may not be stereo array");
            return false;
        }
    }
    log("  Stereo Texture2DArray RTVs: OK  (slice0=left  slice1=right)");
    return true;
}

static void d3d11Shutdown(){
    // No SetFullscreenState(FALSE) needed – we never called SetFullscreenState(TRUE)
    safeRelease(s_pEyeRTV[0]); safeRelease(s_pEyeRTV[1]); safeRelease(s_pBBTex);
    safeRelease(s_pD3DCtx); safeRelease(s_pDXGISC); safeRelease(s_pD3DDev);
    if(s_hD3D11){FreeLibrary(s_hD3D11);s_hD3D11=nullptr;}
}

static HRESULT dxgiPresent(UINT sync){
    return s_pDXGISC?vcall<8>(s_pDXGISC,sync,(UINT)0):E_FAIL;
}

// Create DXGI 1.2 stereo swap chain via IDXGIFactory2::CreateSwapChainForHwnd.
// Stereo=TRUE forces the back buffer to be Texture2DArray[2].
static bool dxgiCreateStereoSwapChain(HWND hwnd){
    log("--- DXGI 1.2 stereo swap chain ---");

    // QI chain: ID3D11Device → IDXGIDevice → GetAdapter → GetParent(IDXGIFactory2)
    IUnknown *pDXGIDev=nullptr;
    HRESULT hr=vcall<0>(s_pD3DDev,&s_iidDXGIDev,(void**)&pDXGIDev);
    log("  QI IDXGIDevice: hr=0x%x",(unsigned)hr);
    if(FAILED(hr)||!pDXGIDev) return false;

    IUnknown *pAdapter=nullptr;
    hr=vcall<7>(pDXGIDev,&pAdapter); pDXGIDev->Release();
    log("  GetAdapter: hr=0x%x",(unsigned)hr);
    if(FAILED(hr)||!pAdapter) return false;

    IUnknown *pFact2=nullptr;
    hr=vcall<6>(pAdapter,&s_iidDXGIFact2,(void**)&pFact2); pAdapter->Release();
    log("  GetParent IDXGIFactory2: hr=0x%x  ptr=%p",(unsigned)hr,(void*)pFact2);
    if(FAILED(hr)||!pFact2) return false;

    // IDXGIFactory2::IsWindowedStereoEnabled (vtable 14)
    // Returns TRUE if the display supports stereo in windowed mode
    BOOL wse=vcall<14,BOOL>(pFact2);
    log("  IsWindowedStereoEnabled = %s",wse?"TRUE":"FALSE");

    // Build DXGI_SWAP_CHAIN_DESC1 with Stereo=TRUE
    DXGI_SWAP_CHAIN_DESC1_ scd1{};
    scd1.Width       = (UINT)s_W;
    scd1.Height      = (UINT)s_H;
    scd1.Format      = DFMT_B8G8R8A8;
    scd1.Stereo      = TRUE;
    scd1.SampleDesc  = {1,0};
    scd1.BufferUsage = DXGI_USAGE_RTO;
    scd1.BufferCount = 2;
    scd1.Scaling     = DSCALE_NONE;
    scd1.SwapEffect  = DSE_FLIP_DISCARD;  // FLIP_DISCARD required for stereo
    scd1.AlphaMode   = DALPHA_UNSPEC;
    scd1.Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    // IDXGIFactory2::CreateSwapChainForHwnd (vtable 15)
    // Args: pDevice, hWnd, pDesc, pFullscreenDesc(nullptr=windowed), pOutput(nullptr), **ppSwapChain
    hr=vcall<15>(pFact2,s_pD3DDev,(UINT_PTR)hwnd,&scd1,(void*)nullptr,(void*)nullptr,&s_pDXGISC);
    log("  CreateSwapChainForHwnd(Stereo=TRUE): hr=0x%x (%s)  sc=%p",
        (unsigned)hr,SUCCEEDED(hr)?"OK":"FAILED",(void*)s_pDXGISC);

    if(FAILED(hr)||!s_pDXGISC){
        // Fallback: try without FLIP_DISCARD (some drivers require DISCARD for stereo)
        log("  Retrying with SwapEffect=DISCARD...");
        scd1.SwapEffect=DSE_DISCARD;
        hr=vcall<15>(pFact2,s_pD3DDev,(UINT_PTR)hwnd,&scd1,(void*)nullptr,(void*)nullptr,&s_pDXGISC);
        log("  Retry hr=0x%x (%s)  sc=%p",(unsigned)hr,SUCCEEDED(hr)?"OK":"FAILED",(void*)s_pDXGISC);
    }

    pFact2->Release();
    return SUCCEEDED(hr)&&s_pDXGISC;
}

// FLIP model swap chains (required for DXGI stereo) do NOT support
// SetFullscreenState – it returns E_FAIL.  Instead we rely on the OS
// "fullscreen optimisation" path: a WS_POPUP window covering the full
// display resolution is automatically promoted to exclusive-like fullscreen
// by DXGI without an explicit FSE transition.
// We just build the RTVs directly on the swap chain back buffer.
static bool d3d11BuildRTVsWindowed(){
    log("--- Building RTVs (borderless fullscreen – no SetFullscreenState) ---");
    log("  (FLIP stereo swap chains cannot use SetFullscreenState; WS_POPUP");
    log("   at full resolution is promoted to fullscreen optimisation path)");
    return d3d11BuildSliceRTVs();
}

// ============================================================================
// NVAPI (monitoring only – not used for per-eye rendering)
// ============================================================================
#define NVAPI_OK 0
typedef void*(*PfnNvQI)(unsigned);
typedef int(*PfnNvV)();
typedef int(*PfnNvFromIUnk)(IUnknown*,void**);
typedef int(*PfnNvDH)(void*);

static HMODULE  s_hNvapi =nullptr;
static PfnNvQI  s_nvQI   =nullptr;
static void    *s_hStereo=nullptr;

static void *nvQ(unsigned id){return s_nvQI?s_nvQI(id):nullptr;}

#define NvID_Initialize    0x0150E828u
#define NvID_Unload        0xD22BDD7Eu
#define NvID_Stereo_Enable 0x239C4545u
#define NvID_Stereo_Create 0xAC7E37F4u
#define NvID_Stereo_Destroy 0x3A153134u
#define NvID_Stereo_Activate  0xF6A1AD68u
#define NvID_Stereo_IsActive  0x1FB0BC30u
#define NvID_Stereo_SetSep    0x5C069FA3u
#define NvID_Stereo_GetSep    0xCE5729DFu

static bool nvapiInit(){
    log("--- NVAPI init ---");
    s_hNvapi=LoadLibraryA("nvapi64.dll");
    if(!s_hNvapi) return false;
    s_nvQI=(PfnNvQI)GetProcAddress(s_hNvapi,"nvapi_QueryInterface");
    if(!s_nvQI) return false;
    {auto fn=(PfnNvV)nvQ(NvID_Initialize); if(!fn||fn()) return false;}
    // Enable: hints the driver to treat this process as a stereo app
    {auto fn=(PfnNvV)nvQ(NvID_Stereo_Enable); if(fn) fn();}
    log("  NVAPI OK  (Stereo_Enable called)");
    return true;
}

static bool nvapiCreateHandle(){
    if(!s_pD3DDev) return false;
    auto fn=(PfnNvFromIUnk)nvQ(NvID_Stereo_Create);
    if(!fn) return false;
    int r=fn(s_pD3DDev,&s_hStereo);
    log("  NvAPI_Stereo_CreateHandleFromIUnknown = %d  handle=%p",r,(void*)s_hStereo);
    if(r!=NVAPI_OK){s_hStereo=nullptr; return false;}
    return true;
}

static void nvapiActivate(){
    if(!s_hStereo) return;
    {auto fn=(PfnNvDH)nvQ(NvID_Stereo_Activate); if(fn){int r=fn(s_hStereo); log("  NvAPI_Stereo_Activate = %d",r);}}
    {typedef int(*Pfn)(void*,float); auto fn=(Pfn)nvQ(NvID_Stereo_SetSep); if(fn) fn(s_hStereo,50.f);}
}

static void nvapiShutdown(){
    if(s_hStereo){auto f=(PfnNvDH)nvQ(NvID_Stereo_Destroy);if(f)f(s_hStereo);s_hStereo=nullptr;}
    if(s_hNvapi){auto f=(PfnNvV)nvQ(NvID_Unload);if(f)f();FreeLibrary(s_hNvapi);s_hNvapi=nullptr;}
}

// ============================================================================
// Stereo render loop
// ============================================================================
static void runStereoLoop(){
    log("--- DXGI stereo render loop ---");
    log("  slice 0 = left  eye = CYAN");
    log("  slice 1 = right eye = RED");
    log("  ESC to quit, Alt+Tab/click to restore");

    typedef int(*PfnIA)(void*,unsigned char*); auto fnIA=(PfnIA)nvQ(NvID_Stereo_IsActive);
    typedef int(*PfnGS)(void*,float*);          auto fnGS=(PfnGS)nvQ(NvID_Stereo_GetSep);
    typedef int(*PfnAct)(void*);                auto fnAct=(PfnAct)nvQ(NvID_Stereo_Activate);

    bool wasActivated=false;
    uint32_t frame=0;
    MSG msg{};

    while(!s_quit){
        while(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE) s_quit=true;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        if(s_quit) break;

        if(s_restore){
            s_restore=false;
            log("  frame=%-6u  Rebuilding RTVs after focus restore...",frame);
            safeRelease(s_pEyeRTV[0]); safeRelease(s_pEyeRTV[1]); safeRelease(s_pBBTex);
            d3d11BuildSliceRTVs();
            nvapiActivate();
            SetForegroundWindow(s_hwnd); BringWindowToTop(s_hwnd);
            log("  Restore complete");
        }
        if(!s_pEyeRTV[0]||!s_pEyeRTV[1]){Sleep(16);continue;}

        // Left eye  (slice 0) = CYAN
        float cyan[4]={0.f,1.f,1.f,1.f};
        vcall<33,void>(s_pD3DCtx,(UINT)1,&s_pEyeRTV[0],(void*)nullptr);
        vcall<50,void>(s_pD3DCtx,s_pEyeRTV[0],cyan);

        // Right eye (slice 1) = RED
        float red[4]={1.f,0.f,0.f,1.f};
        vcall<33,void>(s_pD3DCtx,(UINT)1,&s_pEyeRTV[1],(void*)nullptr);
        vcall<50,void>(s_pD3DCtx,s_pEyeRTV[1],red);

        HRESULT hr=dxgiPresent(1);
        ++frame;

        unsigned char active=0;
        if(fnIA) fnIA(s_hStereo,&active);
        if(active&&!wasActivated){wasActivated=true; log("  frame=%-6u  *** IsActivated 0→1 ***",frame);}
        if(!active&&wasActivated){
            wasActivated=false;
            log("  frame=%-6u  IsActivated 1→0 – re-activating",frame);
            if(fnAct) fnAct(s_hStereo);
        }
        if(frame==1||frame%120==0){
            float sep=0.f; if(fnGS) fnGS(s_hStereo,&sep);
            log("  frame=%-6u  present=0x%x  activated=%d  sep=%.1f%%",
                frame,(unsigned)hr,(int)active,sep);
        }
    }
    log("--- stereo loop exited after %u frames ---",frame);
}

// ============================================================================
// Vulkan non-stereo (fallback when DXGI stereo swap chain unavailable)
// ============================================================================
template<class T>static T vkclamp(T v,T lo,T hi){return v<lo?lo:v>hi?hi:v;}

struct VkApp {
    VkInstance       inst =VK_NULL_HANDLE; VkSurfaceKHR surf=VK_NULL_HANDLE;
    VkPhysicalDevice phys =VK_NULL_HANDLE; VkDevice     dev =VK_NULL_HANDLE;
    uint32_t qf=~0u; VkQueue q=VK_NULL_HANDLE;
    VkSwapchainKHR sc=VK_NULL_HANDLE; VkFormat fmt=VK_FORMAT_UNDEFINED;
    VkExtent2D ext{}; uint32_t cnt=0;
    std::vector<VkImage> imgs; std::vector<VkImageView> views;
    std::vector<VkFramebuffer> fbs;
    VkRenderPass rp=VK_NULL_HANDLE; VkCommandPool pool=VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cbs;
    static const uint32_t MF=2;
    VkSemaphore imgRdy[MF]{},renDone[MF]{}; VkFence inFlt[MF]{};
    uint32_t fi=0; bool needRebuild=false;

    void initBase(HINSTANCE hInst,HWND hwnd){
        log("=== Vulkan non-stereo init ===");
        const char *ie[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo=&ai; ici.enabledExtensionCount=2; ici.ppEnabledExtensionNames=ie;
        vkCreateInstance(&ici,nullptr,&inst);
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance=hInst; sci.hwnd=hwnd; vkCreateWin32SurfaceKHR(inst,&sci,nullptr,&surf);
        uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,nullptr);
        std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(inst,&n,devs.data());
        for(auto pd:devs){
            VkPhysicalDeviceProperties pr{}; vkGetPhysicalDeviceProperties(pd,&pr);
            log("  GPU: %s",pr.deviceName);
            uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,nullptr);
            std::vector<VkQueueFamilyProperties> qp(qn); vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qp.data());
            for(uint32_t i=0;i<qn;++i){
                VkBool32 ps=VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(pd,i,surf,&ps);
                if(!(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)||!ps) continue;
                phys=pd; qf=i; break;
            }
            if(phys) break;
        }
        if(!phys) throw std::runtime_error("no GPU");
        float p=1.f; VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        dq.queueFamilyIndex=qf; dq.queueCount=1; dq.pQueuePriorities=&p;
        const char *de=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&dq; dc.enabledExtensionCount=1; dc.ppEnabledExtensionNames=&de;
        vkCreateDevice(phys,&dc,nullptr,&dev); vkGetDeviceQueue(dev,qf,0,&q);
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex=qf; cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(dev,&cpi,nullptr,&pool);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fi2{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi2.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        for(uint32_t i=0;i<MF;++i){
            vkCreateSemaphore(dev,&si,nullptr,&imgRdy[i]); vkCreateSemaphore(dev,&si,nullptr,&renDone[i]);
            vkCreateFence(dev,&fi2,nullptr,&inFlt[i]);
        }
    }
    void buildSwapchain(){
        if(dev) vkDeviceWaitIdle(dev);
        for(auto &fb:fbs) if(fb){vkDestroyFramebuffer(dev,fb,nullptr);fb=VK_NULL_HANDLE;}
        for(auto &iv:views) if(iv){vkDestroyImageView(dev,iv,nullptr);iv=VK_NULL_HANDLE;}
        if(rp){vkDestroyRenderPass(dev,rp,nullptr);rp=VK_NULL_HANDLE;}
        fbs.clear(); views.clear(); imgs.clear();
        if(!cbs.empty()){vkFreeCommandBuffers(dev,pool,(uint32_t)cbs.size(),cbs.data());cbs.clear();}
        VkSwapchainKHR old=sc; sc=VK_NULL_HANDLE;
        VkSurfaceCapabilitiesKHR caps{}; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys,surf,&caps);
        uint32_t fn=0; vkGetPhysicalDeviceSurfaceFormatsKHR(phys,surf,&fn,nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fn); vkGetPhysicalDeviceSurfaceFormatsKHR(phys,surf,&fn,fmts.data());
        VkSurfaceFormatKHR sf=fmts[0];
        for(auto &f:fmts) if(f.format==VK_FORMAT_B8G8R8A8_UNORM&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR){sf=f;break;}
        fmt=sf.format;
        ext=(caps.currentExtent.width!=~0u)?caps.currentExtent:VkExtent2D{(uint32_t)s_W,(uint32_t)s_H};
        uint32_t ic=caps.minImageCount+1; if(caps.maxImageCount&&ic>caps.maxImageCount)ic=caps.maxImageCount;
        VkSwapchainCreateInfoKHR scc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        scc.surface=surf;scc.minImageCount=ic;scc.imageFormat=fmt;scc.imageColorSpace=sf.colorSpace;
        scc.imageExtent=ext;scc.imageArrayLayers=1;scc.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        scc.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
        scc.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        scc.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        scc.presentMode=VK_PRESENT_MODE_FIFO_KHR;scc.clipped=VK_TRUE;scc.oldSwapchain=old;
        vkCreateSwapchainKHR(dev,&scc,nullptr,&sc);
        if(old) vkDestroySwapchainKHR(dev,old,nullptr);
        vkGetSwapchainImagesKHR(dev,sc,&cnt,nullptr); imgs.resize(cnt); vkGetSwapchainImagesKHR(dev,sc,&cnt,imgs.data());
        views.resize(cnt,VK_NULL_HANDLE);
        for(uint32_t i=0;i<cnt;++i){
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image=imgs[i];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=fmt;
            vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; vkCreateImageView(dev,&vi,nullptr,&views[i]);
        }
        VkAttachmentDescription att{};att.format=fmt;att.samples=VK_SAMPLE_COUNT_1_BIT;
        att.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;att.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        VkSubpassDependency dep{};dep.srcSubpass=VK_SUBPASS_EXTERNAL;dep.dstSubpass=0;
        dep.srcStageMask=dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rci.attachmentCount=1;rci.pAttachments=&att;rci.subpassCount=1;rci.pSubpasses=&sub;
        rci.dependencyCount=1;rci.pDependencies=&dep; vkCreateRenderPass(dev,&rci,nullptr,&rp);
        fbs.resize(cnt,VK_NULL_HANDLE);
        for(uint32_t i=0;i<cnt;++i){
            VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbi.renderPass=rp;fbi.attachmentCount=1;fbi.pAttachments=&views[i];
            fbi.width=ext.width;fbi.height=ext.height;fbi.layers=1; vkCreateFramebuffer(dev,&fbi,nullptr,&fbs[i]);
        }
        cbs.resize(cnt); VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cba.commandPool=pool;cba.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cba.commandBufferCount=cnt;
        vkAllocateCommandBuffers(dev,&cba,cbs.data()); needRebuild=false;
        log("  Swapchain: %ux%u  %u images",ext.width,ext.height,cnt);
    }
    bool drawFrame(){
        if(needRebuild) buildSwapchain();
        vkWaitForFences(dev,1,&inFlt[fi],VK_TRUE,UINT64_MAX);
        uint32_t idx=0;
        VkResult r=vkAcquireNextImageKHR(dev,sc,UINT64_MAX,imgRdy[fi],VK_NULL_HANDLE,&idx);
        if(r==VK_ERROR_OUT_OF_DATE_KHR||r==VK_SUBOPTIMAL_KHR){needRebuild=true;return true;}
        if(r!=VK_SUCCESS) return false;
        vkResetFences(dev,1,&inFlt[fi]); vkResetCommandBuffer(cbs[idx],0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cbs[idx],&bi);
        VkClearValue cv{};cv.color.float32[1]=1;cv.color.float32[2]=1;cv.color.float32[3]=1;
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass=rp;rpbi.framebuffer=fbs[idx];rpbi.renderArea={{0,0},ext};
        rpbi.clearValueCount=1;rpbi.pClearValues=&cv;
        vkCmdBeginRenderPass(cbs[idx],&rpbi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cbs[idx]); vkEndCommandBuffer(cbs[idx]);
        VkPipelineStageFlags ws=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo subi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        subi.waitSemaphoreCount=1;subi.pWaitSemaphores=&imgRdy[fi];subi.pWaitDstStageMask=&ws;
        subi.commandBufferCount=1;subi.pCommandBuffers=&cbs[idx];
        subi.signalSemaphoreCount=1;subi.pSignalSemaphores=&renDone[fi];
        vkQueueSubmit(q,1,&subi,inFlt[fi]);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&renDone[fi];
        pi.swapchainCount=1;pi.pSwapchains=&sc;pi.pImageIndices=&idx;
        r=vkQueuePresentKHR(q,&pi);
        if(r==VK_ERROR_OUT_OF_DATE_KHR||r==VK_SUBOPTIMAL_KHR) needRebuild=true;
        fi=(fi+1)%MF; return true;
    }
    void cleanup(){
        if(dev) vkDeviceWaitIdle(dev);
        for(uint32_t i=0;i<MF;++i){
            if(imgRdy[i])  vkDestroySemaphore(dev,imgRdy[i],nullptr);
            if(renDone[i]) vkDestroySemaphore(dev,renDone[i],nullptr);
            if(inFlt[i])   vkDestroyFence(dev,inFlt[i],nullptr);
        }
        if(!cbs.empty()) vkFreeCommandBuffers(dev,pool,(uint32_t)cbs.size(),cbs.data());
        if(pool) vkDestroyCommandPool(dev,pool,nullptr);
        for(auto &fb:fbs)   if(fb) vkDestroyFramebuffer(dev,fb,nullptr);
        for(auto &iv:views) if(iv) vkDestroyImageView(dev,iv,nullptr);
        if(rp)   vkDestroyRenderPass(dev,rp,nullptr);
        if(sc)   vkDestroySwapchainKHR(dev,sc,nullptr);
        if(dev)  vkDestroyDevice(dev,nullptr);
        if(surf) vkDestroySurfaceKHR(inst,surf,nullptr);
        if(inst) vkDestroyInstance(inst,nullptr);
    }
};

// ============================================================================
// Entry point
// ============================================================================
int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int){
    AllocConsole(); FILE *f=nullptr; freopen_s(&f,"CONOUT$","w",stdout);
    logInit();
    log("=== VkStereo3DVision v24 startup ===");
    DEVMODEA dm{}; dm.dmSize=sizeof(dm);
    if(EnumDisplaySettingsA(nullptr,ENUM_CURRENT_SETTINGS,&dm))
        log("  Display: %ux%u @ %u Hz",dm.dmPelsWidth,dm.dmPelsHeight,dm.dmDisplayFrequency);

    s_W=GetSystemMetrics(SM_CXSCREEN); s_H=GetSystemMetrics(SM_CYSCREEN);
    log("  Monitor: %dx%d",s_W,s_H);
    s_hwnd=createWindow(hInst,s_W,s_H);
    log("  Window: WS_POPUP %dx%d",s_W,s_H);

    // NVAPI: Enable stereo flag before D3D11 (may influence DXGI stereo availability)
    nvapiInit();

    // D3D11 device (no swap chain — DXGI factory path creates the swap chain)
    bool d3dOk=d3d11CreateDevice();

    // DXGI 1.2 stereo swap chain (Stereo=TRUE → Texture2DArray[2] back buffer)
    bool scOk=d3dOk && dxgiCreateStereoSwapChain(s_hwnd);

    // NVAPI handle for monitoring
    if(scOk) nvapiCreateHandle();

    // Build RTVs directly (no FSE – FLIP stereo doesn't support SetFullscreenState)
    bool rtvOk=scOk && d3d11BuildRTVsWindowed();
    bool stereoOk=rtvOk && s_pEyeRTV[0] && s_pEyeRTV[1];

    log("  d3d=%s  sc=%s  rtv=%s  stereo=%s",
        d3dOk?"OK":"FAIL", scOk?"OK":"FAIL", rtvOk?"OK":"FAIL", stereoOk?"YES":"NO");

    if(stereoOk){
        nvapiActivate();
        s_restore=false;
        runStereoLoop();
        nvapiShutdown(); d3d11Shutdown();
    } else {
        log("  DXGI stereo swap chain unavailable – falling back to Vulkan (mono)");
        log("  (If 3D Vision is enabled in control panel, run again after enabling it)");
        d3d11Shutdown(); nvapiShutdown();
        VkApp app{};
        try{
            app.initBase(hInst,s_hwnd); app.buildSwapchain();
            uint32_t fc=0; MSG msg{};
            while(!s_quit){
                while(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){
                    if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE) s_quit=true;
                    TranslateMessage(&msg); DispatchMessageA(&msg);
                }
                if(!s_quit&&!app.drawFrame()) break;
                if(++fc==5) log("  (5 Vulkan frames OK)");
            }
            log("--- Vulkan: %u frames ---",fc);
        }catch(const std::exception &e){
            log("EXCEPTION: %s",e.what());
            MessageBoxA(nullptr,e.what(),"Fatal Error",MB_OK|MB_ICONERROR);
        }
        app.cleanup();
    }
    if(s_logFile) fclose(s_logFile);
    return 0;
}
