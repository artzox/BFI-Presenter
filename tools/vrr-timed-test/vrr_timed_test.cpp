// VRR timed-present test (standalone, no game, no ReShade).
//
// Question it answers: with VRR on, can the display's refreshes be timed exactly by handing each image over with
// a target time (Windows' presentation API: IPresentationManager::SetTargetTime), instead of presenting at that
// moment? With BFI every refresh's length is part of the brightness, so they must hold within a few tenths of a ms.
//
// It shows a BFI strobe on its own borderless fullscreen window: a lit refresh with a moving bar, then Frames-1
// black refreshes, at RATE Hz, queued a few refreshes ahead with exact target times. The system reports when each
// image was actually displayed (independent-flip statistics), or that it was composed by DWM instead. Once a second
// VrrTimedTest_log.txt (next to the exe) gets the measured refresh lengths and how late each image was against its
// target.
//
// Usage:  VrrTimedTest.exe [rate Hz, default 100] [frames per cycle, default 2] [queue depth, default 3] [seconds, default 120]
//                          [GPU load in ms per strobe cycle on a second device, default 0]
// Esc quits. Needs G-SYNC/VRR on for VrrTimedTest.exe (NVIDIA profile: windowed and full screen) and VSync on.
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <atomic>
#include <d3dcompiler.h>

#ifndef D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE
#define D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE 0x100000
#endif

// ---- Windows presentation API (presentation.h, Windows SDK 10.0.19041+), declared here for MinGW.
// Methods returning a struct follow MSVC's member-function ABI: the result goes through a hidden pointer.
struct SystemInterruptTime { UINT64 value; };
enum PresentStatisticsKind { PresentStatisticsKind_PresentStatus = 1, PresentStatisticsKind_CompositionFrame = 2,
                             PresentStatisticsKind_IndependentFlipFrame = 3 };
enum PresentStatus { PresentStatus_Queued = 0, PresentStatus_Skipped = 1, PresentStatus_Canceled = 2 };
typedef unsigned char boolean_t;

struct IPresentationBuffer : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetAvailableEvent(HANDLE *ev) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsAvailable(boolean_t *avail) = 0;
};
struct IPresentationContent : IUnknown { virtual void STDMETHODCALLTYPE SetTag(UINT_PTR tag) = 0; };
struct IPresentationSurface : IPresentationContent
{
    virtual HRESULT STDMETHODCALLTYPE SetBuffer(IPresentationBuffer *b) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetColorSpace(DXGI_COLOR_SPACE_TYPE cs) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAlphaMode(DXGI_ALPHA_MODE m) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSourceRect(const RECT *r) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTransform(void *t) = 0;
    virtual HRESULT STDMETHODCALLTYPE RestrictToOutput(IUnknown *o) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDisableReadback(boolean_t v) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLetterboxingMargins(float l, float t, float r, float b) = 0;
};
struct IPresentStatistics : IUnknown
{
    virtual UINT64 STDMETHODCALLTYPE GetPresentId() = 0;
    virtual PresentStatisticsKind STDMETHODCALLTYPE GetKind() = 0;
};
struct IPresentStatusPresentStatistics : IPresentStatistics
{
    virtual UINT64 STDMETHODCALLTYPE GetCompositionFrameId() = 0;
    virtual PresentStatus STDMETHODCALLTYPE GetPresentStatus() = 0;
};
struct IIndependentFlipFramePresentStatistics : IPresentStatistics
{
    virtual void STDMETHODCALLTYPE GetOutputAdapterLUID(LUID *ret) = 0;
    virtual UINT STDMETHODCALLTYPE GetOutputVidPnSourceId() = 0;
    virtual UINT_PTR STDMETHODCALLTYPE GetContentTag() = 0;
    virtual void STDMETHODCALLTYPE GetDisplayedTime(SystemInterruptTime *ret) = 0;
    virtual void STDMETHODCALLTYPE GetPresentDuration(SystemInterruptTime *ret) = 0;
};
struct IPresentationManager : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE AddBufferFromResource(IUnknown *res, IPresentationBuffer **b) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreatePresentationSurface(HANDLE h, IPresentationSurface **s) = 0;
    virtual UINT64 STDMETHODCALLTYPE GetNextPresentId() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTargetTime(SystemInterruptTime t) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreferredPresentDuration(SystemInterruptTime d, SystemInterruptTime tol) = 0;
    virtual HRESULT STDMETHODCALLTYPE ForceVSyncInterrupt(boolean_t f) = 0;
    virtual HRESULT STDMETHODCALLTYPE Present() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPresentRetiringFence(REFIID riid, void **fence) = 0;
    virtual HRESULT STDMETHODCALLTYPE CancelPresentsFrom(UINT64 id) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLostEvent(HANDLE *ev) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPresentStatisticsAvailableEvent(HANDLE *ev) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnablePresentStatisticsKind(PresentStatisticsKind k, boolean_t on) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNextPresentStatistics(IPresentStatistics **s) = 0;
};
struct IPresentationFactory : IUnknown
{
    virtual boolean_t STDMETHODCALLTYPE IsPresentationSupported() = 0;
    virtual boolean_t STDMETHODCALLTYPE IsPresentationSupportedWithIndependentFlip() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreatePresentationManager(IPresentationManager **m) = 0;
};
static const IID IID_IPresentationFactory_ = {0x8FB37B58, 0x1D74, 0x4F64, {0xA4, 0x9C, 0x1F, 0x97, 0xA8, 0x0A, 0x2E, 0xC0}};
static const IID IID_IPresentStatusPS_      = {0xC9ED2A41, 0x79CB, 0x435E, {0x96, 0x4E, 0xC8, 0x55, 0x30, 0x55, 0x42, 0x0C}};
static const IID IID_IIndependentFlipPS_    = {0x8C93BE27, 0xAD94, 0x4DA0, {0x8F, 0xD4, 0x24, 0x13, 0x13, 0x2D, 0x12, 0x4E}};
static const IID IID_IDCompositionDevice_   = {0xc37ea93a, 0xe7aa, 0x450d, {0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3}};

typedef HRESULT (WINAPI *PFN_CreatePresentationFactory)(IUnknown *, REFIID, void **);
typedef HRESULT (WINAPI *PFN_DCompCreateSurfaceHandle)(DWORD, SECURITY_ATTRIBUTES *, HANDLE *);
typedef HRESULT (WINAPI *PFN_DCompCreateDevice)(IDXGIDevice *, REFIID, void **);
typedef VOID (WINAPI *PFN_QITP)(PULONGLONG);

static FILE *g_log = nullptr;
static void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    SYSTEMTIME st; GetLocalTime(&st);
    if (g_log) { fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf); fflush(g_log); }
    OutputDebugStringA(buf);
}
static bool g_quit = false;
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { g_quit = true; return 0; }
    if (m == WM_CLOSE || m == WM_DESTROY) { g_quit = true; return 0; }
    if (m == WM_SETCURSOR) { SetCursor(nullptr); return TRUE; }
    return DefWindowProcW(h, m, w, l);
}
// ---- GPU load (optional): a second D3D11 device, like a game in another process, rendering a heavy pixel shader into
// an offscreen 4K target once per strobe cycle, steered to about loadMs of GPU time per frame (timestamp queries).
static std::atomic<bool> g_loadRun{true};
static std::atomic<bool> g_loadCalibrated{false};   // iterations are fixed after 3 s of steering, before the strobe starts
static std::atomic<double> g_loadGpuMs{0.0};
static std::atomic<int> g_loadFrames{0}, g_loadMeasured{0};
struct LoadArgs { double loadMs, fps; };
static const char kLoadHlsl[] =
    "cbuffer C : register(b0) { uint iters; uint3 pad; };\n"
    "float4 VS(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 PS(float4 p : SV_Position) : SV_Target { float3 v = float3(p.xy * 0.001, 0.5); [loop] for (uint i = 0; i < iters; i++) v = frac(sin(v * 12.9898 + v.yzx * 78.233) * 43758.5453); return float4(v, 1); }\n";
static DWORD WINAPI LoadThread(void *pv)
{
    LoadArgs a = *(LoadArgs *)pv;
    typedef HRESULT (WINAPI *PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
    HMODULE dcm = LoadLibraryA("d3dcompiler_47.dll");
    PFN_Compile comp = dcm ? (PFN_Compile)(void *)GetProcAddress(dcm, "D3DCompile") : nullptr;
    ID3D11Device *d = nullptr; ID3D11DeviceContext *c = nullptr;
    if (!comp || FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c)))
    { Log("GPU load: could not start (d3dcompiler_47 or device)"); return 0; }
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    comp(kLoadHlsl, sizeof(kLoadHlsl) - 1, "load", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vb, &err);
    comp(kLoadHlsl, sizeof(kLoadHlsl) - 1, "load", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &pb, &err);
    if (!vb || !pb) { Log("GPU load: shader compile failed"); return 0; }
    ID3D11VertexShader *vs = nullptr; ID3D11PixelShader *ps = nullptr;
    d->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
    d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps);
    D3D11_TEXTURE2D_DESC td = {}; td.Width = 3840; td.Height = 2160; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = nullptr; ID3D11RenderTargetView *rv = nullptr;
    d->CreateTexture2D(&td, nullptr, &rt); d->CreateRenderTargetView(rt, nullptr, &rv);
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *cb = nullptr; d->CreateBuffer(&bd, nullptr, &cb);
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, qt = {D3D11_QUERY_TIMESTAMP, 0};
    ID3D11Query *qdis[4], *q0[4], *q1[4];
    for (int i = 0; i < 4; i++) { d->CreateQuery(&qd, &qdis[i]); d->CreateQuery(&qt, &q0[i]); d->CreateQuery(&qt, &q1[i]); }
    double iters = 64.0; int fi = 0; double sum = 0; int n = 0;
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    double next = 0.0;
    D3D11_VIEWPORT vp = {0, 0, 3840, 2160, 0, 1};
    Log("GPU load: about %.1f ms of GPU time per frame at %.1f fps on a second device", a.loadMs, a.fps);
    while (g_loadRun.load())
    {
        QueryPerformanceCounter(&t);
        const double now = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
        if (now < next) { Sleep(1); continue; }
        next = (next == 0.0 ? now : next) + 1.0 / a.fps;
        const int s = fi % 4;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(c->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { ((UINT *)ms.pData)[0] = (UINT)iters; c->Unmap(cb, 0); }
        c->Begin(qdis[s]); c->End(q0[s]);
        c->OMSetRenderTargets(1, &rv, nullptr); c->RSSetViewports(1, &vp);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->PSSetConstantBuffers(0, 1, &cb);
        c->Draw(3, 0);
        c->End(q1[s]); c->End(qdis[s]); c->Flush();
        fi++; g_loadFrames.fetch_add(1);
        if (fi >= 4)
        {
            const int r = fi % 4;   // the oldest of the four, three frames back
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a0, a1;
            // Wait briefly for the oldest frame's queries (they used to be missed when the strobe ran at a higher priority).
            HRESULT g = S_FALSE;
            for (int w = 0; w < 50 && (g = c->GetData(qdis[r], &dj, sizeof(dj), 0)) == S_FALSE; w++) Sleep(1);
            if (g == S_OK && c->GetData(q0[r], &a0, 8, 0) == S_OK && c->GetData(q1[r], &a1, 8, 0) == S_OK && !dj.Disjoint && dj.Frequency)
            {
                const double ms = (double)(a1 - a0) * 1000.0 / dj.Frequency;
                if (ms > 0.05 && !g_loadCalibrated.load()) iters = std::max(1.0, std::min(1e6, iters * std::min(1.5, std::max(0.67, a.loadMs / ms))));
                if (!g_loadCalibrated.load() && now > 3.0) { g_loadCalibrated.store(true); Log("GPU load calibrated alone: %.0f iterations, %.1f ms per frame", iters, ms); }
                sum += ms; n++; g_loadMeasured.fetch_add(1);
                if (n >= 10) { g_loadGpuMs.store(sum / n); sum = 0; n = 0; }
            }
        }
    }
    return 0;
}

#define FAIL(...) do { Log(__VA_ARGS__); MessageBoxA(nullptr, "VRR timed test failed -- see VrrTimedTest_log.txt", "VRR timed test", MB_OK); return 1; } while (0)

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    double rate = argc > 1 ? _wtof(argv[1]) : 100.0;
    int frames  = argc > 2 ? _wtoi(argv[2]) : 2;
    int queue   = argc > 3 ? _wtoi(argv[3]) : 3;
    int seconds = argc > 4 ? _wtoi(argv[4]) : 120;
    double loadMs = argc > 5 ? _wtof(argv[5]) : 0.0;
    // Mode (6th argument): 0 = steady targets + preferred present duration; 1 = steady targets, no preferred duration;
    // 2 = targets alternating 1 ms short and long (9/11 ms at 100 Hz) -- if the refreshes follow, the targets are honoured.
    int mode = argc > 6 ? _wtoi(argv[6]) : 0;
    // Clock for the target times (7th argument): 0 = QPC in 100 ns units (the clock the displayed times come back in),
    // 1 = interrupt time (1.0-1.3: those targets were about half a second in the past, so images went out at once).
    const int clk = argc > 7 ? _wtoi(argv[7]) : 0;
    // Buffer scale (8th argument): the images are drawn at this fraction of the screen size and scaled up by the display
    // hardware (an overlay plane, as video playback uses) -- 1.1 at 1280x720 on a 4K screen kept every refresh within 0.25 ms.
    double scale = argc > 8 ? _wtof(argv[8]) : 1.0;
    // GPU priority for the strobe's own drawing (9th argument, -7..7, default 7): ahead of the load's 14 ms draws.
    const int gprio = argc > 9 ? _wtoi(argv[9]) : 7;
    if (scale < 0.2 || scale > 1.0) scale = 1.0;
    if (mode < 0 || mode > 2) mode = 0;
    if (loadMs < 0 || loadMs > 100) loadMs = 0;
    if (rate < 30 || rate > 500) rate = 100; if (frames < 1 || frames > 8) frames = 2;
    if (queue < 1 || queue > 8) queue = 3; if (seconds < 5) seconds = 120;

    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t *sl = wcsrchr(path, L'\\'); if (sl) swprintf(sl + 1, MAX_PATH - (sl + 1 - path), L"VrrTimedTest_log_%.0fHz_load%.0f_mode%d_scale%.0f_q%d_prio%d.txt", rate, loadMs, mode, scale * 100.0, queue, gprio);
    g_log = _wfopen(path, L"w");
    {
        typedef BOOL (WINAPI *PFN_DpiCtx)(HANDLE);
        auto setDpi = (PFN_DpiCtx)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
        if (setDpi) setDpi((HANDLE)-4 /*DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2*/);
    }
    Log("VRR timed-present test 1.8: %.1f Hz, %d frames per cycle (strobe %.1f Hz), %d presents queued ahead, %d s, GPU load %.1f ms, target clock %s, mode %d (%s)",
        rate, frames, rate / frames, queue, seconds, loadMs, clk ? "interrupt time" : "QPC", mode,
        mode == 0 ? "steady targets + preferred duration" : mode == 1 ? "steady targets, no preferred duration" : "targets alternating short/long");
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    PFN_QITP qitp = nullptr;
    for (const char *mod : {"kernelbase.dll", "kernel32.dll", "api-ms-win-core-realtime-l1-1-1.dll"})
    {
        HMODULE hm = LoadLibraryA(mod);
        if (hm && (qitp = (PFN_QITP)(void *)GetProcAddress(hm, "QueryInterruptTimePrecise"))) break;
    }
    HMODULE dc = LoadLibraryA("dcomp.dll");
    auto createPF = dc ? (PFN_CreatePresentationFactory)(void *)GetProcAddress(dc, "CreatePresentationFactory") : nullptr;
    auto createSH = dc ? (PFN_DCompCreateSurfaceHandle)(void *)GetProcAddress(dc, "DCompositionCreateSurfaceHandle") : nullptr;
    auto createDC = dc ? (PFN_DCompCreateDevice)(void *)GetProcAddress(dc, "DCompositionCreateDevice") : nullptr;
    if (!qitp || !createPF || !createSH || !createDC)
        FAIL("Not available: %s%s%s%s(dcomp.dll %s)", qitp ? "" : "QueryInterruptTimePrecise ", createPF ? "" : "CreatePresentationFactory ",
             createSH ? "" : "DCompositionCreateSurfaceHandle ", createDC ? "" : "DCompositionCreateDevice ", dc ? "loaded" : "not loaded");

    // Window: borderless over the primary monitor.
    POINT pt = {0, 0};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    const int W = mi.rcMonitor.right - mi.rcMonitor.left, H = mi.rcMonitor.bottom - mi.rcMonitor.top;
    WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"VrrTimedTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"VrrTimedTest", L"VRR timed test", WS_POPUP | WS_VISIBLE,
                                mi.rcMonitor.left, mi.rcMonitor.top, W, H, nullptr, nullptr, inst, nullptr);
    if (!hwnd) FAIL("CreateWindow failed (%lu)", GetLastError());

    // D3D11 device.
    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx0 = nullptr;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 1,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx0);
    if (FAILED(hr)) FAIL("D3D11CreateDevice 0x%08lX", (unsigned long)hr);
    ID3D11DeviceContext1 *ctx = nullptr; ctx0->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx);

    // HDR desktop? Then scRGB float buffers, so the surface can stay independently flipped.
    bool hdr = false; double modeHz = 0.0;
    {
        IDXGIDevice *dd = nullptr; IDXGIAdapter *ad = nullptr; IDXGIOutput *out = nullptr; IDXGIOutput6 *o6 = nullptr;
        dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd);
        if (dd && SUCCEEDED(dd->GetAdapter(&ad)))
            for (UINT i = 0; ad->EnumOutputs(i, &out) == S_OK; i++)
            {
                DXGI_OUTPUT_DESC od; out->GetDesc(&od);
                if (od.Monitor == mon && SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), (void **)&o6)))
                {
                    DXGI_OUTPUT_DESC1 d1; o6->GetDesc1(&d1);
                    hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
                    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) modeHz = dm.dmDisplayFrequency;
                    o6->Release();
                }
                out->Release();
            }
        if (ad) ad->Release(); if (dd) dd->Release();
    }
    const int BW = std::max(64, (int)(W * scale + 0.5)), BH = std::max(64, (int)(H * scale + 0.5));
    Log("Display %dx%d, mode %.0f Hz, Windows HDR %s; images drawn at %dx%d%s", W, H, modeHz, hdr ? "on (scRGB buffers)" : "off (8-bit buffers)",
        BW, BH, (BW != W || BH != H) ? " and scaled to the screen by the display hardware" : "");
    if (modeHz > 0 && rate > modeHz - 0.5) Log("Note: %.1f Hz is not below the display mode's %.0f Hz -- VRR has no room to act", rate, modeHz);

    // Presentation manager and surface, shown through DirectComposition.
    IPresentationFactory *pf = nullptr;
    hr = createPF(dev, IID_IPresentationFactory_, (void **)&pf);
    if (FAILED(hr) || !pf) FAIL("CreatePresentationFactory 0x%08lX", (unsigned long)hr);
    Log("Presentation supported: %d, with independent flip: %d", (int)pf->IsPresentationSupported(),
        (int)pf->IsPresentationSupportedWithIndependentFlip());
    if (!pf->IsPresentationSupported()) FAIL("The presentation API isn't supported on this GPU/driver");
    IPresentationManager *pm = nullptr;
    hr = pf->CreatePresentationManager(&pm);
    if (FAILED(hr)) FAIL("CreatePresentationManager 0x%08lX", (unsigned long)hr);
    HANDLE sh = nullptr;
    hr = createSH(0x0003 /*COMPOSITIONOBJECT_ALL_ACCESS*/, nullptr, &sh);
    if (FAILED(hr)) FAIL("DCompositionCreateSurfaceHandle 0x%08lX", (unsigned long)hr);
    IPresentationSurface *surf = nullptr;
    hr = pm->CreatePresentationSurface(sh, &surf);
    if (FAILED(hr)) FAIL("CreatePresentationSurface 0x%08lX", (unsigned long)hr);
    IDXGIDevice *dxgiDev = nullptr; dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDev);
    if (dxgiDev) { const HRESULT ph = dxgiDev->SetGPUThreadPriority(std::max(-7, std::min(7, gprio))); Log("GPU thread priority %d: 0x%08lX", gprio, (unsigned long)ph); }
    IDCompositionDevice *dcd = nullptr;
    hr = createDC(dxgiDev, IID_IDCompositionDevice_, (void **)&dcd);
    if (FAILED(hr)) FAIL("DCompositionCreateDevice 0x%08lX", (unsigned long)hr);
    IDCompositionTarget *tgt = nullptr; IDCompositionVisual *vis = nullptr; IUnknown *surfObj = nullptr;
    hr = dcd->CreateTargetForHwnd(hwnd, TRUE, &tgt);
    if (SUCCEEDED(hr)) hr = dcd->CreateVisual(&vis);
    if (SUCCEEDED(hr)) hr = dcd->CreateSurfaceFromHandle(sh, &surfObj);
    if (SUCCEEDED(hr)) hr = vis->SetContent(surfObj);
    if (SUCCEEDED(hr)) hr = tgt->SetRoot(vis);
    if (SUCCEEDED(hr)) hr = dcd->Commit();
    if (FAILED(hr)) FAIL("DirectComposition setup 0x%08lX", (unsigned long)hr);
    surf->SetAlphaMode(DXGI_ALPHA_MODE_IGNORE);
    surf->SetColorSpace(hdr ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    RECT src = {0, 0, BW, BH}; surf->SetSourceRect(&src);
    if (BW != W || BH != H) { float tf[6] = {(float)W / BW, 0, 0, (float)H / BH, 0, 0}; surf->SetTransform(tf); }

    // Buffers: queue + 2, displayable textures.
    const int nb = queue + 2;
    std::vector<ID3D11Texture2D *> tex(nb); std::vector<ID3D11RenderTargetView *> rtv(nb); std::vector<IPresentationBuffer *> pb(nb);
    std::vector<HANDLE> availEv(nb);
    for (int i = 0; i < nb; i++)
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = BW; td.Height = BH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_DISPLAYABLE;
        hr = dev->CreateTexture2D(&td, nullptr, &tex[i]);
        if (FAILED(hr)) FAIL("CreateTexture2D (displayable) 0x%08lX", (unsigned long)hr);
        dev->CreateRenderTargetView(tex[i], nullptr, &rtv[i]);
        hr = pm->AddBufferFromResource(tex[i], &pb[i]);
        if (FAILED(hr)) FAIL("AddBufferFromResource 0x%08lX", (unsigned long)hr);
        pb[i]->GetAvailableEvent(&availEv[i]);
    }
    pm->EnablePresentStatisticsKind(PresentStatisticsKind_PresentStatus, 1);
    pm->EnablePresentStatisticsKind(PresentStatisticsKind_CompositionFrame, 1);
    pm->EnablePresentStatisticsKind(PresentStatisticsKind_IndependentFlipFrame, 1);
    const UINT64 per = (UINT64)(1e7 / rate + 0.5);                  // interrupt time: 100 ns units
    SystemInterruptTime pd = {per}, tol = {per / 50};
    hr = mode == 0 ? pm->SetPreferredPresentDuration(pd, tol) : S_OK;
    Log("SetPreferredPresentDuration(%.3f ms) 0x%08lX", per / 1e4, (unsigned long)hr);

    // The displayed times may use another clock than the interrupt time the targets are given in: sample both.
    ULONGLONG it0 = 0; LARGE_INTEGER qpc0, qpf; qitp(&it0); QueryPerformanceCounter(&qpc0); QueryPerformanceFrequency(&qpf);
    const double qpcMinusIt = (double)qpc0.QuadPart * (1e7 / (double)qpf.QuadPart) - (double)it0;   // 100 ns units
    Log("Clocks: QPC frequency %lld; QPC (in 100 ns) minus interrupt time %.3f ms", (long long)qpf.QuadPart, qpcMinusIt / 1e4);
    static LoadArgs la; la.loadMs = loadMs; la.fps = rate / frames;
    HANDLE lt = loadMs > 0 ? CreateThread(nullptr, 0, LoadThread, &la, 0, nullptr) : nullptr;
    if (lt) { for (int w = 0; w < 600 && !g_loadCalibrated.load(); w++) Sleep(10); }
    qitp(&it0);   // (unused afterwards; keeps the clock sample fresh)
    // Strobe loop.
    const float lit = hdr ? 2.5f : 1.0f;                            // 200 nits in scRGB, white in SDR
    auto nowT = [&](ULONGLONG *o) { if (clk) qitp(o); else { LARGE_INTEGER q; QueryPerformanceCounter(&q); *o = (ULONGLONG)((double)q.QuadPart * (1e7 / (double)qpf.QuadPart)); } };
    const double clkToQpc = clk ? qpcMinusIt : 0.0;   // target clock -> displayed-time clock
    ULONGLONG now = 0; nowT(&now);
    UINT64 nextT = now + per * 10;
    UINT64 k = 0;
    struct Pending { UINT64 id, target; double callQ; };
    std::vector<Pending> pend;                                       // presents waiting for their statistics
    UINT64 lastId = 0, lastShown = 0;
    std::vector<double> dev_ms, late_ms, lateQ_ms, callShow_ms, devOdd, devEven; int composed = 0, iflip = 0, skipped = 0, gaps = 0;
    ULONGLONG winStart = now; int logs = 0; ULONGLONG endT = now + (ULONGLONG)seconds * 10000000ull;
    int bi = 0;
    while (!g_quit)
    {
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        nowT(&now);
        if (now >= endT) break;
        // Keep `queue` presents ahead of the display: hand over the next one when its target is within the queue.
        if (nextT > now + per * (UINT64)queue) { Sleep(1); continue; }
        if (nextT + per < now) { nextT = now + per * 2; gaps++; }    // fell behind (stall): restart the clock
        // A free buffer.
        int found = -1;
        for (int t = 0; t < nb && found < 0; t++) { int c = (bi + t) % nb; boolean_t av = 0; if (SUCCEEDED(pb[c]->IsAvailable(&av)) && av) found = c; }
        if (found < 0) { WaitForMultipleObjects((DWORD)nb, availEv.data(), FALSE, 5); continue; }
        bi = (found + 1) % nb;
        const int slot = (int)(k % (UINT64)frames);
        const float black[4] = {0, 0, 0, 1};
        ctx->ClearView(rtv[found], black, nullptr, 0);
        if (slot == 0)
        {
            // Lit refresh: grey field with a white bar moving across (one step per cycle) to judge motion.
            const float grey[4] = {lit * 0.35f, lit * 0.35f, lit * 0.35f, 1}, white[4] = {lit, lit, lit, 1};
            D3D11_RECT r = {0, BH / 4, BW, BH * 3 / 4};
            ctx->ClearView(rtv[found], grey, &r, 1);
            const int cyc = (int)(k / (UINT64)frames), bw = BW / 24, x = (cyc * (BW / 120)) % (BW + bw) - bw;
            D3D11_RECT b = {std::max(0, x), BH / 4, std::min(BW, x + bw), BH * 3 / 4};
            if (b.right > b.left) ctx->ClearView(rtv[found], white, &b, 1);
        }
        ctx->Flush();
        surf->SetBuffer(pb[found]);
        SystemInterruptTime tt = {nextT};
        pm->SetTargetTime(tt);
        const UINT64 id = pm->GetNextPresentId();
        hr = pm->Present();
        if (FAILED(hr)) { Log("Present 0x%08lX", (unsigned long)hr); break; }
        LARGE_INTEGER cq; QueryPerformanceCounter(&cq);
        pend.push_back({id, nextT, (double)cq.QuadPart * (1e7 / (double)qpf.QuadPart)});
        // Mode 2: the next target 1 ms early or late, alternately (the average rate stays the same).
        nextT += per; if (mode == 2) nextT += (k % 2 == 0) ? 10000 : (UINT64)-10000; k++;

        // Statistics.
        IPresentStatistics *ps = nullptr;
        while (SUCCEEDED(pm->GetNextPresentStatistics(&ps)) && ps)
        {
            const UINT64 pid = ps->GetPresentId();
            const PresentStatisticsKind kind = ps->GetKind();
            if (kind == PresentStatisticsKind_IndependentFlipFrame)
            {
                IIndependentFlipFramePresentStatistics *fs = nullptr;
                if (SUCCEEDED(ps->QueryInterface(IID_IIndependentFlipPS_, (void **)&fs)) && fs)
                {
                    SystemInterruptTime shown = {}; fs->GetDisplayedTime(&shown);
                    iflip++;
                    for (auto &p : pend) if (p.id == pid) { late_ms.push_back(((double)shown.value - ((double)p.target + clkToQpc)) / 1e4); lateQ_ms.push_back(((double)shown.value - ((double)p.target + clkToQpc)) / 1e4); callShow_ms.push_back(((double)shown.value - p.callQ) / 1e4); break; }
                    if (lastId && pid == lastId + 1 && lastShown)
                    {
                        const double len = ((double)shown.value - (double)lastShown) / 1e4;
                        dev_ms.push_back(len - per / 1e4);
                        (pid % 2 ? devOdd : devEven).push_back(len);
                    }
                    lastId = pid; lastShown = shown.value;
                    fs->Release();
                }
            }
            else if (kind == PresentStatisticsKind_CompositionFrame) composed++;
            else if (kind == PresentStatisticsKind_PresentStatus)
            {
                IPresentStatusPresentStatistics *ss = nullptr;
                if (SUCCEEDED(ps->QueryInterface(IID_IPresentStatusPS_, (void **)&ss)) && ss)
                {
                    if (ss->GetPresentStatus() == PresentStatus_Skipped) skipped++;
                    ss->Release();
                }
            }
            ps->Release(); ps = nullptr;
        }
        while (pend.size() > 64) pend.erase(pend.begin());

        if (now - winStart >= (logs < 5 ? 10000000ull : 100000000ull))
        {
            auto pct = [](std::vector<double> v, double q) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, (size_t)(q * v.size()))]; };
            std::vector<double> ad; for (double d : dev_ms) ad.push_back(fabs(d));
            int h[9] = {};
            for (double d : dev_ms) h[d < -2 ? 0 : d < -1 ? 1 : d < -0.5 ? 2 : d < -0.25 ? 3 : d <= 0.25 ? 4 : d <= 0.5 ? 5 : d <= 1 ? 6 : d <= 2 ? 7 : 8]++;
            double meanLen = 0; for (double d : dev_ms) meanLen += d + per / 1e4; if (!dev_ms.empty()) meanLen /= dev_ms.size();
            Log("%zu refreshes measured (flipped %d, composed by DWM %d, skipped %d, clock restarts %d): mean %.3f ms (%.1f Hz); "
                "off by median %.3f, 99%% %.3f ms; within 0.25 ms %d", dev_ms.size(), iflip, composed, skipped, gaps, meanLen,
                meanLen > 0 ? 1000.0 / meanLen : 0.0, pct(ad, 0.5), pct(ad, 0.99), h[4]);
            Log("  refresh length minus %.2f ms: below -2 %d, -2..-1 %d, -1..-0.5 %d, -0.5..-0.25 %d, within 0.25 %d, +0.25..0.5 %d, "
                "+0.5..1 %d, +1..2 %d, above +2 %d; shown after target: median %.3f, 99%% %.3f ms (both clocks as QPC: "
                "median %.3f, 99%% %.3f ms)", per / 1e4, h[0], h[1], h[2], h[3],
                h[4], h[5], h[6], h[7], h[8], pct(late_ms, 0.5), pct(late_ms, 0.99), pct(lateQ_ms, 0.5), pct(lateQ_ms, 0.99));
            if (loadMs > 0) Log("  GPU load: %d frames drawn, %d timed, %.1f ms per frame", g_loadFrames.exchange(0), g_loadMeasured.exchange(0), g_loadGpuMs.load());
            {
                auto mean = [](const std::vector<double> &v) { double a = 0; for (double x : v) a += x; return v.empty() ? 0.0 : a / v.size(); };
                Log("  Present call -> displayed (displayed time as QPC): median %.3f, 99%% %.3f ms; refresh length after odd / even "
                    "presents: %.3f / %.3f ms%s", pct(callShow_ms, 0.5), pct(callShow_ms, 0.99), mean(devOdd), mean(devEven),
                    mode == 2 ? " (targets honoured if these are about 1 ms apart from 10)" : "");
            }
            if (composed > iflip) Log("  most images were composed by DWM, not flipped -- timing then follows DWM, not the targets");
            dev_ms.clear(); late_ms.clear(); lateQ_ms.clear(); callShow_ms.clear(); devOdd.clear(); devEven.clear(); composed = iflip = skipped = gaps = 0; winStart = now; logs++;
        }
    }
    g_loadRun.store(false); if (lt) { WaitForSingleObject(lt, 2000); CloseHandle(lt); }
    Log("done");
    timeEndPeriod(1);
    return 0;
}
