/*
 * vulkan_stereo_3dvision.cpp  v21
 *
 * Stereoscopic 3D – NVIDIA 3D Vision, driver 426.06 / 452.06.
 *
 * ── HISTORY ──────────────────────────────────────────────────────────────────
 *
 *  v20: Combined D3D11CreateDeviceAndSwapChain after Stereo_Enable.
 *       GetSurfaceCreationMode still returned 0 (companion not allocated).
 *       RSBC was no-op.  Both eyes CYAN.
 *
 * ── v21 HYPOTHESES ───────────────────────────────────────────────────────────
 *
 *  Hypothesis A: ResizeBuffers destroys the stereo companion.
 *    The driver allocates the companion during D3D11CreateDeviceAndSwapChain.
 *    ResizeBuffers then recreates the back-buffer textures WITHOUT going through
 *    the stereo hook, stripping the companion.
 *
 *  Hypothesis B: companion is allocated but GetSurfaceCreationMode=0 is
 *    misleading (it reflects the mode SETTING, not the allocation state).
 *
 * ── v21 CHANGES ───────────────────────────────────────────────────────────────
 *
 *  1. NVAPI handle + GetSurfaceCreationMode probe IMMEDIATELY after
 *     D3D11CreateDeviceAndSwapChain, before FSE – to see if companion exists
 *     in windowed mode before ResizeBuffers can touch it.
 *
 *  2. Skip ResizeBuffers.  Going from 1920×1080 windowed to 1920×1080 FSE does
 *     not require ResizeBuffers – the dimensions already match.  This keeps the
 *     original back-buffer allocation intact, preserving any companion.
 *
 *  3. GetSurfaceCreationMode is probed again after SetFullscreenState to verify
 *     FSE transition alone doesn't strip the companion.
 *
 *  4. Render loop unchanged (RSBC order: RED→right staging, RSBC copy, CYAN→BB).
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
    HWND hwnd=CreateWindowExA(0,"VkStereoWnd","3D Vision Stereo  ESC=quit",
        WS_POPUP,0,0,w,h,nullptr,nullptr,hInst,nullptr);
    ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd);
    SetForegroundWindow(hwnd); BringWindowToTop(hwnd);
    MSG msg{}; for(int i=0;i<10;++i)
        while(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageA(&msg);}
    return hwnd;
}

// ============================================================================
// Minimal D3D11 / DXGI types
// ============================================================================
typedef enum { DDTH_HW=1       } D3D_DRIVER_TYPE_;
typedef enum { DFL_11_0=0xb000 } D3D_FEATURE_LEVEL_;
typedef enum { DFMT_B8G8R8A8=87, DFMT_UNKNOWN=0 } DXGI_FORMAT_;
typedef enum { DSE_DISCARD=0   } DXGI_SWAP_EFFECT_;
#define DXGI_USAGE_RTO 0x20u
typedef struct { UINT N,D; } DXGI_RATIONAL_;
typedef struct { UINT W,H; DXGI_RATIONAL_ Refresh; DXGI_FORMAT_ Fmt; UINT SLO,Scale; } DXGI_MODE_DESC_;
typedef struct { UINT Count,Quality; } DXGI_SAMPLE_DESC_;
typedef struct {
    DXGI_MODE_DESC_ BD; DXGI_SAMPLE_DESC_ SD;
    UINT BU,BC; HWND OW; BOOL Win; DXGI_SWAP_EFFECT_ SE; UINT Fl;
} DXGI_SWAP_CHAIN_DESC_;
typedef struct {
    UINT Width,Height,MipLevels,ArraySize;
    DXGI_FORMAT_ Format; DXGI_SAMPLE_DESC_ SampleDesc;
    UINT Usage,BindFlags,CPUAccessFlags,MiscFlags;
} D3D11_TEX2D_DESC_;

static const GUID s_iidTex2D={0x6F15AAF2,0xD208,0x4E89,{0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C}};

typedef HRESULT(WINAPI *PfnDAS)(
    void*,D3D_DRIVER_TYPE_,HMODULE,UINT,
    const D3D_FEATURE_LEVEL_*,UINT,UINT,
    const DXGI_SWAP_CHAIN_DESC_*,
    IUnknown**,IUnknown**,D3D_FEATURE_LEVEL_*,IUnknown**);

template<int N,typename R=HRESULT,typename...A>
static R vcall(void *o,A...args){
    return ((R(__stdcall*)(void*,A...))(*(void***)o)[N])(o,args...);
}
// IDXGISwapChain:      8=Present 9=GetBuffer 10=SetFullscreenState 13=ResizeBuffers
// ID3D11Device:        5=CreateTexture2D 9=CreateRenderTargetView 40=GetImmediateContext
// ID3D11DeviceContext: 33=OMSetRenderTargets 47=CopyResource 50=ClearRenderTargetView

static HMODULE   s_hD3D11   =nullptr;
static IUnknown *s_pD3DDev  =nullptr;
static IUnknown *s_pDXGISC  =nullptr;
static IUnknown *s_pD3DCtx  =nullptr;
static IUnknown *s_pBBTex   =nullptr;
static IUnknown *s_pBBRTV   =nullptr;
static IUnknown *s_pRightTex=nullptr;
static IUnknown *s_pRightRTV=nullptr;
static int s_W=0,s_H=0;

static void safeRelease(IUnknown *&p){if(p){p->Release();p=nullptr;}}

static bool d3d11Init(HWND hwnd){
    log("--- D3D11 + DXGI (D3D11CreateDeviceAndSwapChain, windowed) ---");
    s_hD3D11=LoadLibraryA("d3d11.dll");
    if(!s_hD3D11){log("  d3d11.dll: not found"); return false;}
    auto fn=(PfnDAS)GetProcAddress(s_hD3D11,"D3D11CreateDeviceAndSwapChain");
    if(!fn){log("  D3D11CreateDeviceAndSwapChain: not found"); return false;}

    D3D_FEATURE_LEVEL_ fl=DFL_11_0,flOut{};
    DXGI_SWAP_CHAIN_DESC_ scd{};
    scd.BD.W=(UINT)s_W; scd.BD.H=(UINT)s_H;
    scd.BD.Refresh={120,1}; scd.BD.Fmt=DFMT_B8G8R8A8;
    scd.SD={1,0}; scd.BU=DXGI_USAGE_RTO; scd.BC=2;
    scd.OW=hwnd; scd.Win=TRUE; scd.SE=DSE_DISCARD;
    // DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2 (required for FSE transitions
    // on some driver versions; may also be required for stereo companion hook)
    scd.Fl=2;

    IUnknown *pCtx=nullptr;
    HRESULT hr=fn(nullptr,DDTH_HW,nullptr,0,&fl,1,7,&scd,
                  &s_pDXGISC,&s_pD3DDev,&flOut,&pCtx);
    log("  hr=0x%x (%s)",(unsigned)hr,SUCCEEDED(hr)?"OK":"FAILED");
    if(pCtx){pCtx->Release();}
    if(FAILED(hr)) return false;
    log("  Device=%p  SwapChain=%p",(void*)s_pD3DDev,(void*)s_pDXGISC);
    return true;
}

static bool d3d11BuildRTVs(){
    safeRelease(s_pBBRTV); safeRelease(s_pBBTex);
    safeRelease(s_pRightRTV); safeRelease(s_pRightTex);
    if(!s_pD3DDev||!s_pDXGISC) return false;
    if(!s_pD3DCtx) vcall<40,void>(s_pD3DDev,(IUnknown**)&s_pD3DCtx);

    HRESULT hr=vcall<9>(s_pDXGISC,(UINT)0,&s_iidTex2D,(void**)&s_pBBTex);
    log("  GetBuffer: hr=0x%x  tex=%p",(unsigned)hr,(void*)s_pBBTex);
    if(FAILED(hr)||!s_pBBTex) return false;

    hr=vcall<9>(s_pD3DDev,s_pBBTex,(void*)nullptr,(IUnknown**)&s_pBBRTV);
    log("  CreateRTV(BB): hr=0x%x  rtv=%p",(unsigned)hr,(void*)s_pBBRTV);
    if(FAILED(hr)||!s_pBBRTV) return false;

    D3D11_TEX2D_DESC_ td{};
    td.Width=(UINT)s_W; td.Height=(UINT)s_H;
    td.MipLevels=1; td.ArraySize=1; td.Format=DFMT_B8G8R8A8;
    td.SampleDesc={1,0}; td.Usage=0; td.BindFlags=0x20;
    hr=vcall<5>(s_pD3DDev,&td,(void*)nullptr,(IUnknown**)&s_pRightTex);
    log("  CreateTexture2D(rightEye): hr=0x%x  tex=%p",(unsigned)hr,(void*)s_pRightTex);
    if(FAILED(hr)||!s_pRightTex) return false;

    hr=vcall<9>(s_pD3DDev,s_pRightTex,(void*)nullptr,(IUnknown**)&s_pRightRTV);
    log("  CreateRTV(right): hr=0x%x  rtv=%p",(unsigned)hr,(void*)s_pRightRTV);
    return SUCCEEDED(hr)&&s_pRightRTV;
}

// Go FSE WITHOUT ResizeBuffers.
// Hypothesis: ResizeBuffers destroys the stereo companion.
// 1920x1080 windowed → 1920x1080 FSE does not require ResizeBuffers.
static bool d3d11GoFSE(){
    log("--- Going fullscreen exclusive (NO ResizeBuffers) ---");
    // Release back-buffer refs so DXGI doesn't complain about outstanding refs
    safeRelease(s_pBBRTV); safeRelease(s_pBBTex);
    safeRelease(s_pRightRTV); safeRelease(s_pRightTex);
    HRESULT hr=vcall<10>(s_pDXGISC,TRUE,(void*)nullptr);
    log("  SetFullscreenState(TRUE): hr=0x%x (%s)",(unsigned)hr,SUCCEEDED(hr)?"OK":"FAILED");
    // Do NOT call ResizeBuffers – preserve companion surface allocation
    if(FAILED(hr)) return false;
    bool ok=d3d11BuildRTVs();
    log("  RTVs (post-FSE, no resize): %s",ok?"OK":"FAILED");
    return ok;
}

static void d3d11Shutdown(){
    if(s_pDXGISC) vcall<10>(s_pDXGISC,FALSE,(void*)nullptr);
    safeRelease(s_pBBRTV); safeRelease(s_pBBTex);
    safeRelease(s_pRightRTV); safeRelease(s_pRightTex);
    safeRelease(s_pD3DCtx); safeRelease(s_pDXGISC); safeRelease(s_pD3DDev);
    if(s_hD3D11){FreeLibrary(s_hD3D11);s_hD3D11=nullptr;}
}

static HRESULT dxgiPresent(UINT sync){
    return s_pDXGISC?vcall<8>(s_pDXGISC,sync,(UINT)0):E_FAIL;
}

// ============================================================================
// NVAPI
// ============================================================================
#define NVAPI_OK 0
typedef void*(*PfnNvQI)(unsigned);
typedef int(*PfnNvV)();
typedef int(*PfnNvFromIUnk)(IUnknown*,void**);
typedef int(*PfnNvDH)(void*);
typedef int(*PfnRSBC)(void*,unsigned char);

static HMODULE  s_hNvapi =nullptr;
static PfnNvQI  s_nvQI   =nullptr;
static void    *s_hStereo=nullptr;
static PfnRSBC  s_fnRSBC =nullptr;

static void *nvQ(unsigned id){return s_nvQI?s_nvQI(id):nullptr;}

#define NvID_Initialize     0x0150E828u
#define NvID_Unload         0xD22BDD7Eu
#define NvID_Stereo_Enable  0x239C4545u
#define NvID_Stereo_SetMode 0x5E8F0BECu
#define NvID_Stereo_Create  0xAC7E37F4u
#define NvID_Stereo_Destroy 0x3A153134u
#define NvID_Stereo_Activate   0xF6A1AD68u
#define NvID_Stereo_IsActive   0x1FB0BC30u
#define NvID_Stereo_SetSep     0x5C069FA3u
#define NvID_Stereo_GetSep     0xCE5729DFu
#define NvID_Stereo_RSBC       0x3CD58F89u
#define NvID_Stereo_GetSCMode  0x36F1C736u  // GetSurfaceCreationMode

// Probe and log GetSurfaceCreationMode at any point for diagnostics
static void logSurfaceCreationMode(const char *when){
    typedef int(*Pfn)(void*,unsigned*); auto fn=(Pfn)nvQ(NvID_Stereo_GetSCMode);
    if(!fn){log("  [%s] GetSurfaceCreationMode fn not found",when); return;}
    if(!s_hStereo){log("  [%s] GetSurfaceCreationMode: no handle yet",when); return;}
    unsigned m=99; fn(s_hStereo,&m);
    log("  [%s] GetSurfaceCreationMode = %u  (0=AUTO  1=FORCESTEREO  2=FORCEMONO)",when,m);
}

static bool nvapiPreInit(){
    log("--- NVAPI pre-init (before D3D11CreateDeviceAndSwapChain) ---");
    s_hNvapi=LoadLibraryA("nvapi64.dll");
    if(!s_hNvapi){log("  nvapi64.dll: not found"); return false;}
    s_nvQI=(PfnNvQI)GetProcAddress(s_hNvapi,"nvapi_QueryInterface");
    if(!s_nvQI){log("  nvapi_QueryInterface: not found"); return false;}
    log("  nvapi64.dll OK");

    {auto fn=(PfnNvV)nvQ(NvID_Initialize);
     if(!fn){log("  NvAPI_Initialize: not found"); return false;}
     int r=fn(); log("  NvAPI_Initialize = %d",r); if(r)return false;}

    {typedef int(*Pfn)(unsigned); auto fn=(Pfn)nvQ(NvID_Stereo_SetMode);
     if(fn){int r=fn(0); log("  NvAPI_Stereo_SetDriverMode(AUTO=0) = %d",r);}}

    {auto fn=(PfnNvV)nvQ(NvID_Stereo_Enable);
     if(fn){int r=fn(); log("  NvAPI_Stereo_Enable = %d",r);}}

    log("  Pre-init done");
    return true;
}

// Called IMMEDIATELY after D3D11CreateDeviceAndSwapChain (before FSE).
// This is the earliest point we can create the handle and probe companion state.
static bool nvapiCreateHandle(){
    log("--- NVAPI handle creation (windowed, immediately after D3D11 init) ---");
    if(!s_pD3DDev){log("  no D3D device"); return false;}
    auto fn=(PfnNvFromIUnk)nvQ(NvID_Stereo_Create);
    if(!fn){log("  CreateHandleFromIUnknown fn: not found"); return false;}
    int r=fn(s_pD3DDev,&s_hStereo);
    log("  NvAPI_Stereo_CreateHandleFromIUnknown = %d (%s)  handle=%p",
        r,r==NVAPI_OK?"OK":"FAILED",(void*)s_hStereo);
    if(r!=NVAPI_OK){s_hStereo=nullptr; return false;}

    s_fnRSBC=(PfnRSBC)nvQ(NvID_Stereo_RSBC);
    log("  RSBC fn: %s",s_fnRSBC?"FOUND":"NOT FOUND");

    // ── KEY DIAGNOSTIC: is companion present in windowed mode? ────────────────
    logSurfaceCreationMode("windowed pre-FSE");

    return true;
}

static void nvapiActivate(){
    if(!s_hStereo) return;
    {auto fn=(PfnNvDH)nvQ(NvID_Stereo_Activate);
     if(fn){int r=fn(s_hStereo); log("  NvAPI_Stereo_Activate = %d",r);}}
    {typedef int(*Pfn)(void*,float); auto fn=(Pfn)nvQ(NvID_Stereo_SetSep);
     if(fn) fn(s_hStereo,50.f);}
}

static void nvapiShutdown(){
    if(s_hStereo){auto f=(PfnNvDH)nvQ(NvID_Stereo_Destroy);if(f)f(s_hStereo);s_hStereo=nullptr;}
    if(s_hNvapi){auto f=(PfnNvV)nvQ(NvID_Unload);if(f)f();FreeLibrary(s_hNvapi);s_hNvapi=nullptr;}
}

// ============================================================================
// Stereo render loop
// ============================================================================
static void runStereoLoop(){
    log("--- D3D11 stereo loop (RSBC) ---");
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
            log("  frame=%-6u  Restoring FSE + 3DV...",frame);
            safeRelease(s_pBBRTV); safeRelease(s_pBBTex);
            safeRelease(s_pRightRTV); safeRelease(s_pRightTex);
            HRESULT hr=vcall<10>(s_pDXGISC,TRUE,(void*)nullptr);
            log("  SetFullscreenState(TRUE): hr=0x%x",(unsigned)hr);
            // No ResizeBuffers on restore either
            d3d11BuildRTVs();
            logSurfaceCreationMode("after restore FSE");
            nvapiActivate();
            SetForegroundWindow(s_hwnd); BringWindowToTop(s_hwnd);
            log("  Restore complete");
        }
        if(!s_pBBRTV||!s_pRightRTV||!s_pBBTex||!s_pRightTex){Sleep(16);continue;}

        // Step 1: right-eye staging = RED
        float red[4]={1.f,0.f,0.f,1.f};
        vcall<33,void>(s_pD3DCtx,(UINT)1,&s_pRightRTV,(void*)nullptr);
        vcall<50,void>(s_pD3DCtx,s_pRightRTV,red);

        // Step 2: RSBC + CopyResource → right-eye companion = RED
        if(s_fnRSBC){
            s_fnRSBC(s_hStereo,1);
            vcall<47,void>(s_pD3DCtx,s_pBBTex,s_pRightTex);
            s_fnRSBC(s_hStereo,0);
        }

        // Step 3: clear BB to CYAN → left eye = CYAN
        float cyan[4]={0.f,1.f,1.f,1.f};
        vcall<33,void>(s_pD3DCtx,(UINT)1,&s_pBBRTV,(void*)nullptr);
        vcall<50,void>(s_pD3DCtx,s_pBBRTV,cyan);

        HRESULT hr=dxgiPresent(1);
        ++frame;

        unsigned char active=0;
        if(fnIA) fnIA(s_hStereo,&active);
        if(active&&!wasActivated){
            wasActivated=true;
            log("  frame=%-6u  *** IsActivated 0→1 ***",frame);
            logSurfaceCreationMode("first activation");
        }
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
// Vulkan non-stereo (with swapchain recreation on OUT_OF_DATE)
// ============================================================================
template<class T>static T vkclamp(T v,T lo,T hi){return v<lo?lo:v>hi?hi:v;}

struct VkApp {
    VkInstance       inst =VK_NULL_HANDLE;
    VkSurfaceKHR     surf =VK_NULL_HANDLE;
    VkPhysicalDevice phys =VK_NULL_HANDLE;
    VkDevice         dev  =VK_NULL_HANDLE;
    uint32_t         qf   =~0u;
    VkQueue          q    =VK_NULL_HANDLE;
    VkSwapchainKHR   sc   =VK_NULL_HANDLE;
    VkFormat         fmt  =VK_FORMAT_UNDEFINED;
    VkExtent2D       ext  {};
    uint32_t         cnt  =0;
    std::vector<VkImage>       imgs;
    std::vector<VkImageView>   views;
    std::vector<VkFramebuffer> fbs;
    VkRenderPass  rp  =VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cbs;
    static const uint32_t MF=2;
    VkSemaphore imgRdy[MF]{},renDone[MF]{};
    VkFence     inFlt[MF]{};
    uint32_t fi=0;
    bool needRebuild=false;

    void initBase(HINSTANCE hInst,HWND hwnd){
        log("=== Vulkan non-stereo init ===");
        const char *ie[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo=&ai; ici.enabledExtensionCount=2; ici.ppEnabledExtensionNames=ie;
        if(vkCreateInstance(&ici,nullptr,&inst)!=VK_SUCCESS) throw std::runtime_error("instance");
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance=hInst; sci.hwnd=hwnd;
        vkCreateWin32SurfaceKHR(inst,&sci,nullptr,&surf);
        uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,nullptr);
        std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(inst,&n,devs.data());
        for(auto pd:devs){
            VkPhysicalDeviceProperties pr{}; vkGetPhysicalDeviceProperties(pd,&pr);
            log("  GPU: %s",pr.deviceName);
            uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,nullptr);
            std::vector<VkQueueFamilyProperties> qp(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qp.data());
            for(uint32_t i=0;i<qn;++i){
                VkBool32 ps=VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(pd,i,surf,&ps);
                if(!(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)||!ps) continue;
                phys=pd; qf=i; break;
            }
            if(phys) break;
        }
        if(!phys) throw std::runtime_error("no GPU");
        float p=1.f;
        VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        dq.queueFamilyIndex=qf; dq.queueCount=1; dq.pQueuePriorities=&p;
        const char *de=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&dq;
        dc.enabledExtensionCount=1; dc.ppEnabledExtensionNames=&de;
        vkCreateDevice(phys,&dc,nullptr,&dev);
        vkGetDeviceQueue(dev,qf,0,&q);
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex=qf; cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(dev,&cpi,nullptr,&pool);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fi2{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi2.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        for(uint32_t i=0;i<MF;++i){
            vkCreateSemaphore(dev,&si,nullptr,&imgRdy[i]);
            vkCreateSemaphore(dev,&si,nullptr,&renDone[i]);
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
        vkGetSwapchainImagesKHR(dev,sc,&cnt,nullptr); imgs.resize(cnt);
        vkGetSwapchainImagesKHR(dev,sc,&cnt,imgs.data());
        views.resize(cnt,VK_NULL_HANDLE);
        for(uint32_t i=0;i<cnt;++i){
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image=imgs[i];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=fmt;
            vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCreateImageView(dev,&vi,nullptr,&views[i]);
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
        rci.dependencyCount=1;rci.pDependencies=&dep;
        vkCreateRenderPass(dev,&rci,nullptr,&rp);
        fbs.resize(cnt,VK_NULL_HANDLE);
        for(uint32_t i=0;i<cnt;++i){
            VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbi.renderPass=rp;fbi.attachmentCount=1;fbi.pAttachments=&views[i];
            fbi.width=ext.width;fbi.height=ext.height;fbi.layers=1;
            vkCreateFramebuffer(dev,&fbi,nullptr,&fbs[i]);
        }
        cbs.resize(cnt);
        VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cba.commandPool=pool;cba.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cba.commandBufferCount=cnt;
        vkAllocateCommandBuffers(dev,&cba,cbs.data());
        needRebuild=false;
        log("  Swapchain built: %ux%u  %u images",ext.width,ext.height,cnt);
    }

    bool drawFrame(){
        if(needRebuild) buildSwapchain();
        vkWaitForFences(dev,1,&inFlt[fi],VK_TRUE,UINT64_MAX);
        uint32_t idx=0;
        VkResult r=vkAcquireNextImageKHR(dev,sc,UINT64_MAX,imgRdy[fi],VK_NULL_HANDLE,&idx);
        if(r==VK_ERROR_OUT_OF_DATE_KHR||r==VK_SUBOPTIMAL_KHR){needRebuild=true;return true;}
        if(r!=VK_SUCCESS) return false;
        vkResetFences(dev,1,&inFlt[fi]);
        vkResetCommandBuffer(cbs[idx],0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cbs[idx],&bi);
        VkClearValue cv{};cv.color.float32[1]=1;cv.color.float32[2]=1;cv.color.float32[3]=1;
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass=rp;rpbi.framebuffer=fbs[idx];
        rpbi.renderArea={{0,0},ext};rpbi.clearValueCount=1;rpbi.pClearValues=&cv;
        vkCmdBeginRenderPass(cbs[idx],&rpbi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cbs[idx]);
        vkEndCommandBuffer(cbs[idx]);
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
        fi=(fi+1)%MF;
        return true;
    }

    void cleanup(){
        if(dev) vkDeviceWaitIdle(dev);
        for(uint32_t i=0;i<MF;++i){
            if(imgRdy[i])  vkDestroySemaphore(dev,imgRdy[i],nullptr);
            if(renDone[i]) vkDestroySemaphore(dev,renDone[i],nullptr);
            if(inFlt[i])   vkDestroyFence(dev,inFlt[i],nullptr);
        }
        if(!cbs.empty()) vkFreeCommandBuffers(dev,pool,(uint32_t)cbs.size(),cbs.data());
        if(pool)  vkDestroyCommandPool(dev,pool,nullptr);
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
    log("=== VkStereo3DVision v21 startup ===");
    DEVMODEA dm{}; dm.dmSize=sizeof(dm);
    if(EnumDisplaySettingsA(nullptr,ENUM_CURRENT_SETTINGS,&dm))
        log("  Display: %ux%u @ %u Hz",dm.dmPelsWidth,dm.dmPelsHeight,dm.dmDisplayFrequency);

    s_W=GetSystemMetrics(SM_CXSCREEN); s_H=GetSystemMetrics(SM_CYSCREEN);
    log("  Monitor: %dx%d",s_W,s_H);
    s_hwnd=createWindow(hInst,s_W,s_H);
    log("  Window: WS_POPUP %dx%d",s_W,s_H);

    // Step 1: NVAPI pre-init (must precede D3D11CreateDeviceAndSwapChain)
    bool nvOk=nvapiPreInit();

    // Step 2: Combined device + swap chain (driver hooks this to allocate companion)
    bool d3dOk=nvOk && d3d11Init(s_hwnd);

    // Step 3: Create NVAPI handle IMMEDIATELY – before FSE or ResizeBuffers
    // Probe GetSurfaceCreationMode here to test if companion was allocated
    bool hvOk=d3dOk && nvapiCreateHandle();
    // If companion was allocated, GetSurfaceCreationMode = 1 at this point
    // If it returns 0 here, the combined call failed to trigger the stereo hook

    // Step 4: Go FSE (no ResizeBuffers – see hypothesis)
    bool fseOk=hvOk && d3d11GoFSE();

    // Step 5: Probe again after FSE to see if FSE transition affected companion
    if(fseOk) logSurfaceCreationMode("after FSE (no ResizeBuffers)");

    bool stereoMode = hvOk && s_hStereo!=nullptr && fseOk && s_pBBRTV && s_pRightRTV;
    log("  nv=%s  d3d=%s  handle=%s  fse=%s  stereo=%s",
        nvOk?"OK":"FAIL", d3dOk?"OK":"FAIL", hvOk?"OK":"FAIL",
        fseOk?"OK":"FAIL", stereoMode?"YES":"NO");

    if(stereoMode){
        nvapiActivate();
        s_restore=false;
        runStereoLoop();
        nvapiShutdown(); d3d11Shutdown();
    } else {
        d3d11Shutdown(); nvapiShutdown();
        VkApp app{};
        try{
            app.initBase(hInst,s_hwnd); app.buildSwapchain();
            log("--- Vulkan render loop (ESC to quit) ---");
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
