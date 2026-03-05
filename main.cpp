/*
 * vulkan_stereo_3dvision.cpp  v13
 *
 * Stereoscopic 3D – NVIDIA 3D Vision, driver 426.06 / 452.06.
 *
 * ── ROOT CAUSE FROM v12 LOG ───────────────────────────────────────────────────
 *
 *  NvAPI_Stereo_SetActiveEye is NEVER found across 1444 frames regardless of
 *  IsActivated state or SetDriverMode value.  It does not exist in this driver's
 *  NVAPI dispatch table.
 *
 *  SetDriverMode(2) succeeds.  Value 2 = NVAPI_STEREO_DRIVER_MODE_DIRECT (not
 *  MANUAL as assumed previously).  In DIRECT mode SetActiveEye is not used.
 *
 *  DIRECT mode requires a stereo companion surface — a Texture2DArray with
 *  ArraySize=2 where slice 0 = left eye and slice 1 = right eye.  Without
 *  calling NvAPI_Stereo_SetSurfaceCreationMode(FORCESTEREO) before swap chain
 *  creation, DXGI allocates a plain mono Texture2D back buffer and the driver
 *  has no stereo surface to write to — both eyes see the same mono buffer.
 *
 * ── v13 FIX ───────────────────────────────────────────────────────────────────
 *
 *  Correct DIRECT-mode stereo sequence:
 *    1. D3D11CreateDevice (NO swap chain yet)
 *    2. NvAPI_Stereo_CreateHandleFromIUnknown(device)
 *    3. NvAPI_Stereo_SetDriverMode(DIRECT=2)
 *    4. NvAPI_Stereo_SetSurfaceCreationMode(handle, FORCESTEREO=1)
 *           → All subsequently created surfaces will be stereo Texture2DArray[2]
 *    5. Create DXGI swap chain via IDXGIFactory::CreateSwapChain
 *           → Back buffer is now Texture2DArray, ArraySize=2
 *    6. IDXGISwapChain::SetFullscreenState(TRUE) + ResizeBuffers
 *    7. NvAPI_Stereo_Activate(handle)
 *    8. GetBuffer → Texture2DArray; create two RTVs:
 *           Left  eye: Dim=TEXTURE2DARRAY, FirstArraySlice=0, ArraySize=1
 *           Right eye: Dim=TEXTURE2DARRAY, FirstArraySlice=1, ArraySize=1
 *    9. Per frame:
 *           ClearRenderTargetView(rtvLeft,  CYAN)
 *           ClearRenderTargetView(rtvRight, RED)
 *           IDXGISwapChain::Present(1, 0)
 *       The driver composites slice0→left-eye buffer, slice1→right-eye buffer
 *       and fires the IR emitter during Present.
 *
 * ── DXGI FACTORY PATH ────────────────────────────────────────────────────────
 *
 *  D3D11CreateDevice gives us an IUnknown (ID3D11Device).
 *  We QI → IDXGIDevice → GetAdapter → GetParent(IDXGIFactory) → CreateSwapChain.
 *  All via vtable calls, no DXGI SDK headers needed.
 *
 *  GUIDs used (not in any included header):
 *    IDXGIDevice:  {54EC77FA-1377-44E6-8C32-88FD5F44C84C}
 *    IDXGIFactory: {7B7166EC-21C7-44AE-B21A-C9AE321AE369}
 *
 * ── BUILD ────────────────────────────────────────────────────────────────────
 *   cl /std:c++17 /W3 /O2 main.cpp /I%VULKAN_SDK%\Include ^
 *      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib
 *
 * ── OUTPUT ───────────────────────────────────────────────────────────────────
 *   Stereo: left=CYAN  right=RED  (3D Vision shutter glasses)
 *   Non-stereo (3DV off): CYAN fullscreen (Vulkan)
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
// Minimal D3D11/DXGI types (no SDK headers)
// ============================================================================
typedef enum { DDTH_HW=1          } D3D_DRIVER_TYPE_;
typedef enum { DFL_11_0=0xb000    } D3D_FEATURE_LEVEL_;
typedef enum { DFMT_B8G8R8A8=87,
               DFMT_UNKNOWN=0     } DXGI_FORMAT_;
typedef enum { DSE_DISCARD=0      } DXGI_SWAP_EFFECT_;
typedef UINT DXGI_USAGE_;
#define DXGI_USAGE_RTO 0x20u

typedef struct { UINT N,D; }                DXGI_RATIONAL_;
typedef struct { UINT W,H; DXGI_RATIONAL_ Refresh;
                 DXGI_FORMAT_ Fmt; UINT SLO,Scale; } DXGI_MODE_DESC_;
typedef struct { UINT Count,Quality; }      DXGI_SAMPLE_DESC_;
typedef struct {
    DXGI_MODE_DESC_    BD;
    DXGI_SAMPLE_DESC_  SD;
    DXGI_USAGE_        BU;
    UINT               BC;
    HWND               OW;
    BOOL               Win;
    DXGI_SWAP_EFFECT_  SE;
    UINT               Fl;
} DXGI_SWAP_CHAIN_DESC_;

// D3D11_RENDER_TARGET_VIEW_DESC
// D3D11_RTV_DIMENSION: TEXTURE2D=4  TEXTURE2DARRAY=5
typedef struct { UINT MipSlice; }                     D3D11_TEX2D_RTV_;
typedef struct { UINT MipSlice; UINT FirstArraySlice;
                 UINT ArraySize; }                    D3D11_TEX2DARRAY_RTV_;
typedef struct { DXGI_FORMAT_ Fmt; UINT Dim;
    union {
        D3D11_TEX2D_RTV_      Tex2D;
        D3D11_TEX2DARRAY_RTV_ Tex2DArray;
    };
    char pad[48];
} D3D11_RTV_DESC_;

typedef struct { FLOAT X,Y,W,H,MinZ,MaxZ; } D3D11_VIEWPORT_;

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

// IDXGISwapChain vtable:
//  IUnknown:0-2  IDXGIObject:3-6  IDXGIDeviceSubObject:7
//  8=Present  9=GetBuffer  10=SetFullscreenState  11=GetFullscreenState
//  12=GetDesc  13=ResizeBuffers
// ID3D11Device vtable:
//  IUnknown:0-2  then device methods from 3
//  3=CreateBuffer  9=CreateRenderTargetView  40=GetImmediateContext
// ID3D11DeviceContext vtable:
//  IUnknown:0-2  ID3D11DeviceChild:3-6  context methods from 7
//  33=OMSetRenderTargets  44=RSSetViewports  50=ClearRenderTargetView
// IDXGIDevice vtable:
//  IUnknown:0-2  IDXGIObject:3-6  7=GetAdapter
// IDXGIAdapter (IDXGIObject):
//  IUnknown:0-2  IDXGIObject:3-6 (GetParent=6)  7=EnumOutputs  8=GetDesc  9=CheckInterfaceSupport
// IDXGIFactory vtable:
//  IUnknown:0-2  IDXGIObject:3-6  7=EnumAdapters  8=MakeWindowAssociation
//  9=GetWindowAssociation  10=CreateSwapChain

static HMODULE   s_hD3D11     = nullptr;
static IUnknown *s_pD3DDev    = nullptr;  // ID3D11Device
static IUnknown *s_pDXGISC    = nullptr;  // IDXGISwapChain
static IUnknown *s_pD3DCtx    = nullptr;  // ID3D11DeviceContext
static IUnknown *s_pBBTex     = nullptr;  // ID3D11Texture2D back buffer
static IUnknown *s_pRTVLeft   = nullptr;  // RTV – slice 0 (left eye)
static IUnknown *s_pRTVRight  = nullptr;  // RTV – slice 1 (right eye)
static bool      s_stereoSC   = false;    // swap chain is stereo Texture2DArray
static int       s_bbW=0, s_bbH=0;

static void safeRelease(IUnknown *&p){if(p){p->Release();p=nullptr;}}

// Create D3D11 device only (no swap chain).
// Swap chain is created separately after NvAPI SetSurfaceCreationMode.
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

// Create DXGI swap chain via IDXGIFactory::CreateSwapChain.
// Must be called AFTER NvAPI_Stereo_SetSurfaceCreationMode(FORCESTEREO) so
// that the back buffer is allocated as a stereo Texture2DArray[2].
static bool d3d11CreateSwapChain(HWND hwnd, int w, int h) {
    log("--- DXGI swap chain creation (via factory) ---");

    // QI: ID3D11Device → IDXGIDevice (vtable 0 = QueryInterface)
    IUnknown *pDXGIDev=nullptr;
    HRESULT hr=vcall<0>(s_pD3DDev,&s_iidDXGIDev,(void**)&pDXGIDev);
    log("  QI IDXGIDevice: hr=0x%x  ptr=%p",(unsigned)hr,(void*)pDXGIDev);
    if(FAILED(hr)||!pDXGIDev) return false;

    // IDXGIDevice::GetAdapter (vtable 7)
    IUnknown *pAdapter=nullptr;
    hr=vcall<7>(pDXGIDev,&pAdapter);
    pDXGIDev->Release();
    log("  GetAdapter: hr=0x%x  ptr=%p",(unsigned)hr,(void*)pAdapter);
    if(FAILED(hr)||!pAdapter) return false;

    // IDXGIAdapter → IDXGIFactory via GetParent (IDXGIObject::GetParent = vtable 6)
    IUnknown *pFactory=nullptr;
    hr=vcall<6>(pAdapter,&s_iidDXGIFact,(void**)&pFactory);
    pAdapter->Release();
    log("  GetParent IDXGIFactory: hr=0x%x  ptr=%p",(unsigned)hr,(void*)pFactory);
    if(FAILED(hr)||!pFactory) return false;

    // IDXGIFactory::CreateSwapChain (vtable 10)
    DXGI_SWAP_CHAIN_DESC_ scd{};
    scd.BD.W=0; scd.BD.H=0; scd.BD.Refresh={120,1}; scd.BD.Fmt=DFMT_B8G8R8A8;
    scd.SD={1,0};
    scd.BU=DXGI_USAGE_RTO; scd.BC=2; scd.OW=hwnd;
    scd.Win=TRUE;   // windowed first; go FSE after
    scd.SE=DSE_DISCARD;
    hr=vcall<10>(pFactory,s_pD3DDev,&scd,&s_pDXGISC);
    pFactory->Release();
    log("  CreateSwapChain: hr=0x%x (%s)  sc=%p",(unsigned)hr,
        SUCCEEDED(hr)?"OK":"FAILED",(void*)s_pDXGISC);
    if(FAILED(hr)||!s_pDXGISC) return false;

    s_bbW=w; s_bbH=h;
    return true;
}

// Create left-eye and right-eye RTVs on the back buffer.
// When SetSurfaceCreationMode(FORCESTEREO) was active, GetBuffer returns a
// Texture2DArray with ArraySize=2.  We create one RTV per slice.
// If the back buffer turns out to be mono (FORCESTEREO not honoured), we fall
// back to a single RTV on both slots and log a warning.
static bool d3d11BuildRTVs() {
    safeRelease(s_pRTVLeft);
    safeRelease(s_pRTVRight);
    safeRelease(s_pBBTex);
    if(!s_pD3DDev||!s_pDXGISC) return false;

    if(!s_pD3DCtx)
        vcall<40,void>(s_pD3DDev,(IUnknown**)&s_pD3DCtx);

    // GetBuffer(0, IID_ID3D11Texture2D)  – SC vtable 9
    HRESULT hr=vcall<9>(s_pDXGISC,(UINT)0,&s_iidTex2D,(void**)&s_pBBTex);
    log("  GetBuffer: hr=0x%x  tex=%p",(unsigned)hr,(void*)s_pBBTex);
    if(FAILED(hr)||!s_pBBTex) return false;

    // Try stereo Texture2DArray RTVs first (slice 0 = left, slice 1 = right).
    // D3D11_RTV_DIMENSION_TEXTURE2DARRAY = 5
    auto makeArrayRTV=[&](UINT slice, IUnknown *&out)->bool {
        D3D11_RTV_DESC_ desc{};
        desc.Fmt=DFMT_B8G8R8A8;
        desc.Dim=5; // TEXTURE2DARRAY
        desc.Tex2DArray={0, slice, 1};
        HRESULT h=vcall<9>(s_pD3DDev,s_pBBTex,&desc,(IUnknown**)&out);
        log("  CreateRTV(slice=%u, Dim=ARRAY): hr=0x%x  rtv=%p",slice,(unsigned)h,(void*)out);
        return SUCCEEDED(h)&&out;
    };

    bool leftOk  = makeArrayRTV(0, s_pRTVLeft);
    bool rightOk = makeArrayRTV(1, s_pRTVRight);

    if(leftOk && rightOk){
        s_stereoSC=true;
        log("  Back buffer is stereo Texture2DArray – per-eye RTVs created");
        return true;
    }

    // Fallback: back buffer is mono – FORCESTEREO not honoured.
    // Both eyes will see the same image.
    log("  WARNING: Array RTVs failed – back buffer appears MONO.");
    log("  SetSurfaceCreationMode(FORCESTEREO) may not have been accepted.");
    safeRelease(s_pRTVLeft);
    safeRelease(s_pRTVRight);
    s_stereoSC=false;

    // Fall back to mono RTV (nullptr desc = full texture, Dim=TEXTURE2D)
    hr=vcall<9>(s_pD3DDev,s_pBBTex,(void*)nullptr,(IUnknown**)&s_pRTVLeft);
    s_pRTVRight=s_pRTVLeft; // same surface for both eyes
    if(s_pRTVRight) s_pRTVRight->AddRef();
    log("  Mono fallback RTV: hr=0x%x  rtv=%p",(unsigned)hr,(void*)s_pRTVLeft);
    return s_pRTVLeft!=nullptr;
}

// Go FSE then rebuild RTVs.
static bool d3d11GoFSE() {
    if(!s_pDXGISC) return false;
    log("--- Going fullscreen exclusive ---");
    safeRelease(s_pRTVLeft); safeRelease(s_pRTVRight); safeRelease(s_pBBTex);

    HRESULT hr=vcall<10>(s_pDXGISC,TRUE,(void*)nullptr);
    log("  SetFullscreenState(TRUE): hr=0x%x (%s)",(unsigned)hr,SUCCEEDED(hr)?"OK":"FAILED");
    if(FAILED(hr)) return false;

    hr=vcall<13>(s_pDXGISC,(UINT)2,(UINT)0,(UINT)0,(int)DFMT_UNKNOWN,(UINT)0);
    log("  ResizeBuffers: hr=0x%x (%s)",(unsigned)hr,SUCCEEDED(hr)?"OK":"FAILED");

    bool ok=d3d11BuildRTVs();
    log("  RTVs: %s  stereoSC=%s",ok?"OK":"FAILED",s_stereoSC?"YES":"NO");
    return ok;
}

static void d3d11Shutdown(){
    if(s_pDXGISC){
        log("  SetFullscreenState(FALSE)...");
        vcall<10>(s_pDXGISC,FALSE,(void*)nullptr);
    }
    // Avoid double-release when both RTVs point to the same mono surface
    if(s_pRTVRight && s_pRTVRight==s_pRTVLeft){s_pRTVRight->Release();s_pRTVRight=nullptr;}
    safeRelease(s_pRTVLeft); safeRelease(s_pRTVRight);
    safeRelease(s_pBBTex); safeRelease(s_pD3DCtx);
    safeRelease(s_pDXGISC); safeRelease(s_pD3DDev);
    if(s_hD3D11){FreeLibrary(s_hD3D11);s_hD3D11=nullptr;}
}

static void clearRTV(IUnknown *rtv, float r,float g,float b){
    if(!s_pD3DCtx||!rtv) return;
    float c[4]={r,g,b,1.f};
    D3D11_VIEWPORT_ vp{0.f,0.f,(float)s_bbW,(float)s_bbH,0.f,1.f};
    vcall<44,void>(s_pD3DCtx,(UINT)1,&vp);
    vcall<33,void>(s_pD3DCtx,(UINT)1,&rtv,(void*)nullptr);
    vcall<50,void>(s_pD3DCtx,rtv,c);
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

#define NvID_Initialize              0x0150E828u
#define NvID_Unload                  0xD22BDD7Eu
#define NvID_Stereo_Enable           0x239C4545u
#define NvID_Stereo_SetMode          0x5E8F0BECu  // 0=AUTO 2=DIRECT
#define NvID_Stereo_CreateFromIUnk   0xAC7E37F4u
#define NvID_Stereo_DestroyHandle    0x3A153134u
#define NvID_Stereo_Activate         0xF6A1AD68u
#define NvID_Stereo_IsActivated      0x1FB0BC30u  // (handle, *NvU8)
#define NvID_Stereo_SetSeparation    0x5C069FA3u  // (handle, float)
#define NvID_Stereo_GetSeparation    0xCE5729DFu  // (handle, *float)
// NvAPI_Stereo_SetSurfaceCreationMode – called before any surface creation
// to force stereo Texture2DArray back buffer.
// NVAPI_STEREO_SURFACECREATEMODE: AUTO=0  FORCESTEREO=1  FORCEMONO=2
#define NvID_Stereo_SetSurfaceCreationMode 0xF5DCFDE8u
#define NvID_Stereo_GetSurfaceCreationMode 0x36F1C736u

static bool nvapiPhase1(IUnknown *pDev) {
    log("--- NVAPI init phase 1 ---");
    s_hNvapi=LoadLibraryA("nvapi64.dll");
    if(!s_hNvapi){log("  nvapi64.dll: not found"); return false;}
    s_nvQI=(PfnNvQI)GetProcAddress(s_hNvapi,"nvapi_QueryInterface");
    if(!s_nvQI){log("  nvapi_QueryInterface: not found"); return false;}
    log("  nvapi64.dll OK");

    {auto fn=(PfnNvV)nvQ(NvID_Initialize);
     if(!fn){log("  NvAPI_Initialize fn: not found"); return false;}
     int r=fn(); log("  NvAPI_Initialize = %d",r); if(r)return false;}

    {auto fn=(PfnNvV)nvQ(NvID_Stereo_Enable);
     if(fn){int r=fn(); log("  NvAPI_Stereo_Enable = %d",r);}}

    // Create handle from device
    {auto fn=(PfnNvFromIUnk)nvQ(NvID_Stereo_CreateFromIUnk);
     if(fn&&pDev){
         int r=fn(pDev,&s_hStereo);
         log("  NvAPI_Stereo_CreateHandleFromIUnknown = %d (%s)  handle=%p",
             r,r==NVAPI_OK?"OK":"FAILED",(void*)s_hStereo);
         if(r!=NVAPI_OK) s_hStereo=nullptr;
     }}
    if(!s_hStereo){log("  No stereo handle – aborting"); return false;}

    // SetDriverMode DIRECT=2 (mode 1 returns -5, mode 2 returns 0 per v12 log)
    {typedef int(*Pfn)(unsigned); auto fn=(Pfn)nvQ(NvID_Stereo_SetMode);
     if(fn){int r=fn(2); log("  NvAPI_Stereo_SetDriverMode(DIRECT=2) = %d",r);}}

    // SetSurfaceCreationMode(FORCESTEREO=1) – CRITICAL.
    // Must be called BEFORE swap chain creation so the back buffer is
    // allocated as a stereo Texture2DArray[2] instead of a mono Texture2D.
    {typedef int(*Pfn)(void*,unsigned); auto fn=(Pfn)nvQ(NvID_Stereo_SetSurfaceCreationMode);
     if(fn){
         int r=fn(s_hStereo,1);
         log("  NvAPI_Stereo_SetSurfaceCreationMode(FORCESTEREO=1) = %d (%s)",
             r,r==NVAPI_OK?"OK":"FAILED/fn-absent");
     } else {
         log("  NvAPI_Stereo_SetSurfaceCreationMode: fn NOT FOUND in dispatch table");
         log("  Back buffer will be MONO – per-eye rendering will not work");
     }}

    // Query current mode for confirmation
    {typedef int(*Pfn)(void*,unsigned*); auto fn=(Pfn)nvQ(NvID_Stereo_GetSurfaceCreationMode);
     if(fn){unsigned mode=99; fn(s_hStereo,&mode);
            log("  NvAPI_Stereo_GetSurfaceCreationMode = %u (1=FORCESTEREO)",mode);}}

    log("  Phase 1 complete  handle=%p",(void*)s_hStereo);
    return true;
}

static void nvapiPhase2() {
    if(!s_hStereo) return;
    log("--- NVAPI init phase 2 (post-FSE) ---");

    {typedef int(*Pfn)(void*); auto fn=(Pfn)nvQ(NvID_Stereo_Activate);
     if(fn){int r=fn(s_hStereo); log("  NvAPI_Stereo_Activate = %d",r);}}

    {typedef int(*Pfn)(void*,unsigned char*); auto fn=(Pfn)nvQ(NvID_Stereo_IsActivated);
     if(fn){unsigned char a=0; fn(s_hStereo,&a);
            log("  NvAPI_Stereo_IsActivated: active=%d",(int)a);}}

    {typedef int(*Pfn)(void*,float); auto fn=(Pfn)nvQ(NvID_Stereo_SetSeparation);
     if(fn){fn(s_hStereo,50.f); log("  NvAPI_Stereo_SetSeparation(50%%) done");}}

    log("  Phase 2 complete");
}

static void nvapiShutdown(){
    if(s_hStereo){auto f=(PfnNvDH)nvQ(NvID_Stereo_DestroyHandle);if(f)f(s_hStereo);s_hStereo=nullptr;}
    if(s_hNvapi){auto f=(PfnNvV)nvQ(NvID_Unload);if(f)f();FreeLibrary(s_hNvapi);s_hNvapi=nullptr;}
}

// ============================================================================
// Stereo render loop
// ============================================================================
static void runStereoLoop() {
    log("--- D3D11 DIRECT stereo render loop (ESC to quit) ---");
    log("  stereoSC=%s  rtvLeft=%p  rtvRight=%p",
        s_stereoSC?"YES(Texture2DArray)":"NO(mono fallback)",
        (void*)s_pRTVLeft,(void*)s_pRTVRight);
    if(!s_stereoSC)
        log("  WARNING: no stereo surface – both eyes will show the same image");
    log("  Slice-0 (left)=CYAN   Slice-1 (right)=RED");

    typedef int(*PfnIA)(void*,unsigned char*); auto fnIA=(PfnIA)nvQ(NvID_Stereo_IsActivated);
    typedef int(*PfnGS)(void*,float*);          auto fnGS=(PfnGS)nvQ(NvID_Stereo_GetSeparation);
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

        // Left eye: CYAN → slice 0
        clearRTV(s_pRTVLeft,  0.f,1.f,1.f);
        // Right eye: RED  → slice 1
        clearRTV(s_pRTVRight, 1.f,0.f,0.f);

        HRESULT hr=dxgiPresent(1);
        ++frame;

        // Monitor activation state
        unsigned char active=0;
        if(fnIA) fnIA(s_hStereo,&active);
        if(active&&!wasActivated){
            wasActivated=true;
            log("  frame=%-6u *** IsActivated 0→1 ***",frame);
        }
        if(!active&&wasActivated){
            wasActivated=false;
            log("  frame=%-6u  IsActivated 1→0 – re-activating",frame);
            if(fnAct) fnAct(s_hStereo);
        }

        if(frame==1||frame%120==0){
            float sep=0.f;
            if(fnGS) fnGS(s_hStereo,&sep);
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
    log("=== VkStereo3DVision v13 startup ===");
    logDisplayMode();

    s_hwnd=createFullscreenWindow(hInst);
    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);

    // ── D3D11 device (no swap chain yet) ─────────────────────────────────────
    bool d3dOk=d3d11InitDevice();

    // ── NVAPI phase 1: handle + SetDriverMode(DIRECT) + SetSurfaceCreationMode(FORCESTEREO)
    //    Must happen BEFORE swap chain creation so back buffer is stereo array
    bool nvapiOk = d3dOk && nvapiPhase1(s_pD3DDev);
    bool stereoMode = (s_hStereo != nullptr);
    log("  D3D11=%s  NVAPI=%s  stereoMode=%s",
        d3dOk?"OK":"FAIL", nvapiOk?"OK":"FAIL", stereoMode?"YES":"NO");

    if(stereoMode){
        // ── Create DXGI swap chain after NVAPI setup ─────────────────────────
        bool scOk = d3d11CreateSwapChain(s_hwnd, sw, sh);

        // ── Go FSE + build stereo RTVs ────────────────────────────────────────
        bool fseOk = scOk && d3d11GoFSE();

        // ── NVAPI phase 2: Activate (post-FSE) ───────────────────────────────
        nvapiPhase2();

        if(!s_pRTVLeft){
            log("FATAL: no RTV");
            MessageBoxA(nullptr,"D3D11 RTV setup failed","Error",MB_OK|MB_ICONERROR);
        } else {
            runStereoLoop();
        }
        nvapiShutdown();
        d3d11Shutdown();
    } else {
        d3d11Shutdown();
        nvapiShutdown();
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
