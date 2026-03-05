/*
 * vulkan_stereo_3dvision.cpp  v15
 *
 * Stereoscopic 3D – NVIDIA 3D Vision, driver 426.06 / 452.06.
 *
 * ── ROOT CAUSE FROM v14 LOG ───────────────────────────────────────────────────
 *
 *  Both eyes showed RED.  ClearRenderTargetView IGNORES the viewport/scissor
 *  state — it always clears the ENTIRE render target.  Setting RSSetViewports
 *  to the left half before clearing has no effect.  The right-eye RED clear
 *  therefore overwrote the full double-width back buffer including the left
 *  half every frame.
 *
 * ── v15 FIX ───────────────────────────────────────────────────────────────────
 *
 *  Two separate W×H textures (eyeLeft, eyeRight) with their own RTVs.
 *  ClearRenderTargetView on each fills the full W×H texture with one colour.
 *  CopySubresourceRegion copies each texture into the correct half of the
 *  double-width back buffer:
 *    eyeLeft  (CYAN) → back buffer at DstX=0
 *    eyeRight (RED)  → back buffer at DstX=W
 *  No shaders, no pipeline state, no draw calls.
 *
 * ── AUTO-RESTORE ─────────────────────────────────────────────────────────────
 *
 *  WM_ACTIVATE (wParam != WA_INACTIVE) → request FSE + NVAPI re-activation.
 *  WM_ACTIVATEAPP (wParam == TRUE)     → same.
 *  A restore flag is set in WndProc and acted on at the top of the render loop
 *  (COM/NVAPI calls must happen on the render thread, not the message thread).
 *
 * ── BUILD ────────────────────────────────────────────────────────────────────
 *   cl /std:c++17 /W3 /O2 main.cpp /I%VULKAN_SDK%\Include ^
 *      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib
 *
 * ── OUTPUT ───────────────────────────────────────────────────────────────────
 *   Stereo: left half=CYAN  right half=RED  → 3D Vision composites per eye
 *   Non-stereo: CYAN fullscreen (Vulkan)
 *   Log: vulkan_stereo.log   ESC to quit.
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
static HWND s_hwnd     = nullptr;
static bool s_quit     = false;
static bool s_restore  = false;  // set by WndProc; acted on in render loop

static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_DESTROY:
    case WM_CLOSE:
        s_quit=true; PostQuitMessage(0); return 0;
    case WM_ACTIVATE:
        // WA_INACTIVE=0; any other value means we're being activated
        if(LOWORD(w)!=WA_INACTIVE){ s_restore=true; log("  WM_ACTIVATE → restore requested"); }
        break;
    case WM_ACTIVATEAPP:
        if(w){ s_restore=true; log("  WM_ACTIVATEAPP → restore requested"); }
        break;
    case WM_SETFOCUS:
        s_restore=true;
        break;
    }
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
// Minimal D3D11/DXGI types (no SDK headers)
// ============================================================================
typedef enum { DDTH_HW=1          } D3D_DRIVER_TYPE_;
typedef enum { DFL_11_0=0xb000    } D3D_FEATURE_LEVEL_;
typedef enum { DFMT_B8G8R8A8=87,
               DFMT_UNKNOWN=0     } DXGI_FORMAT_;
typedef enum { DSE_DISCARD=0      } DXGI_SWAP_EFFECT_;
typedef UINT DXGI_USAGE_;
#define DXGI_USAGE_RTO 0x20u

typedef struct { UINT N,D; }              DXGI_RATIONAL_;
typedef struct { UINT W,H; DXGI_RATIONAL_ Refresh;
                 DXGI_FORMAT_ Fmt; UINT SLO,Scale; } DXGI_MODE_DESC_;
typedef struct { UINT Count,Quality; }    DXGI_SAMPLE_DESC_;
typedef struct {
    DXGI_MODE_DESC_   BD;
    DXGI_SAMPLE_DESC_ SD;
    DXGI_USAGE_       BU;
    UINT              BC;
    HWND              OW;
    BOOL              Win;
    DXGI_SWAP_EFFECT_ SE;
    UINT              Fl;
} DXGI_SWAP_CHAIN_DESC_;

// D3D11_TEXTURE2D_DESC (CreateTexture2D = ID3D11Device vtable 5)
// D3D11_USAGE: DEFAULT=0   D3D11_BIND_RENDER_TARGET=0x20
typedef struct {
    UINT Width, Height, MipLevels, ArraySize;
    DXGI_FORMAT_ Format;
    DXGI_SAMPLE_DESC_ SampleDesc;
    UINT Usage, BindFlags, CPUAccessFlags, MiscFlags;
} D3D11_TEX2D_DESC_;

// D3D11_BOX (pass nullptr to CopySubresourceRegion for whole texture)
typedef struct { UINT left,top,front,right,bottom,back; } D3D11_BOX_;

// IID_ID3D11Texture2D
static const GUID s_iidTex2D    = {0x6F15AAF2,0xD208,0x4E89,{0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C}};
// IID_IDXGIDevice  {54EC77FA-1377-44E6-8C32-88FD5F44C84C}
static const GUID s_iidDXGIDev  = {0x54EC77FA,0x1377,0x44E6,{0x8C,0x32,0x88,0xFD,0x5F,0x44,0xC8,0x4C}};
// IID_IDXGIFactory {7B7166EC-21C7-44AE-B21A-C9AE321AE369}
static const GUID s_iidDXGIFact = {0x7B7166EC,0x21C7,0x44AE,{0xB2,0x1A,0xC9,0xAE,0x32,0x1A,0xE3,0x69}};

typedef HRESULT(WINAPI *PfnD3D11CreateDevice)(
    void*,D3D_DRIVER_TYPE_,HMODULE,UINT,
    const D3D_FEATURE_LEVEL_*,UINT,UINT,
    IUnknown**,D3D_FEATURE_LEVEL_*,IUnknown**);

// COM vtable helper
template<int N,typename R=HRESULT,typename...A>
static R vcall(void *o,A...args){
    auto vt=*(void***)o;
    return ((R(__stdcall*)(void*,A...))vt[N])(o,args...);
}

// Vtable layout summary:
//   IUnknown:            0=QI  1=AddRef  2=Release
//   IDXGIObject:         3..6 (GetParent=6)
//   IDXGIDeviceSubObject:7=GetDevice
//   IDXGISwapChain:      8=Present  9=GetBuffer  10=SetFullscreenState
//                        11=GetFullscreenState  12=GetDesc  13=ResizeBuffers
//   IDXGIDevice:         7=GetAdapter
//   IDXGIFactory:        10=CreateSwapChain
//   ID3D11Device:        9=CreateRenderTargetView  40=GetImmediateContext
//   ID3D11DeviceContext: 33=OMSetRenderTargets  44=RSSetViewports
//                        50=ClearRenderTargetView

static HMODULE   s_hD3D11   = nullptr;
static IUnknown *s_pD3DDev  = nullptr;  // ID3D11Device
static IUnknown *s_pDXGISC  = nullptr;  // IDXGISwapChain
static IUnknown *s_pD3DCtx      = nullptr;  // ID3D11DeviceContext
static IUnknown *s_pBBTex       = nullptr;  // ID3D11Texture2D back buffer (double-wide)
static IUnknown *s_pEyeTex[2]   = {};       // W×H per-eye staging textures
static IUnknown *s_pEyeRTV[2]   = {};       // RTVs on the staging textures
static int s_dispW=0, s_dispH=0;            // display resolution (single eye)

static void safeRelease(IUnknown *&p){if(p){p->Release();p=nullptr;}}

static bool d3d11InitDevice() {
    log("--- D3D11 device init ---");
    s_hD3D11=LoadLibraryA("d3d11.dll");
    if(!s_hD3D11){log("  d3d11.dll: not found"); return false;}
    auto fn=(PfnD3D11CreateDevice)GetProcAddress(s_hD3D11,"D3D11CreateDevice");
    if(!fn){log("  D3D11CreateDevice: not found"); return false;}
    D3D_FEATURE_LEVEL_ fl=DFL_11_0, flOut{};
    IUnknown *pCtx=nullptr;
    HRESULT hr=fn(nullptr,DDTH_HW,nullptr,0,&fl,1,7,&s_pD3DDev,&flOut,&pCtx);
    log("  D3D11CreateDevice: hr=0x%x (%s)",(unsigned)hr,SUCCEEDED(hr)?"OK":"FAILED");
    if(pCtx){pCtx->Release();}
    if(FAILED(hr)){s_pD3DDev=nullptr; return false;}
    log("  Device=%p",(void*)s_pD3DDev);
    return true;
}

// Create swap chain with width = 2 * display width (DIRECT stereo layout).
// The driver detects the 2:1 aspect ratio and composites left/right halves
// as left/right eyes during Present.
static bool d3d11CreateSwapChain(HWND hwnd) {
    log("--- DXGI swap chain (double-width for DIRECT stereo) ---");
    log("  back buffer: %dx%d (2x%d display width)", s_dispW*2, s_dispH, s_dispW);

    IUnknown *pDXGIDev=nullptr;
    HRESULT hr=vcall<0>(s_pD3DDev,&s_iidDXGIDev,(void**)&pDXGIDev);
    log("  QI IDXGIDevice: hr=0x%x  ptr=%p",(unsigned)hr,(void*)pDXGIDev);
    if(FAILED(hr)||!pDXGIDev) return false;

    IUnknown *pAdapter=nullptr;
    hr=vcall<7>(pDXGIDev,&pAdapter);
    pDXGIDev->Release();
    log("  GetAdapter: hr=0x%x  ptr=%p",(unsigned)hr,(void*)pAdapter);
    if(FAILED(hr)||!pAdapter) return false;

    IUnknown *pFactory=nullptr;
    hr=vcall<6>(pAdapter,&s_iidDXGIFact,(void**)&pFactory);
    pAdapter->Release();
    log("  GetParent IDXGIFactory: hr=0x%x  ptr=%p",(unsigned)hr,(void*)pFactory);
    if(FAILED(hr)||!pFactory) return false;

    DXGI_SWAP_CHAIN_DESC_ scd{};
    scd.BD.W=(UINT)(s_dispW*2); // double-width: left eye | right eye
    scd.BD.H=(UINT)s_dispH;
    scd.BD.Refresh={120,1};
    scd.BD.Fmt=DFMT_B8G8R8A8;
    scd.SD={1,0};
    scd.BU=DXGI_USAGE_RTO;
    scd.BC=2; scd.OW=hwnd;
    scd.Win=TRUE;  // windowed; go FSE after NVAPI setup
    scd.SE=DSE_DISCARD;
    hr=vcall<10>(pFactory,s_pD3DDev,&scd,&s_pDXGISC);
    pFactory->Release();
    log("  CreateSwapChain: hr=0x%x (%s)  sc=%p",(unsigned)hr,
        SUCCEEDED(hr)?"OK":"FAILED",(void*)s_pDXGISC);
    return SUCCEEDED(hr)&&s_pDXGISC;
}

// Build a single RTV covering the full double-width back buffer.
// We draw CYAN to the left viewport and RED to the right viewport
// using two separate ClearRenderTargetView + RSSetViewports calls.
// Build:
//  - s_pBBTex  : back buffer (double-wide W*2 × H)
//  - s_pEyeTex : two W×H staging textures (cleared per-eye each frame)
//  - s_pEyeRTV : one RTV per staging texture
// CopySubresourceRegion (ctx vtable 46) copies each into its half of the BB.
// ID3D11Device vtable: 5=CreateTexture2D  9=CreateRenderTargetView
//                      40=GetImmediateContext
static bool d3d11BuildRTV() {
    safeRelease(s_pEyeRTV[0]); safeRelease(s_pEyeRTV[1]);
    safeRelease(s_pEyeTex[0]); safeRelease(s_pEyeTex[1]);
    safeRelease(s_pBBTex);
    if(!s_pD3DDev||!s_pDXGISC) return false;
    if(!s_pD3DCtx) vcall<40,void>(s_pD3DDev,(IUnknown**)&s_pD3DCtx);

    // Grab the double-wide back buffer (used as copy destination only)
    HRESULT hr=vcall<9>(s_pDXGISC,(UINT)0,&s_iidTex2D,(void**)&s_pBBTex);
    log("  GetBuffer: hr=0x%x  tex=%p",(unsigned)hr,(void*)s_pBBTex);
    if(FAILED(hr)||!s_pBBTex) return false;

    // Create two W×H staging textures and their RTVs
    D3D11_TEX2D_DESC_ td{};
    td.Width=(UINT)s_dispW; td.Height=(UINT)s_dispH;
    td.MipLevels=1; td.ArraySize=1;
    td.Format=DFMT_B8G8R8A8;
    td.SampleDesc={1,0};
    td.Usage=0;                // D3D11_USAGE_DEFAULT
    td.BindFlags=0x20;         // D3D11_BIND_RENDER_TARGET
    for(int eye=0;eye<2;++eye){
        hr=vcall<5>(s_pD3DDev,&td,(void*)nullptr,(IUnknown**)&s_pEyeTex[eye]);
        log("  CreateTexture2D[%d]: hr=0x%x  tex=%p",eye,(unsigned)hr,(void*)s_pEyeTex[eye]);
        if(FAILED(hr)||!s_pEyeTex[eye]) return false;
        hr=vcall<9>(s_pD3DDev,s_pEyeTex[eye],(void*)nullptr,(IUnknown**)&s_pEyeRTV[eye]);
        log("  CreateRenderTargetView[%d]: hr=0x%x  rtv=%p",eye,(unsigned)hr,(void*)s_pEyeRTV[eye]);
        if(FAILED(hr)||!s_pEyeRTV[eye]) return false;
    }
    return true;
}

// Go FSE, resize buffers (preserves the double-width layout), rebuild RTV.
static bool d3d11GoFSE() {
    log("--- Going fullscreen exclusive ---");
    safeRelease(s_pEyeRTV[0]); safeRelease(s_pEyeRTV[1]);
    safeRelease(s_pEyeTex[0]); safeRelease(s_pEyeTex[1]);
    safeRelease(s_pBBTex);
    HRESULT hr=vcall<10>(s_pDXGISC,TRUE,(void*)nullptr);
    log("  SetFullscreenState(TRUE): hr=0x%x (%s)",(unsigned)hr,SUCCEEDED(hr)?"OK":"FAILED");
    if(FAILED(hr)) return false;
    // Pass explicit double-width; DXGI_FORMAT_UNKNOWN preserves format
    hr=vcall<13>(s_pDXGISC,(UINT)2,(UINT)(s_dispW*2),(UINT)s_dispH,
                 (int)DFMT_UNKNOWN,(UINT)0);
    log("  ResizeBuffers(%dx%d): hr=0x%x (%s)",s_dispW*2,s_dispH,
        (unsigned)hr,SUCCEEDED(hr)?"OK":"FAILED");
    bool ok=d3d11BuildRTV();
    log("  RTV: %s",ok?"OK":"FAILED");
    return ok;
}

static void d3d11Shutdown(){
    if(s_pDXGISC){vcall<10>(s_pDXGISC,FALSE,(void*)nullptr);}
    safeRelease(s_pEyeRTV[0]); safeRelease(s_pEyeRTV[1]);
    safeRelease(s_pEyeTex[0]); safeRelease(s_pEyeTex[1]);
    safeRelease(s_pBBTex);
    safeRelease(s_pD3DCtx); safeRelease(s_pDXGISC); safeRelease(s_pD3DDev);
    if(s_hD3D11){FreeLibrary(s_hD3D11);s_hD3D11=nullptr;}
}

// Render both eyes into the double-wide back buffer:
//   1. ClearRenderTargetView on each W×H staging RTV (clears entire texture)
//   2. CopySubresourceRegion copies each into the correct half of the BB
//
// ID3D11DeviceContext vtable:
//   33=OMSetRenderTargets  50=ClearRenderTargetView
//   46=CopySubresourceRegion
//
// CopySubresourceRegion signature:
//   void Copy(pDst, DstSub, DstX, DstY, DstZ, pSrc, SrcSub, pSrcBox)
//   pSrcBox=nullptr copies the entire source texture.
static void renderEyes() {
    if(!s_pD3DCtx||!s_pEyeRTV[0]||!s_pEyeRTV[1]||!s_pBBTex) return;

    // Left eye: CYAN
    float cyan[4]={0.f,1.f,1.f,1.f};
    vcall<33,void>(s_pD3DCtx,(UINT)1,&s_pEyeRTV[0],(void*)nullptr);
    vcall<50,void>(s_pD3DCtx,s_pEyeRTV[0],cyan);

    // Right eye: RED
    float red[4]={1.f,0.f,0.f,1.f};
    vcall<33,void>(s_pD3DCtx,(UINT)1,&s_pEyeRTV[1],(void*)nullptr);
    vcall<50,void>(s_pD3DCtx,s_pEyeRTV[1],red);

    // Copy left eye into left half of double-wide back buffer (DstX=0)
    vcall<46,void>(s_pD3DCtx,
        s_pBBTex,      (UINT)0, (UINT)0,          (UINT)0, (UINT)0,
        s_pEyeTex[0],  (UINT)0, (void*)nullptr);

    // Copy right eye into right half of double-wide back buffer (DstX=W)
    vcall<46,void>(s_pD3DCtx,
        s_pBBTex,      (UINT)0, (UINT)s_dispW,    (UINT)0, (UINT)0,
        s_pEyeTex[1],  (UINT)0, (void*)nullptr);
}

static HRESULT dxgiPresent(UINT sync){
    return s_pDXGISC?vcall<8>(s_pDXGISC,sync,(UINT)0):E_FAIL;
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
#define NvID_Stereo_SetMode       0x5E8F0BECu   // 0=AUTO 2=DIRECT
#define NvID_Stereo_CreateFromIUnk 0xAC7E37F4u
#define NvID_Stereo_DestroyHandle 0x3A153134u
#define NvID_Stereo_Activate      0xF6A1AD68u
#define NvID_Stereo_IsActivated   0x1FB0BC30u   // (handle, *NvU8)
#define NvID_Stereo_SetSeparation 0x5C069FA3u
#define NvID_Stereo_GetSeparation 0xCE5729DFu

static bool nvapiInit(IUnknown *pDev) {
    log("--- NVAPI init ---");
    s_hNvapi=LoadLibraryA("nvapi64.dll");
    if(!s_hNvapi){log("  nvapi64.dll: not found"); return false;}
    s_nvQI=(PfnNvQI)GetProcAddress(s_hNvapi,"nvapi_QueryInterface");
    if(!s_nvQI){log("  nvapi_QueryInterface: not found"); return false;}
    log("  nvapi64.dll OK");

    {auto fn=(PfnNvV)nvQ(NvID_Initialize);
     if(!fn){log("  NvAPI_Initialize: not found"); return false;}
     int r=fn(); log("  NvAPI_Initialize = %d",r); if(r)return false;}

    {auto fn=(PfnNvV)nvQ(NvID_Stereo_Enable);
     if(fn){int r=fn(); log("  NvAPI_Stereo_Enable = %d",r);}}

    {auto fn=(PfnNvFromIUnk)nvQ(NvID_Stereo_CreateFromIUnk);
     if(fn&&pDev){
         int r=fn(pDev,&s_hStereo);
         log("  NvAPI_Stereo_CreateHandleFromIUnknown = %d (%s)  handle=%p",
             r,r==NVAPI_OK?"OK":"FAILED",(void*)s_hStereo);
         if(r!=NVAPI_OK) s_hStereo=nullptr;
     }}
    if(!s_hStereo){log("  No stereo handle"); return false;}

    // DIRECT mode (value 2 confirmed working in v12/v13)
    {typedef int(*Pfn)(unsigned); auto fn=(Pfn)nvQ(NvID_Stereo_SetMode);
     if(fn){int r=fn(2); log("  NvAPI_Stereo_SetDriverMode(DIRECT=2) = %d",r);}}

    log("  NVAPI init done  handle=%p",(void*)s_hStereo);
    return true;
}

// Activate stereo – call after FSE is acquired (or re-acquired).
static void nvapiActivate() {
    if(!s_hStereo) return;
    typedef int(*Pfn)(void*); auto fn=(Pfn)nvQ(NvID_Stereo_Activate);
    if(fn){int r=fn(s_hStereo); log("  NvAPI_Stereo_Activate = %d",r);}
    typedef int(*PfnSep)(void*,float); auto fnS=(PfnSep)nvQ(NvID_Stereo_SetSeparation);
    if(fnS) fnS(s_hStereo,50.f);
}

static void nvapiShutdown(){
    if(s_hStereo){auto f=(PfnNvDH)nvQ(NvID_Stereo_DestroyHandle);if(f)f(s_hStereo);s_hStereo=nullptr;}
    if(s_hNvapi){auto f=(PfnNvV)nvQ(NvID_Unload);if(f)f();FreeLibrary(s_hNvapi);s_hNvapi=nullptr;}
}

// ============================================================================
// Stereo render + restore loop
// ============================================================================
static void runStereoLoop() {
    log("--- D3D11 DIRECT stereo render loop (ESC to quit) ---");
    log("  Back buffer: %dx%d  Left=[0..%d] CYAN  Right=[%d..%d] RED",
        s_dispW*2, s_dispH, s_dispW-1, s_dispW, s_dispW*2-1);
    log("  Alt+Tab / click window to restore FSE+3DV");

    typedef int(*PfnIA)(void*,unsigned char*); auto fnIA=(PfnIA)nvQ(NvID_Stereo_IsActivated);
    typedef int(*PfnGS)(void*,float*);          auto fnGS=(PfnGS)nvQ(NvID_Stereo_GetSeparation);
    typedef int(*PfnAct)(void*);                auto fnAct=(PfnAct)nvQ(NvID_Stereo_Activate);

    bool wasActivated=false;
    uint32_t frame=0;
    MSG msg{};

    while(!s_quit){
        // ── Message pump ─────────────────────────────────────────────────────
        while(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE) s_quit=true;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        if(s_quit) break;

        // ── FSE / stereo restore on focus regain ─────────────────────────────
        if(s_restore){
            s_restore=false;
            log("  frame=%-6u  Restoring FSE + 3DV...",frame);
            // Release back-buffer refs before FSE transition
            safeRelease(s_pEyeRTV[0]); safeRelease(s_pEyeRTV[1]);
            safeRelease(s_pEyeTex[0]); safeRelease(s_pEyeTex[1]);
            safeRelease(s_pBBTex);
            // Re-assert FSE
            HRESULT hr=vcall<10>(s_pDXGISC,TRUE,(void*)nullptr);
            log("  SetFullscreenState(TRUE): hr=0x%x",unsigned(hr));
            // Resize to double-width (no-op if already correct size)
            vcall<13>(s_pDXGISC,(UINT)2,(UINT)(s_dispW*2),(UINT)s_dispH,
                      (int)DFMT_UNKNOWN,(UINT)0);
            // Rebuild RTV on new back buffer
            if(!d3d11BuildRTV())
                log("  WARNING: RTV rebuild failed after restore");
            // Re-activate stereo
            nvapiActivate();
            SetForegroundWindow(s_hwnd); BringWindowToTop(s_hwnd);
            log("  Restore complete");
        }

        // ── Render: clear eye textures and copy into double-wide back buffer ───
        renderEyes();

        // ── Present ───────────────────────────────────────────────────────────
        HRESULT hr=dxgiPresent(1);

        ++frame;

        // ── Activation monitor: re-activate if stereo drops ──────────────────
        unsigned char active=0;
        if(fnIA) fnIA(s_hStereo,&active);
        if(active && !wasActivated){
            wasActivated=true;
            log("  frame=%-6u  *** IsActivated 0→1 (3DV live) ***",frame);
        }
        if(!active && wasActivated){
            wasActivated=false;
            log("  frame=%-6u  IsActivated 1→0 – re-activating",frame);
            if(fnAct) fnAct(s_hStereo);
        }

        // ── Periodic diagnostic ───────────────────────────────────────────────
        if(frame==1||frame%120==0){
            float sep=0.f; if(fnGS) fnGS(s_hStereo,&sep);
            log("  frame=%-6u  present=0x%x  activated=%d  sep=%.1f%%",
                frame,(unsigned)hr,(int)active,sep);
        }
    }
    log("--- stereo loop exited after %u frames ---",frame);
}

// ============================================================================
// Vulkan non-stereo path
// ============================================================================
static const char *vkResultStr(VkResult r){
    switch(r){
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    default: return "(unknown)";
    }
}
#define VK_CHECK(expr) do{ log("  CALL  " #expr); VkResult _r=(expr); \
    if(_r!=VK_SUCCESS){char _b[512]; snprintf(_b,sizeof(_b),"FATAL %s (%d)\n" #expr, \
    vkResultStr(_r),(int)_r); log(_b); throw std::runtime_error(_b);} \
    log("  OK    " #expr); }while(0)
#define VK_CHECK_SC(expr,out) do{(out)=(expr); \
    if((out)!=VK_SUCCESS&&(out)!=VK_SUBOPTIMAL_KHR&&(out)!=VK_ERROR_OUT_OF_DATE_KHR){ \
    char _b[512]; snprintf(_b,sizeof(_b),"FATAL %s (%d)\n" #expr, \
    vkResultStr((VkResult)(out)),(int)(out)); log(_b); throw std::runtime_error(_b);} \
    if((out)!=VK_SUCCESS)log("  WARN " #expr " -> %s",vkResultStr((VkResult)(out)));} while(0)

template<class T> static T vk_clamp(T v,T lo,T hi){return v<lo?lo:(v>hi?hi:v);}
static bool hasInstExt(const char *n){
    uint32_t c=0; vkEnumerateInstanceExtensionProperties(nullptr,&c,nullptr);
    std::vector<VkExtensionProperties> v(c); vkEnumerateInstanceExtensionProperties(nullptr,&c,v.data());
    for(auto &e:v)if(!strcmp(e.extensionName,n))return true; return false;
}

struct VkApp {
    VkInstance       instance=VK_NULL_HANDLE; VkSurfaceKHR  surface=VK_NULL_HANDLE;
    VkPhysicalDevice physDev=VK_NULL_HANDLE;  VkDevice      device =VK_NULL_HANDLE;
    uint32_t         queueFamily=UINT32_MAX;  VkQueue       queue  =VK_NULL_HANDLE;
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
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance=hInst; sci.hwnd=hwnd;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance,&sci,nullptr,&surface));
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
        float p=1.f;
        VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        dq.queueFamilyIndex=queueFamily;dq.queueCount=1;dq.pQueuePriorities=&p;
        const char *de=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&dq;dc.enabledExtensionCount=1;dc.ppEnabledExtensionNames=&de;
        VK_CHECK(vkCreateDevice(physDev,&dc,nullptr,&device));
        vkGetDeviceQueue(device,queueFamily,0,&queue);
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
        fbs.resize(swapCount,VK_NULL_HANDLE);
        for(uint32_t i=0;i<swapCount;++i){
            VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbi.renderPass=rp;fbi.attachmentCount=1;fbi.pAttachments=&views[i];
            fbi.width=swapExtent.width;fbi.height=swapExtent.height;fbi.layers=1;
            VK_CHECK(vkCreateFramebuffer(device,&fbi,nullptr,&fbs[i]));
        }
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex=queueFamily;cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device,&cpi,nullptr,&pool));
        cbs.resize(swapCount);
        VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cba.commandPool=pool;cba.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cba.commandBufferCount=swapCount;
        VK_CHECK(vkAllocateCommandBuffers(device,&cba,cbs.data()));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fci.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        for(uint32_t i=0;i<MF;++i){
            VK_CHECK(vkCreateSemaphore(device,&si,nullptr,&imgRdy[i]));
            VK_CHECK(vkCreateSemaphore(device,&si,nullptr,&renDone[i]));
            VK_CHECK(vkCreateFence(device,&fci,nullptr,&inFlt[i]));
        }
        log("=== Vulkan init: %ux%u  %u images ===",swapExtent.width,swapExtent.height,swapCount);
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
        VkClearValue cv{};cv.color.float32[1]=1.f;cv.color.float32[2]=1.f;cv.color.float32[3]=1.f;
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
    }
};

// ============================================================================
// Entry point
// ============================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int){
    AllocConsole();
    FILE *f=nullptr; freopen_s(&f,"CONOUT$","w",stdout);
    logInit();
    log("=== VkStereo3DVision v15 startup ===");
    logDisplayMode();

    s_hwnd=createFullscreenWindow(hInst);
    s_dispW=GetSystemMetrics(SM_CXSCREEN);
    s_dispH=GetSystemMetrics(SM_CYSCREEN);

    // D3D11 device (no swap chain yet)
    bool d3dOk=d3d11InitDevice();

    // NVAPI: enable stereo, create handle, set DIRECT mode
    // Must happen before swap chain so driver knows this is a stereo process
    bool nvapiOk = d3dOk && nvapiInit(s_pD3DDev);
    bool stereoMode = (s_hStereo != nullptr);
    log("  D3D11=%s  NVAPI=%s  stereoMode=%s",
        d3dOk?"OK":"FAIL", nvapiOk?"OK":"FAIL", stereoMode?"YES":"NO");

    if(stereoMode){
        // Create double-width swap chain
        bool scOk = d3d11CreateSwapChain(s_hwnd);
        // Go FSE + build RTV
        bool fseOk = scOk && d3d11GoFSE();
        // Activate stereo after FSE
        nvapiActivate();

        if(!s_pEyeRTV[0]||!s_pEyeRTV[1]){
            log("FATAL: no RTV");
            MessageBoxA(nullptr,"D3D11 RTV setup failed","Error",MB_OK|MB_ICONERROR);
        } else {
            // Clear the restore flag that was set during init window messages
            s_restore=false;
            runStereoLoop();
        }
        nvapiShutdown();
        d3d11Shutdown();
    } else {
        d3d11Shutdown(); nvapiShutdown();
        VkApp app{};
        try{
            app.init(hInst,s_hwnd);
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
    if(s_logFile)fclose(s_logFile);
    return 0;
}
