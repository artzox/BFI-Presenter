// BFI Motion Test -- a small Direct3D 12 program that draws moving test patterns, so ReShade and
// BFI Presenter can be judged on the same picture on every display (inspired by Blur Busters' TestUFO, but as a "game"
// the presenter hooks). Everything is drawn with ClearRenderTargetView rectangles: no shaders.
//
// Motion is computed from the clock, not from the frame count, so a late or repeated frame shows
// as a visible jump, exactly as it would in a game.
//
// Keys:  Up/Down  speed (px/s)        Left/Right  pattern
//        1..4     frame rate: every refresh, every 2nd, every 3rd, every 4th
//        V        VSync on/off        F / F11     borderless fullscreen / window
//        Space    pause               Esc         quit
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cmath>
#include <vector>

static const UINT kBuffers = 3;
static HWND g_hwnd;
static ID3D12Device *g_dev;
static ID3D12CommandQueue *g_queue;
static IDXGISwapChain3 *g_sc;
static ID3D12DescriptorHeap *g_rtvHeap;
static UINT g_rtvSize;
static ID3D12Resource *g_bb[kBuffers];
static ID3D12CommandAllocator *g_alloc[kBuffers];
static ID3D12GraphicsCommandList *g_list;
static ID3D12Fence *g_fence;
static UINT64 g_fenceVal[kBuffers], g_nextFence = 1;
static HANDLE g_fenceEvt;
static UINT g_w = 1280, g_h = 720;
static bool g_resize = false, g_fullscreen = false, g_vsync = true, g_paused = false, g_tearing = false;
static int g_speedIdx = 3, g_pattern = 0, g_rateDiv = 1;
static const int kSpeeds[] = { 240, 480, 720, 960, 1440, 1920, 2880, 3840 };
static const char *kPatterns[] = { "Bars and marker", "Checkerboard", "Thin lines", "Gradient blocks" };
static RECT g_winRect;
static double g_pos = 0.0;

static void Fail(const char *what, HRESULT hr)
{
    char b[256];
    snprintf(b, sizeof b, "%s failed (0x%08lX).", what, (unsigned long)hr);
    MessageBoxA(nullptr, b, "BFI Motion Test", MB_ICONERROR);
    ExitProcess(1);
}
#define CHECK(x, what) do { HRESULT hr_ = (x); if (FAILED(hr_)) Fail(what, hr_); } while (0)

static void WaitIdle()
{
    const UINT64 v = g_nextFence++;
    g_queue->Signal(g_fence, v);
    if (g_fence->GetCompletedValue() < v) { g_fence->SetEventOnCompletion(v, g_fenceEvt); WaitForSingleObject(g_fenceEvt, INFINITE); }
}

static void CreateRTVs()
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBuffers; i++)
    {
        CHECK(g_sc->GetBuffer(i, IID_PPV_ARGS(&g_bb[i])), "GetBuffer");
        g_dev->CreateRenderTargetView(g_bb[i], nullptr, h);
        h.ptr += g_rtvSize;
    }
}

static void Resize()
{
    WaitIdle();
    for (UINT i = 0; i < kBuffers; i++) if (g_bb[i]) { g_bb[i]->Release(); g_bb[i] = nullptr; }
    RECT rc; GetClientRect(g_hwnd, &rc);
    g_w = rc.right > 0 ? rc.right : 1; g_h = rc.bottom > 0 ? rc.bottom : 1;
    CHECK(g_sc->ResizeBuffers(kBuffers, g_w, g_h, DXGI_FORMAT_R8G8B8A8_UNORM,
                              g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0), "ResizeBuffers");
    CreateRTVs();
}

static void UpdateTitle(double fps)
{
    char t[256];
    snprintf(t, sizeof t, "BFI Motion Test -- %s, %d px/s, every %s refresh, VSync %s, %.0f fps%s   [arrows, 1-4, V, F, Space]",
             kPatterns[g_pattern], kSpeeds[g_speedIdx], g_rateDiv == 1 ? "" : g_rateDiv == 2 ? "2nd" : g_rateDiv == 3 ? "3rd" : "4th",
             g_vsync ? "on" : "off", fps, g_paused ? " (paused)" : "");
    SetWindowTextA(g_hwnd, t);
}

static void ToggleFullscreen()
{
    g_fullscreen = !g_fullscreen;
    if (g_fullscreen)
    {
        GetWindowRect(g_hwnd, &g_winRect);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    }
    else
    {
        SetWindowLongW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hwnd, nullptr, g_winRect.left, g_winRect.top, g_winRect.right - g_winRect.left,
                     g_winRect.bottom - g_winRect.top, SWP_FRAMECHANGED | SWP_NOZORDER);
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_SIZE: g_resize = true; return 0;
    case WM_KEYDOWN:
        switch (w)
        {
        case VK_ESCAPE: PostQuitMessage(0); break;
        case VK_UP: if (g_speedIdx < 7) g_speedIdx++; break;
        case VK_DOWN: if (g_speedIdx > 0) g_speedIdx--; break;
        case VK_RIGHT: g_pattern = (g_pattern + 1) % 4; break;
        case VK_LEFT: g_pattern = (g_pattern + 3) % 4; break;
        case '1': case '2': case '3': case '4': g_rateDiv = (int)(w - '0'); break;
        case 'V': g_vsync = !g_vsync; break;
        case 'F': case VK_F11: ToggleFullscreen(); break;
        case VK_SPACE: g_paused = !g_paused; break;
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

struct Rect { LONG x, y, w, h; };
static void Fill(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const float c[4], std::vector<D3D12_RECT> &rs)
{
    // A few rectangles per call: some layers on the present path (overlays, capture tools) handle
    // long rectangle lists badly, and the patterns with many tiles crashed there.
    for (size_t i = 0; i < rs.size(); i += 8)
        g_list->ClearRenderTargetView(rtv, c, (UINT)((rs.size() - i) < 8 ? (rs.size() - i) : 8), rs.data() + i);
    rs.clear();
}
static void Add(std::vector<D3D12_RECT> &rs, LONG x, LONG y, LONG w, LONG h)
{
    D3D12_RECT r{ x, y, x + w, y + h };
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > (LONG)g_w) r.right = (LONG)g_w;
    if (r.bottom > (LONG)g_h) r.bottom = (LONG)g_h;
    if (r.right > r.left && r.bottom > r.top) rs.push_back(r);
}

// A lane: band from y0 of height bh, a pattern repeating every `period` px, moving by `off`.
static void Lane(std::vector<D3D12_RECT> &rs, LONG y0, LONG bh, double off, int period, int barW, int kind)
{
    const int o = (int)fmod(off, (double)period);
    for (LONG x = -period + o; x < (LONG)g_w + period; x += period)
    {
        switch (kind)
        {
        case 0: Add(rs, x, y0, barW, bh); break;                                   // solid bars
        case 1: for (LONG y = y0, i = 0; y < y0 + bh; y += barW, i++)              // checkerboard
                    Add(rs, x + ((i & 1) ? barW : 0), y, barW, barW);
                break;
        case 2: Add(rs, x, y0, 2, bh); Add(rs, x + 6, y0, 1, bh); break;            // thin lines
        case 3: for (int k = 0; k < 4; k++) Add(rs, x + k * (barW / 4), y0 + k * (bh / 8), barW / 4, bh - k * (bh / 4)); break;
        }
    }
}

static void Draw(UINT bi, double t)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += bi * g_rtvSize;
    const float bg[4] = { 0.18f, 0.18f, 0.18f, 1 }, white[4] = { 0.92f, 0.92f, 0.92f, 1 }, dark[4] = { 0.04f, 0.04f, 0.04f, 1 },
                mid[4] = { 0.5f, 0.5f, 0.5f, 1 }, accent[4] = { 0.95f, 0.75f, 0.2f, 1 }, cyan[4] = { 0.3f, 0.8f, 0.95f, 1 };
    g_list->ClearRenderTargetView(rtv, bg, 0, nullptr);
    std::vector<D3D12_RECT> rs;
    const LONG H = (LONG)g_h, laneH = H / 6;
    const double px = g_pos;
    // three lanes at full, half and quarter speed
    for (int lane = 0; lane < 3; lane++)
    {
        const double off = px / (1 << lane);
        const LONG y0 = H / 12 + lane * (laneH + H / 24);
        Add(rs, 0, y0, (LONG)g_w, laneH); Fill(rtv, dark, rs);
        int kind = g_pattern;
        Lane(rs, y0 + laneH / 8, laneH * 3 / 4, off, 160, 40, kind);
        Fill(rtv, kind == 1 ? mid : white, rs);
        if (kind == 0)
        {   // the marker: a tall block with a contrasting core and thin edges, one per lane
            const LONG ux = (LONG)fmod(off, (double)g_w + 200) - 100, uy = y0 + laneH / 2;
            Add(rs, ux - 36, uy - laneH * 3 / 8, 72, laneH * 3 / 4); Fill(rtv, accent, rs);
            Add(rs, ux - 12, uy - laneH * 3 / 8, 24, laneH * 3 / 4); Fill(rtv, cyan, rs);
            Add(rs, ux - 2, uy - laneH * 3 / 8, 4, laneH * 3 / 4); Fill(rtv, dark, rs);
        }
    }
    // frame counter: 10 bits of the frame index -- a repeated frame shows the same pattern twice in a photo
    static UINT64 frameNo = 0; frameNo++;
    const LONG s = H / 40 > 8 ? H / 40 : 8, by = H - s * 2;
    for (int b = 0; b < 10; b++)
    {
        Add(rs, s + b * (s + s / 3), by, s, s);
        Fill(rtv, ((frameNo >> b) & 1) ? white : dark, rs);
    }
    // a sweeping column that moves one step per frame: gaps or doubles show missed or repeated frames
    const LONG cols = 30, cw = ((LONG)g_w - 20 * s) / cols, cx = 14 * s + (LONG)(frameNo % cols) * cw;
    Add(rs, 14 * s, by, cols * cw, s); Fill(rtv, dark, rs);
    Add(rs, cx, by, cw, s); Fill(rtv, accent, rs);
    (void)t;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BFIMotionTest";
    RegisterClassW(&wc);
    RECT r{ 0, 0, 1600, 900 }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"BFI Motion Test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);

    IDXGIFactory4 *fac = nullptr;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)), "CreateDXGIFactory2");
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)), "CreateCommandQueue");
    IDXGIFactory5 *f5 = nullptr;
    if (SUCCEEDED(fac->QueryInterface(IID_PPV_ARGS(&f5))))
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow))) g_tearing = allow != FALSE;
        f5->Release();
    }
    RECT rc; GetClientRect(g_hwnd, &rc); g_w = rc.right; g_h = rc.bottom;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_w; sd.Height = g_h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = kBuffers; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1 *sc1 = nullptr;
    CHECK(fac->CreateSwapChainForHwnd(g_queue, g_hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
    CHECK(sc1->QueryInterface(IID_PPV_ARGS(&g_sc)), "IDXGISwapChain3");
    sc1->Release();
    fac->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    fac->Release();
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kBuffers;
    CHECK(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)), "CreateDescriptorHeap");
    g_rtvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < kBuffers; i++) CHECK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i])), "CreateCommandAllocator");
    CHECK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list)), "CreateCommandList");
    g_list->Close();
    CHECK(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "CreateFence");
    g_fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CreateRTVs();
    // Most games resize their swap chain at start-up; BFI Presenter starts BFI at that first resize.
    g_resize = true;

    LARGE_INTEGER freq, now, last, fpsT0;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last); fpsT0 = last;
    int fpsFrames = 0; double fps = 0;
    UpdateTitle(0);
    MSG msg{};
    while (msg.message != WM_QUIT)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (msg.message == WM_QUIT) break;
        if (g_resize) { g_resize = false; Resize(); }
        QueryPerformanceCounter(&now);
        const double dt = double(now.QuadPart - last.QuadPart) / freq.QuadPart; last = now;
        if (!g_paused) g_pos += dt * kSpeeds[g_speedIdx];

        const UINT bi = g_sc->GetCurrentBackBufferIndex();
        if (g_fence->GetCompletedValue() < g_fenceVal[bi]) { g_fence->SetEventOnCompletion(g_fenceVal[bi], g_fenceEvt); WaitForSingleObject(g_fenceEvt, INFINITE); }
        g_alloc[bi]->Reset();
        g_list->Reset(g_alloc[bi], nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_bb[bi]; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_list->ResourceBarrier(1, &b);
        Draw(bi, double(now.QuadPart) / freq.QuadPart);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_list->ResourceBarrier(1, &b);
        g_list->Close();
        ID3D12CommandList *lists[] = { g_list };
        g_queue->ExecuteCommandLists(1, lists);
        // Rate 2..4 presents with a longer sync interval (one frame every 2nd..4th refresh), as a game capped below the refresh rate.
        const UINT interval = g_vsync ? (UINT)g_rateDiv : 0;
        g_sc->Present(interval, (!g_vsync && g_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        g_fenceVal[bi] = g_nextFence;
        g_queue->Signal(g_fence, g_nextFence++);

        fpsFrames++;
        const double el = double(now.QuadPart - fpsT0.QuadPart) / freq.QuadPart;
        if (el >= 0.5) { fps = fpsFrames / el; fpsFrames = 0; fpsT0 = now; UpdateTitle(fps); }
    }
    WaitIdle();
    return 0;
}
