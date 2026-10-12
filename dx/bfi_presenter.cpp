// ============================================================================
// BFI Presenter  --  ReShade add-on (Direct3D 12, 11 and 9); version: BFI_ADDON_VERSION below
//
// Copyright (C) 2026 artzox -- https://github.com/artzox/BFI-Presenter
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Presenter-clocked black frame insertion for games. The game (plus ReShade's
// effects, e.g. CRT-Standalone with ENABLE_DECAY=0) renders into virtual back
// buffers; each finished frame is copied aside, and a separate presenter thread
// shows the newest finished frame on EVERY display refresh, alternating lit and
// dark on the display's own clock. A late game frame repeats the last image
// (CRT-like stutter) instead of holding a lit or dark frame (flash).
//
// The lit/dark maths match CRT-Standalone's decay pass, applied to the final
// display signal: gain in display light (sRGB EOTF / scRGB / PQ), BFI with
// hue-preserving highlight roll-off, Variable MPRT brightness budget (spill),
// signal-relative dark floor, sine BFI, inverted cycle.
//
// Requirements: ReShade 6.4 or newer with add-on support, a D3D12 game using a
// flip-model swap chain, fixed refresh (VRR off), VSync behaviour is provided
// by the presenter itself.
//
// Settings: "BFI Presenter" window in the ReShade overlay (saved to
// BFIPresenter.ini next to this file).
// Hotkeys:  Ctrl+Shift+B  BFI on/off      Ctrl+Shift+M  BFI <-> Variable MPRT
//           Ctrl+Shift+R  reload ini
// Diagnostics: with Diagnostics=1 in the ini, D3D12 DRED is enabled and a
// device-removed report (last GPU operations, page-fault allocations) is
// written to ReShade.log and BFIPresenter_dred.txt.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WIDL_EXPLICIT_AGGREGATE_RETURNS   // MinGW: MSVC-compatible struct returns
#include <windows.h>
#include <tlhelp32.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <d3d11_4.h>
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#ifdef _MSC_VER
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#endif

extern "C" __declspec(dllexport) const char *NAME;
const char *NAME = "BFI Presenter";
extern "C" __declspec(dllexport) const char *DESCRIPTION;
const char *DESCRIPTION =
    "Presenter-clocked black frame insertion (D3D12): the lit/dark strobe runs on the display's clock, "
    "so game hitches become repeats instead of flashes. Settings in its own overlay window.";

// ---------------------------------------------------------------- logging
static void Log(reshade::log::level lvl, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    reshade::log::message(lvl, buf);
}
#define LOGI(...) Log(reshade::log::level::info, __VA_ARGS__)
#define LOGW(...) Log(reshade::log::level::warning, __VA_ARGS__)
#define LOGE(...) Log(reshade::log::level::error, __VA_ARGS__)

static double g_qpcFreq = 1.0;
static double Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / g_qpcFreq;
}

template <class T> static void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

#define BFI_ADDON_VERSION "1.0.2"

// ---------------------------------------------------------------- settings
struct Settings
{
    int   enabled  = 1;       // 0 = passthrough (presenter still owns timing)
    int   method   = 1;       // 0 = BFI, 1 = Variable MPRT (brightness budget)
    int   frames   = 2;       // sub-frames per cycle (2 = 60 Hz strobe at 120 Hz)
    float gain     = 0.5f;    // average light kept; lit gain = frames x gain
    float dfloor   = 0.0f;    // dark frame floor, fraction of the signal
    int   sine     = 0;
    int   invert   = 0;
    int   rolloff  = 1;       // BFI highlight roll-off
    int   rolling  = 0;       // rolling scan: one band per sub-frame, rolling down the screen
    float rollSeam = 0.08f;   // blended zone between bands, fraction of the screen height
    float rollDecay = 0.0f;   // rolling scan phosphor decay, in sub-frames (0 = off: hard bands + seam)
    float knee     = 0.75f;
    float peakNits = 1400.0f; // display peak (HDR ceiling)
    int   output   = -1;      // -1 auto, 0 SDR, 1 scRGB, 2 HDR10
    int   gameCap  = 1;       // 0 off, 1 = refresh rate, 2 = half refresh, 3 = quarter refresh (30 fps at 120 Hz)
    int   ownQueue = 1;       // present on the add-on's own queue (read when a swap chain is set up)
    int   pacingMode = 0;     // 0 = smooth (every game frame shown once, in order), 1 = lowest latency (newest)
    int   maxAhead = 1;       // game may run this many frames ahead of the GPU (0 = no limit)
    int   smoothCatchUp = 0;  // Smooth: 1 = skip a frame when two have waited for 3 cycles (standing latency)
    int   autoAhead = 1;      // automatic: 1 frame ahead, 2 while the game keeps missing strobe cycles
    int   presBuffers = 3;    // real swap chain buffers for the presenter (read when presentation moves)
    int   queueAhead = 0;     // with vblank pacing: keep this many extra presents queued (0 or 1)
    int   laterRelease = 0;   // Lowest latency + Refresh cap: release the game just in time for the pick
    int   capJit = 1;         // Half / Quarter cap: release just in time for the lit refresh (both pacing modes)
    float laterReleaseMs = 1.5f; // safety margin on top of the measured build time (widened automatically)
    int   deferRecreated = 1; // recreated swap chains: no BFI until the game resizes (safe)
    int   virtualStats = 1;   // report frame statistics in terms of the game's own frames
    int   reuseBuffers = 1;   // present an already-drawn buffer again instead of redrawing it
    int   waitResizeAll = 1;  // every swap chain: no BFI until the game first resizes it
    int   bypassResizeHooks = 1; // move to the presenter queue with DXGI's own ResizeBuffers1
    int   ablGuard = 1;       // lower the boost while a lit refresh would be too bright overall (panel ABL)
    int   ablNits = 700;      // lit-refresh ceiling (nits) while the guard is engaged
    int   ablTrigNits = 400;  // (v0.51 area mode, kept for the ini only)
    int   ablBudget = 250;    // panel budget: average brightness (nits) of a lit refresh after capping
    int   ablArea = 5;        // percent of the screen above the ceiling that counts as "large"
    int   hitchGuard = 1;     // pause BFI (show every refresh lit) while refreshes are missed or the game stalls
    int   hitchGapMs = 1000;  // no new game frame for this long counts as loading (shorter gameplay hitches don't)
    int   reportTyped = 1;    // stand-in back buffers report the swap chain's typed format from GetDesc
    int   queuePriority = 2;  // presenter GPU queue: 0 normal, 1 high, 2 realtime if allowed else high
    int   pacing   = 1;       // 1 = wait for vblank before each present, 0 = present queue only
    int   log      = 0;
    int   dx11     = 1;       // manage DX11 games (through the add-on's own D3D12 presenter)
};

static HMODULE  g_module = nullptr;
static char     g_iniPath[MAX_PATH] = {};
static char     g_logPath[MAX_PATH] = {};
static char     g_dredPath[MAX_PATH] = {};
static CRITICAL_SECTION g_cfgLock;             // guards g_shared and g_status
static Settings g_shared;                      // edited by overlay / hotkeys, read per refresh
static std::atomic<int> g_gameCap{1};
static std::atomic<UINT64> g_tickCount{0};             // refreshes presented, for the game frame cap
static std::atomic<int> g_tickSlot{0};        // strobe slot of the refresh about to be drawn at the last tick
static std::atomic<int> g_strobeFrames{2};
static std::atomic<int> g_maxAhead{1};          // in effect for the game (set by settings or the automatic mode)
static std::atomic<long long> g_capWaitUs{0};    // game time spent waiting in the frame cap (for the automatic mode)
static std::atomic<int> g_capWaits{0};
// Later release (Lowest latency + Refresh cap). The cap normally lets the game
// go at the vblank, a full refresh before the pick that shows its frame; a
// light game (118 fps, frames finished ~1 ms after release) then waited ~7 ms
// in the cap with its frame already rendered and its input already read.
// Instead the release is held until the pick's vblank minus the game's
// measured build time (release to frame finished on the GPU, 95th percentile
// of the last 128 frames) and a margin that widens on misses.
static std::atomic<bool>   g_lrAllowed{false};    // setting on, Lowest latency, Refresh cap, vblank ticks
static std::atomic<double> g_tickT{0.0};          // Now() of the last tick
static std::atomic<double> g_lrPeriod{1.0 / 120.0};
static std::atomic<double> g_lrLead{0.0};         // release this long before the pick's vblank (0 = at the tick)
static std::atomic<float>  g_lrBaseMs{1.5f};
static std::atomic<double> g_lrWindow{1.0 / 120.0}; // time between releases at the current cap (1, 2 or 4 refreshes)
static const int           kLr = 64;
static double              g_lrRelT[kLr] = {};     // when game frame id was released (CapWait of id-1)
static double              g_lrDeadline[kLr] = {}; // vblank it was released for (0 = not held back)
static UINT64              g_lrRelId[kLr] = {};
static UINT64              g_lrFence[kLr] = {};
static std::atomic<UINT64> g_lrStaged{0};          // newest frame id with g_lrFence set
static HANDLE              g_lrEv = nullptr;       // set when a frame is staged
static HANDLE              g_lrThread = nullptr;
static std::atomic<bool>   g_lrRun{false};
static std::atomic<int> g_virtualStats{1};
static std::atomic<int> g_reportTyped{1};
static int      g_diagnostics = 1;

static float IniFloat(const char *key, float def)
{
    char v[64], d[64];
    snprintf(d, sizeof(d), "%g", def);
    GetPrivateProfileStringA("BFI", key, d, v, sizeof(v), g_iniPath);
    return (float)atof(v);
}
static int IniInt(const char *key, int def) { return (int)GetPrivateProfileIntA("BFI", key, def, g_iniPath); }

static void LoadSettings(Settings &s)
{
    s.enabled  = IniInt("Enabled", 1);
    s.method   = IniInt("Method", 1) ? 1 : 0;
    s.frames   = IniInt("Frames", 2);
    if (s.frames < 2) s.frames = 2;
    if (s.frames > 8) s.frames = 8;
    s.gain     = IniFloat("Gain", 0.5f);
    s.dfloor   = IniFloat("DarkFloor", 0.0f);
    s.sine     = IniInt("SineBFI", 0);
    s.invert   = IniInt("InvertCycle", 0);
    s.rolloff  = IniInt("HighlightRolloff", 1);
    s.rolling  = IniInt("RollingScan", 0) ? 1 : 0;
    s.rollSeam = IniFloat("RollingSeam", 0.08f);
    if (s.rollSeam < 0.0f) s.rollSeam = 0.0f;
    if (s.rollSeam > 0.5f) s.rollSeam = 0.5f;
    s.rollDecay = IniFloat("RollingPhosphorDecay", 0.0f);
    if (s.rollDecay < 0.0f) s.rollDecay = 0.0f;
    if (s.rollDecay > 1.0f) s.rollDecay = 1.0f;
    s.knee     = IniFloat("RolloffKnee", 0.75f);
    s.peakNits = IniFloat("DisplayPeakNits", 1400.0f);
    s.gameCap  = IniInt("CapGameToRefresh", 1);
    if (s.gameCap < 0 || s.gameCap > 3) s.gameCap = 1;
    s.ownQueue = IniInt("PresentOnOwnQueue", 1);
    s.pacingMode = IniInt("FramePacing", 0) ? 1 : 0;
    s.maxAhead = IniInt("MaxFramesAhead", 1);
    s.autoAhead = IniInt("AutoFramesAhead", 1) ? 1 : 0;
    s.smoothCatchUp = IniInt("SmoothCatchUp", 0) ? 1 : 0;
    s.presBuffers = IniInt("PresenterBuffers", 3);
    s.queueAhead = IniInt("QueueAhead", 0) ? 1 : 0;
    s.laterRelease = IniInt("LaterRelease", 0) ? 1 : 0;
    s.capJit = IniInt("CapJustInTime", 1) ? 1 : 0;
    s.laterReleaseMs = IniFloat("LaterReleaseMarginMs", 1.5f);
    if (s.laterReleaseMs < 0.5f) s.laterReleaseMs = 0.5f;
    if (s.laterReleaseMs > 5.0f) s.laterReleaseMs = 5.0f;
    s.deferRecreated = IniInt("WaitForResizeOnRecreatedSwapChain", 1) ? 1 : 0;
    s.virtualStats = IniInt("GameFrameStatistics", 1) ? 1 : 0;
    s.reuseBuffers = IniInt("ReuseDrawnBuffers", 1) ? 1 : 0;
    s.waitResizeAll = IniInt("StartBFIAfterGameResize", 1) ? 1 : 0;
    s.bypassResizeHooks = IniInt("BypassOverlayResizeHooks", 1) ? 1 : 0;
    s.reportTyped = IniInt("TypedBackBufferDesc", 1) ? 1 : 0;
    s.hitchGuard = IniInt("HitchGuard", 1) ? 1 : 0;
    s.ablGuard = IniInt("AblGuard", 1) ? 1 : 0;
    s.ablNits = IniInt("AblGuardCapNits", 700);
    if (s.ablNits < 200) s.ablNits = 200;
    if (s.ablNits > 2000) s.ablNits = 2000;
    s.ablBudget = IniInt("AblGuardBudgetNits", 250);
    if (s.ablBudget < 100) s.ablBudget = 100;
    if (s.ablBudget > 1500) s.ablBudget = 1500;
    s.ablTrigNits = IniInt("AblGuardTriggerNits", 400);
    if (s.ablTrigNits < 100) s.ablTrigNits = 100;
    if (s.ablTrigNits > 2000) s.ablTrigNits = 2000;
    s.ablArea = IniInt("AblGuardArea", 5);
    if (s.ablArea < 1) s.ablArea = 1;
    if (s.ablArea > 50) s.ablArea = 50;
    // v0.54: new key. HitchGuardStallMs (150) paused BFI on ordinary gameplay
    // hitches (Control: 190 ms), which is no longer wanted.
    s.hitchGapMs = IniInt("HitchGuardLoadingMs", 1000);
    if (s.hitchGapMs < 150) s.hitchGapMs = 150;
    if (s.hitchGapMs > 5000) s.hitchGapMs = 5000;
    g_reportTyped.store(s.reportTyped);
    s.queuePriority = IniInt("PresenterQueuePriority", 2);
    if (s.queuePriority < 0 || s.queuePriority > 2) s.queuePriority = 2;
    g_virtualStats.store(s.virtualStats);
    if (s.presBuffers < 2) s.presBuffers = 2;
    if (s.presBuffers > 4) s.presBuffers = 4;
    if (s.maxAhead < 0) s.maxAhead = 0;
    if (s.maxAhead > 3) s.maxAhead = 3;
    g_maxAhead.store(s.autoAhead && s.maxAhead ? 1 : s.maxAhead);
    s.pacing   = IniInt("WaitForVBlank", 1);
    s.log      = IniInt("Log", 0);
    s.dx11     = IniInt("DX11Support", 1) ? 1 : 0;
    char out[32];
    GetPrivateProfileStringA("BFI", "Output", "auto", out, sizeof(out), g_iniPath);
    s.output = !_stricmp(out, "sdr") ? 0 : !_stricmp(out, "scrgb") ? 1 : !_stricmp(out, "hdr10") ? 2 : -1;
    g_gameCap.store(s.gameCap);
}

// When g_iniOnlyMissing is set, writes go only to keys the file doesn't have
// yet (see UpgradeIni): your values are never touched.
static bool g_iniOnlyMissing = false;
static char g_iniKeys[8192];                 // keys present in [BFI], double-null list
static char g_iniAdded[512];                 // names added by UpgradeIni, for the log
static bool IniHasKey(const char *key)
{
    for (const char *k = g_iniKeys; *k; k += strlen(k) + 1)
        if (_stricmp(k, key) == 0) return true;
    return false;
}
static void IniWrite(const char *key, const char *v)
{
    if (g_iniOnlyMissing)
    {
        if (IniHasKey(key)) return;
        if (strlen(g_iniAdded) + strlen(key) + 3 < sizeof(g_iniAdded))
        {
            if (g_iniAdded[0]) strcat(g_iniAdded, ", ");
            strcat(g_iniAdded, key);
        }
    }
    WritePrivateProfileStringA("BFI", key, v, g_iniPath);
}
static void SaveSettings(const Settings &s);
// Keep your BFIPresenter.ini across updates: settings a new version adds are
// appended with their defaults, everything already in the file is left as it
// is. A missing file is created with every setting at its default
// (BFIPresenter.example.ini holds the commented reference).
static void UpgradeIni(const Settings &s)
{
    g_iniKeys[0] = g_iniKeys[1] = 0;
    GetPrivateProfileStringA("BFI", nullptr, "", g_iniKeys, sizeof(g_iniKeys), g_iniPath);
    const bool fresh = g_iniKeys[0] == 0;
    g_iniAdded[0] = 0;
    g_iniOnlyMissing = true;
    SaveSettings(s);
    if (!IniHasKey("Diagnostics")) IniWrite("Diagnostics", "1");
    if (!IniHasKey("AllowRTSS"))   IniWrite("AllowRTSS", "1");
    g_iniOnlyMissing = false;
    if (fresh)
        LOGI("BFI Presenter: created BFIPresenter.ini with default settings (see BFIPresenter.example.ini for what each does)");
    else if (g_iniAdded[0])
        LOGI("BFI Presenter: added new settings to your BFIPresenter.ini with their defaults: %s", g_iniAdded);
}

static void LoadShared()
{
    Settings s;
    LoadSettings(s);
    static bool upgraded = false;
    if (!upgraded) { upgraded = true; UpgradeIni(s); }
    EnterCriticalSection(&g_cfgLock);
    g_shared = s;
    LeaveCriticalSection(&g_cfgLock);
}

static void IniWriteInt(const char *key, int v) { char b[32]; snprintf(b, sizeof(b), "%d", v); IniWrite(key, b); }
static void IniWriteFloat(const char *key, float v) { char b[32]; snprintf(b, sizeof(b), "%.3f", v); IniWrite(key, b); }

static void SaveSettings(const Settings &s)
{
    IniWriteInt("Enabled", s.enabled);
    IniWriteInt("Method", s.method);
    IniWriteInt("Frames", s.frames);
    IniWriteFloat("Gain", s.gain);
    IniWriteFloat("DarkFloor", s.dfloor);
    IniWriteInt("SineBFI", s.sine);
    IniWriteInt("InvertCycle", s.invert);
    IniWriteInt("HighlightRolloff", s.rolloff);
    IniWriteInt("RollingScan", s.rolling);
    IniWriteFloat("RollingSeam", s.rollSeam);
    IniWriteFloat("RollingPhosphorDecay", s.rollDecay);
    IniWriteFloat("RolloffKnee", s.knee);
    IniWriteFloat("DisplayPeakNits", s.peakNits);
    IniWrite("Output", s.output == 0 ? "sdr" : s.output == 1 ? "scrgb" : s.output == 2 ? "hdr10" : "auto");
    IniWriteInt("CapGameToRefresh", s.gameCap);
    IniWriteInt("PresentOnOwnQueue", s.ownQueue);
    IniWriteInt("FramePacing", s.pacingMode);
    IniWriteInt("MaxFramesAhead", s.maxAhead);
    IniWriteInt("AutoFramesAhead", s.autoAhead);
    IniWriteInt("SmoothCatchUp", s.smoothCatchUp);
    IniWriteInt("PresenterBuffers", s.presBuffers);
    IniWriteInt("QueueAhead", s.queueAhead);
    IniWriteInt("LaterRelease", s.laterRelease);
    IniWriteInt("CapJustInTime", s.capJit);
    IniWriteFloat("LaterReleaseMarginMs", s.laterReleaseMs);
    IniWriteInt("WaitForResizeOnRecreatedSwapChain", s.deferRecreated);
    IniWriteInt("GameFrameStatistics", s.virtualStats);
    IniWriteInt("ReuseDrawnBuffers", s.reuseBuffers);
    IniWriteInt("StartBFIAfterGameResize", s.waitResizeAll);
    IniWriteInt("BypassOverlayResizeHooks", s.bypassResizeHooks);
    IniWriteInt("PresenterQueuePriority", s.queuePriority);
    IniWriteInt("TypedBackBufferDesc", s.reportTyped);
    IniWriteInt("HitchGuard", s.hitchGuard);
    IniWriteInt("AblGuard", s.ablGuard);
    IniWriteInt("AblGuardCapNits", s.ablNits);
    IniWriteInt("AblGuardArea", s.ablArea);
    IniWriteInt("AblGuardBudgetNits", s.ablBudget);
    IniWriteInt("HitchGuardLoadingMs", s.hitchGapMs);
    IniWriteInt("DX11Support", s.dx11);
    IniWriteInt("WaitForVBlank", s.pacing);
    IniWriteInt("Log", s.log);
}

// ---------------------------------------------------------------- status (overlay)
struct Status
{
    char   reason[200] = "Waiting for a swap chain...";
    int    managed = 0;
    int    running = 0;
    UINT   width = 0, height = 0, buffers = 0;
    UINT   format = 0;
    int    outType = -1;
    double refreshHz = 0.0, gameFps = 0.0, frameAgeMs = 0.0;
    int    late = 0, repeats = 0;
    int    frameRepeats = 0, frameSkips = 0;
    int    unevenRep = 0, unevenSkip = 0;           // per second: strobe cycles one or more frames short / ahead of the expected step
    int    slotWaits = 0;                       // per second: game copies that waited for the presenter
    bool   vsyncOverridden = false;             // presents far faster than the display: driver ignores VSync
    int    queued = -1;                         // presents queued but not yet on screen (-1 = unknown)
    bool   hitchPaused = false;                 // hitch guard has BFI paused
    double displayHz = 0.0;                     // the display's own refresh rate (frame statistics)
    bool   vrrSuspect = false;
    bool   limitSuspect = false;
    int    lrState = 0;         // later release: 0 off, 1 measuring, 2 active, 3 paused (frames take too long)
    double lrEst = 0.0, lrMargin = 0.0;
    int    lrMisses = 0;
    int    aheadInUse = 1;
    double rawAvg  = -1.0;                      // ABL guard: unclipped lit-refresh average (nits)
    double hotArea = -1.0;                      // ABL guard: percent of the screen whose lit refresh exceeds the cap
    double ceilInUse = 0.0;                     // ABL guard: lit-refresh ceiling in effect (nits)                  // display refreshing slower than it can, with no missed refreshes
    int    reused = 0;                          // per second: refreshes that reused an already-drawn buffer   // per second: strobe cycles with no new game frame / frames never shown
    int    deviceLost = 0;
};
static Status g_status;
static char   g_hooksSeen[256] = "";               // other Present hooks found in the process
static char   g_fgSeen[256] = "";                  // frame-generation components found in the process
static char   g_scOwner[128] = "";                 // module that implements the swap chain ReShade sees
static std::atomic<bool> g_firstStaged{false}, g_firstPresented{false};
static std::atomic<double> g_lastPresentT{0.0};   // presenter heartbeat
static std::atomic<bool>   g_stallLogged{false};
static std::atomic<int> g_stagedThisSecond{0};

static void SetReason(int managed, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_cfgLock);
    g_status.managed = managed;
    strncpy(g_status.reason, buf, sizeof(g_status.reason) - 1);
    g_status.reason[sizeof(g_status.reason) - 1] = '\0';
    LeaveCriticalSection(&g_cfgLock);
    char line[240];
    snprintf(line, sizeof(line), "BFI Presenter: %s", buf);
    reshade::log::message(managed ? reshade::log::level::info : reshade::log::level::warning, line);
}

// ---------------------------------------------------------------- swap chain vtable hooks
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBuffer)(IDXGISwapChain *, UINT, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef UINT    (STDMETHODCALLTYPE *PFN_GetCBBI)(IDXGISwapChain3 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers1)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT *, IUnknown *const *);

// IDXGISwapChain4 vtable slots
enum { VT_SetFullscreenState = 10, VT_GetFullscreenState = 11, VT_SetColorSpace1 = 38 };
enum { VT_Present = 8, VT_GetBuffer = 9, VT_ResizeBuffers = 13, VT_Present1 = 22,
       VT_GetDesc = 12, VT_GetDesc1 = 18, VT_GetFrameStatistics = 16, VT_GetLastPresentCount = 17,
       VT_SetMaximumFrameLatency = 31, VT_GetMaximumFrameLatency = 32, VT_GetFrameLatencyWaitableObject = 33,
       VT_GetCurrentBackBufferIndex = 36, VT_ResizeBuffers1 = 39 };
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetMFL)(IDXGISwapChain2 *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetMFL)(IDXGISwapChain2 *, UINT *);
typedef HANDLE  (STDMETHODCALLTYPE *PFN_GetFLWO)(IDXGISwapChain2 *);

static PFN_Present        o_Present = nullptr;
static PFN_GetBuffer      o_GetBuffer = nullptr;
static PFN_ResizeBuffers  o_ResizeBuffers = nullptr;
static PFN_Present1       o_Present1 = nullptr;
static PFN_GetCBBI        o_GetCBBI = nullptr;
static PFN_ResizeBuffers1 o_ResizeBuffers1 = nullptr;
static PFN_SetMFL         o_SetMFL = nullptr;
static PFN_GetMFL         o_GetMFL = nullptr;
static PFN_GetFLWO        o_GetFLWO = nullptr;
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc)(IDXGISwapChain *, DXGI_SWAP_CHAIN_DESC *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc1)(IDXGISwapChain1 *, DXGI_SWAP_CHAIN_DESC1 *);
static PFN_GetDesc        o_GetDesc = nullptr;
static PFN_GetDesc1       o_GetDesc1 = nullptr;
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetFS)(IDXGISwapChain *, DXGI_FRAME_STATISTICS *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetLPC)(IDXGISwapChain *, UINT *);
static PFN_GetFS          o_GetFS = nullptr;
static PFN_GetLPC         o_GetLPC = nullptr;
// Frame statistics as the game should see them: the real ones count the
// presenter's 120 presents per second, not the game's frames, which can
// confuse games that pace themselves with them.
static std::atomic<int>    g_lostFrames{0};         // finished frames overwritten before they were shown
static std::atomic<UINT64> g_shownGameFrames{0};    // id of the newest game frame shown (earlier ones count as done)
static std::atomic<bool>   g_statsReadLogged{false};

// The game sees g_virtCount buffers; the real swap chain may have more
// (PresenterBuffers) so the presenter can queue one refresh ahead and ride
// out a late GPU/CPU slot without a lit or dark frame being held twice.
static UINT               g_virtCount = 0;
static UINT               g_queuePriority = 0;   // priority the presenter queue got

// The game's frame-latency waitable. DXGI's own object is driven by the
// presenter's 120 Hz presents: at a fixed refresh they keep the flip queue
// full, so it almost never let the game through (Control with G-SYNC off:
// six frames a minute). The game gets this semaphore instead, released each
// time the presenter takes a new game frame.
static HANDLE            g_waitSem = nullptr;
static std::atomic<UINT> g_maxLatency{1};
static void ReleaseGameSlot() { if (g_waitSem) ReleaseSemaphore(g_waitSem, 1, nullptr); }
// Slots are released once per game frame. Smooth pacing releases a frame's
// slot when the presenter takes it (or a later one), which paces the game to
// the strobe. Lowest latency releases it as soon as the frame is staged:
// releasing only at the 60 Hz picks capped Unity games (Prince of Persia: The
// Lost Crown) at 60 fps -- with two slots and a frame still on the GPU at
// each pick, only one slot came back per strobe cycle.
static void ReleaseGameSlots(UINT64 n);
static std::atomic<UINT64> g_releasedUpTo{0};      // game frames whose slot has been given back
static std::atomic<bool>   g_releaseOnSubmit{false};
static void ReleaseUpTo(UINT64 id)
{
    UINT64 prev = g_releasedUpTo.load();
    while (id > prev && !g_releasedUpTo.compare_exchange_weak(prev, id)) {}
    if (id > prev) ReleaseGameSlots(id - prev);
}
// Back to a full set of slots. When the presenter stops (resize, swap chain
// change) the frames it held are dropped and will never release theirs, so a
// game that waits on the waitable before its next frame waited forever -- and
// the presenter only restarts on that next frame (No Rest for the Wicked froze
// on a resize after a 1 s loading pause).
static void TopUpGameSlots()
{
    if (!g_waitSem) return;
    while (WaitForSingleObject(g_waitSem, 0) == WAIT_OBJECT_0) {}
    ReleaseSemaphore(g_waitSem, (LONG)g_maxLatency.load(), nullptr);
}
// One release per game frame the presenter is done with -- shown or skipped.
// Releasing only per shown frame starved games whose present rate exceeds
// the strobe's 60 lit refreshes (frame generation: two presents per game
// frame), throttling Pragmata from 76 to 15-30 fps. The semaphore's maximum
// caps how far ahead the game can get.
static void ReleaseGameSlots(UINT64 n)
{
    if (!g_waitSem) return;
    for (UINT64 i = 0; i < n && i < 16; i++)
        if (!ReleaseSemaphore(g_waitSem, 1, nullptr)) break;
}
static void             **g_hookedVtbl = nullptr;

// ---------------------------------------------------------------- shared state
static const UINT kMaxBuf = 8;
// Ring of finished game frames. Six, not four: in a GPU-bound game with two
// frames ahead, a copy finishes ~3 game frames after it is queued, so one slot
// shown + three in flight left no room for a finished frame to wait for its
// strobe cycle -- the game overwrote it unseen and only every 3rd-5th frame
// reached the screen (God of War with DLSS 5 NR: 37 fps rendered, ~11 shown).
static const UINT kStage  = 6;

static CRITICAL_SECTION       g_lock;
static CRITICAL_SECTION       g_submitLock;      // serialises game-side Present calls (FG add-ons present from other threads)
static DWORD                  g_presentThreads[4] = {};
static std::atomic<HWND>      g_watchHwnd{nullptr};    // game window the watchdog checks for "not responding"
static std::atomic<DWORD>     g_uiTid{0};              // DX11: thread that owns the presenter's child window
static IDXGISwapChain3       *g_sc = nullptr;    // managed real swap chain (not AddRef'd)
static reshade::api::swapchain *g_rsSwapchain = nullptr;
static std::atomic<bool>      g_active{false};   // hooks redirect for g_sc
static ID3D12Device          *g_device = nullptr;
static ID3D12CommandQueue    *g_gameQueue = nullptr;

static DXGI_SWAP_CHAIN_DESC1  g_desc = {};
static ID3D12Resource        *g_virt[kMaxBuf] = {};
static UINT                   g_bufCount = 0;
static UINT                   g_cur = 0;

static ID3D12Resource        *g_stage[kStage] = {};
static UINT64                 g_stageFence[kStage] = {};   // game-queue fence value of the copy
static UINT64                 g_stageId[kStage] = {};      // game frame number (0 = empty)
static double                 g_stageTime[kStage] = {};
static UINT64                 g_readFence[kStage] = {};    // presenter fence value of the last read
static int                    g_presCurSlot = -1;          // staging slot the presenter is showing (g_lock)
static std::atomic<int>       g_slotWaits{0};
// Swap chains the game recreates (God of War Ragnarok on loading) hung when
// the presenter presented on the game's own queue, and also when moved to
// the presenter's queue from inside Present. For those, the game presents
// its own frames (no BFI) until it resizes the swap chain; the move to the
// presenter's queue then happens inside that resize, and BFI starts.
static int                    g_swapChainsManaged = 0;
static std::atomic<bool>      g_deferPresenter{false};              // game copies that had to wait for the presenter
// Per-game-frame timing (written to BFIPresenter_game_<date>_<time>.csv while
// the timing log is on). Times are Now() seconds; serialised by g_submitLock.
static std::atomic<bool>      g_logOn{false};
static double                 g_gtEnter = 0, g_gtSerial = 0, g_gtSlot = 0, g_gtLock = 0, g_gtCopy = 0,
                              g_gtAhead = 0, g_gtExit = 0, g_gtPrevExit = 0;
static int                    g_gtFallback = 0;
static FILE                  *g_gameLog = nullptr;
static UINT                   g_stageNext = 0;
static UINT64                 g_frameId = 0;
static UINT64                 g_recentFence[8] = {};      // copy fence of the last 8 game frames

// game-queue copy resources
static ID3D12CommandAllocator    *g_copyAlloc[kStage] = {};
static ID3D12GraphicsCommandList *g_copyList = nullptr;
static ID3D12Fence               *g_gameFence = nullptr;
static UINT64                     g_gameFenceVal = 0;
static HANDLE                     g_gameFenceEvent = nullptr;

// presenter
static ID3D12Fence           *g_presFence = nullptr;
static ID3D12CommandQueue    *g_presQueue = nullptr;      // presents run on this queue
static bool                   g_ownPresentQueue = false;  // swap chain rebound to g_presQueue
#ifdef _WIN64
#define BFI_TLS(type, name) static thread_local type name = 0
#else
// 32-bit: plain Win32 TLS. MinGW's emulated thread_local crashed inside
// __emutls_get_address on the first swap chain (Twelve Minutes, ReShade
// loads and unloads add-ons several times while the game starts).
template <class T> struct TlsVar
{
    DWORD idx = TlsAlloc();
    T get() const { const DWORD e = GetLastError(); T v = (T)(intptr_t)TlsGetValue(idx); SetLastError(e); return v; }
    void set(T v) { TlsSetValue(idx, (LPVOID)(intptr_t)v); }
    operator T() const { return get(); }
    TlsVar &operator=(T v) { set(v); return *this; }
    TlsVar &operator++() { set((T)(get() + 1)); return *this; }
    TlsVar &operator--() { set((T)(get() - 1)); return *this; }
    T operator++(int) { T v = get(); set((T)(v + 1)); return v; }
    T operator--(int) { T v = get(); set((T)(v - 1)); return v; }
};
#define BFI_TLS(type, name) static TlsVar<type> name
#endif
BFI_TLS(bool, t_presenterThread);   // calls made on the presenter thread
BFI_TLS(int, t_ownCall);
struct OwnCall { OwnCall() { t_ownCall++; } ~OwnCall() { t_ownCall--; } };
static HRESULT OwnPresent();
static std::atomic<bool>      g_needRebind{false};        // move presentation to g_presQueue on the first frame
static std::atomic<bool>      g_presenterFailed{false};   // presenter could not start: plain presentation
static ID3D12CommandAllocator    *g_fbAlloc[kMaxBuf] = {};  // fallback path (presenter failed)
static ID3D12GraphicsCommandList *g_fbList = nullptr;
static UINT64                     g_fbFence[kMaxBuf] = {};
static UINT64                 g_presFenceVal = 0;          // highest value reserved
static HANDLE                 g_thread = nullptr;
static std::atomic<bool>      g_running{false};
static HANDLE                 g_tickEvent = nullptr;
static std::atomic<HRESULT>   g_lastPresentHr{S_OK};

// ---------------------------------------------------------------- Direct3D 11 front end (declarations)
// DX11 games keep their own swap chain and device (DLSS, ReShade and the CRT
// shader work as usual), but their swap chain never presents: each finished
// frame is copied into the staging ring, which here lives on a D3D12 device of
// the add-on's own (shared textures and a shared fence), and the D3D12
// presenter shows it through a swap chain of its own on a child window that
// covers the game's window.
static std::atomic<bool> g_dx11{false};
// DX11GamePresent=1: the game's own swap chain still presents each frame
// (sync interval 0, under the presenter's window), so the game's driver sees
// its frame boundaries. Off by default: the game's swap chain normally never
// presents. For games that slow down more and more without it (Wreckfest).
static std::atomic<int> g_gamePresent11{0};
static IDXGISwapChain3 *g_sc11 = nullptr;          // the game's DX11 swap chain (not AddRef'd)
static bool IsGame11(void *sc) { return g_dx11.load() && g_active.load() && sc == (void *)g_sc11; }
static HRESULT SubmitGameFrame11(IDXGISwapChain *sc, UINT sync, UINT flags);
static HRESULT Resize11(IDXGISwapChain *sc, UINT cnt, UINT w, UINT h, DXGI_FORMAT f, UINT fl,
                        const UINT *mask, IUnknown *const *queues, bool one);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetFS)(IDXGISwapChain *, BOOL, IDXGIOutput *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GetFS2)(IDXGISwapChain *, BOOL *, IDXGIOutput **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetCS1)(IDXGISwapChain3 *, DXGI_COLOR_SPACE_TYPE);
static PFN_SetFS  o_SetFS  = nullptr;
static PFN_GetFS2 o_GetFSt = nullptr;
static PFN_SetCS1 o_SetCS1 = nullptr;
static BOOL g_fsWanted = FALSE;                    // what the game asked for (exclusive fullscreen is held off)
static std::atomic<int> g_cs11{-1};                // colour space the game set on its DX11 swap chain



// ---------------------------------------------------------------- helpers
static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = {};
    br.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource   = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter  = b;
    return br;
}

// Sleep until a Now() time with sub-millisecond accuracy: a high-resolution
// waitable timer for most of it, a short spin for the last 0.3 ms.
static void SleepUntil(double target, HANDLE timer)
{
    double left = target - Now();
    if (left > 0.0004 && timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((left - 0.0003) * 1e7);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 20);
    }
    while (Now() < target) YieldProcessor();
}

// A global realtime GPU queue needs SeIncreaseBasePriorityPrivilege enabled in
// the process token. An administrator's token holds it, but disabled, so the
// queue fell back to high priority even with the game run as administrator.
static bool EnableBasePriorityPrivilege()
{
    static int done = -1;
    if (done >= 0) return done == 1;
    done = 0;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el = {};
    DWORD len = 0;
    const bool elevated = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &len) && el.TokenIsElevated;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    DWORD err = 0;
    if (!LookupPrivilegeValueW(nullptr, L"SeIncreaseBasePriorityPrivilege", &tp.Privileges[0].Luid)) err = GetLastError();
    else if (!AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr)) err = GetLastError();
    else err = GetLastError();                       // ERROR_NOT_ALL_ASSIGNED (1300): not held at all
    if (err == ERROR_SUCCESS) done = 1;
    CloseHandle(tok);
    if (done == 1)
        LOGI("BFI Presenter: realtime GPU priority privilege enabled (process %selevated)", elevated ? "" : "not ");
    else
        LOGW("BFI Presenter: realtime GPU priority privilege not available -- the game process is %s (error %lu). %s",
             elevated ? "elevated" : "NOT elevated", (unsigned long)err,
             elevated ? "Its token lacks 'Increase scheduling priority' (Local Security Policy, User Rights Assignment)."
                      : "Steam and other launchers restart the game without administrator rights: run the launcher itself "
                        "as administrator, or grant your account 'Increase scheduling priority' in Local Security Policy.");
    return done == 1;
}

// Later release: time each game frame from its release to its copy finishing
// on the GPU, and once a second turn that into the release lead.
static void LrStaged(UINT64 id, UINT64 fence)
{
    g_lrFence[id % kLr] = fence;
    g_lrStaged.store(id);
    if (g_lrEv) SetEvent(g_lrEv);
}
static DWORD WINAPI LrWatchThread(void *)
{
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    UINT64 next = g_lrStaged.load() + 1;
    double builds[128] = {}, sorted[128];
    int nb = 0, wb = 0, state = 0, misses = 0, frames = 0, clean = 0, good = 0;
    double margin = g_lrBaseMs.load() * 0.001, est = 0.0, secT = Now(), lastWindow = 0.0;
    while (g_lrRun.load())
    {
        const UINT64 staged = g_lrStaged.load();
        if (staged < next) WaitForSingleObject(g_lrEv, 50);
        else
        {
            if (staged > next + kLr / 2) next = staged;        // fell behind: skip ahead
            const UINT64 id = next++;
            const int k = (int)(id % kLr);
            const UINT64 f = g_lrFence[k];
            if (g_gameFence && ev && g_gameFence->GetCompletedValue() < f)
            {
                g_gameFence->SetEventOnCompletion(f, ev);
                WaitForSingleObject(ev, 100);
            }
            const double fin = Now();
            if (g_lrRelId[k] == id)
            {
                const double b = fin - g_lrRelT[k];
                if (b > 0.0 && b < 0.1) { builds[wb] = b; wb = (wb + 1) % 128; if (nb < 128) nb++; }
                if (g_lrDeadline[k] > 0.0) { frames++; if (fin > g_lrDeadline[k]) misses++; }
            }
        }
        const double now = Now();
        if (now - secT < 1.0) continue;
        secT = now;
        const bool allowed = g_lrAllowed.load();
        // The release can come at most one cap interval (1, 2 or 4 refreshes) before its pick.
        const double period = g_lrWindow.load(), base = g_lrBaseMs.load() * 0.001;
        const bool capMode = period > 1.5 * g_lrPeriod.load();   // Half / Quarter cap
        if (fabs(period - lastWindow) > 1e-4) { lastWindow = period; if (state != 0) { state = 0; nb = wb = 0; margin = base; } }
        if (margin < base) margin = base;
        if (nb >= 30)
        {
            memcpy(sorted, builds, nb * sizeof(double));
            std::sort(sorted, sorted + nb);
            est = sorted[std::min(nb - 1, (int)(nb * 0.95))];
        }
        const int prev = state;
        if (!allowed) { state = 0; nb = wb = 0; margin = base; clean = good = 0; lastWindow = 0.0; }
        else if (state == 0) { state = 1; nb = wb = 0; }
        else if (state == 1)
        {
            if (nb >= 60) state = (est + margin < period - 0.002) ? 2 : 3;
        }
        else if (state == 2 || state == 3)
        {
            // Margin: wider after misses (at most 3 ms over the base), narrower
            // after 5 clean seconds -- also while at the limit, so a margin
            // widened in a heavy scene doesn't keep it there afterwards.
            if (misses >= 2)
            {
                const double m0 = margin;
                margin = std::min(margin + 0.001, base + 0.003); clean = 0;
                if (margin > m0)
                    LOGI("BFI Presenter: just-in-time release: %d of %d frames missed their refresh -- margin widened to %.1f ms",
                         misses, frames, margin * 1000.0);
            }
            else if (misses == 0)
            {
                if (++clean >= 5 && margin > base) { margin = std::max(base, margin - 0.0005); clean = 0; }
            }
            else clean = 0;
            if (state == 2) { if (est + margin > period - 0.001) { state = 3; good = 0; } }
            else
            {
                good = (est + margin < period - 0.002) ? good + 1 : 0;
                if (good >= 3) { state = 2; good = 0; }
            }
        }
        if (state != prev && (state == 2 || state == 3))
        {
            if (state == 2)
                LOGI("BFI Presenter: just-in-time release active -- game frames take %.1f ms (95th percentile), released %.1f ms "
                     "before the refresh that shows them (cap interval %.1f ms)", est * 1000.0, (est + margin) * 1000.0, period * 1000.0);
            else
                LOGI("BFI Presenter: just-in-time release at its limit -- game frames take %.1f ms, close to the %.1f ms cap "
                     "interval; %s", est * 1000.0, period * 1000.0,
                     capMode ? "released as early as the cap allows" : "released at the refresh as before");
        }
        // At its limit: at the Half / Quarter cap, still aim every frame at a
        // lit refresh, released as early as the cap allows (1 ms after the
        // previous lit one) -- falling back to the release one refresh before
        // the lit refresh gave those frames only 8 ms and brought back the
        // repeat-then-skip judder (Obduction). At the Refresh cap the earliest
        // release is the vblank itself, i.e. the normal behaviour.
        double lead = 0.0;
        if (state == 2) lead = est + margin;
        else if (state == 3 && capMode) lead = period - 0.001;
        g_lrLead.store(lead);
        EnterCriticalSection(&g_cfgLock);
        g_status.lrState  = state;
        g_status.lrEst    = est * 1000.0;
        g_status.lrMargin = margin * 1000.0;
        g_status.lrMisses = misses;
        LeaveCriticalSection(&g_cfgLock);
        misses = frames = 0;
    }
    g_lrLead.store(0.0);
    if (ev) CloseHandle(ev);
    return 0;
}

static void WaitFence(ID3D12Fence *f, UINT64 v, HANDLE ev)
{
    if (f->GetCompletedValue() < v)
    {
        f->SetEventOnCompletion(v, ev);
        WaitForSingleObject(ev, 2000);
    }
}

// 8-bit back buffers may be viewed as sRGB by the game. Real swap chain
// buffers allow that; ordinary textures only if they are TYPELESS, so the
// stand-ins are created typeless and views without a typed format are fixed
// up in the create_resource_view event.
static DXGI_FORMAT VirtualFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default:                         return f;
    }
}

static ID3D12Resource *CreateTex(const DXGI_SWAP_CHAIN_DESC1 &d, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                 const wchar_t *name, UINT index)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = d.Width;
    rd.Height           = d.Height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = format;
    rd.SampleDesc.Count = 1;
    rd.Flags            = flags;
    ID3D12Resource *r = nullptr;
    HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) LOGE("BFI Presenter: CreateCommittedResource failed (0x%08lX)", (unsigned long)hr);
    else
    {
        wchar_t n[64];
        swprintf(n, 64, L"%ls %u", name, index);
        r->SetName(n);
    }
    return r;
}

// The stand-in back buffers are typeless (so the game can create sRGB as well
// as UNORM views, as on real swap-chain buffers), but a real swap-chain buffer
// reports its typed format from ID3D12Resource::GetDesc. Games that create
// their own screen-size render targets from that description (The Last of Us
// Part I) then passed a typeless clear value, every such texture failed to
// create, and the first clear of one removed the device. GetDesc on a stand-in
// now reports the swap chain's typed format. (The hook sits in the shared
// resource vtable and only changes the answer for the stand-ins.)
typedef D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *PFN_ResGetDesc)(ID3D12Resource *, D3D12_RESOURCE_DESC *);
static void          **g_resVtbl[4] = {};
static PFN_ResGetDesc  g_resGetDesc[4] = {};
static D3D12_RESOURCE_DESC *STDMETHODCALLTYPE hk_ResGetDesc(ID3D12Resource *self, D3D12_RESOURCE_DESC *out);
static D3D12_RESOURCE_DESC *CallOrigResGetDesc(ID3D12Resource *self, D3D12_RESOURCE_DESC *out)
{
    void **vt = *(void ***)self;
    for (int i = 0; i < 4; i++)
        if (g_resVtbl[i] == vt && g_resGetDesc[i]) return g_resGetDesc[i](self, out);
    for (int i = 0; i < 4; i++)                       // an object whose vtable we didn't patch: first known original
        if (g_resGetDesc[i]) return g_resGetDesc[i](self, out);
    memset(out, 0, sizeof(*out));
    return out;
}
static D3D12_RESOURCE_DESC *STDMETHODCALLTYPE hk_ResGetDesc(ID3D12Resource *self, D3D12_RESOURCE_DESC *out)
{
    CallOrigResGetDesc(self, out);
    if (g_reportTyped.load() && !t_presenterThread && !t_ownCall)
        for (UINT i = 0; i < kMaxBuf; i++)
            if (g_virt[i] == self)
            {
                const DXGI_FORMAT typed = g_desc.Format;
                if (VirtualFormat(typed) == out->Format) out->Format = typed;
                break;
            }
    return out;
}
static void HookResourceGetDesc(ID3D12Resource *r)
{
    if (!r) return;
    void **vt = *(void ***)r;
    int freeSlot = -1;
    for (int i = 0; i < 4; i++)
    {
        if (g_resVtbl[i] == vt) return;               // already hooked
        if (!g_resVtbl[i] && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot < 0 || vt[10] == (void *)&hk_ResGetDesc) return;
    DWORD old;
    if (!VirtualProtect(&vt[10], sizeof(void *), PAGE_READWRITE, &old)) return;
    g_resGetDesc[freeSlot] = (PFN_ResGetDesc)vt[10];
    g_resVtbl[freeSlot] = vt;
    vt[10] = (void *)&hk_ResGetDesc;
    VirtualProtect(&vt[10], sizeof(void *), old, &old);
    LOGI("BFI Presenter: stand-in back buffers report their typed format (format %d)", (int)g_desc.Format);
}

static bool CreateFrameResources()
{
    g_sc->GetDesc1(&g_desc);
    if (g_virtCount) g_desc.BufferCount = g_virtCount;          // stand-ins match what the game asked for
    g_bufCount = g_desc.BufferCount < kMaxBuf ? g_desc.BufferCount : kMaxBuf;
    D3D12_RESOURCE_FLAGS vf = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (g_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) vf |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (UINT i = 0; i < g_bufCount; i++)
        if (!(g_virt[i] = CreateTex(g_desc, VirtualFormat(g_desc.Format), vf, L"BFI virtual back buffer", i))) return false;
    if (g_reportTyped.load() && VirtualFormat(g_desc.Format) != g_desc.Format) HookResourceGetDesc(g_virt[0]);
    for (UINT s = 0; s < kStage; s++)
    {
        if (!(g_stage[s] = CreateTex(g_desc, g_desc.Format, D3D12_RESOURCE_FLAG_NONE, L"BFI staging frame", s))) return false;
        g_stageFence[s] = g_stageId[s] = g_readFence[s] = 0;
        g_presCurSlot = -1;
        g_stageTime[s] = 0.0;
    }
    g_cur = 0;
    g_stageNext = 0;
    LOGI("BFI Presenter: %ux%u, format %u (virtual %u), usage 0x%X, flags 0x%X, %u virtual back buffers",
         g_desc.Width, g_desc.Height, (unsigned)g_desc.Format, (unsigned)VirtualFormat(g_desc.Format),
         (unsigned)g_desc.BufferUsage, (unsigned)g_desc.Flags, g_bufCount);
    EnterCriticalSection(&g_cfgLock);
    g_status.width = g_desc.Width; g_status.height = g_desc.Height;
    g_status.buffers = g_bufCount; g_status.format = (UINT)g_desc.Format;
    LeaveCriticalSection(&g_cfgLock);
    return true;
}

static void ReleaseFrameResources()
{
    for (UINT i = 0; i < kMaxBuf; i++) SafeRelease(g_virt[i]);
    for (UINT s = 0; s < kStage; s++) SafeRelease(g_stage[s]);
    g_bufCount = 0;
}


// ---------------------------------------------------------------- DRED diagnostics
// Device Removed Extended Data (DRED 1.0). Declared here because the MinGW
// headers only carry the settings interface; layouts match d3d12.h.
struct BfiBreadcrumbNode
{
    const char *pCommandListDebugNameA;
    const wchar_t *pCommandListDebugNameW;
    const char *pCommandQueueDebugNameA;
    const wchar_t *pCommandQueueDebugNameW;
    ID3D12GraphicsCommandList *pCommandList;
    ID3D12CommandQueue *pCommandQueue;
    UINT32 BreadcrumbCount;
    const UINT32 *pLastBreadcrumbValue;
    const UINT32 *pCommandHistory;
    const BfiBreadcrumbNode *pNext;
};
struct BfiBreadcrumbsOutput { const BfiBreadcrumbNode *pHeadAutoBreadcrumbNode; };
struct BfiAllocationNode
{
    const char *ObjectNameA;
    const wchar_t *ObjectNameW;
    UINT AllocationType;
    const BfiAllocationNode *pNext;
};
struct BfiPageFaultOutput
{
    UINT64 PageFaultVA;
    const BfiAllocationNode *pHeadExistingAllocationNode;
    const BfiAllocationNode *pHeadRecentFreedAllocationNode;
};
struct IBfiDred : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetAutoBreadcrumbsOutput(BfiBreadcrumbsOutput *out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput(BfiPageFaultOutput *out) = 0;
};
static const GUID IID_BfiDred = { 0x98931D33, 0x5AE8, 0x4791, { 0xAA, 0x3C, 0x1A, 0x73, 0xA2, 0x93, 0x4E, 0x71 } };

static const char *kBreadcrumbOps[] = {
    "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect",
    "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
    "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
    "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission" };

static DWORD WINAPI EnableDredThread(void *)
{
    typedef HRESULT (WINAPI *PFN_GetDebug)(REFIID, void **);
    PFN_GetDebug getDebug = (PFN_GetDebug)(void *)GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12GetDebugInterface");
    ID3D12DeviceRemovedExtendedDataSettings *dred = nullptr;
    if (getDebug && SUCCEEDED(getDebug(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), (void **)&dred)))
    {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->Release();
        LOGI("BFI Presenter: DRED diagnostics enabled (takes effect for devices created from now on)");
    }
    else
        LOGW("BFI Presenter: could not enable DRED diagnostics");
    if (g_diagnostics >= 2)
    {
        // D3D12 debug layer: names the exact invalid call behind a "device
        // removed, invalid call" (0x887A0001). Slow; for diagnosis only.
        ID3D12Debug *dbg = nullptr;
        if (getDebug && SUCCEEDED(getDebug(__uuidof(ID3D12Debug), (void **)&dbg)) && dbg)
        {
            dbg->EnableDebugLayer();
            dbg->Release();
            LOGI("BFI Presenter: D3D12 debug layer enabled (Diagnostics=2) -- errors go to BFIPresenter_debug.txt");
        }
        else
            LOGW("BFI Presenter: D3D12 debug layer not available -- install the Windows optional feature \"Graphics Tools\"");
    }
    return 0;
}

static std::atomic<bool> g_dredDumped{false};

// Debug-layer messages (Diagnostics=2), written to BFIPresenter_debug.txt.
static ID3D12InfoQueue *g_infoQueue = nullptr;
static CRITICAL_SECTION g_infoLock;
static char g_debugPath[MAX_PATH] = {};
static void DrainInfoQueue()
{
    if (!g_infoQueue) return;
    EnterCriticalSection(&g_infoLock);
    static int logged = 0;
    const UINT64 n = g_infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    FILE *f = n ? fopen(g_debugPath, "a") : nullptr;
    for (UINT64 i = 0; i < n; i++)
    {
        SIZE_T len = 0;
        if (FAILED(g_infoQueue->GetMessage(i, nullptr, &len)) || !len || len > 65536) continue;
        std::vector<char> buf(len);
        D3D12_MESSAGE *m = (D3D12_MESSAGE *)buf.data();
        if (FAILED(g_infoQueue->GetMessage(i, m, &len))) continue;
        if (m->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
        const char *sev = m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" :
                          m->Severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "WARNING";
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        if (f) fprintf(f, "%02u:%02u:%02u.%03u %s #%d: %s\n", lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds,
                       sev, (int)m->ID, m->pDescription ? m->pDescription : "");
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR && logged < 40)
        {
            logged++;
            LOGE("BFI Presenter: D3D12 %s #%d: %.900s", sev, (int)m->ID, m->pDescription ? m->pDescription : "");
        }
    }
    if (f) fclose(f);
    g_infoQueue->ClearStoredMessages();
    LeaveCriticalSection(&g_infoLock);
}

static void DredOut(FILE *f, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    reshade::log::message(reshade::log::level::error, buf);
    if (f) fprintf(f, "%s\n", buf);
}

static void NameOf(const char *a, const wchar_t *w, char *out, size_t n)
{
    if (a) snprintf(out, n, "%s", a);
    else if (w) snprintf(out, n, "%ls", w);
    else snprintf(out, n, "(unnamed)");
}

// Writes what the GPU was doing when the device was removed.
static void DumpDeviceRemoved(ID3D12Device *device, const char *where)
{
    DrainInfoQueue();
    if (!device || g_dredDumped.exchange(true)) return;
    EnterCriticalSection(&g_cfgLock);
    g_status.deviceLost = 1;
    LeaveCriticalSection(&g_cfgLock);

    FILE *f = fopen(g_dredPath, "w");
    const HRESULT reason = device->GetDeviceRemovedReason();
    DredOut(f, "BFI Presenter: DEVICE REMOVED (detected in %s), reason 0x%08lX", where, (unsigned long)reason);

    IBfiDred *dred = nullptr;
    if (FAILED(device->QueryInterface(IID_BfiDred, (void **)&dred)) || !dred)
    {
        DredOut(f, "BFI Presenter: no DRED data (set Diagnostics=1 in BFIPresenter.ini and restart the game)");
        if (f) fclose(f);
        return;
    }
    BfiBreadcrumbsOutput bc = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
    {
        int nodes = 0;
        for (const BfiBreadcrumbNode *n = bc.pHeadAutoBreadcrumbNode; n && nodes < 32; n = n->pNext, nodes++)
        {
            const UINT32 last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (last >= n->BreadcrumbCount) continue;           // this list completed
            char q[128], l[128];
            NameOf(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW, q, sizeof(q));
            NameOf(n->pCommandListDebugNameA, n->pCommandListDebugNameW, l, sizeof(l));
            DredOut(f, "  incomplete command list: queue '%s', list '%s', %u of %u operations done",
                    q, l, last, n->BreadcrumbCount);
            const UINT32 from = last > 3 ? last - 3 : 0;
            const UINT32 to   = (last + 3 < n->BreadcrumbCount) ? last + 3 : n->BreadcrumbCount - 1;
            for (UINT32 i = from; i <= to && n->pCommandHistory; i++)
            {
                const UINT32 op = n->pCommandHistory[i];
                DredOut(f, "    %s[%u] %s", i == last ? ">>" : "  ", i,
                        op < sizeof(kBreadcrumbOps) / sizeof(kBreadcrumbOps[0]) ? kBreadcrumbOps[op] : "op");
            }
        }
    }
    BfiPageFaultOutput pf = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA)
    {
        DredOut(f, "  page fault at GPU VA 0x%016llX", (unsigned long long)pf.PageFaultVA);
        int k = 0;
        for (const BfiAllocationNode *a = pf.pHeadExistingAllocationNode; a && k < 10; a = a->pNext, k++)
        {
            char nm[128]; NameOf(a->ObjectNameA, a->ObjectNameW, nm, sizeof(nm));
            DredOut(f, "    existing allocation: %s", nm);
        }
        k = 0;
        for (const BfiAllocationNode *a = pf.pHeadRecentFreedAllocationNode; a && k < 10; a = a->pNext, k++)
        {
            char nm[128]; NameOf(a->ObjectNameA, a->ObjectNameW, nm, sizeof(nm));
            DredOut(f, "    recently freed allocation: %s", nm);
        }
    }
    dred->Release();
    if (f) fclose(f);
}

// ---------------------------------------------------------------- per-refresh shader
static const char kShader[] = R"HLSL(
cbuffer C : register(b0)
{
    uint  outType;    // 0 SDR (sRGB), 1 scRGB, 2 HDR10 (PQ)
    uint  slot;       // sub-frame index within the cycle
    uint  frames;
    uint  method;     // 0 BFI, 1 Variable MPRT, 2 passthrough, 3 steady (hitch guard)
    float gain;
    float dfloor;
    float knee;
    float ceilLin;    // panel ceiling, linear (1.0 = SDR white / 80 nits)
    uint  flags;      // 1 roll-off, 2 sine, 4 invert, 8 rolling scan
    float seam;       // rolling scan: blended zone between bands, fraction of the screen height
    float decay;      // rolling scan: phosphor decay time in sub-frames (0 = hard bands)
};
Texture2D<float4> src : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 SrgbToLin(float3 c) { c = max(c, 0); return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c)); }
float3 LinToSrgb(float3 l) { l = max(l, 0); return lerp(l * 12.92, 1.055 * pow(l, 1.0 / 2.4) - 0.055, step(0.0031308, l)); }
static const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
float3 PqToLin(float3 e)
{
    float3 p = pow(saturate(e), 1.0 / m2);
    return pow(max(p - c1, 0) / (c2 - c3 * p), 1.0 / m1) * (10000.0 / 80.0);
}
float3 LinToPq(float3 l)
{
    float3 p = pow(saturate(max(l, 0) * (80.0 / 10000.0)), m1);
    return pow((c1 + c2 * p) / (1 + c3 * p), m2);
}
float3 Decode(float3 c) { if (outType == 0) return SrgbToLin(c); if (outType == 2) return PqToLin(c); return c; }
float3 Encode(float3 l) { if (outType == 0) return saturate(LinToSrgb(l)); if (outType == 2) return LinToPq(l); return l; }

bool Inverted() { return (flags & 4) != 0 && frames > 2; }

float Target(uint j, float litGain, float fl)
{
    float g;
    if ((flags & 2) != 0)
    {
        float ph = (float(j) / float(frames)) * 6.28318531;
        if (Inverted()) ph += 3.14159265;
        g = lerp(fl, litGain, 0.5 + 0.5 * cos(ph));
    }
    else
    {
        bool lit = Inverted() ? (j != 0) : (j == 0);
        g = lit ? litGain : fl;
    }
    return g;
}

// BFI highlight roll-off: hue-preserving shoulder toward the ceiling P.
float3 Rolloff(float3 lin, float g, float P, float k)
{
    float m = max(max(lin.r, lin.g), lin.b);
    float3 r = lin * g;
    if (g > 1.0 && m > 1e-6)
    {
        float u = m / P;
        float v = g * u;
        k = clamp(k, 0.05, 0.99);
        if (u >= 1.0)
            r = lin;
        else if (v > k)
        {
            float a = (g - k) / (1.0 - k);
            float t = saturate((v - k) / (g - k));
            float o = k + (1.0 - k) * (1.0 - pow(1.0 - t, a));
            r = lin * (o / u);
        }
    }
    return r;
}

// Variable MPRT brightness budget: front-loaded fill, overflow wraps round.
float MprtScale(uint k, float litGain, float fl, float S)
{
    uint k0 = 0;
    if (Inverted()) k0 = ((flags & 2) != 0) ? (frames / 2) : 1;
    uint pos = (k + frames - k0) % frames;
    float fill[8];
    float carry = 0.0;
    [unroll] for (uint j = 0; j < 8; j++)
    {
        fill[j] = 0.0;
        if (j < frames)
        {
            float want = Target((k0 + j) % frames, litGain, fl) + carry;
            fill[j] = min(want, S);
            carry = want - fill[j];
        }
    }
    float s = 0.0;
    [unroll] for (uint j2 = 0; j2 < 8; j2++)
    {
        if (j2 < frames)
        {
            float add = min(carry, S - fill[j2]);
            carry -= add;
            if (j2 == pos) s = fill[j2] + add;
        }
    }
    return s;
}

// ABL guard measurement: average luminance (80-nit units) of the lit refresh
// this frame would produce at the configured boost, over a 32x18 grid with
// 4x4 samples per cell.
float PSMeasure(float4 pos : SV_Position) : SV_Target
{
    uint w, h;
    src.GetDimensions(w, h);
    float2 cell = float2(w, h) / float2(32.0, 18.0);
    float litGain = Inverted() ? float(frames) * gain / float(frames - 1) : float(frames) * gain;
    float sum = 0.0;
    [unroll] for (int y = 0; y < 4; y++)
    [unroll] for (int x = 0; x < 4; x++)
    {
        int2 p = int2((floor(pos.xy) + (float2(x, y) + 0.5) / 4.0) * cell);
        float3 lin = max(Decode(src.Load(int3(p, 0)).rgb), 0);
        float m = max(max(lin.r, lin.g), lin.b);
        float S = (m > 1e-6) ? max(ceilLin / m, 1.0) : 1e6;
        sum += dot(lin, float3(0.2126, 0.7152, 0.0722)) * min(litGain, S);
    }
    return sum / 16.0;
}

// One pixel's light in sub-frame j of the cycle.
float3 Strobe(float3 lin, uint j, float litGain)
{
    if (method == 0)
    {
        float g = Target(j, litGain, dfloor);
        return ((flags & 1) != 0) ? Rolloff(lin, g, ceilLin, knee) : lin * g;
    }
    float m = max(max(lin.r, lin.g), lin.b);
    float S = (m > 1e-6) ? max(ceilLin / m, 1.0) : 1e6;
    return lin * MprtScale(j, litGain, dfloor, S);
}

// Phosphor decay: share of a pixel's light that falls in sub-frame s when the
// beam excites it at phase phi (in sub-frames, 0..frames) and it fades with
// time constant T (sub-frames). Periodic over the cycle, so the shares of all
// sub-frames add up to 1.
float DecayShare(uint s, float phi, float T, float n)
{
    float a = float(s) - phi;
    a = a - n * floor(a / n);                       // time from excitation to the sub-frame's start, [0, n)
    float norm = 1.0 - exp(-n / T);
    float b = a + 1.0;
    if (b <= n) return (exp(-a / T) - exp(-b / T)) / norm;
    return (exp(-a / T) - exp(-n / T) + 1.0 - exp(-(b - n) / T)) / norm;
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    float3 c = src.Load(int3(pos.xy, 0)).rgb;
    if (method == 2) return float4(c, 1);
    float3 lin = Decode(c);
    float litGain = Inverted() ? float(frames) * gain / float(frames - 1) : float(frames) * gain;
    float3 o;
    if (method == 3)
    {
        // Hitch guard: every refresh at the strobe's average brightness, so
        // pausing and resuming BFI changes motion clarity only, not brightness.
        float a = 0.0;
        [unroll] for (uint j = 0; j < 8; j++) if (j < frames) a += Target(j, litGain, dfloor);
        o = lin * (a / float(frames));
    }
    else if ((flags & 8) != 0 && decay > 0.0)
    {
        // Rolling scan with phosphor decay (Blur Busters CRT beam simulator
        // style): the beam sweeps the screen once per cycle, reaching row y at
        // phase y * frames; the row then fades exponentially, so its light is
        // spread over the sub-frames after that. Sub-frame s shows the strobe
        // profile convolved with that fade -- each pixel still gets exactly the
        // light of one full cycle, and there is no seam: the band edge moves
        // with the beam and fades out instead of switching.
        uint w, h;
        src.GetDimensions(w, h);
        float n = float(frames);
        float phi = (pos.y / float(h)) * n;
        float T = max(decay, 0.02);
        o = float3(0, 0, 0);
        [unroll] for (uint j = 0; j < 8; j++)
        {
            if (j < frames)
            {
                uint d = (slot + frames - j) % frames;    // sub-frame offset from profile step j
                float e = DecayShare(d, phi, T, n);
                if (e > 1e-5) o += Strobe(lin, j, litGain) * e;
            }
        }
    }
    else if ((flags & 8) != 0)
    {
        // Rolling scan: the screen is cut into one horizontal band per
        // sub-frame, and band b runs b sub-frames behind the top one, so the
        // lit band rolls down the screen and the screen as a whole never goes
        // dark. Each pixel still gets the same light over the cycle; across
        // the seam between two bands it is shared between their sub-frames.
        uint w, h;
        src.GetDimensions(w, h);
        float u = (pos.y / float(h)) * float(frames);
        uint band = min((uint)u, frames - 1);
        float f = u - float(band);
        float hw = max(seam * float(frames) * 0.5, 1e-4);
        o = Strobe(lin, (slot + frames - band) % frames, litGain);
        float wn = 0.0; uint nb = band;
        if (f < hw && band > 0)                    { wn = 0.5 * (1.0 - f / hw);         nb = band - 1; }
        else if (1.0 - f < hw && band + 1 < frames) { wn = 0.5 * (1.0 - (1.0 - f) / hw); nb = band + 1; }
        if (wn > 0.0) o = lerp(o, Strobe(lin, (slot + frames - nb) % frames, litGain), wn);
    }
    else o = Strobe(lin, slot, litGain);
    return float4(Encode(o), 1);
}
)HLSL";

struct Constants
{
    uint32_t outType, slot, frames, method;
    float    gain, dfloor, knee, ceilLin;
    uint32_t flags;
    float    seam;        // rolling scan: blended zone between bands (fraction of the height)
    float    decay;       // rolling scan: phosphor decay time in sub-frames (0 = hard bands)
};

// ABL guard: the screen average of the brightest refresh in the cycle when
// every 32x18 cell's lit value is capped at C. Plain strobe: the lit refresh.
// Rolling scan: refresh j lights band j only (plus, with Variable MPRT, what
// the other bands spill above C into their dark refreshes), so each refresh
// carries roughly 1/frames of the light. NOT used: on the A95L the panel's
// own ABL still dimmed bright areas with rolling scan on, so its limiter does
// not go by each refresh's average -- the guard keeps measuring as if the
// whole screen were lit, rolling or not.
static double AblAvgAt(const float *cells, double C, bool rolling, int frames, bool mprt)
{
    if (frames < 2) frames = 2;          // BFI off: a measurement taken while it was on
    if (!rolling || frames < 2)
    {
        double a = 0.0;
        for (int i = 0; i < 32 * 18; i++) a += std::min((double)cells[i], C);
        return a / (32.0 * 18.0);
    }
    double worst = 0.0;
    for (int j = 0; j < frames; j++)
    {
        double a = 0.0;
        for (int y = 0; y < 18; y++)
        {
            const int band = std::min((int)((y + 0.5) * frames / 18.0), frames - 1);
            for (int x = 0; x < 32; x++)
            {
                const double v = cells[y * 32 + x];
                if (band == j) a += std::min(v, C);
                else if (mprt)
                {
                    const double spill = std::max(v - C, 0.0);
                    a += frames == 2 ? std::min(spill, C) : spill / (frames - 1);
                }
            }
        }
        worst = std::max(worst, a);
    }
    return worst / (32.0 * 18.0);
}


typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static bool Compile(PFN_D3DCompile compile, const char *entry, const char *target, ID3DBlob **out)
{
    ID3DBlob *err = nullptr;
    HRESULT hr = compile(kShader, sizeof(kShader) - 1, "bfi_presenter", nullptr, nullptr, entry, target,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr))
    {
        LOGE("BFI Presenter: shader %s failed: %s", entry, err ? (const char *)err->GetBufferPointer() : "?");
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);
    return true;
}

// ---------------------------------------------------------------- presenter thread
static int ResolveOutput(const Settings &s)
{
    if (s.output >= 0) return s.output;
    // A 16-bit float swap chain is always scRGB to Windows, whatever colour
    // space is reported. Mods that upgrade an SDR game's swap chain to HDR
    // (RenoDX) leave the reported space at sRGB some or all of the time;
    // reading that as SDR clipped every highlight above SDR white.
    if (g_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) return 1;
    if (g_rsSwapchain)
    {
        switch (g_rsSwapchain->get_color_space())
        {
        case reshade::api::color_space::extended_srgb_linear: return 1;
        case reshade::api::color_space::hdr10_st2084:         return 2;
        default: break;
        }
    }
    return (g_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 1 : 0;
}

static bool KeyCombo(int vk)
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
           (GetAsyncKeyState(vk) & 0x8000);
}

// ---------------------------------------------------------------- hang watchdog
// Each side records which step it is in; a watchdog thread logs any step
// that takes longer than a second, so a freeze shows exactly where.
static const char *kStageNames[] = {
    "idle", "wait for vblank", "wait for its previous frame on the GPU", "pick + record + submit",
    "Present (DXGI)", "game: wait for a copy slot", "game: copy + submit", "game: frame cap wait",
    "game: move to presenter queue", "outside BFI Presenter (in the game or another hook)",
    "game own frame: back buffer index", "game own frame: get back buffer", "game own frame: copy + submit",
    "game own frame: Present (DXGI)",
    "game resize: stop presenter", "game resize: release buffers", "game resize: ResizeBuffers onto the presenter queue",
    "game resize: game's own ResizeBuffers", "game resize: create stand-in buffers" };
enum { ST_IDLE, ST_VBLANK, ST_BUFFENCE, ST_SUBMIT, ST_PRESENT, ST_G_SLOT, ST_G_COPY, ST_G_CAP, ST_G_REBIND, ST_G_OUTSIDE,
       ST_G_OWN_INDEX, ST_G_OWN_GETBUF, ST_G_OWN_COPY, ST_G_OWN_PRESENT,
       ST_R_STOP, ST_R_RELEASE, ST_R_REBIND, ST_R_ORIG, ST_R_CREATE };
static std::atomic<int>    g_presStage{ST_IDLE}, g_gameStage{ST_IDLE};
static std::atomic<int>    g_dbgStaged{0};                 // presenter idle: staged frames seen
static std::atomic<UINT64> g_dbgDone{0}, g_dbgNeed{0};     // game fence completed vs needed
static std::atomic<double> g_presStageT{0.0}, g_gameStageT{0.0};
static void PresStage(int st) { g_presStage.store(st); g_presStageT.store(Now()); }
static std::atomic<DWORD> g_gameStageTid{0};               // thread that set the game-side step
static void GameStage(int st) { g_gameStage.store(st); g_gameStageT.store(Now()); g_gameStageTid.store(GetCurrentThreadId()); }

static void ModuleOf(const void *addr, char *out, size_t n);

// True when addr lies in a mapped image (an EXE or DLL). Uses VirtualQuery,
// which takes no loader or runtime locks. Code outside images is JIT code
// (Mono/.NET, scripting engines): those runtimes register their unwind data
// through callbacks that take the runtime's own locks, so asking
// RtlLookupFunctionEntry about such an address from an exception handler or a
// suspended thread can deadlock the game. Stack walks stop there instead.
static bool InImage(DWORD64 addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    return addr && VirtualQuery((const void *)(uintptr_t)addr, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.Type == MEM_IMAGE;
}

#ifndef _WIN64
// 32-bit builds: follow the EBP frame chain (no unwind tables on x86). Frames
// of code built without frame pointers are skipped; good enough for a log.
static int WalkFramePointers(const CONTEXT &ctx, DWORD64 *frames, int max)
{
    int nf = 0;
    if (ctx.Eip) frames[nf++] = ctx.Eip;
    DWORD ebp = ctx.Ebp;
    if (ctx.Eip < 0x10000 && !IsBadReadPtr((const void *)(uintptr_t)ctx.Esp, 4) && nf < max)
        frames[nf++] = *(const DWORD *)(uintptr_t)ctx.Esp;        // call through a null pointer
    while (nf < max && ebp && !IsBadReadPtr((const void *)(uintptr_t)ebp, 8))
    {
        const DWORD next = ((const DWORD *)(uintptr_t)ebp)[0], ret = ((const DWORD *)(uintptr_t)ebp)[1];
        if (!ret) break;
        frames[nf++] = ret;
        if (next <= ebp) break;
        ebp = next;
    }
    return nf;
}
#endif

// Call stack of another thread, as "module+offset" frames. The thread is
// suspended only while its context is captured and its stack unwound; module
// names are resolved after it is resumed (that path takes the loader lock).
static void LogThreadStack(DWORD tid, const char *who)
{
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) return;
    DWORD64 frames[32];
    int nf = 0;
    if (SuspendThread(th) != (DWORD)-1)
    {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx))
        {
#ifndef _WIN64
            nf = WalkFramePointers(ctx, frames, 32);
#else
            for (; nf < 32 && ctx.Rip; nf++)
            {
                frames[nf] = ctx.Rip;
                if (!InImage(ctx.Rip)) { nf++; break; }       // JIT code: don't ask the runtime to unwind
                DWORD64 base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                if (!fn) { ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8; continue; }
                PVOID hd = nullptr;
                DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &hd, &est, nullptr);
            }
#endif
        }
        ResumeThread(th);
    }
    CloseHandle(th);
    char line[3072];
    int len = snprintf(line, sizeof(line), "BFI Presenter: WATCHDOG -- %s stack:", who);
    for (int i = 0; i < nf && len < (int)sizeof(line) - 80; i++)
    {
        char mod[64];
        ModuleOf((const void *)frames[i], mod, sizeof(mod));
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)frames[i], &m);
        len += snprintf(line + len, sizeof(line) - len, " %s+0x%llX", mod,
                        (unsigned long long)(frames[i] - (DWORD64)(uintptr_t)m));
    }
    reshade::log::message(reshade::log::level::warning, line);
}

// ---------------------------------------------------------------- crash logger
// Serious exceptions anywhere in the game are written to BFIPresenter_crash.txt
// (module + offset of the fault and the call stack), flushed immediately, so a
// hard crash leaves a record even when the game dies before ReShade's log does.
// These are first-chance records: a game may handle some itself (the last
// entry before the process dies is the crash).
static char g_crashPath[MAX_PATH] = {};
static PVOID g_crashHandler = nullptr;
static bool  g_crashLogOn = true;          // CrashLog in the ini
static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS *ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000374 /* heap corruption */)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!g_crashPath[0]) return EXCEPTION_CONTINUE_SEARCH;
    // Mods probe memory and handle their own access violations by the
    // thousand (REFramework's pattern scans): log each faulting location once,
    // so those can't use up the budget before the real crash.
    static std::atomic<int> count{0};
    static void *seen[256];
    static std::atomic<int> nseen{0};
    void *where = ep->ExceptionRecord->ExceptionAddress;
    const int ns = nseen.load() < 256 ? nseen.load() : 256;
    for (int i = 0; i < ns; i++)
        if (seen[i] == where) return EXCEPTION_CONTINUE_SEARCH;
    const int slot = nseen.fetch_add(1);
    if (slot < 256) seen[slot] = where;
    if (count.fetch_add(1) >= 200) return EXCEPTION_CONTINUE_SEARCH;

    DWORD64 frames[24];
    int nf = 0;
    CONTEXT ctx = *ep->ContextRecord;
#ifndef _WIN64
    nf = WalkFramePointers(ctx, frames, 24);
#else
    if (ctx.Rip < 0x10000 && !IsBadReadPtr((const void *)ctx.Rsp, 8))
    {
        // Call through a null (or tiny) pointer: the caller's return address
        // is on top of the stack.
        frames[nf++] = ctx.Rip;
        ctx.Rip = *(DWORD64 *)ctx.Rsp;
        ctx.Rsp += 8;
    }
    for (; nf < 24 && ctx.Rip; nf++)
    {
        frames[nf] = ctx.Rip;
        if (!InImage(ctx.Rip)) { nf++; break; }               // JIT code: don't ask the runtime to unwind
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!fn)
        {
            if (IsBadReadPtr((const void *)ctx.Rsp, 8)) { nf++; break; }
            ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8; continue;
        }
        PVOID hd = nullptr;
        DWORD64 est = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &hd, &est, nullptr);
    }
#endif
    char line[2048];
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    int len = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u exception 0x%08lX on thread %lu%s",
                       lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds, (unsigned long)code,
                       (unsigned long)GetCurrentThreadId(), t_presenterThread ? " (BFI presenter thread)" : "");
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        len += snprintf(line + len, sizeof(line) - len, ", %s address 0x%llX",
                        ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading" :
                        ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing" : "executing",
                        (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    len += snprintf(line + len, sizeof(line) - len, "\n  presenter step: %s, game step: %s, presenter %s, %s queue\n  stack:",
                    kStageNames[g_presStage.load()], kStageNames[g_gameStage.load()],
                    g_running.load() ? "running" : "stopped", g_ownPresentQueue ? "own" : "game's");
    for (int i = 0; i < nf && len < (int)sizeof(line) - 96; i++)
    {
        char mod[64];
        ModuleOf((const void *)frames[i], mod, sizeof(mod));
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)frames[i], &m);
        len += snprintf(line + len, sizeof(line) - len, " %s+0x%llX", mod,
                        (unsigned long long)(frames[i] - (DWORD64)(uintptr_t)m));
    }
    len += snprintf(line + len, sizeof(line) - len, "\n");
    HANDLE f = CreateFileA(g_crashPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD wr = 0;
        WriteFile(f, line, (DWORD)len, &wr, nullptr);
        CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI WatchdogThread(void *)
{
    int lastPres = -1, lastGame = -1;
    double lastPresT = 0.0, lastGameT = 0.0;
    for (;;)
    {
        Sleep(500);
        DrainInfoQueue();
        const double t = Now();
        if (g_running.load())
        {
            const int st = g_presStage.load();
            const double since = t - g_presStageT.load();
            if (since > 1.0 && (st != lastPres || t - lastPresT > 5.0))
            {
                LOGW("BFI Presenter: WATCHDOG -- presenter stuck for %.1f s in: %s", since, kStageNames[st]);
                lastPres = st; lastPresT = t;
            }
            if (since <= 1.0) lastPres = -1;
            // Running but not presenting: say why (nothing staged, or the
            // staged frames are not finished on the game's queue).
            const double lp = g_lastPresentT.load();
            if (lp > 0.0 && t - lp > 2.0 && t - lastPresT > 5.0)
            {
                LOGW("BFI Presenter: WATCHDOG -- presenter idle for %.1f s: %d staged frame(s), game queue fence at %llu, "
                     "oldest staged frame needs %llu", t - lp, g_dbgStaged.load(),
                     (unsigned long long)g_dbgDone.load(), (unsigned long long)g_dbgNeed.load());
                lastPresT = t;
            }
        }
        // The game's window stops responding (Windows' "not responding" test:
        // no message processed for 5 s): log where its thread is, once per hang.
        {
            static bool hungLogged = false;
            HWND gw = g_watchHwnd.load();
            const bool hung = gw && IsWindow(gw) && IsHungAppWindow(gw);
            if (hung && !hungLogged)
            {
                hungLogged = true;
                const DWORD wt = GetWindowThreadProcessId(gw, nullptr);
                LOGW("BFI Presenter: WATCHDOG -- the game's window is not responding (window thread %lu)", (unsigned long)wt);
                if (wt) LogThreadStack(wt, "game window thread");
                const DWORD ut = g_uiTid.load();
                if (ut) LogThreadStack(ut, "presenter window thread");
                if (g_presentThreads[0] && g_presentThreads[0] != wt) LogThreadStack(g_presentThreads[0], "game present thread");
            }
            else if (!hung && hungLogged)
            {
                hungLogged = false;
                LOGI("BFI Presenter: WATCHDOG -- the game's window responds again");
            }
        }
        if (g_active.load() && g_gameStageT.load() > 0.0)
        {
            const int st = g_gameStage.load();
            const double since = t - g_gameStageT.load();
            if (since > 2.0 && (st != lastGame || t - lastGameT > 5.0))
            {
                LOGW("BFI Presenter: WATCHDOG -- game Present thread: %.1f s since its last step, now %s",
                     since, kStageNames[st]);
                // Once per stall: where the game's present thread is waiting.
                if (st != lastGame)
                {
                    // The thread in that step (a resize can come from another
                    // thread than the one presenting), then the present thread.
                    const DWORD stTid = g_gameStageTid.load();
                    char who[96];
                    snprintf(who, sizeof(who), "thread %lu (in that step)", (unsigned long)stTid);
                    if (stTid) LogThreadStack(stTid, who);
                    if (g_presentThreads[0] && g_presentThreads[0] != stTid)
                    {
                        snprintf(who, sizeof(who), "game present thread %lu", (unsigned long)g_presentThreads[0]);
                        LogThreadStack(g_presentThreads[0], who);
                    }
                }
                if (st != lastGame && g_waitSem)
                {
                    const bool free = WaitForSingleObject(g_waitSem, 0) == WAIT_OBJECT_0;
                    if (free) ReleaseSemaphore(g_waitSem, 1, nullptr);
                    else TopUpGameSlots();          // the game is blocked on it: let it through
                    LOGW("BFI Presenter: WATCHDOG -- game's frame-latency waitable was %s",
                         free ? "signalled (the game is not waiting on it)" : "not signalled -- released so the game can continue");
                }
                lastGame = st; lastGameT = t;
            }
            if (since <= 2.0) lastGame = -1;
        }
    }
    return 0;
}

// Calls on the swap chain the presenter shows through. With a DX11 game that
// is the add-on's own D3D12 swap chain, and every call goes straight to
// DXGI's own code: past ReShade, Steam's overlay and the add-on's own vtable
// hooks (it shares DXGI's swap-chain class with the game's). Steam's vtable
// hooks call back into the vtable, which with ours on top recursed until the
// stack overflowed (God of War, SetColorSpace1).
static void *PristineVtableEntry(void **vtbl, int idx);
static void *MakeUnhookedEntry(void *target, char *why, size_t whyLen);
static void *DirectSlot(void **vt, int idx)
{
    static CRITICAL_SECTION cs;
    static bool init = false;
    static struct { void **vt; int idx; void *fn; } cache[48];
    static int n = 0;
    if (!init) { InitializeCriticalSection(&cs); init = true; }
    EnterCriticalSection(&cs);
    for (int i = 0; i < n; i++)
        if (cache[i].vt == vt && cache[i].idx == idx) { void *f = cache[i].fn; LeaveCriticalSection(&cs); return f; }
    void *pr = PristineVtableEntry(vt, idx);
    if (!pr) pr = vt[idx];
    char why[96];
    void *u = MakeUnhookedEntry(pr, why, sizeof(why));
    if (!u) u = pr;
    if (n < 48) cache[n++] = { vt, idx, u };
    LeaveCriticalSection(&cs);
    return u;
}
#define SC_DIRECT(T, idx) ((T)DirectSlot(*(void ***)g_sc, idx))
static void ScDesc1(DXGI_SWAP_CHAIN_DESC1 *d)
{
    if (g_dx11.load()) { OwnCall own; SC_DIRECT(HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain1 *, DXGI_SWAP_CHAIN_DESC1 *), 18)(g_sc, d); }
    else if (o_GetDesc1) o_GetDesc1(g_sc, d);
    else *d = g_desc;
}
static HRESULT ScBuffer(UINT i, ID3D12Resource **r)
{
    if (g_dx11.load()) { OwnCall own; return SC_DIRECT(PFN_GetBuffer, 9)(g_sc, i, __uuidof(ID3D12Resource), (void **)r); }
    return o_GetBuffer(g_sc, i, __uuidof(ID3D12Resource), (void **)r);
}
static UINT ScCBBI()
{
    if (g_dx11.load()) { OwnCall own; return SC_DIRECT(PFN_GetCBBI, 36)(g_sc); }
    return o_GetCBBI(g_sc);
}
static HRESULT ScStats(DXGI_FRAME_STATISTICS *st)
{
    if (g_dx11.load()) { OwnCall own; return SC_DIRECT(HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain *, DXGI_FRAME_STATISTICS *), 16)(g_sc, st); }
    return g_sc->GetFrameStatistics(st);
}
static HRESULT ScLPC(UINT *n)
{
    if (g_dx11.load()) { OwnCall own; return SC_DIRECT(HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT *), 17)(g_sc, n); }
    return g_sc->GetLastPresentCount(n);
}
static HRESULT ScOutput(IDXGIOutput **o)
{
    if (g_dx11.load()) { OwnCall own; return SC_DIRECT(HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain *, IDXGIOutput **), 15)(g_sc, o); }
    return g_sc->GetContainingOutput(o);
}
static void Sync11Window();

static DWORD WINAPI PresenterThread(void *)
{
    // --- real back buffers + render state
    DXGI_SWAP_CHAIN_DESC1 realDesc = {};
    ScDesc1(&realDesc);
    const UINT n = realDesc.BufferCount < kMaxBuf ? realDesc.BufferCount : kMaxBuf;
    ID3D12Resource *real[kMaxBuf] = {};
    // Draw on the queue the swap chain presents on: writing its buffers from
    // any other queue gets the device removed (seen in Resonance).
    ID3D12CommandQueue *queue = g_ownPresentQueue ? g_presQueue : g_gameQueue;
    t_presenterThread = true;
    ID3D12DescriptorHeap *rtvHeap = nullptr, *srvHeap = nullptr;
    ID3D12CommandAllocator *alloc[kMaxBuf] = {};
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12RootSignature *rootSig = nullptr;
    ID3D12PipelineState *pso = nullptr, *psoMeas = nullptr;
    ID3DBlob *vs = nullptr, *ps = nullptr, *psMeas = nullptr, *rsBlob = nullptr, *rsErr = nullptr;
    ID3D12Resource *measRt = nullptr, *measRb = nullptr;
    ID3D12DescriptorHeap *measHeap = nullptr;
    UINT64 measFence = 0;
    IDXGIOutput *output = nullptr;
    UINT64 bufFence[kMaxBuf] = {};
    // What each real buffer currently holds. A refresh that would draw the
    // same game frame with the same settings into a buffer that already has
    // it just presents it again -- no GPU work, so a GPU busy with the game
    // (a hitch, shader compilation) can't hold the strobe up.
    UINT64   bufFrame[kMaxBuf] = {};
    Constants bufConst[kMaxBuf] = {};
    bool     bufValid[kMaxBuf] = {};
    int      secReused = 0;
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE llEv = CreateEventA(nullptr, FALSE, FALSE, nullptr);   // lowest latency: short wait for the expected frame
    FILE *logf = nullptr;
    UINT rtvStride = 0, srvStride = 0;
    bool ok = true;

    ok = ok && queue != nullptr;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = n;
    ok = ok && SUCCEEDED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kStage;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ok = ok && SUCCEEDED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)));

    if (ok)
    {
        rtvStride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvStride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rvd = {};
        rvd.Format        = g_desc.Format;
        rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i < n && ok; i++)
        {
            ok = SUCCEEDED(ScBuffer(i, &real[i]));
            if (ok) g_device->CreateRenderTargetView(real[i], &rvd, h);
            h.ptr += rtvStride;
            ok = ok && SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i])));
        }
        D3D12_CPU_DESCRIPTOR_HANDLE sh = srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC svd = {};
        svd.Format                  = g_desc.Format;
        svd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        svd.Texture2D.MipLevels     = 1;
        for (UINT s = 0; s < kStage; s++)
        {
            g_device->CreateShaderResourceView(g_stage[s], &svd, sh);
            sh.ptr += srvStride;
        }
    }
    if (ok)
        ok = SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&list)));
    if (ok) { list->SetName(L"BFI present list"); list->Close(); }

    // --- shaders (d3dcompiler_47 ships with Windows 10/11)
    HMODULE dc = ok ? LoadLibraryA("d3dcompiler_47.dll") : nullptr;
    PFN_D3DCompile compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (ok && !compile) { LOGE("BFI Presenter: d3dcompiler_47.dll not available"); ok = false; }
    ok = ok && Compile(compile, "VSMain", "vs_5_0", &vs) && Compile(compile, "PSMain", "ps_5_0", &ps);
    if (ok && !Compile(compile, "PSMeasure", "ps_5_0", &psMeas)) psMeas = nullptr;   // ABL guard optional

    if (ok)
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors     = 1;
        range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &range;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2;
        rsd.pParameters   = params;
        ok = SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr));
        ok = ok && SUCCEEDED(g_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                                           IID_PPV_ARGS(&rootSig)));
    }
    if (ok)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig;
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0]    = g_desc.Format;
        pd.SampleDesc.Count = 1;
        ok = SUCCEEDED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
        if (!ok) LOGE("BFI Presenter: CreateGraphicsPipelineState failed");
        // ABL guard: a 32x18 float target, its view and a readback buffer.
        if (ok && psMeas)
        {
            pd.PS = { psMeas->GetBufferPointer(), psMeas->GetBufferSize() };
            pd.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
            bool m = SUCCEEDED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoMeas)));
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = 32; rd.Height = 18; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R32_FLOAT; rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            m = m && SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&measRt)));
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 256 * 18; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            m = m && SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&measRb)));
            D3D12_DESCRIPTOR_HEAP_DESC mh = {};
            mh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; mh.NumDescriptors = 1;
            m = m && SUCCEEDED(g_device->CreateDescriptorHeap(&mh, IID_PPV_ARGS(&measHeap)));
            if (m)
            {
                measRt->SetName(L"BFI ABL guard measure");
                g_device->CreateRenderTargetView(measRt, nullptr, measHeap->GetCPUDescriptorHandleForHeapStart());
            }
            else
            {
                LOGW("BFI Presenter: ABL guard unavailable (could not create its measurement objects)");
                SafeRelease(psoMeas); SafeRelease(measRt); SafeRelease(measRb); SafeRelease(measHeap);
            }
            pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            pd.RTVFormats[0] = g_desc.Format;
        }
    }
    if (ok && FAILED(ScOutput(&output))) output = nullptr;
    // The display mode's own refresh rate (120 on the A95L). The VRR check
    // compares against it, so a refresh that is slow from the very start
    // (G-SYNC driving the panel at 118 Hz) is caught too.
    double modeHz = 0.0;
    if (output)
    {
        DXGI_OUTPUT_DESC od = {};
        DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
        if (SUCCEEDED(output->GetDesc(&od)) && EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)
            && dm.dmDisplayFrequency > 1)
            modeHz = (double)dm.dmDisplayFrequency;
    }

    if (!ok)
    {
        LOGE("BFI Presenter: presenter initialisation failed -- falling back to normal presentation");
        g_presenterFailed.store(true);
        g_running.store(false);
    }
    else
        LOGI("BFI Presenter: presenter running (high-priority queue, %s pacing, presents on %s queue)",
             output ? "vblank" : "present-queue", g_ownPresentQueue ? "its own" : "the game's");
    EnterCriticalSection(&g_cfgLock);
    g_status.running = ok ? 1 : 0;
    LeaveCriticalSection(&g_cfgLock);

    Settings s;
    EnterCriticalSection(&g_cfgLock);
    s = g_shared;
    LeaveCriticalSection(&g_cfgLock);

    uint64_t presents = 0, lastShownId = 0;
    int secFrameRepeats = 0, secFrameSkips = 0, extraCycles = 0;
    // Strobe cycles whose step differs from the expected one: at the Refresh
    // cap at 120 fps every lit refresh moves on 2 frames (the one between is
    // never lit), at Half 1, at Quarter 1 every other cycle. Counting raw
    // repeats and skipped frames instead flagged a steady 120 fps game as
    // "does not match the strobe" (Hades 2), and the Quarter cap's normal
    // repeats kept automatic frames ahead at 2 (Obduction).
    int secRepU = 0, secSkipU = 0;
    double llStep = 0.0;          // lowest latency: game frames per strobe cycle (average)
    int autoAhead = 1, missSecs = 0, spareSecs = 0;
    int lastOutType = -1;
    double ceilNow = -1.0, ceilTarget = -1.0, hotArea = -1.0, ablSolve = -1.0, ablT = Now();
    double rawAvg = -1.0, ablLowT = 0.0;      // unclipped lit average; last time the guard was pulling down
    double aheadDropT = -1.0;      // when automatic frames ahead last went back to 1
    bool   aheadLocked = false;    // settled at 2 for this swap chain (no more switching)
    bool refillNow = false;          // present again without waiting for the next vblank
    bool vsyncOverridden = false;    // the driver is not honouring the presenter's VSync
    int  queuedNow = -1;
    int lastShown = -1;
    double lastT = Now(), secStart = Now(), ageSum = 0.0;
    int secPresents = 0, secLate = 0;
    // Hitch guard: while the presenter misses refreshes (GPU saturated by
    // loading) or the game stops delivering frames, every refresh is shown
    // lit -- a held dark or lit frame is what flashes. BFI returns after a
    // second without either.
    double lateTimes[4] = {};
    int    lateIdx = 0;
    double lastNewFrameT = 0.0, hitchUntil = 0.0;
    double peakHz = 0.0, displayHz = 0.0;
    int    vrrSlowSecs = 0;
    bool   vrrLogged = false;
    int    limitSecs = 0;          // presenter held below a fixed display refresh
    bool   limitLogged = false;
    bool   lostLogged = false;
    bool   hitchPaused = false;
    UINT lastSync = 0, lastPC = 0;
    bool haveStats = false;
    bool kb = false, km = false, kr = false, krs = false;

    while (ok && g_running.load())
    {
        // hotkeys (edge-triggered) act on the shared settings
        bool b = KeyCombo('B'), m = KeyCombo('M'), r = KeyCombo('R'), rs = KeyCombo('S');
        if ((b && !kb) || (m && !km) || (rs && !krs))
        {
            EnterCriticalSection(&g_cfgLock);
            if (b && !kb) g_shared.enabled = !g_shared.enabled;
            if (m && !km) g_shared.method = !g_shared.method;
            if (rs && !krs) g_shared.rolling = !g_shared.rolling;
            LeaveCriticalSection(&g_cfgLock);
        }
        krs = rs;
        if (r && !kr) { LoadShared(); LOGI("BFI Presenter: settings reloaded"); }
        kb = b; km = m; kr = r;

        EnterCriticalSection(&g_cfgLock);
        s = g_shared;
        // BFI off: a plain presenter, a new game frame every refresh. Kept at
        // the strobe's cycle, every second frame of a 120 fps game was dropped
        // and each shown twice (60 Hz motion), which also made a CRT shader's
        // own BFI flicker (Twelve Minutes, Vulkan layer).
        if (!s.enabled) s.frames = 1;
        // Release the game's frame-latency slot when the frame is handed
        // over (not when the strobe takes it) in Lowest latency, and also
        // whenever a Half/Quarter cap already paces the game to the strobe:
        // releasing at the pick started each frame just after a lit refresh,
        // so it always missed it and waited a full extra cycle (The Midnight
        // Walk, Half cap + Smooth: frames 21 ms old when lit, against 6 ms
        // with Lowest latency).
        // Smooth at the Refresh cap too (v0.79): the cap already paces the
        // game, and releasing at the 60 lit picks per second held a game that
        // can't reach 120 back -- God of War Ragnarok fell from ~100 to ~73 fps
        // when switching from Lowest latency to Smooth. Only Smooth with no cap
        // still releases at the pick (it is then the only pacing).
        g_releaseOnSubmit.store(s.pacingMode == 1 || s.gameCap >= 1);
        LeaveCriticalSection(&g_cfgLock);
        {
            const int per = s.gameCap == 2 ? 2 : s.gameCap == 3 ? 4 : 1;
            const bool capOk = s.capJit && s.gameCap >= 2 && s.frames >= 2 && per % s.frames == 0;
            const bool llOk  = s.laterRelease && s.pacingMode == 1 && s.gameCap == 1;
            g_lrAllowed.store((capOk || llOk) && s.pacing && !vsyncOverridden && output);
            g_lrWindow.store(per / (modeHz > 0.0 ? modeHz : 120.0));
        }
        g_lrBaseMs.store(s.laterReleaseMs);
        g_lrPeriod.store(1.0 / (modeHz > 0.0 ? modeHz : 120.0));
        g_gameCap.store(s.gameCap);

        PresStage(ST_VBLANK);
        // Timing log, switchable at any time from the overlay. Each session
        // gets its own time-stamped file; every line records the settings in
        // force, so runs with different settings can be told apart.
        g_logOn.store(s.log != 0);
        if (s.log && !logf)
        {
            char path[MAX_PATH];
            SYSTEMTIME lt;
            GetLocalTime(&lt);
            snprintf(path, sizeof(path), "%.*sBFIPresenter_log_%04u%02u%02u_%02u%02u%02u.csv",
                     (int)(strlen(g_logPath) - strlen("BFIPresenter_log.csv")), g_logPath,
                     lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
            logf = fopen(path, "w");
            if (logf)
            {
                fprintf(logf, "time_ms,present,slot,frame_id,frame_age_ms,interval_ms,present_count,sync_refresh_count,"
                              "bfi,method,frames,gain,floor,rolloff,pacing,cap,ahead,vblank,queue_ahead,queued,"
                              "wait_vblank_ms,wait_gpu_ms,submit_ms,present_ms,hitch_paused,ceiling_nits,lit_avg_nits,raw_avg_nits,"
                              "lr_state,lr_lead_ms,lr_est_ms,lr_margin_ms,smooth_catchup\n");
                LOGI("BFI Presenter: timing log started: %s", path);
            }
        }
        else if (!s.log && logf)
        {
            fclose(logf);
            logf = nullptr;
            LOGI("BFI Presenter: timing log stopped");
        }

        // Pacing. Wait for vblank: one present per refresh, nothing queued.
        // With Queue ahead, one extra present is kept queued so a late slot
        // is covered by the frame already waiting (needs 3+ real buffers),
        // while the driver never sees a deep present queue. If the driver
        // ignores the presenter's VSync (presents far faster than refreshes),
        // vblank pacing is forced so the strobe keeps its cadence.
        const double tLoop0 = Now();
        // The game's frame cap is released at the vblank itself. Releasing
        // after the presenter's own Present returned let its varying length
        // (pick, record, submit, Present: 0.2-3 ms) shift each release, and
        // the game timed its animation to those uneven gaps -- 15-18 ms frames
        // at a steady 60 fps, visible judder under BFI in every pacing mode
        // (The Midnight Walk).
        bool tickedAtVb = false;
        if ((s.pacing || vsyncOverridden) && output && !refillNow)
        {
            output->WaitForVBlank();
            g_strobeFrames.store(s.frames);
            g_tickSlot.store((int)(presents % (uint64_t)s.frames));
            g_tickT.store(Now());
            g_tickCount.fetch_add(1);
            SetEvent(g_tickEvent);
            tickedAtVb = true;
        }
        refillNow = false;
        const double tVb = Now();
        const UINT bi = ScCBBI();
        if (bi >= n) { LOGE("BFI Presenter: unexpected back buffer index %u", bi); break; }

        double tFence = Now();
        PresStage(ST_SUBMIT);

        // Which game frame this refresh shows. A new frame is taken only at
        // the start of each strobe cycle (the lit refresh); the rest of the
        // cycle shows the same frame, as one CRT refresh shows one image.
        //   Smooth (default): the oldest finished frame not shown yet, so every
        //     game frame is lit exactly once and in order -- GPU timing jitter
        //     no longer causes repeat-then-skip stutter. At most one finished
        //     frame is held in reserve; if the game runs ahead, older ones drop.
        //   Lowest latency: the newest finished frame (v0.12 behaviour).
        // g_lock is held from the pick until the read is submitted, so the
        // game's staging submissions are ordered around it.
        const uint32_t slotNow = (uint32_t)(presents % (uint64_t)s.frames);
        int pick = -1;
        UINT64 reserve = 0, pickId = 0;
        double pickTime = 0.0;
        // The frame this cycle should show (1 or 2 after the last one, from
        // the recent average) sometimes finishes a moment after the pick:
        // the refresh then repeats a frame or takes one too few, and the next
        // jumps two or three -- uneven steps that judder under BFI (ABZU, The
        // Midnight Walk: up to 40% of cycles at 120 fps, 6% at the Half cap).
        // If that frame is already submitted, give it up to 3 ms; the refresh
        // is still drawn well before its vblank.
        if ((slotNow == 0 || lastShown < 0) && !s.queueAhead && llEv && lastShownId)
        {
            const int step = (int)std::floor(llStep + 0.5);
            if (step >= 1)
            {
                const UINT64 want = lastShownId + (UINT64)step;
                UINT64 wantFence = 0;
                EnterCriticalSection(&g_lock);
                for (UINT i = 0; i < kStage; i++)
                    if (g_stageId[i] == want) wantFence = g_stageFence[i];
                LeaveCriticalSection(&g_lock);
                if (wantFence && g_gameFence->GetCompletedValue() < wantFence)
                {
                    g_gameFence->SetEventOnCompletion(wantFence, llEv);
                    WaitForSingleObject(llEv, 3);
                }
            }
        }
        const UINT64 done = g_gameFence->GetCompletedValue();
        EnterCriticalSection(&g_lock);
        int newest = -1, oldestNew = -1, unseen = 0;
        UINT64 newestId = 0, oldestNewId = ~0ull;
        for (UINT i = 0; i < kStage; i++)
        {
            if (g_stageId[i] == 0 || g_stageFence[i] > done) continue;   // empty, being rewritten or unfinished
            if (g_stageId[i] > newestId) { newest = (int)i; newestId = g_stageId[i]; }
            if (g_stageId[i] > lastShownId)
            {
                unseen++;
                if (g_stageId[i] < oldestNewId) { oldestNew = (int)i; oldestNewId = g_stageId[i]; }
            }
        }
        const bool cycleStart = (slotNow == 0) || lastShown < 0;
        if (!cycleStart && lastShown >= 0 && g_stageId[lastShown] == lastShownId && g_stageFence[lastShown] <= done)
            pick = lastShown;                                     // rest of the cycle: same frame
        else if (s.pacingMode == 0 && oldestNew >= 0)
        {
            pick = oldestNew;
            // A second waiting frame that persists for 3 cycles is standing
            // latency, not jitter: skip one to catch up.
            if (cycleStart) extraCycles = (unseen >= 2) ? extraCycles + 1 : 0;
            if (s.smoothCatchUp && unseen <= 2 && extraCycles >= 3 && newest >= 0)
            {
                // Two frames waiting for 3 cycles in a row is standing latency:
                // show the newer one. (This used to pick "the newest but one",
                // which with two waiting is the older one again, so Smooth sat
                // a full cycle behind for good -- Half cap: 21 ms against 5 ms
                // in Lowest latency, The Midnight Walk.)
                pick = newest;
                extraCycles = 0;
            }
            else if (unseen > 2)                                  // running ahead: keep one in reserve, drop older
            {
                UINT64 bestId = 0;
                for (UINT i = 0; i < kStage; i++)
                    if (g_stageId[i] > lastShownId && g_stageId[i] < newestId && g_stageFence[i] <= done && g_stageId[i] > bestId)
                    { pick = (int)i; bestId = g_stageId[i]; }
                extraCycles = 0;
            }
        }
        else if (newest >= 0)
            pick = (s.pacingMode == 0 && lastShown >= 0 && g_stageId[lastShown] == lastShownId) ? lastShown : newest;
        if (pick >= 0) { pickId = g_stageId[pick]; g_presCurSlot = pick; }
        if (pick < 0)
        {
            int staged = 0;
            UINT64 need = ~0ull;
            for (UINT i = 0; i < kStage; i++)
                if (g_stageId[i] > 0) { staged++; if (g_stageFence[i] < need) need = g_stageFence[i]; }
            g_dbgStaged.store(staged);
            g_dbgDone.store(done);
            g_dbgNeed.store(staged ? need : 0);
            LeaveCriticalSection(&g_lock);
            if (!output) Sleep(1);
            continue;                                         // nothing shown yet
        }
        if (cycleStart && lastShownId && pickId >= lastShownId)
        {
            const int step = (int)(pickId - lastShownId);
            if (s.gameCap == 3) { if (step >= 2) secSkipU++; }        // Quarter: 0/1 alternate normally
            else
            {
                const int expect = std::max(1, (int)std::floor(llStep + 0.5));
                if (step < expect) secRepU++;
                else if (step > expect) secSkipU++;
            }
        }
        if (cycleStart && lastShownId && pickId > lastShownId)
        {
            const double d = (double)(pickId - lastShownId);
            llStep = llStep > 0.0 ? 0.95 * llStep + 0.05 * std::min(d, 8.0) : d;
        }
        if (cycleStart)
        {
            if (pickId == lastShownId) secFrameRepeats++;
            else if (lastShownId && pickId > lastShownId + 1) secFrameSkips += (int)(pickId - lastShownId - 1);
        }
        pickTime = g_stageTime[pick];

        Constants c = {};
        c.outType = (uint32_t)ResolveOutput(s);
        if ((int)c.outType != lastOutType)
        {
            static const char *kOut[] = { "SDR", "HDR scRGB", "HDR10 (PQ)" };
            LOGI("BFI Presenter: output %s (%s, swap chain format %d)", kOut[c.outType % 3],
                 s.output >= 0 ? "set in the settings" : "Auto", (int)g_desc.Format);
            lastOutType = (int)c.outType;
        }
        c.frames  = (uint32_t)s.frames;
        c.slot    = (uint32_t)(presents % (uint64_t)s.frames);
        c.method  = !s.enabled ? 2u : hitchPaused ? 3u : (uint32_t)s.method;
        // ABL guard: read the last measurement (non-blocking) and steer the
        // boost so a lit refresh stays under AblGuardNits on average -- never
        // below 0.5 (the lit refresh then equals the original image).
        if (measFence && measRb && g_presFence->GetCompletedValue() >= measFence)
        {
            float *px = nullptr;
            D3D12_RANGE rr = { 0, 256 * 18 };
            if (SUCCEEDED(measRb->Map(0, &rr, (void **)&px)) && px)
            {
                // Share of the screen (32x18 cells) whose lit refresh would be
                // brighter than the cap at full boost and full ceiling.
                // Panel workload: the highest ceiling C at which the lit
                // refresh, with every cell capped at C, averages no more than
                // the budget. Area and brightness both count: a small bright
                // highlight barely moves the average, a screen of white does.
                static float cells[32 * 18];
                for (int y = 0; y < 18; y++)
                    for (int x = 0; x < 32; x++) cells[y * 32 + x] = std::max(px[y * 64 + x], 0.0f) * 80.0f;
                D3D12_RANGE wr = { 0, 0 };
                measRb->Unmap(0, &wr);
                auto avgAt = [&](double C) { return AblAvgAt(cells, C, false, s.frames, s.method == 1); };
                const double peak = s.peakNits, B = s.ablBudget;
                if (avgAt(peak) <= B) ablSolve = peak;
                else
                {
                    double lo = 100.0, hi = peak;
                    for (int it = 0; it < 24; it++)
                    {
                        const double mid = 0.5 * (lo + hi);
                        if (avgAt(mid) <= B) lo = mid; else hi = mid;
                    }
                    ablSolve = lo;
                }
                hotArea = avgAt(ablSolve);                     // lit-refresh average at that ceiling (nits)
                rawAvg  = avgAt(1.0e9);                        // what the scene would ask for with no ceiling at all
                static bool firstMeas = true;
                if (firstMeas) { firstMeas = false; LOGI("BFI Presenter: ABL guard measuring (budget %d nits)", s.ablBudget); }
            }
            measFence = 0;
        }
        // ABL guard: while a large part of the lit refresh would exceed the
        // cap, the panel's power limit dims the picture (South of Midnight's
        // sky). Lower the lit-refresh ceiling to the cap for those scenes only
        // -- the energy above it spills into the dark refresh -- and restore
        // the full ceiling when the bright area is gone.
        const bool ablOn = s.ablGuard && psoMeas && s.enabled && ResolveOutput(s) != 0;
        ceilTarget = (ablOn && ablSolve > 0.0) ? std::min(ablSolve, (double)s.peakNits) : (double)s.peakNits;
        {
            const double nowT = Now(), dt = std::min(nowT - ablT, 0.1);
            ablT = nowT;
            if (ceilNow < 0.0 || !ablOn) ceilNow = ceilTarget;
            else if (ceilTarget < ceilNow - 5.0)
            {
                ablLowT = nowT;                                             // pulling down: fast
                ceilNow += (ceilTarget - ceilNow) * (1.0 - exp(-dt / 0.08));
            }
            else if (nowT - ablLowT > 1.5)
            {
                // Release only after the scene has stayed under budget for
                // 1.5 s, then slowly (tau 2.5 s). A quick release lets a bright
                // scene that flickers around the budget (camera pans, clouds)
                // bounce the ceiling between ~250 and ~1000 nits every second,
                // and each bounce trips the panel's own limiter: a flash.
                ceilNow += (ceilTarget - ceilNow) * (1.0 - exp(-dt / 2.5));
            }
        }
        c.gain    = s.gain;
        c.dfloor  = s.dfloor;
        c.knee    = s.knee;
        c.ceilLin = (c.outType == 0) ? 1.0f : (float)(floor(ceilNow / 10.0 + 0.5) * 10.0 / 80.0);
        c.flags   = (s.rolloff ? 1u : 0u) | (s.sine ? 2u : 0u) | (s.invert ? 4u : 0u) | (s.rolling ? 8u : 0u);
        c.seam    = s.rollSeam;
        c.decay   = s.rollDecay;

        const bool reuse = s.reuseBuffers && bufValid[bi] && bufFrame[bi] == pickId &&
                           memcmp(&bufConst[bi], &c, sizeof(c)) == 0;
        if (!reuse)
        {
            reserve = ++g_presFenceVal;
            g_readFence[pick] = reserve;
        }
        // The game never copies into the slot being shown (g_presCurSlot), so
        // the lock can go before waiting on the GPU.
        LeaveCriticalSection(&g_lock);
        if (!reuse)
        {
            PresStage(ST_BUFFENCE);
            WaitFence(g_presFence, bufFence[bi], ev);         // previous use of this real buffer done
            PresStage(ST_SUBMIT);
        }
        tFence = Now();
        if (reuse) secReused++;
        else
        {
        alloc[bi]->Reset();
        list->Reset(alloc[bi], pso);
        D3D12_RESOURCE_BARRIER br[2] = {
            Transition(real[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Transition(g_stage[pick], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) };
        list->ResourceBarrier(2, br);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)bi * rtvStride;
        D3D12_GPU_DESCRIPTOR_HANDLE srv = srvHeap->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += (UINT64)pick * srvStride;
        list->SetDescriptorHeaps(1, &srvHeap);
        list->SetGraphicsRootSignature(rootSig);
        list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
        list->SetGraphicsRootDescriptorTable(1, srv);
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)g_desc.Width, (float)g_desc.Height, 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, (LONG)g_desc.Width, (LONG)g_desc.Height };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        if (ablOn && !measFence && pickId != lastShownId && c.method < 2)
        {
            Constants mc = c;
            mc.gain = s.gain;                                  // brightness at the configured boost
            mc.ceilLin = s.peakNits / 80.0f;                   // and the full ceiling
            D3D12_RESOURCE_BARRIER mb = Transition(measRt, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &mb);
            list->SetPipelineState(psoMeas);
            list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &mc, 0);
            D3D12_CPU_DESCRIPTOR_HANDLE mrtv = measHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_VIEWPORT mvp = { 0.0f, 0.0f, 32.0f, 18.0f, 0.0f, 1.0f };
            D3D12_RECT msc = { 0, 0, 32, 18 };
            list->RSSetViewports(1, &mvp);
            list->RSSetScissorRects(1, &msc);
            list->OMSetRenderTargets(1, &mrtv, FALSE, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
            mb = Transition(measRt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &mb);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, srcLoc = {};
            dst.pResource = measRb;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = 0;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
            dst.PlacedFootprint.Footprint.Width = 32;
            dst.PlacedFootprint.Footprint.Height = 18;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = 256;
            srcLoc.pResource = measRt;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
            mb = Transition(measRt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &mb);
            measFence = reserve;
        }
        br[0] = Transition(real[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        br[1] = Transition(g_stage[pick], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(2, br);
        if (FAILED(list->Close())) LOGE("BFI Presenter: presenter draw list failed to record (Close) -- see BFIPresenter_debug.txt with Diagnostics=2");

        ID3D12CommandList *lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(g_presFence, reserve);
        bufFence[bi] = reserve;
        bufFrame[bi] = pickId;
        bufConst[bi] = c;
        bufValid[bi] = true;
        }

        const double tSub = Now();
        PresStage(ST_PRESENT);
        HRESULT hr;
        hr = OwnPresent();
        const double tPres = Now();
        PresStage(ST_IDLE);
        g_lastPresentHr.store(hr);
        g_lastPresentT.store(Now());
        if (!g_firstPresented.exchange(true))
            LOGI("BFI Presenter: first frame presented (0x%08lX)", (unsigned long)hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            LOGE("BFI Presenter: device lost (0x%08lX)", (unsigned long)hr);
            DumpDeviceRemoved(g_device, "presenter Present");
            break;
        }
        presents++;
        if (!tickedAtVb)
        {
            g_tickT.store(Now());
            g_tickCount.fetch_add(1);
            SetEvent(g_tickEvent);
        }
        if (s.pacing && s.queueAhead && n >= 3 && !vsyncOverridden)
        {
            UINT lastPC = 0;
            DXGI_FRAME_STATISTICS qs = {};
            if (SUCCEEDED(ScLPC(&lastPC)) && SUCCEEDED(ScStats(&qs)))
            {
                queuedNow = (int)(lastPC - qs.PresentCount);
                // Right after a vblank-paced present one present is pending;
                // keep one more so the next slot is already covered.
                if (queuedNow >= 0 && queuedNow < 2) refillNow = true;
            }
            else queuedNow = -1;
        }
        else queuedNow = -1;

        const double t = Now();
        secPresents++;
        ageSum += (t - pickTime) * 1000.0;
        if (t - lastT > 1.5 / (modeHz > 0.0 ? modeHz : 120.0) && presents > 1)   // a missed refresh at the display's own rate
        {
            secLate++;
            lateTimes[lateIdx++ & 3] = t;
        }
        if (s.hitchGuard && s.enabled)
        {
            // 3 missed refreshes within half a second, or a game stall.
            const double third = lateTimes[(lateIdx + 1) & 3];
            const bool lateBurst = lateIdx >= 3 && t - third < 0.5;
            const bool stalled   = lastNewFrameT > 0.0 && (t - lastNewFrameT) * 1000.0 > s.hitchGapMs;
            if (lateBurst || stalled) hitchUntil = t + 1.0;
            const bool now = t < hitchUntil;
            if (now != hitchPaused)
            {
                hitchPaused = now;
                LOGI("BFI Presenter: hitch guard: BFI %s (%s)", now ? "paused" : "resumed",
                     now ? (stalled ? "game stalled" : "missed refreshes") : "steady again");
            }
        }
        else hitchPaused = false;
        if (t - secStart >= 1.0)
        {
            DXGI_FRAME_STATISTICS st = {};
            const bool sok = SUCCEEDED(ScStats(&st));
            int rep = 0;
            const double rate = secPresents / (t - secStart);
            if (!vsyncOverridden)
            {
                const bool fast = (sok && haveStats && st.SyncRefreshCount > lastSync)
                                ? rate > 1.4 * (double)(st.SyncRefreshCount - lastSync) / (t - secStart)
                                : rate > 300.0;
                if (fast && presents > 240)
                {
                    vsyncOverridden = true;
                    LOGE("BFI Presenter: %.0f presents/s -- the driver is ignoring VSync (NVIDIA profile: Vertical Sync "
                         "Force off or Fast Sync?). Pacing to vblank from now on; set Vertical Sync to Force on.", rate);
                }
            }
            // VRR check: the display's own refresh count. With G-SYNC active the
            // panel refreshes when the presenter's frame arrives, a little later
            // than the fixed vblank each time, so the strobe runs slow and
            // uneven (South of Midnight: 110-112 Hz in stretches, 120 between).
            if (sok && haveStats && st.SyncRefreshCount > lastSync)
            {
                const double hz = (double)(st.SyncRefreshCount - lastSync) / (t - secStart);
                if (hz > peakHz) peakHz = hz;
                // The display mode's rate is the reference when known; a single
                // second that counts a few extra refreshes (after a stall) must
                // not make a steady 120 look slow.
                if (modeHz > 0.0) peakHz = modeHz;
                vrrSlowSecs = (peakHz > 0.0 && hz < (modeHz > 0.0 ? 0.99 : 0.96) * peakHz && secLate < 10) ? vrrSlowSecs + 1 : 0;
                if (vrrSlowSecs == 3 && !vrrLogged)
                {
                    vrrLogged = true;
                    LOGW("BFI Presenter: the display is refreshing at %.1f Hz instead of %.1f Hz with no missed refreshes -- "
                         "VRR (G-SYNC) is active. Turn G-SYNC off for this game (NVIDIA profile) or turn on Queue one refresh ahead.",
                         hz, peakHz);
                }
                displayHz = hz;
                // A frame limiter hooked into the presenter's own swap chain
                // (RTSS at 118 in God of War): the display keeps its fixed
                // refresh, but the presenter only gets a little under that many
                // presents through, so the strobe slips a refresh every half
                // second or so. VRR would pull the display rate down with it.
                const double prate = secPresents / (t - secStart);
                limitSecs = (prate < 0.985 * hz && prate > 0.5 * hz && secLate < 3 && hz > 0.97 * peakHz) ? limitSecs + 1 : 0;
                if (limitSecs == 3 && !limitLogged)
                {
                    limitLogged = true;
                    LOGW("BFI Presenter: the presenter is held to %.1f presents/s on a %.1f Hz display -- a frame limiter "
                         "is acting on it (RTSS: set the frame limit to 0 for this game). The strobe slips a refresh "
                         "about %.0f times a second, which shows as flicker.", prate, hz, hz - prate);
                }
            }
            if (sok && haveStats) rep = (int)(st.SyncRefreshCount - lastSync) - (int)(st.PresentCount - lastPC);
            if (sok) { lastSync = st.SyncRefreshCount; lastPC = st.PresentCount; haveStats = true; }
            EnterCriticalSection(&g_cfgLock);
            g_status.refreshHz  = secPresents / (t - secStart);
            g_status.gameFps    = g_stagedThisSecond.exchange(0) / (t - secStart);
            g_status.frameAgeMs = secPresents ? ageSum / secPresents : 0.0;
            g_status.late       = secLate;
            g_status.repeats    = rep > 0 ? rep : 0;
            g_status.frameRepeats = secFrameRepeats;
            g_status.unevenRep    = secRepU;
            g_status.unevenSkip   = secSkipU;
            g_status.slotWaits    = g_slotWaits.exchange(0);
            g_status.vsyncOverridden = vsyncOverridden;
            g_status.queued       = queuedNow;
            g_status.reused       = secReused;
            g_status.hitchPaused  = hitchPaused;
            g_status.hotArea      = (s.ablGuard && psoMeas) ? hotArea : -1.0;
            g_status.rawAvg       = (s.ablGuard && psoMeas) ? rawAvg : -1.0;
            g_status.ceilInUse    = c.ceilLin * 80.0;
            g_status.displayHz    = displayHz;
            g_status.vrrSuspect   = vrrSlowSecs >= 3;
            g_status.limitSuspect = limitSecs >= 3;
            {
                const int lost = g_lostFrames.exchange(0);
                if (lost > 0 && !lostLogged)
                {
                    lostLogged = true;
                    LOGW("BFI Presenter: %d finished game frame(s) in the last second were replaced before they could be shown "
                         "(the GPU is running far behind the game).", lost);
                }
            }
            secReused = 0;
            g_status.frameSkips   = secFrameSkips;
            // Automatic frames ahead: 1 (least latency) normally; 2 while the
            // game keeps missing strobe cycles (CPU and GPU then overlap again,
            // South of Midnight: 55 -> 60 fps); back to 1 when the game waits
            // long enough in the frame cap to show it has the headroom.
            {
                const long long us = g_capWaitUs.exchange(0);
                const int n = g_capWaits.exchange(0);
                const double capWaitMs = n ? us / 1000.0 / n : 0.0;
                if (s.autoAhead && s.maxAhead != 0)
                {
                    if (autoAhead == 1)
                    {
                        missSecs = (secRepU >= 2 && s.gameCap >= 1) ? missSecs + 1 : 0;
                        if (missSecs >= 3)
                        {
                            autoAhead = 2; missSecs = 0; spareSecs = 0;
                            // Needed 2 again within a minute of trying 1: the game
                            // is close to the limit -- stay at 2, no more switching.
                            aheadLocked = aheadDropT >= 0.0 && t - aheadDropT < 60.0;
                            LOGI("BFI Presenter: automatic frames ahead: 2 (the game kept missing strobe cycles)%s",
                                 aheadLocked ? " -- staying at 2 for this session" : "");
                        }
                    }
                    else
                    {
                        spareSecs = (!aheadLocked && secRepU == 0 && capWaitMs > 6.0) ? spareSecs + 1 : 0;
                        if (spareSecs >= 10)
                        {
                            autoAhead = 1; spareSecs = 0; missSecs = 0; aheadDropT = t;
                            LOGI("BFI Presenter: automatic frames ahead: 1 (headroom: %.1f ms spare per frame)", capWaitMs);
                        }
                    }
                    g_maxAhead.store(autoAhead);
                }
                else g_maxAhead.store(s.maxAhead);
                g_status.aheadInUse = g_maxAhead.load();
            }
            secFrameRepeats = secFrameSkips = 0;
            secRepU = secSkipU = 0;
            g_status.outType    = (int)c.outType;
            LeaveCriticalSection(&g_cfgLock);
            secStart = t; secPresents = 0; secLate = 0; ageSum = 0.0;
            if (g_device->GetDeviceRemovedReason() != S_OK) { DumpDeviceRemoved(g_device, "presenter check"); break; }
            if (g_dx11.load()) Sync11Window();
        }
        if (logf)
        {
            DXGI_FRAME_STATISTICS st = {};
            const bool sok = SUCCEEDED(ScStats(&st));
            Status lrLog;
            EnterCriticalSection(&g_cfgLock);
            lrLog.lrState = g_status.lrState; lrLog.lrEst = g_status.lrEst; lrLog.lrMargin = g_status.lrMargin;
            LeaveCriticalSection(&g_cfgLock);
            fprintf(logf, "%.3f,%llu,%u,%llu,%.3f,%.3f,%d,%d,%d,%d,%d,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%d,%.0f,%.1f,%.1f,%d,%.3f,%.3f,%.3f,%d\n", t * 1000.0,
                    (unsigned long long)presents, c.slot,
                    (unsigned long long)pickId, (t - pickTime) * 1000.0, (t - lastT) * 1000.0,
                    sok ? (int)st.PresentCount : -1, sok ? (int)st.SyncRefreshCount : -1,
                    s.enabled, s.method, s.frames, s.gain, s.dfloor, s.rolloff, s.pacingMode, s.gameCap,
                    g_maxAhead.load(), s.pacing, s.queueAhead, queuedNow,
                    (tVb - tLoop0) * 1000.0, (tFence - tVb) * 1000.0, (tSub - tFence) * 1000.0, (tPres - tSub) * 1000.0, hitchPaused ? 1 : 0, c.ceilLin * 80.0f, hotArea, rawAvg,
                    lrLog.lrState, g_lrLead.load() * 1000.0, lrLog.lrEst, lrLog.lrMargin, s.smoothCatchUp);
        }
        lastT = t;
        if (pickId != lastShownId) lastNewFrameT = Now();
        if (pickId != lastShownId)
        {
            // The game may render on: one release per frame done with.
            ReleaseUpTo(pickId);
            g_shownGameFrames.store(pickId);
        }
        lastShown = pick;
        lastShownId = pickId;
    }
    (void)lastShown; (void)lastShownId;

    // --- drain and release
    if (queue && g_presFence)
    {
        UINT64 v;
        EnterCriticalSection(&g_lock);
        v = ++g_presFenceVal;
        LeaveCriticalSection(&g_lock);
        queue->Signal(g_presFence, v);
        WaitFence(g_presFence, v, ev);
    }
    if (logf) fclose(logf);
    SafeRelease(output);
    SafeRelease(pso);
    SafeRelease(psoMeas);
    SafeRelease(measRt);
    SafeRelease(measRb);
    SafeRelease(measHeap);
    SafeRelease(psMeas);
    SafeRelease(rootSig);
    SafeRelease(rsBlob);
    SafeRelease(rsErr);
    SafeRelease(vs);
    SafeRelease(ps);
    SafeRelease(list);
    for (UINT i = 0; i < kMaxBuf; i++) { SafeRelease(alloc[i]); SafeRelease(real[i]); }
    SafeRelease(srvHeap);
    SafeRelease(rtvHeap);
    CloseHandle(ev);
    if (llEv) CloseHandle(llEv);
    EnterCriticalSection(&g_cfgLock);
    g_status.running = 0;
    LeaveCriticalSection(&g_cfgLock);
    g_lrAllowed.store(false);
    g_running.store(false);
    SetEvent(g_tickEvent);
    return 0;
}

static void StartPresenter()
{
    if (g_running.load() || (!g_gameQueue && !g_dx11.load()) || g_bufCount == 0) return;
    g_presStageT.store(Now());   // the watchdog measures from here, not from 0
    g_running.store(true);
    g_thread = CreateThread(nullptr, 0, PresenterThread, nullptr, 0, nullptr);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_TIME_CRITICAL);
    else g_running.store(false);
    if (g_thread && !g_lrThread)
    {
        if (!g_lrEv) g_lrEv = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g_lrRun.store(true);
        g_lrThread = CreateThread(nullptr, 0, LrWatchThread, nullptr, 0, nullptr);
        if (g_lrThread) SetThreadPriority(g_lrThread, THREAD_PRIORITY_HIGHEST);
    }
}

// A presenter thread that would not stop. Nothing it uses is ever freed
// after this (the resources are left to the process exit), and BFI stays off
// until the game restarts. Freeing them under it crashed The Rogue Prince of
// Persia (presenter thread on a released fence) when the game left exclusive
// fullscreen.
static std::atomic<bool> g_presenterStuck{false};

// Returns false when the presenter thread is still running after the wait.
static bool StopPresenter()
{
    if (g_presenterStuck.load()) return false;
    if (g_lrThread)
    {
        g_lrRun.store(false);
        if (g_lrEv) SetEvent(g_lrEv);
        WaitForSingleObject(g_lrThread, 2000);
        CloseHandle(g_lrThread);
        g_lrThread = nullptr;
    }
    g_lrLead.store(0.0);
    if (g_thread)
    {
        g_running.store(false);
        SetEvent(g_tickEvent);
        // Wait while serving messages sent to this thread's windows. This is
        // usually the game's window thread (a swap chain is being destroyed),
        // and DXGI can send that window a message from inside the presenter's
        // Present (DX11 path: the presenter's window is a child of the
        // game's). A plain wait deadlocked The Rogue Prince of Persia when it
        // left exclusive fullscreen and recreated its swap chain.
        const DWORD until = GetTickCount() + 5000;
        for (;;)
        {
            const DWORD now = GetTickCount();
            if ((LONG)(until - now) <= 0)
            {
                // Keep everything it may touch alive and leave it be.
                g_presenterStuck.store(true);
                g_presenterFailed.store(true);
                LOGE("BFI Presenter: presenter thread did not stop within 5 s -- BFI is off until the game restarts "
                     "(its resources are kept so the game can continue)");
                return false;
            }
            const DWORD r = MsgWaitForMultipleObjects(1, &g_thread, FALSE, until - now, QS_SENDMESSAGE);
            if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
            MSG m;
            PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);   // dispatches sent messages only
        }
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    // Drain the presenter queue. Signalling a value above every reservation
    // also releases game-queue waits on presenter reads that never happened.
    if (g_presQueue && g_presFence)
    {
        EnterCriticalSection(&g_lock);
        const UINT64 v = ++g_presFenceVal;
        LeaveCriticalSection(&g_lock);
        g_presQueue->Signal(g_presFence, v);
        WaitFence(g_presFence, v, g_gameFenceEvent);
    }
    // Let the game queue finish our copies before resources go away.
    if (g_dx11.load() && g_gameFence)
        WaitFence(g_gameFence, g_gameFenceVal, g_gameFenceEvent);     // the DX11 copies (flushed at each frame)
    else if (g_gameQueue && g_gameFence)
    {
        g_gameQueue->Signal(g_gameFence, ++g_gameFenceVal);
        WaitFence(g_gameFence, g_gameFenceVal, g_gameFenceEvent);
    }
    TopUpGameSlots();
    g_releasedUpTo.store(g_frameId);            // every frame so far counts as given back
    return true;
}

// ---------------------------------------------------------------- game side
// Called for the game's (and ReShade's) Present: copy the finished frame into
// the staging ring, then return without presenting. The presenter shows it.
// Frame cap: release the game every 1, 2 or 4 refreshes, counted against the
// presenter's refreshes so the rate is exact and stays in step with the
// strobe. With one game frame per strobe cycle (Half cap at 2 frames per
// cycle, Quarter at 4) the release is also placed one refresh before the lit
// one, so the frame finishes in time for it. Released at the vblank before the
// lit refresh, a frame missed that pick and was shown a whole cycle later
// (The Midnight Walk, Half cap: 13 ms old with Lowest latency, 30 ms with
// Smooth, which also kept the late frame in reserve).
// Half / Quarter cap, just in time: each game frame targets a lit refresh
// (per refreshes after the previous frame's) and is released at that lit
// refresh's vblank minus the measured build time and margin. Releasing one
// refresh before the lit one (v0.69) left frames that need a little more --
// Obduction: 4 ms CPU + GPU -- finishing right at the pick, so some made it
// and some were shown a cycle later: repeat-then-skip pairs at Half, frames
// held 1 then 3 cycles at Quarter.
static bool CapWaitJit(UINT64 per, int frames, double lead)
{
    static UINT64 target = 0;       // tick count of the lit refresh the last frame was released for
    static HANDLE timer = nullptr;
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    const double period = g_lrPeriod.load();
    UINT64 c = 0; int sl = 0; double tT = 0.0;
    for (int i = 0; i < 4; i++)
    {
        c = g_tickCount.load(); sl = g_tickSlot.load(); tT = g_tickT.load();
        if (g_tickCount.load() == c) break;
    }
    if (c == 0 || tT <= 0.0) return false;
    auto timeOf = [&](UINT64 x) { return tT + (double)(INT64)(x - c) * period; };
    const double now = Now();
    UINT64 want = target ? target + per : 0;
    // Stale (presenter restarted, game paused) or that lit refresh already
    // passed: aim at the first lit refresh there is still time for.
    if (want == 0 || want <= c || want > c + 2 * per || timeOf(want) <= now)
    {
        UINT64 k = 1;
        for (; k < (UINT64)(4 * per + frames); k++)
            if ((sl + (int)k) % frames == 0 && timeOf(c + k) - lead >= now - 0.0005) break;
        want = c + k;
    }
    target = want;
    const double vb = timeOf(want), rel = vb - lead;
    bool held = false;
    if (rel > now && rel - now < (double)(per + 1) * period) { SleepUntil(rel, timer); held = true; }
    const UINT64 id = g_frameId + 1;
    const int k = (int)(id % kLr);
    g_lrRelT[k] = Now();
    g_lrDeadline[k] = held ? vb + 0.0005 : 0.0;
    g_lrRelId[k] = id;
    return true;
}

static void CapWait(UINT64 per)
{
    if (per >= 2 && g_lrAllowed.load())
    {
        const int fr = g_strobeFrames.load();
        const double lead = g_lrLead.load();
        const double w0 = Now();
        if (fr >= 2 && per % (UINT64)fr == 0 && lead > 0.0 && CapWaitJit(per, fr, lead))
        {
            g_capWaitUs.fetch_add((long long)((Now() - w0) * 1e6));
            g_capWaits.fetch_add(1);
            return;
        }
    }
    static UINT64 nextTick = 0;
    UINT64 now = g_tickCount.load();
    if (nextTick == 0 || nextTick > now + per) nextTick = now;   // first frame, or the presenter restarted
    const double waitStart = Now();
    const bool onTime = now <= nextTick;
    while (now < nextTick && Now() - waitStart < 0.25)
    {
        WaitForSingleObject(g_tickEvent, 20);
        now = g_tickCount.load();
    }
    const int frames = g_strobeFrames.load();
    if (onTime && (int)per == frames && frames >= 2)
    {
        // At most per-1 more refreshes to land on the one before the lit refresh.
        const UINT64 limit = now + per - 1;
        while (g_tickSlot.load() != frames - 1 && now < limit && Now() - waitStart < 0.25)
        {
            WaitForSingleObject(g_tickEvent, 20);
            now = g_tickCount.load();
        }
        nextTick = now + per;
    }
    else
        nextTick = (now >= nextTick + per ? now : nextTick) + per;   // late: start counting from now
    if (per >= 2 && g_lrAllowed.load())
    {
        // Measuring (or paused): released at the refresh as before; record the
        // release so the build time is known.
        const UINT64 id = g_frameId + 1;
        const int k = (int)(id % kLr);
        g_lrRelT[k] = Now();
        g_lrDeadline[k] = 0.0;
        g_lrRelId[k] = id;
    }
    if (per == 1 && g_lrAllowed.load())
    {
        // Later release: hold the game until just in time for the pick at the
        // next vblank, so the frame it renders reads its input later.
        static HANDLE timer = nullptr;
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        const double lead = g_lrLead.load(), period = g_lrPeriod.load();
        const double pickVb = g_tickT.load() + period;
        const double target = pickVb - lead;
        const double t0 = Now();
        bool held = false;
        if (lead > 0.0 && target > t0 && target - t0 < period) { SleepUntil(target, timer); held = true; }
        const UINT64 id = g_frameId + 1;                    // the frame the game renders next
        const int k = (int)(id % kLr);
        g_lrRelT[k] = Now();
        g_lrDeadline[k] = held ? pickVb + 0.0005 : 0.0;
        g_lrRelId[k] = id;
    }
    g_capWaitUs.fetch_add((long long)((Now() - waitStart) * 1e6));
    g_capWaits.fetch_add(1);
}

static HRESULT ResizeOnPresenterQueue(IDXGISwapChain3 *sc, UINT cnt, UINT w, UINT h, DXGI_FORMAT f, UINT fl);

static HRESULT SubmitGameFrameLocked()
{
    if (!g_gameQueue) return OwnPresent();

    // First frame: move presentation to the presenter's queue and start the
    // presenter. Done here rather than in init_swapchain, where ReShade holds
    // its queue lock -- ResizeBuffers1 there hung Resonance's second swap
    // chain while the game's other threads were submitting work.
    // The move to the presenter's own queue is only ever done inside the
    // game's own ResizeBuffers call. Resizing from here (inside Present) hung
    // God of War Ragnarok's recreated swap chain. Until the game resizes, the
    // presenter draws and presents on the game's queue.
    if (!g_running.load() && !g_presenterFailed.load() && !(g_deferPresenter.load() && !g_ownPresentQueue))
    {
        if (g_needRebind.load())
            LOGI("BFI Presenter: presenting on the game's queue until the game resizes its swap chain");
        StartPresenter();
    }
    if (!g_running.load())
    {
        // Presenter not running: copy the frame to the real back buffer and
        // present it directly, on whichever queue the swap chain presents on.
        ID3D12CommandQueue *q = g_ownPresentQueue ? g_presQueue : g_gameQueue;
        ID3D12Resource *rb = nullptr;
        GameStage(ST_G_OWN_INDEX);
        const UINT bi = o_GetCBBI(g_sc);
        GameStage(ST_G_OWN_GETBUF);
        if (bi < kMaxBuf && SUCCEEDED(o_GetBuffer(g_sc, bi, __uuidof(ID3D12Resource), (void **)&rb)))
        {
            GameStage(ST_G_OWN_COPY);
            g_gameQueue->Signal(g_gameFence, ++g_gameFenceVal);          // game frame finished
            if (q != g_gameQueue) q->Wait(g_gameFence, g_gameFenceVal);
            WaitFence(g_presFence, g_fbFence[bi], g_gameFenceEvent);
            g_fbAlloc[bi]->Reset();
            g_fbList->Reset(g_fbAlloc[bi], nullptr);
            D3D12_RESOURCE_BARRIER br[2] = {
                Transition(g_virt[g_cur], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST) };
            g_fbList->ResourceBarrier(2, br);
            g_fbList->CopyResource(rb, g_virt[g_cur]);
            br[0] = Transition(g_virt[g_cur], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            br[1] = Transition(rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            g_fbList->ResourceBarrier(2, br);
            if (FAILED(g_fbList->Close())) LOGE("BFI Presenter: pass-through copy list failed to record (Close)");
            ID3D12CommandList *l[] = { g_fbList };
            q->ExecuteCommandLists(1, l);
            UINT64 v;
            EnterCriticalSection(&g_lock);
            v = ++g_presFenceVal;
            LeaveCriticalSection(&g_lock);
            q->Signal(g_presFence, v);
            g_fbFence[bi] = v;
            if (q != g_gameQueue) g_gameQueue->Wait(g_presFence, v);   // copy done before reuse
            rb->Release();
        }
        g_cur = (g_cur + 1) % g_bufCount;
        ReleaseGameSlot();
        GameStage(ST_G_OWN_PRESENT);
        return OwnPresent();
    }

    // Staging slot for this frame: the oldest one the presenter is neither
    // showing nor still reading. Taking slots strictly in turn made the game's
    // GPU queue wait for the presenter's queued reads -- with presenter
    // buffering (3 buffers, vblank off) those complete up to a few refreshes
    // late, and the game's frame rate collapsed.
    UINT s = g_stageNext;
    {
        const UINT64 pdone = g_presFence->GetCompletedValue();
        const UINT64 gdone = g_gameFence->GetCompletedValue();
        int best = -1;
        UINT64 bestId = ~0ull;
        EnterCriticalSection(&g_lock);
        for (UINT i = 0; i < kStage; i++)
        {
            if ((int)i == g_presCurSlot) continue;
            if (g_readFence[i] > pdone || g_stageFence[i] > gdone) continue;
            if (g_stageId[i] < bestId) { best = (int)i; bestId = g_stageId[i]; }
        }
        LeaveCriticalSection(&g_lock);
        if (best >= 0) s = (UINT)best;
        else g_slotWaits.fetch_add(1);
        g_gtFallback = best < 0;
    }
    GameStage(ST_G_SLOT);
    // Allocator for this slot must be idle (previous copy into it finished).
    WaitFence(g_gameFence, g_stageFence[s], g_gameFenceEvent);
    g_gtSlot = Now();

    GameStage(ST_G_COPY);
    // g_lock covers only marking the slot as being rewritten (after which the
    // presenter can't pick it, and any read of it it already made is in
    // g_readFence) and publishing the finished frame. It used to be held
    // across recording and submitting the copy: when the driver blocked that
    // submission for 50-70 ms (Obduction, streaming), the presenter waited
    // on the lock, missed 6-8 refreshes and the panel held one -- a flash.
    UINT64 waitRead;
    EnterCriticalSection(&g_lock);
    g_gtLock = Now();
    waitRead = g_readFence[s];
    if (g_stageId[s] > g_shownGameFrames.load()) g_lostFrames.fetch_add(1);
    g_stageId[s] = 0;                         // being rewritten: not selectable
    LeaveCriticalSection(&g_lock);

    g_copyAlloc[s]->Reset();
    g_copyList->Reset(g_copyAlloc[s], nullptr);
    D3D12_RESOURCE_BARRIER br[2] = {
        Transition(g_virt[g_cur], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(g_stage[s], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
    g_copyList->ResourceBarrier(2, br);
    g_copyList->CopyResource(g_stage[s], g_virt[g_cur]);
    br[0] = Transition(g_virt[g_cur], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    br[1] = Transition(g_stage[s], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    g_copyList->ResourceBarrier(2, br);
    {
        const HRESULT ch = g_copyList->Close();
        static bool once = false;
        if (FAILED(ch) && !once)
        {
            once = true;
            const D3D12_RESOURCE_DESC vd = g_virt[g_cur]->GetDesc(), sd = g_stage[s]->GetDesc();
            LOGE("BFI Presenter: frame copy list failed to record (0x%08lX): game buffer %llux%u fmt %d flags 0x%X, "
                 "staging %llux%u fmt %d flags 0x%X", (unsigned long)ch, (unsigned long long)vd.Width, vd.Height,
                 (int)vd.Format, (unsigned)vd.Flags, (unsigned long long)sd.Width, sd.Height, (int)sd.Format, (unsigned)sd.Flags);
        }
    }

    if (waitRead) g_gameQueue->Wait(g_presFence, waitRead);   // presenter done reading this slot
    ID3D12CommandList *l[] = { g_copyList };
    g_gameQueue->ExecuteCommandLists(1, l);
    g_gameQueue->Signal(g_gameFence, ++g_gameFenceVal);
    EnterCriticalSection(&g_lock);            // publish: selectable from here on
    g_stageFence[s] = g_gameFenceVal;
    g_stageId[s]    = ++g_frameId;
    g_recentFence[g_frameId % 8] = g_gameFenceVal;
    g_stageTime[s]  = Now();
    LeaveCriticalSection(&g_lock);
    LrStaged(g_frameId, g_gameFenceVal);
    g_gtCopy = Now();

    g_stageNext = (s + 1) % kStage;
    g_cur = (g_cur + 1) % g_bufCount;
    g_stagedThisSecond.fetch_add(1);
    if (!g_firstStaged.exchange(true)) LOGI("BFI Presenter: first game frame received");
    if (g_gameFence->GetCompletedValue() == UINT64_MAX) DumpDeviceRemoved(g_device, "game Present");

    // VSync-like cap: at most one game frame per display refresh.
    const int cap = g_gameCap.load();
    // Latency limit: before the game starts its next frame, the frame from
    // MaxFramesAhead frames ago must be finished on the GPU, so the game can't
    // queue several frames ahead (each one is ~17 ms of lag at 60 fps).
    const int ahead = g_maxAhead.load();
    if (ahead > 0 && g_frameId > (UINT64)ahead)
    {
        GameStage(ST_G_CAP);
        const UINT64 need = g_recentFence[(g_frameId - ahead) % 8];
        WaitFence(g_gameFence, need, g_gameFenceEvent);
    }
    g_gtAhead = Now();
    if (g_releaseOnSubmit.load()) ReleaseUpTo(g_frameId);   // lowest latency: the game is not paced by the strobe
    GameStage(ST_G_CAP);
    // Release the game every 1, 2 or 4 refreshes, counted against the
    // presenter's refreshes so the rate is exact and stays in step with the
    // strobe (a game that runs slower is simply released when it arrives).
    const UINT64 per = cap == 1 ? 1 : cap == 2 ? 2 : cap == 3 ? 4 : 0;
    if (per && g_running.load()) CapWait(per);

    const double since = Now() - g_lastPresentT.load();
    if (g_lastPresentT.load() > 0.0 && since > 2.0 && !g_stallLogged.exchange(true))
        LOGE("BFI Presenter: presenter has not presented for %.1f s while the game is still rendering", since);

    const HRESULT hr = g_lastPresentHr.load();
    return (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) ? hr : S_OK;
}

// Per-frame breakdown of the game's Present call (BFIPresenter_game_*.csv).
static void WriteGameLog()
{
    // Per-frame breakdown: where the game's Present call spent its time, and
    // how long the game spent outside it (its own CPU work and waits).
    if (g_logOn.load() && !g_gameLog)
    {
        char path[MAX_PATH];
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        snprintf(path, sizeof(path), "%.*sBFIPresenter_game_%04u%02u%02u_%02u%02u%02u.csv",
                 (int)(strlen(g_logPath) - strlen("BFIPresenter_log.csv")), g_logPath,
                 lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
        g_gameLog = fopen(path, "w");
        if (g_gameLog)
            fprintf(g_gameLog, "time_ms,frame_id,outside_ms,serial_ms,slot_ms,lock_ms,copy_ms,ahead_ms,cap_ms,total_ms,slot_fallback\n");
    }
    else if (!g_logOn.load() && g_gameLog)
    {
        fclose(g_gameLog);
        g_gameLog = nullptr;
    }
    if (g_gameLog && g_gtCopy > 0.0)
    {
        const double ahead = g_gtAhead > 0.0 ? g_gtAhead : g_gtCopy;
        fprintf(g_gameLog, "%.3f,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", g_gtEnter * 1000.0,
                (unsigned long long)g_frameId,
                g_gtPrevExit > 0.0 ? (g_gtEnter - g_gtPrevExit) * 1000.0 : 0.0,
                (g_gtSerial - g_gtEnter) * 1000.0, (g_gtSlot - g_gtSerial) * 1000.0,
                (g_gtLock - g_gtSlot) * 1000.0, (g_gtCopy - g_gtLock) * 1000.0,
                (ahead - g_gtCopy) * 1000.0, (g_gtExit - ahead) * 1000.0,
                (g_gtExit - g_gtEnter) * 1000.0, g_gtFallback);
        if ((g_frameId % 60) == 0) fflush(g_gameLog);
    }
}

// Present can arrive from more than one thread (frame-generation add-ons
// present generated frames themselves). The staging path shares one command
// list and ring, so calls are serialised; new presenting threads are logged.
static HRESULT SubmitGameFrame()
{
    const DWORD tid = GetCurrentThreadId();
    const double enter = Now();
    GameStage(ST_G_SLOT);
    EnterCriticalSection(&g_submitLock);
    g_gtEnter = enter;
    g_gtSerial = Now();
    g_gtSlot = g_gtLock = g_gtCopy = g_gtAhead = 0.0;
    g_gtFallback = 0;
    for (DWORD &t : g_presentThreads)
    {
        if (t == tid) break;
        if (t == 0)
        {
            t = tid;
            LOGI("BFI Presenter: game frames presented from thread %lu", (unsigned long)tid);
            break;
        }
    }
    const HRESULT hr = SubmitGameFrameLocked();
    g_gtExit = Now();
    WriteGameLog();
    g_gtPrevExit = g_gtExit;
    GameStage(ST_G_OUTSIDE);
    LeaveCriticalSection(&g_submitLock);
    return hr;
}

static bool IsManaged(void *sc) { return g_active.load() && !g_dx11.load() && sc == (void *)g_sc; }

static HRESULT STDMETHODCALLTYPE hk_GetDesc(IDXGISwapChain *sc, DXGI_SWAP_CHAIN_DESC *d)
{
    const HRESULT hr = o_GetDesc(sc, d);
    if (SUCCEEDED(hr) && d && IsManaged(sc) && g_virtCount && !t_presenterThread && !t_ownCall) d->BufferCount = g_virtCount;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_GetDesc1(IDXGISwapChain1 *sc, DXGI_SWAP_CHAIN_DESC1 *d)
{
    const HRESULT hr = o_GetDesc1(sc, d);
    if (SUCCEEDED(hr) && d && IsManaged(sc) && g_virtCount && !t_presenterThread && !t_ownCall) d->BufferCount = g_virtCount;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_GetFS(IDXGISwapChain *sc, DXGI_FRAME_STATISTICS *st)
{
    if (!t_ownCall && !t_presenterThread && IsGame11(sc) && g_sc && st)
    {
        // Its own swap chain never presents: report the presenter's, in terms
        // of the game's frames.
        HRESULT hr;
        hr = ScStats(st);
        if (SUCCEEDED(hr) && g_virtualStats.load() && g_running.load())
        {
            st->PresentCount        = (UINT)g_shownGameFrames.load();
            st->PresentRefreshCount = st->SyncRefreshCount;
        }
        return hr;
    }
    const HRESULT hr = o_GetFS(sc, st);
    if (t_presenterThread || t_ownCall || !IsManaged(sc)) return hr;
    if (!g_statsReadLogged.exchange(true))
        LOGI("BFI Presenter: the game reads frame statistics (%s)",
             g_virtualStats.load() ? "reported in terms of its own frames" : "real values, GameFrameStatistics=0");
    if (SUCCEEDED(hr) && st && g_virtualStats.load() && g_running.load())
    {
        // Its last shown frame appeared at the latest vblank.
        st->PresentCount        = (UINT)g_shownGameFrames.load();
        st->PresentRefreshCount = st->SyncRefreshCount;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_GetLPC(IDXGISwapChain *sc, UINT *n)
{
    if (!t_ownCall && !t_presenterThread && IsGame11(sc) && n) { *n = (UINT)g_frameId; return S_OK; }
    const HRESULT hr = o_GetLPC(sc, n);
    if (t_presenterThread || t_ownCall || !IsManaged(sc)) return hr;
    if (!g_statsReadLogged.exchange(true))
        LOGI("BFI Presenter: the game reads its present count (%s)",
             g_virtualStats.load() ? "reported in terms of its own frames" : "real value, GameFrameStatistics=0");
    if (SUCCEEDED(hr) && n && g_virtualStats.load() && g_running.load()) *n = (UINT)g_frameId;
    return hr;
}

static HANDLE STDMETHODCALLTYPE hk_GetFLWO(IDXGISwapChain2 *sc)
{
    if (!(IsManaged(sc) || IsGame11(sc)) || !g_waitSem) return o_GetFLWO(sc);
    LOGI("BFI Presenter: game requested the frame-latency waitable");
    HANDLE h = nullptr;          // a duplicate: the game closes the handle it gets
    DuplicateHandle(GetCurrentProcess(), g_waitSem, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return h;
}

static HRESULT STDMETHODCALLTYPE hk_SetMFL(IDXGISwapChain2 *sc, UINT n)
{
    if (!(IsManaged(sc) || IsGame11(sc)) || !g_waitSem) return o_SetMFL(sc, n);
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    const UINT old = g_maxLatency.exchange(n);
    if (n > old) ReleaseSemaphore(g_waitSem, (LONG)(n - old), nullptr);
    LOGI("BFI Presenter: game frame latency set to %u", n);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hk_GetMFL(IDXGISwapChain2 *sc, UINT *p)
{
    if (!(IsManaged(sc) || IsGame11(sc)) || !g_waitSem) return o_GetMFL(sc, p);
    if (!p) return E_INVALIDARG;
    *p = g_maxLatency.load();
    return S_OK;
}

// Calls made by BFI Presenter itself into the swap chain pass through any
// hooks below it (Steam's overlay, RTSS). Some of those call the swap chain
// again through its vtable -- back into these hooks. Such re-entrant calls on
// the same thread go straight to the original (Steam's ResizeBuffers1 hook
// did this in Pragmata: infinite recursion, stack overflow).

// After the presenter got stuck the game may still wait on the frame-latency
// semaphore it was given; with nobody else releasing it, each of its own
// presents gives one slot back.
static bool StuckPassThrough(void *sc)
{
    return g_presenterStuck.load() && !t_ownCall && !t_presenterThread && g_waitSem &&
           (sc == (void *)g_sc || sc == (void *)g_sc11);
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    if (!t_ownCall && IsGame11(sc) && !(flags & DXGI_PRESENT_TEST)) return SubmitGameFrame11(sc, sync, flags);
    if (StuckPassThrough(sc)) { const HRESULT hr = o_Present(sc, sync, flags); ReleaseGameSlot(); return hr; }
    if (t_ownCall || !IsManaged(sc) || (flags & DXGI_PRESENT_TEST)) return o_Present(sc, sync, flags);
    return SubmitGameFrame();
}

static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *p)
{
    if (!t_ownCall && IsGame11(sc) && !(flags & DXGI_PRESENT_TEST)) return SubmitGameFrame11(sc, sync, flags);
    if (StuckPassThrough(sc)) { const HRESULT hr = o_Present1(sc, sync, flags, p); ReleaseGameSlot(); return hr; }
    if (t_ownCall || !IsManaged(sc) || (flags & DXGI_PRESENT_TEST)) return o_Present1(sc, sync, flags, p);
    return SubmitGameFrame();
}

static HRESULT STDMETHODCALLTYPE hk_GetBuffer(IDXGISwapChain *sc, UINT idx, REFIID riid, void **pp)
{
    // Overlays that hook Present below this add-on (RTSS, Steam...) run on the
    // presenter thread and must see the real buffers.
    if (!IsManaged(sc) || t_presenterThread || t_ownCall) return o_GetBuffer(sc, idx, riid, pp);
    if (idx >= g_bufCount || !g_virt[idx]) return DXGI_ERROR_INVALID_CALL;
    return g_virt[idx]->QueryInterface(riid, pp);
}

static UINT STDMETHODCALLTYPE hk_GetCBBI(IDXGISwapChain3 *sc)
{
    if (!IsManaged(sc) || t_presenterThread || t_ownCall) return o_GetCBBI(sc);
    return g_cur;
}

// ResizeBuffers1 with the presenter queue for every buffer, so DXGI presents
// on the presenter's queue instead of the game's.
// The function a vtable slot held before anyone hooked it: read from the
// module's file on disk (the vtable's slot value at the preferred image base,
// rebased to where the module is loaded). Bypasses vtable hooks only.
static void *PristineVtableEntry(void **vtbl, int idx)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)vtbl, &m) || !m) return nullptr;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return nullptr;
    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return nullptr;
    void *result = nullptr;
    LARGE_INTEGER sz = {};
    if (GetFileSizeEx(fh, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64ll << 20))
    {
        std::vector<BYTE> buf((size_t)sz.QuadPart);
        DWORD rd = 0;
        if (ReadFile(fh, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd == buf.size() && rd > sizeof(IMAGE_DOS_HEADER))
        {
            const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)buf.data();
            if (dos->e_magic == IMAGE_DOS_SIGNATURE && (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) < buf.size())
            {
                // PE32+ (64-bit) and PE32 (32-bit builds): slots of 8 or 4 bytes.
                const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(buf.data() + dos->e_lfanew);
                const bool pe64 = nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
                const bool pe32 = nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
                if (nt->Signature == IMAGE_NT_SIGNATURE && (pe64 ? sizeof(void *) == 8 : pe32 && sizeof(void *) == 4))
                {
                    const IMAGE_NT_HEADERS32 *nt32 = (const IMAGE_NT_HEADERS32 *)nt;
                    const UINT64 base = pe64 ? nt->OptionalHeader.ImageBase : nt32->OptionalHeader.ImageBase;
                    const UINT64 size = pe64 ? nt->OptionalHeader.SizeOfImage : nt32->OptionalHeader.SizeOfImage;
                    const size_t slot = sizeof(void *);
                    const UINT64 rva = (UINT64)((BYTE *)&vtbl[idx] - (BYTE *)m);
                    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);   // uses SizeOfOptionalHeader: right for both
                    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
                    {
                        const UINT64 va = sec->VirtualAddress, span = sec->SizeOfRawData;
                        if (rva < va || rva + slot > va + span) continue;
                        const UINT64 off = sec->PointerToRawData + (rva - va);
                        if (off + slot > buf.size()) break;
                        UINT64 val = 0;
                        memcpy(&val, buf.data() + off, slot);
                        if (val >= base && val < base + size) result = (BYTE *)m + (val - base);
                        break;
                    }
                }
            }
        }
    }
    CloseHandle(fh);
    return result;
}

// Code bytes of a function as they are in its module's file on disk.
static bool OriginalCodeBytes(const void *fn, BYTE *out, size_t n)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)fn, &m) || !m) return false;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;
    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    BYTE hdr[4096];
    DWORD rd = 0;
    if (ReadFile(fh, hdr, sizeof(hdr), &rd, nullptr) && rd == sizeof(hdr))
    {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)hdr;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 && (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) < sizeof(hdr))
        {
            const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(hdr + dos->e_lfanew);
            const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
            const UINT64 rva = (UINT64)((const BYTE *)fn - (const BYTE *)m);
            for (WORD i = 0; nt->Signature == IMAGE_NT_SIGNATURE && i < nt->FileHeader.NumberOfSections &&
                             (const BYTE *)(sec + 1) <= hdr + sizeof(hdr); i++, sec++)
            {
                if (rva < sec->VirtualAddress || rva + n > (UINT64)sec->VirtualAddress + sec->SizeOfRawData) continue;
                LARGE_INTEGER pos;
                pos.QuadPart = (LONGLONG)(sec->PointerToRawData + (rva - sec->VirtualAddress));
                if (SetFilePointerEx(fh, pos, nullptr, FILE_BEGIN) && ReadFile(fh, out, (DWORD)n, &rd, nullptr) && rd == n)
                    ok = true;
#ifndef _WIN64
                // x86 code holds absolute addresses: apply the module's base
                // relocations, or a DLL loaded away from its preferred base
                // (BFI Presenter itself under 32-bit ReShade) looks "hooked"
                // everywhere and the copied code reads the wrong addresses.
                if (ok)
                {
                    const IMAGE_NT_HEADERS32 *nt32 = (const IMAGE_NT_HEADERS32 *)(hdr + dos->e_lfanew);
                    const DWORD delta = (DWORD)(uintptr_t)m - nt32->OptionalHeader.ImageBase;
                    const IMAGE_NT_HEADERS32 *mem = (const IMAGE_NT_HEADERS32 *)((const BYTE *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
                    const IMAGE_DATA_DIRECTORY &rd5 = mem->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
                    if (delta && rd5.VirtualAddress && rd5.Size)
                    {
                        const BYTE *p = (const BYTE *)m + rd5.VirtualAddress, *end = p + rd5.Size;
                        while (p + 8 <= end)
                        {
                            const IMAGE_BASE_RELOCATION *b = (const IMAGE_BASE_RELOCATION *)p;
                            if (b->SizeOfBlock < 8) break;
                            if (b->VirtualAddress + 0x1000 + 4 > rva && b->VirtualAddress < rva + n)
                            {
                                const WORD *e = (const WORD *)(p + 8);
                                for (DWORD k = 0; k < (b->SizeOfBlock - 8) / 2; k++)
                                {
                                    if ((e[k] >> 12) != IMAGE_REL_BASED_HIGHLOW) continue;
                                    const DWORD at = b->VirtualAddress + (e[k] & 0xFFF);
                                    if (at + 4 <= rva || at >= rva + n) continue;
                                    if (at >= rva && at + 4 <= rva + n)
                                    {
                                        DWORD val;
                                        memcpy(&val, out + (at - rva), 4);
                                        val += delta;
                                        memcpy(out + (at - rva), &val, 4);
                                    }
                                    else                                 // straddles the window edge: take those bytes as loaded
                                        for (DWORD r = at; r < at + 4; r++)
                                            if (r >= rva && r < rva + n) out[r - rva] = ((const BYTE *)m)[r];
                                }
                            }
                            p += b->SizeOfBlock;
                        }
                    }
                }
#endif
                break;
            }
        }
    }
    CloseHandle(fh);
    return ok;
}

// Inline hooks (a jump patched over a function's first bytes, as Steam's
// overlay does on DXGI) can't be bypassed through the vtable. For one call,
// put the original bytes back, call, then restore the hook exactly as it was.
struct InlineHookLift
{
    BYTE *fn = nullptr;
    BYTE saved[32] = {};
    size_t len = 0;
    explicit InlineHookLift(void *target)
    {
        BYTE orig[32];
        if (!target || !OriginalCodeBytes(target, orig, sizeof(orig))) return;
        BYTE *code = (BYTE *)target;
        size_t last = 0;
        for (size_t i = 0; i < sizeof(orig); i++) if (code[i] != orig[i]) last = i + 1;
        if (!last) return;                                   // not patched
        DWORD old;
        if (!VirtualProtect(code, last, PAGE_EXECUTE_READWRITE, &old)) return;
        memcpy(saved, code, last);
        memcpy(code, orig, last);
        VirtualProtect(code, last, old, &old);
        FlushInstructionCache(GetCurrentProcess(), code, last);
        fn = code; len = last;
    }
    ~InlineHookLift()
    {
        if (!fn) return;
        DWORD old;
        if (!VirtualProtect(fn, len, PAGE_EXECUTE_READWRITE, &old)) return;
        memcpy(fn, saved, len);
        VirtualProtect(fn, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, len);
    }
};

// Length of one x64 instruction (the common prologue subset); -1 if not
// supported. *ripDisp gets the offset of a RIP-relative displacement, or -1.
static int InsnLength(const BYTE *p, int *ripDisp)
{
    *ripDisp = -1;
    int i = 0;
    bool opsize = false, rexw = false;
    while (p[i] == 0x66 || p[i] == 0xF2 || p[i] == 0xF3) { if (p[i] == 0x66) opsize = true; i++; }
#ifdef _WIN64
    if ((p[i] & 0xF0) == 0x40) { rexw = (p[i] & 8) != 0; i++; }
#else
    if ((p[i] & 0xF0) == 0x40) return i + 1;                       // x86: inc / dec reg
#endif
    const BYTE op = p[i++];
    int imm = 0;
    bool modrm = false;
    if (op >= 0x50 && op <= 0x5F) return i;                       // push / pop
    if (op == 0x90 || op == 0xC3 || op == 0xCC) return i;
    if (op == 0x6A) return i + 1;
    if (op == 0x68) return i + 4;
    if (op >= 0xB8 && op <= 0xBF) return i + (rexw ? 8 : 4);
    if (op == 0x0F)
    {
        const BYTE op2 = p[i++];
        if (op2 == 0x1F || op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 || op2 == 0xB6 || op2 == 0xB7 ||
            op2 == 0xBE || op2 == 0xBF || (op2 >= 0x40 && op2 <= 0x4F) || op2 == 0xAF)
            modrm = true;
        else return -1;
    }
    else switch (op)
    {
    case 0x01: case 0x03: case 0x09: case 0x0B: case 0x21: case 0x23: case 0x29: case 0x2B: case 0x31: case 0x33:
    case 0x39: case 0x3B: case 0x63: case 0x84: case 0x85: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
    case 0xD1: case 0xD3: case 0xFF: case 0xF7:
        modrm = true;
        if (op == 0xF7) return -1;                                 // test imm form varies: not needed
        break;
    case 0x80: case 0x83: case 0xC0: case 0xC1: case 0xC6: case 0x6B: modrm = true; imm = 1; break;
    case 0x81: case 0xC7: case 0x69: modrm = true; imm = opsize ? 2 : 4; break;
    default: return -1;                                            // branches and anything unusual
    }
    if (!modrm) return -1;
    const BYTE m = p[i++];
    const int mod = m >> 6, rm = m & 7;
    if (mod != 3 && rm == 4)
    {
        const BYTE sib = p[i++];
        if (mod == 0 && (sib & 7) == 5) i += 4;
    }
#ifdef _WIN64
    if (mod == 0 && rm == 5) { *ripDisp = i; i += 4; }
#else
    if (mod == 0 && rm == 5) i += 4;                                 // x86: absolute address, copies as is
#endif
    else if (mod == 1) i += 1;
    else if (mod == 2) i += 4;
    return i + imm;
}

// A callable copy of a function's original start that jumps on into the rest
// of it: calling it skips an inline hook written over the first bytes,
// without touching the hook. Returns the function itself if it isn't hooked,
// nullptr if the original start can't be relocated safely.
static void *MakeUnhookedEntry(void *target, char *why, size_t whyLen)
{
    {
        // One of our own hooks (the vtable's pristine entry was not found):
        // nothing to bypass.
        HMODULE self = nullptr, tm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&MakeUnhookedEntry, &self);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)target, &tm);
        if (target && self && tm == self) { snprintf(why, whyLen, "own hook"); return target; }
    }
    BYTE orig[64];
    if (!target || !OriginalCodeBytes(target, orig, sizeof(orig))) { snprintf(why, whyLen, "original bytes unavailable"); return nullptr; }
    const BYTE *code = (const BYTE *)target;
    size_t patched = 0;
    for (size_t i = 0; i < 32; i++) if (code[i] != orig[i]) patched = i + 1;
    if (!patched) { snprintf(why, whyLen, "not hooked"); return target; }
    // Whole original instructions covering the patched bytes.
    int ins[16], rip[16], n = 0;
    size_t len = 0;
    while (len < patched)
    {
        int rd;
        const int l = InsnLength(orig + len, &rd);
        if (l <= 0 || len + l > 48 || n >= 16) { snprintf(why, whyLen, "unsupported instruction at +%zu (%02X)", len, orig[len]); return nullptr; }
        ins[n] = (int)len; rip[n] = rd; n++;
        len += (size_t)l;
    }
#ifndef _WIN64
    // x86: no RIP-relative operands; copy the instructions and jmp rel32 back.
    {
        BYTE *t = (BYTE *)VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!t) { snprintf(why, whyLen, "no memory"); return nullptr; }
        memcpy(t, orig, len);
        t[len] = 0xE9;
        const int32_t rel = (int32_t)((intptr_t)(code + len) - (intptr_t)(t + len + 5));
        memcpy(t + len + 1, &rel, 4);
        DWORD o;
        VirtualProtect(t, 4096, PAGE_EXECUTE_READ, &o);
        FlushInstructionCache(GetCurrentProcess(), t, len + 5);
        snprintf(why, whyLen, "%zu patched bytes skipped", patched);
        return t;
    }
#endif
    // Near the function, so RIP-relative displacements still reach.
    BYTE *tramp = nullptr;
    const uintptr_t base = (uintptr_t)target & ~(uintptr_t)0xFFFF;
    for (uintptr_t d = 0x10000; d < 0x40000000 && !tramp; d += 0x10000)
    {
        tramp = (BYTE *)VirtualAlloc((void *)(base - d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!tramp) tramp = (BYTE *)VirtualAlloc((void *)(base + d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    if (!tramp) { snprintf(why, whyLen, "no memory near the function"); return nullptr; }
    memcpy(tramp, orig, len);
    for (int k = 0; k < n; k++)
    {
        if (rip[k] < 0) continue;
        const int at = ins[k] + rip[k];
        int32_t disp;
        memcpy(&disp, orig + at, 4);
        const int64_t abs = (int64_t)(uintptr_t)code + at + 4 + disp;     // what it pointed at
        const int64_t nd  = abs - ((int64_t)(uintptr_t)tramp + at + 4);
        if (nd < INT32_MIN || nd > INT32_MAX) { VirtualFree(tramp, 0, MEM_RELEASE); snprintf(why, whyLen, "displacement out of range"); return nullptr; }
        const int32_t nd32 = (int32_t)nd;
        memcpy(tramp + at, &nd32, 4);
    }
    // jmp qword [rip+0] ; <address of the rest of the function>
    BYTE *j = tramp + len;
    j[0] = 0xFF; j[1] = 0x25; j[2] = j[3] = j[4] = j[5] = 0;
    const uint64_t back = (uint64_t)(uintptr_t)(code + len);
    memcpy(j + 6, &back, 8);
    DWORD old;
    VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 14);
    snprintf(why, whyLen, "%zu patched bytes skipped", patched);
    return tramp;
}

// DXGI's own Present and ResizeBuffers1, past any inline hooks, for the calls
// BFI Presenter makes itself. Overlays (Steam's gameoverlayrenderer64) hook
// these; in Pragmata, Steam's ResizeBuffers1 hook never returned and its
// Present hook then recursed until the stack overflowed.
static PFN_Present        g_directPresent = nullptr;
static PFN_ResizeBuffers1 g_directResize1 = nullptr;
static bool               g_directTried = false;
static void PrepareDirectCalls()
{
    if (g_directTried) return;
    g_directTried = true;
    HMODULE vm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)g_hookedVtbl, &vm);
    struct { void *fn; void **out; const char *name; } items[] = {
        { (void *)o_Present, (void **)&g_directPresent, "Present" },
        { (void *)o_ResizeBuffers1, (void **)&g_directResize1, "ResizeBuffers1" } };
    for (auto &it : items)
    {
        void *fn = it.fn;
        if (g_hookedVtbl)
        {
            void *pristine = PristineVtableEntry(g_hookedVtbl, it.name[0] == 'P' ? VT_Present : VT_ResizeBuffers1);
            if (pristine) fn = pristine;                       // past vtable hooks too
        }
        HMODULE fm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &fm);
        if (!fm || fm != vm) { LOGW("BFI Presenter: %s is not in DXGI's module; overlay hooks stay in the path", it.name); continue; }
        char why[96];
        void *direct = MakeUnhookedEntry(fn, why, sizeof(why));
        *it.out = direct;
        LOGI("BFI Presenter: direct %s: %s", it.name, direct ? why : "not possible -- overlay hooks stay in the path");
        if (!direct) LOGW("BFI Presenter: (%s)", why);
    }
}

static HRESULT OwnPresent()
{
    OwnCall own;
    if (g_dx11.load())
    {
        // The add-on's own swap chain: DXGI's Present past any inline hooks
        // (Steam's overlay hooks dxgi.dll's Present, whatever the API).
        static PFN_Present direct11 = nullptr;
        static void **forVt = nullptr;
        void **vt = *(void ***)g_sc;
        if (forVt != vt)
        {
            forVt = vt;
            void *pr = PristineVtableEntry(vt, VT_Present);
            char why[96] = "no pristine entry";
            direct11 = pr ? (PFN_Present)MakeUnhookedEntry(pr, why, sizeof(why)) : nullptr;
            LOGI("BFI Presenter: DX11 mode, direct Present for the presenter's swap chain: %s", direct11 ? why : "not possible");
        }
        return direct11 ? direct11(g_sc, 1, 0) : g_sc->Present(1, 0);
    }
    // Also before BFI starts, when the game's own frames are passed on: Steam's
    // Present hook recursed until the stack overflowed on Resident Evil 4's
    // first frame.
    if (g_shared.bypassResizeHooks) PrepareDirectCalls();
    if (g_shared.bypassResizeHooks && g_directPresent) return g_directPresent(g_sc, 1, 0);
    return o_Present(g_sc, 1, 0);
}

static HRESULT ResizeOnPresenterQueue(IDXGISwapChain3 *sc, UINT cnt, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    UINT count = cnt ? cnt : g_virtCount;
    if (count == 0) { DXGI_SWAP_CHAIN_DESC1 d = {}; o_GetDesc1(sc, &d); count = d.BufferCount; }
    Settings cur;
    EnterCriticalSection(&g_cfgLock);
    cur = g_shared;
    LeaveCriticalSection(&g_cfgLock);
    if (count < (UINT)cur.presBuffers) count = (UINT)cur.presBuffers;   // slack for the strobe
    if (count == 0 || count > kMaxBuf) return E_INVALIDARG;
    UINT masks[kMaxBuf];
    IUnknown *queues[kMaxBuf];
    for (UINT i = 0; i < count; i++) { masks[i] = 1; queues[i] = g_presQueue; }
    // Pass the explicit count: with BufferCount = 0 ("keep") DXGI may ignore
    // the queue array, leaving presentation on the game's queue while this
    // add-on believes it moved (seen as "no effect" in Tomb Raider).
    HRESULT hr;
    {
        OwnCall own;
        PrepareDirectCalls();
        if (cur.bypassResizeHooks && g_directResize1)
        {
            hr = g_directResize1(sc, count, w, h, f, fl, masks, queues);
            // Overlays that hook ResizeBuffers1 (RTSS, NVIDIA overlay) may
            // hold references to the old buffers and release them only in
            // their hook: DXGI then refuses the direct call. Go through the
            // hooks instead (Steam's hung there in Pragmata, but this one
            // would have failed without them anyway).
            if (hr == DXGI_ERROR_INVALID_CALL)
            {
                LOGI("BFI Presenter: direct ResizeBuffers1 refused (a hook still holds the old buffers) -- retrying through the hooks");
                hr = o_ResizeBuffers1(sc, count, w, h, f, fl, masks, queues);
            }
        }
        else hr = o_ResizeBuffers1(sc, count, w, h, f, fl, masks, queues);
    }
    LOGI("BFI Presenter: ResizeBuffers1 onto the presenter queue (%u buffers): 0x%08lX", count, (unsigned long)hr);
    return hr;
}

// Presenter stuck (see g_presenterStuck): stop managing the swap chain and
// pass the game's call through. Nothing is released.
template <class F> static HRESULT StuckResize(void *sc, F orig)
{
    g_active.store(false);
    SetReason(0, "BFI stopped: the presenter thread hung while the swap chain changed. The game continues without BFI; "
                 "restart it to use BFI. If this repeats, use borderless (windowed) fullscreen instead of exclusive fullscreen.");
    const HRESULT hr = orig();
    LOGW("BFI Presenter: swap chain %p resized unmanaged (0x%08lX)", sc, (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain *sc, UINT cnt, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    if (!t_ownCall && IsGame11(sc)) return Resize11(sc, cnt, w, h, f, fl, nullptr, nullptr, false);
    if (t_ownCall || !IsManaged(sc)) return o_ResizeBuffers(sc, cnt, w, h, f, fl);
    if (cnt) g_virtCount = cnt;
    GameStage(ST_R_STOP);
    if (!StopPresenter()) { GameStage(ST_G_OUTSIDE); return StuckResize(sc, [&] { OwnCall own; return o_ResizeBuffers(sc, cnt, w, h, f, fl); }); }
    GameStage(ST_R_RELEASE);
    ReleaseFrameResources();
    GameStage(ST_R_REBIND);
    // A pending move to the presenter's queue happens here: nothing holds the
    // real buffers and ReShade's lock is not held.
    const bool wantOwn = g_ownPresentQueue || g_needRebind.exchange(false);
    HRESULT hr = wantOwn ? ResizeOnPresenterQueue((IDXGISwapChain3 *)sc, cnt, w, h, f, fl) : E_FAIL;
    if (wantOwn && FAILED(hr) && !g_ownPresentQueue) g_needRebind.store(true);   // try again at the next resize
    g_ownPresentQueue = SUCCEEDED(hr);
    if (wantOwn) LOGI("BFI Presenter: presenting on %s queue after resize", g_ownPresentQueue ? "its own" : "the game's");
    GameStage(ST_R_ORIG);
    if (FAILED(hr)) { OwnCall own; hr = o_ResizeBuffers(sc, cnt, w, h, f, fl); }
    GameStage(ST_R_CREATE);
    if (g_ownPresentQueue && g_deferPresenter.exchange(false))
        LOGI("BFI Presenter: the game resized its recreated swap chain -- BFI starts");
    if (CreateFrameResources() && !g_presenterFailed.load() && !(g_deferPresenter.load() && !g_ownPresentQueue)) StartPresenter();
    LOGI("BFI Presenter: ResizeBuffers (0x%08lX)", (unsigned long)hr);
    GameStage(ST_G_OUTSIDE);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers1(IDXGISwapChain3 *sc, UINT cnt, UINT w, UINT h, DXGI_FORMAT f, UINT fl,
                                                    const UINT *mask, IUnknown *const *queues)
{
    if (!t_ownCall && IsGame11(sc)) return Resize11(sc, cnt, w, h, f, fl, mask, queues, true);
    if (t_ownCall || !IsManaged(sc)) return o_ResizeBuffers1(sc, cnt, w, h, f, fl, mask, queues);
    if (cnt) g_virtCount = cnt;
    GameStage(ST_R_STOP);
    if (!StopPresenter()) { GameStage(ST_G_OUTSIDE); return StuckResize(sc, [&] { OwnCall own; return o_ResizeBuffers1(sc, cnt, w, h, f, fl, mask, queues); }); }
    GameStage(ST_R_RELEASE);
    ReleaseFrameResources();
    GameStage(ST_R_REBIND);
    const bool wantOwn = g_ownPresentQueue || g_needRebind.exchange(false);
    HRESULT hr = wantOwn ? ResizeOnPresenterQueue(sc, cnt, w, h, f, fl) : E_FAIL;
    if (wantOwn && FAILED(hr) && !g_ownPresentQueue) g_needRebind.store(true);   // try again at the next resize
    g_ownPresentQueue = SUCCEEDED(hr);
    if (wantOwn) LOGI("BFI Presenter: presenting on %s queue after resize", g_ownPresentQueue ? "its own" : "the game's");
    GameStage(ST_R_ORIG);
    if (FAILED(hr)) { OwnCall own; hr = o_ResizeBuffers1(sc, cnt, w, h, f, fl, mask, queues); }
    GameStage(ST_R_CREATE);
    if (g_ownPresentQueue && g_deferPresenter.exchange(false))
        LOGI("BFI Presenter: the game resized its recreated swap chain -- BFI starts");
    if (CreateFrameResources() && !g_presenterFailed.load() && !(g_deferPresenter.load() && !g_ownPresentQueue)) StartPresenter();
    LOGI("BFI Presenter: ResizeBuffers1 (0x%08lX)", (unsigned long)hr);
    GameStage(ST_G_OUTSIDE);
    return hr;
}

static void PatchSlot(void **vtbl, int idx, void *hook, void **orig)
{
    // Already ours: keep the original we saved before. This happens when
    // another hook that sat on top of ours (Steam's overlay) unhooks by
    // restoring the pointer it saved -- our hook. Saving it as "original"
    // made our hook call itself (stack overflow in Pragmata on a resize).
    if (vtbl[idx] == hook) return;
    DWORD old;
    VirtualProtect(&vtbl[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *orig = vtbl[idx];
    vtbl[idx] = hook;
    VirtualProtect(&vtbl[idx], sizeof(void *), old, &old);
}

static void UnpatchSlot(void **vtbl, int idx, void *orig, void *hook)
{
    if (vtbl[idx] != hook) return;      // someone hooked on top of us: leave their hook in place
    DWORD old;
    VirtualProtect(&vtbl[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    vtbl[idx] = orig;
    VirtualProtect(&vtbl[idx], sizeof(void *), old, &old);
}

static bool InstallHooks(IDXGISwapChain3 *sc)
{
    void **vtbl = *(void ***)sc;
    if (g_hookedVtbl == vtbl) return true;
    if (g_hookedVtbl) { LOGW("BFI Presenter: swap chain uses a different vtable; not managed"); return false; }
    {
        char mod[64];
        ModuleOf(vtbl[VT_Present], mod, sizeof(mod));
        LOGI("BFI Presenter: hooking the swap chain; Present currently goes to %s", mod);
    }
    PatchSlot(vtbl, VT_Present,                   (void *)&hk_Present,        (void **)&o_Present);
    PatchSlot(vtbl, VT_GetBuffer,                 (void *)&hk_GetBuffer,      (void **)&o_GetBuffer);
    PatchSlot(vtbl, VT_ResizeBuffers,             (void *)&hk_ResizeBuffers,  (void **)&o_ResizeBuffers);
    PatchSlot(vtbl, VT_Present1,                  (void *)&hk_Present1,       (void **)&o_Present1);
    PatchSlot(vtbl, VT_GetCurrentBackBufferIndex, (void *)&hk_GetCBBI,        (void **)&o_GetCBBI);
    PatchSlot(vtbl, VT_ResizeBuffers1,            (void *)&hk_ResizeBuffers1, (void **)&o_ResizeBuffers1);
    PatchSlot(vtbl, VT_SetMaximumFrameLatency,    (void *)&hk_SetMFL,         (void **)&o_SetMFL);
    PatchSlot(vtbl, VT_GetMaximumFrameLatency,    (void *)&hk_GetMFL,         (void **)&o_GetMFL);
    PatchSlot(vtbl, VT_GetFrameLatencyWaitableObject, (void *)&hk_GetFLWO,    (void **)&o_GetFLWO);
    PatchSlot(vtbl, VT_GetDesc,                   (void *)&hk_GetDesc,        (void **)&o_GetDesc);
    PatchSlot(vtbl, VT_GetDesc1,                  (void *)&hk_GetDesc1,       (void **)&o_GetDesc1);
    PatchSlot(vtbl, VT_GetFrameStatistics,        (void *)&hk_GetFS,          (void **)&o_GetFS);
    PatchSlot(vtbl, VT_GetLastPresentCount,       (void *)&hk_GetLPC,         (void **)&o_GetLPC);
    g_hookedVtbl = vtbl;
    return true;
}

static HRESULT STDMETHODCALLTYPE hk_SetFS(IDXGISwapChain *sc, BOOL fs, IDXGIOutput *target);
static HRESULT STDMETHODCALLTYPE hk_GetFSt(IDXGISwapChain *sc, BOOL *fs, IDXGIOutput **target);
static HRESULT STDMETHODCALLTYPE hk_SetCS1(IDXGISwapChain3 *sc, DXGI_COLOR_SPACE_TYPE cs);
static void RemoveHooks()
{
    if (!g_hookedVtbl) return;
    UnpatchSlot(g_hookedVtbl, VT_Present,                   (void *)o_Present, (void *)&hk_Present);
    UnpatchSlot(g_hookedVtbl, VT_GetBuffer,                 (void *)o_GetBuffer, (void *)&hk_GetBuffer);
    UnpatchSlot(g_hookedVtbl, VT_ResizeBuffers,             (void *)o_ResizeBuffers, (void *)&hk_ResizeBuffers);
    UnpatchSlot(g_hookedVtbl, VT_Present1,                  (void *)o_Present1, (void *)&hk_Present1);
    UnpatchSlot(g_hookedVtbl, VT_GetCurrentBackBufferIndex, (void *)o_GetCBBI, (void *)&hk_GetCBBI);
    UnpatchSlot(g_hookedVtbl, VT_ResizeBuffers1,            (void *)o_ResizeBuffers1, (void *)&hk_ResizeBuffers1);
    UnpatchSlot(g_hookedVtbl, VT_SetMaximumFrameLatency,    (void *)o_SetMFL, (void *)&hk_SetMFL);
    UnpatchSlot(g_hookedVtbl, VT_GetMaximumFrameLatency,    (void *)o_GetMFL, (void *)&hk_GetMFL);
    UnpatchSlot(g_hookedVtbl, VT_GetFrameLatencyWaitableObject, (void *)o_GetFLWO, (void *)&hk_GetFLWO);
    UnpatchSlot(g_hookedVtbl, VT_GetDesc,                   (void *)o_GetDesc, (void *)&hk_GetDesc);
    UnpatchSlot(g_hookedVtbl, VT_GetFrameStatistics,        (void *)o_GetFS, (void *)&hk_GetFS);
    UnpatchSlot(g_hookedVtbl, VT_GetLastPresentCount,       (void *)o_GetLPC, (void *)&hk_GetLPC);
    UnpatchSlot(g_hookedVtbl, VT_GetDesc1,                  (void *)o_GetDesc1, (void *)&hk_GetDesc1);
    if (o_SetFS)  UnpatchSlot(g_hookedVtbl, VT_SetFullscreenState, (void *)o_SetFS,  (void *)&hk_SetFS);
    if (o_GetFSt) UnpatchSlot(g_hookedVtbl, VT_GetFullscreenState, (void *)o_GetFSt, (void *)&hk_GetFSt);
    if (o_SetCS1) UnpatchSlot(g_hookedVtbl, VT_SetColorSpace1,     (void *)o_SetCS1, (void *)&hk_SetCS1);
    o_SetFS = nullptr; o_GetFSt = nullptr; o_SetCS1 = nullptr;
    g_hookedVtbl = nullptr;
    g_directTried = false;              // rebuilt for the next swap chain
}

// ---------------------------------------------------------------- ReShade events
static void ReleaseAll()
{
    if (!StopPresenter())
    {
        // Keep g_sc set so no other swap chain is taken over in this run.
        g_active.store(false);
        SetReason(0, "BFI stopped: the presenter thread hung while the swap chain was destroyed. The game continues "
                     "without BFI; restart it to use BFI. If this repeats, use borderless (windowed) fullscreen.");
        return;
    }
    g_active.store(false);
    ReleaseFrameResources();
    for (UINT s = 0; s < kStage; s++) SafeRelease(g_copyAlloc[s]);
    SafeRelease(g_copyList);
    for (UINT i = 0; i < kMaxBuf; i++) SafeRelease(g_fbAlloc[i]);
    SafeRelease(g_fbList);
    SafeRelease(g_gameFence);
    SafeRelease(g_presFence);
    // The presenter queue is deliberately kept alive across swap chains.
    // Present hooks of overlays (Steam's gameoverlayrenderer64) remember the
    // queue a swap chain presents on and don't let go of it when the game
    // recreates its swap chain; releasing it left them calling into a freed
    // queue (God of War Ragnarok crashed on load inside the Steam hook).
    SafeRelease(g_gameQueue);
    if (g_waitSem) { ReleaseSemaphore(g_waitSem, 16, nullptr); CloseHandle(g_waitSem); g_waitSem = nullptr; }
    g_ownPresentQueue = false;
    g_needRebind.store(false);
    g_virtCount = 0;
    g_device = nullptr;
    g_sc = nullptr;
    g_rsSwapchain = nullptr;
}


// Tools that hook Present. They run inside the presenter's Present calls;
// RTSS crashed Control there (OSD and frame limiter), and its limiter would
// throttle the strobe itself.
// Module containing an address (file name only).
static void ModuleOf(const void *addr, char *out, size_t n)
{
    HMODULE m = nullptr;
    char path[MAX_PATH] = {};
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)addr, &m) && m && GetModuleFileNameA(m, path, MAX_PATH))
    {
        const char *b = strrchr(path, '\\');
        snprintf(out, n, "%s", b ? b + 1 : path);
    }
    else
        snprintf(out, n, "unknown");
}

// Frame-generation components. They take over presentation too, so where
// they sit relative to ReShade decides whether the presenter can work.
static void DetectFrameGen()
{
    static const struct { const char *dll; const char *name; } k[] = {
        { "nvngx_dlssg.dll",                    "DLSS Frame Generation" },
        { "sl.dlss_g.dll",                      "Streamline DLSS-G" },
        { "sl.interposer.dll",                  "Streamline interposer" },
        { "dlssg_to_fsr3_amd_is_better.dll",    "DLSS-G to FSR3 mod" },
        { "amd_fidelityfx_dx12.dll",            "AMD FidelityFX (FSR 3.1 / FSR FG)" },
        { "ffx_frameinterpolation_x64.dll",     "FSR 3.0 frame interpolation" },
        { "libxess_fg.dll",                     "XeSS Frame Generation" },
    };
    g_fgSeen[0] = '\0';
    for (const auto &e : k)
    {
        if (!GetModuleHandleA(e.dll)) continue;
        if (g_fgSeen[0]) strncat(g_fgSeen, ", ", sizeof(g_fgSeen) - strlen(g_fgSeen) - 1);
        strncat(g_fgSeen, e.name, sizeof(g_fgSeen) - strlen(g_fgSeen) - 1);
    }
    if (g_fgSeen[0]) LOGW("BFI Presenter: frame-generation components in this game: %s", g_fgSeen);
}

// NVIDIA Smooth Motion (driver frame generation for DX11/DX12) registers a
// window class "InvisibleWindowClassNvPresent" in the game and presents
// generated frames through a swap chain of its own. With the DX11 presenter
// on top, Blasphemous (Unity) closed and relaunched itself in a loop. Smooth
// Motion adds nothing to 2-frame BFI anyway (the strobe shows 60 images a
// second at 120 Hz), so the DX11 path stays off while it is active.
static bool SmoothMotionActive()
{
    static const char kCls[] = "InvisibleWindowClassNvPresent";
    const DWORD me = GetCurrentProcessId();
    for (HWND parent : { (HWND)nullptr, HWND_MESSAGE })
    {
        HWND w = nullptr;
        while ((w = FindWindowExA(parent, w, kCls, nullptr)) != nullptr)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == me) return true;
        }
    }
    // Registered but no window yet: look for the class in every loaded module.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, me);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me32 = { sizeof(me32) };
    bool found = false;
    for (BOOL ok = Module32First(snap, &me32); ok && !found; ok = Module32Next(snap, &me32))
    {
        WNDCLASSEXA wc = { sizeof(wc) };
        if (GetClassInfoExA(me32.hModule, kCls, &wc)) found = true;
    }
    CloseHandle(snap);
    return found;
}

static bool DetectPresentHooks()
{
    static const struct { const char *dll; const char *name; } k[] = {
        { "RTSSHooks64.dll",                       "RivaTuner (RTSS)" },
        { "GameOverlayRenderer64.dll",             "Steam overlay" },
        { "DiscordHook64.dll",                     "Discord overlay" },
        { "nvspcap64.dll",                         "NVIDIA overlay" },
        { "EOSOverlayRenderer-Win64-Shipping.dll", "Epic overlay" },
        { "EOSOVH-Win64-Shipping.dll",             "Epic Online Services overlay" },
        { "SpecialK64.dll",                        "Special K" },
    };
    bool rtss = false;
    g_hooksSeen[0] = '\0';
    for (const auto &e : k)
    {
        if (!GetModuleHandleA(e.dll)) continue;
        if (!strcmp(e.dll, "RTSSHooks64.dll")) rtss = true;
        if (g_hooksSeen[0]) strncat(g_hooksSeen, ", ", sizeof(g_hooksSeen) - strlen(g_hooksSeen) - 1);
        strncat(g_hooksSeen, e.name, sizeof(g_hooksSeen) - strlen(g_hooksSeen) - 1);
    }
    if (g_hooksSeen[0]) LOGW("BFI Presenter: other Present hooks in this game: %s", g_hooksSeen);
    // Epic Online Services loads its overlay helper into games that use EOS
    // (also on Steam, without the Epic launcher). It hooks D3D12 and crashed
    // the DX11 presenter's own present in Silt. Say where it is.
    if (HMODULE eos = GetModuleHandleA("EOSOVH-Win64-Shipping.dll"))
    {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(eos, path, MAX_PATH);
        LOGW("BFI Presenter: the Epic Online Services overlay is loaded (%s). If the game crashes or freezes with "
             "BFI Presenter, rename that file (e.g. to EOSOVH-Win64-Shipping.dll.off) -- EOS keeps working without it.", path);
    }
    return rtss;
}

// ---------------------------------------------------------------- Direct3D 11 front end
static ID3D11Device5        *g_dev11 = nullptr;
static ID3D11DeviceContext4 *g_ctx11 = nullptr;
static ID3D11Fence          *g_gameFence11 = nullptr, *g_presFence11 = nullptr;
static ID3D11Texture2D      *g_stage11[kStage] = {};
static ID3D12Device         *g_dev12own = nullptr;     // the add-on's D3D12 device (kept across swap chains)
static LUID                  g_dev12Luid = {};
static HWND                  g_gameHwnd = nullptr;
static std::atomic<HWND>     g_childHwnd{nullptr};
static HANDLE                g_uiThread = nullptr;
static DXGI_SWAP_CHAIN_DESC1 g_desc11 = {};             // the game's swap chain
static bool                  g_fsLogged = false;
static bool                  g_sizeLogged = false;

// Formats a flip-model swap chain accepts, for the game's back-buffer format.
static DXGI_FORMAT FlipFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// The presenter's window: a child of the game's window covering its client
// area. It is disabled, so the system passes mouse input over it to the game's
// window, and it never takes keyboard focus.
static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_NCHITTEST:    return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND:   return 1;
    case WM_PAINT:        ValidateRect(h, nullptr); return 0;
    case WM_DESTROY:      PostQuitMessage(0); return 0;
    default:              return DefWindowProcW(h, m, w, l);
    }
}

static DWORD WINAPI UiThread(void *ev)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = ChildProc;
    wc.hInstance     = g_module;
    wc.lpszClassName = L"BFIPresenterDX11";
    RegisterClassExW(&wc);                                       // fails harmlessly when already registered
    RECT rc = {};
    GetClientRect(g_gameHwnd, &rc);
    HWND h = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"BFIPresenterDX11", L"BFI Presenter",
                             WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS,
                             0, 0, (std::max)(1L, rc.right - rc.left), (std::max)(1L, rc.bottom - rc.top),
                             g_gameHwnd, nullptr, g_module, nullptr);
    g_childHwnd.store(h);
    SetEvent((HANDLE)ev);
    if (!h) return 0;
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static bool CreateChildWindow()
{
    HANDLE ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_childHwnd.store(nullptr);
    g_uiThread = CreateThread(nullptr, 0, UiThread, ev, 0, nullptr);
    if (g_uiThread) { g_uiTid.store(GetThreadId(g_uiThread)); WaitForSingleObject(ev, 5000); }
    CloseHandle(ev);
    if (!g_childHwnd.load())
    {
        LOGE("BFI Presenter: DX11: could not create the presenter's window (%lu)", GetLastError());
        return false;
    }
    return true;
}

static void DestroyChildWindow()
{
    HWND h = g_childHwnd.exchange(nullptr);
    if (h) PostMessageW(h, WM_CLOSE, 0, 0);
    if (g_uiThread)
    {
        WaitForSingleObject(g_uiThread, 3000);
        CloseHandle(g_uiThread);
        g_uiThread = nullptr;
    }
}

// Keep the child window over the game window's client area.
static void Sync11Window()
{
    HWND h = g_childHwnd.load();
    if (!h || !g_gameHwnd) return;
    RECT pr = {}, cr = {};
    GetClientRect(g_gameHwnd, &pr);
    GetClientRect(h, &cr);
    if (pr.right - pr.left != cr.right - cr.left || pr.bottom - pr.top != cr.bottom - cr.top)
        SetWindowPos(h, HWND_TOP, 0, 0, (std::max)(1L, pr.right - pr.left), (std::max)(1L, pr.bottom - pr.top),
                     SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

// The presenter's swap chain uses the colour space the game set on its own.
static void ApplyColorSpace11()
{
    if (!g_sc) return;
    DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const int set = g_cs11.load();
    if (set >= 0) cs = (DXGI_COLOR_SPACE_TYPE)set;
    else if (g_rsSwapchain)
    {
        switch (g_rsSwapchain->get_color_space())
        {
        case reshade::api::color_space::hdr10_st2084:         cs = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020; break;
        case reshade::api::color_space::extended_srgb_linear: cs = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709; break;
        default: break;
        }
    }
    if (g_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
        cs = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;              // a float swap chain is scRGB
    OwnCall own;
    UINT sup = 0;
    auto check = SC_DIRECT(HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain3 *, DXGI_COLOR_SPACE_TYPE, UINT *), 37);
    auto setCs = SC_DIRECT(PFN_SetCS1, 38);
    if (SUCCEEDED(check(g_sc, cs, &sup)) && (sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
    {
        const HRESULT hr = setCs(g_sc, cs);
        LOGI("BFI Presenter: DX11: presenter swap chain colour space %d (0x%08lX)", (int)cs, (unsigned long)hr);
    }
    else LOGW("BFI Presenter: DX11: colour space %d not supported for presentation here (Windows HDR off?)", (int)cs);
}

static void ReleaseStage11()
{
    for (UINT s = 0; s < kStage; s++) { SafeRelease(g_stage11[s]); SafeRelease(g_stage[s]); }
    g_bufCount = 0;
}

// Staging ring on the add-on's D3D12 device, shared with the game's DX11
// device. Also takes the presenter swap chain's description into g_desc.
static bool CreateStage11()
{
    ScDesc1(&g_desc);
    g_bufCount = g_desc.BufferCount < kMaxBuf ? g_desc.BufferCount : kMaxBuf;
    for (UINT s = 0; s < kStage; s++)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = g_desc.Width;
        rd.Height           = g_desc.Height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = g_desc.Format;
        rd.SampleDesc.Count = 1;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;    // DX11 opens shared textures with a bind flag
        const char *step = "CreateCommittedResource";
        HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr, IID_PPV_ARGS(&g_stage[s]));
        HANDLE h = nullptr;
        if (SUCCEEDED(hr)) { step = "CreateSharedHandle"; hr = g_device->CreateSharedHandle(g_stage[s], nullptr, GENERIC_ALL, nullptr, &h); }
        if (SUCCEEDED(hr)) { step = "OpenSharedResource1"; hr = g_dev11->OpenSharedResource1(h, IID_PPV_ARGS(&g_stage11[s])); }
        if (h) CloseHandle(h);
        if (FAILED(hr))
        {
            LOGE("BFI Presenter: DX11: shared staging texture %u failed in %s (0x%08lX), %ux%u format %u", s, step,
                 (unsigned long)hr, g_desc.Width, g_desc.Height, (unsigned)g_desc.Format);
            ReleaseStage11();
            return false;
        }
        g_stage[s]->SetName(L"BFI staging frame (shared with DX11)");
        g_stageFence[s] = g_stageId[s] = g_readFence[s] = 0;
        g_stageTime[s] = 0.0;
    }
    g_presCurSlot = -1;
    g_stageNext = 0;
    LOGI("BFI Presenter: DX11: presenting %ux%u, format %u, %u buffers (game: format %u, %u buffers)",
         g_desc.Width, g_desc.Height, (unsigned)g_desc.Format, g_bufCount, (unsigned)g_desc11.Format, g_desc11.BufferCount);
    EnterCriticalSection(&g_cfgLock);
    g_status.width = g_desc.Width; g_status.height = g_desc.Height;
    g_status.buffers = g_bufCount; g_status.format = (UINT)g_desc.Format;
    LeaveCriticalSection(&g_cfgLock);
    return true;
}

// Exclusive fullscreen is held off (the presenter needs a window), so the
// game's window is made to cover its monitor without a border instead -- what
// exclusive fullscreen would have looked like. Off with BorderlessFullscreen=0.
static struct { HWND hwnd; LONG_PTR style, exStyle; RECT rect; bool saved; } g_bl = {};
static void MakeBorderless(HWND h, HMONITOR mon)
{
    if (!h || !IsWindow(h) || GetPrivateProfileIntA("BFI", "BorderlessFullscreen", 1, g_iniPath) == 0) return;
    if (!mon) mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoA(mon, &mi)) return;
    const RECT &m = mi.rcMonitor;
    RECT wr = {};
    GetWindowRect(h, &wr);
    const LONG_PTR style = GetWindowLongPtrA(h, GWL_STYLE), ex = GetWindowLongPtrA(h, GWL_EXSTYLE);
    const bool covers = wr.left == m.left && wr.top == m.top && wr.right == m.right && wr.bottom == m.bottom;
    if (covers && !(style & (WS_CAPTION | WS_THICKFRAME))) return;          // already borderless on the monitor
    if (!g_bl.saved || g_bl.hwnd != h) { g_bl.hwnd = h; g_bl.style = style; g_bl.exStyle = ex; g_bl.rect = wr; g_bl.saved = true; }
    SetWindowLongPtrA(h, GWL_STYLE, (style & ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP);
    SetWindowLongPtrA(h, GWL_EXSTYLE, ex & ~(LONG_PTR)(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
    SetWindowPos(h, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    LOGI("BFI Presenter: the game's window made borderless over its monitor (%ldx%ld) in place of exclusive fullscreen",
         m.right - m.left, m.bottom - m.top);
}
static void RestoreWindowed(HWND h)
{
    if (!g_bl.saved || g_bl.hwnd != h || !IsWindow(h)) return;
    g_bl.saved = false;
    SetWindowLongPtrA(h, GWL_STYLE, g_bl.style);
    SetWindowLongPtrA(h, GWL_EXSTYLE, g_bl.exStyle);
    SetWindowPos(h, nullptr, g_bl.rect.left, g_bl.rect.top, g_bl.rect.right - g_bl.rect.left, g_bl.rect.bottom - g_bl.rect.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    LOGI("BFI Presenter: the game left fullscreen -- its window style and size restored");
}
static HMONITOR OutputMonitor(IDXGIOutput *o)
{
    DXGI_OUTPUT_DESC od = {};
    return (o && SUCCEEDED(o->GetDesc(&od))) ? od.Monitor : nullptr;
}

// Exclusive fullscreen would put the game's own swap chain in charge of the
// display: the presenter needs windowed (borderless) mode. The game is told
// what it asked for.
BFI_TLS(int, t_inFsCs);      // re-entry from hooks below ours (Steam) goes straight to DXGI
struct FsCsDepth { FsCsDepth() { t_inFsCs++; } ~FsCsDepth() { t_inFsCs--; } };
static HRESULT STDMETHODCALLTYPE hk_SetFS(IDXGISwapChain *sc, BOOL fs, IDXGIOutput *target)
{
    if (t_inFsCs)
    {
        PFN_SetFS f = (PFN_SetFS)DirectSlot(*(void ***)sc, VT_SetFullscreenState);
        return (f == &hk_SetFS ? o_SetFS : f)(sc, fs, target);      // never back into this hook
    }
    FsCsDepth depth;
    if (t_ownCall || !IsGame11(sc)) return o_SetFS(sc, fs, target);
    g_fsWanted = fs;
    if (fs) MakeBorderless(g_gameHwnd, OutputMonitor(target));
    else    RestoreWindowed(g_gameHwnd);
    if (fs)
    {
        if (!g_fsLogged)
        {
            g_fsLogged = true;
            LOGW("BFI Presenter: DX11: the game asked for exclusive fullscreen -- held off (the presenter needs borderless). "
                 "Choose borderless/windowed fullscreen in the game for the best result.");
        }
        return S_OK;
    }
    OwnCall own;
    return o_SetFS(sc, FALSE, nullptr);
}
static HRESULT STDMETHODCALLTYPE hk_GetFSt(IDXGISwapChain *sc, BOOL *fs, IDXGIOutput **target)
{
    if (t_inFsCs)
    {
        PFN_GetFS2 f = (PFN_GetFS2)DirectSlot(*(void ***)sc, VT_GetFullscreenState);
        return (f == &hk_GetFSt ? o_GetFSt : f)(sc, fs, target);
    }
    FsCsDepth depth;
    if (t_ownCall || !IsGame11(sc)) return o_GetFSt(sc, fs, target);
    if (fs) *fs = g_fsWanted;
    if (target) *target = nullptr;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE hk_SetCS1(IDXGISwapChain3 *sc, DXGI_COLOR_SPACE_TYPE cs)
{
    if (t_inFsCs)
    {
        PFN_SetCS1 f = (PFN_SetCS1)DirectSlot(*(void ***)sc, VT_SetColorSpace1);
        return (f == &hk_SetCS1 ? o_SetCS1 : f)(sc, cs);
    }
    FsCsDepth depth;
    const HRESULT hr = o_SetCS1(sc, cs);
    if (!t_ownCall && IsGame11(sc) && SUCCEEDED(hr) && g_cs11.exchange((int)cs) != (int)cs)
    {
        LOGI("BFI Presenter: DX11: the game set colour space %d", (int)cs);
        ApplyColorSpace11();
    }
    return hr;
}

static void ReleaseDx11()
{
    if (!StopPresenter())
    {
        // Leave everything the presenter thread may still use alive, hide its
        // window so the game's own frames show, and keep g_sc set so no other
        // swap chain is taken over in this run.
        g_active.store(false);
        if (HWND h = g_childHwnd.load()) ShowWindowAsync(h, SW_HIDE);
        SetReason(0, "BFI stopped: the presenter thread hung when the game changed display mode. The game continues "
                     "without BFI; restart it to use BFI. Use borderless (windowed) fullscreen instead of exclusive "
                     "fullscreen in this game.");
        return;
    }
    g_active.store(false);
    ReleaseStage11();
    if (g_sc) { OwnCall own; g_sc->Release(); }
    g_sc = nullptr;
    DestroyChildWindow();
    SafeRelease(g_gameFence11);
    SafeRelease(g_presFence11);
    SafeRelease(g_gameFence);
    SafeRelease(g_presFence);
    SafeRelease(g_ctx11);
    SafeRelease(g_dev11);
    if (g_waitSem) { ReleaseSemaphore(g_waitSem, 16, nullptr); CloseHandle(g_waitSem); g_waitSem = nullptr; }
    g_sc11 = nullptr;
    g_dx11.store(false);
    g_device = nullptr;                // g_dev12own and g_presQueue stay for the next swap chain
    g_rsSwapchain = nullptr;
    g_ownPresentQueue = false;
    g_cs11.store(-1);
    g_fsWanted = FALSE;
}

// The presenter's own D3D12 device, factory and swap chain are created past
// every hook (ReShade's, NVIDIA's present layer, RTSS, Steam): ReShade must not
// see them (it would run its effects and other add-ons on the presenter's
// frames), and nothing else should wrap them.
static void *RawEntry(void *fn, const char *what)
{
    if (!fn) return nullptr;
    char mod[64];
    ModuleOf(fn, mod, sizeof(mod));
    if (_stricmp(mod, "dxgi.dll") && _stricmp(mod, "d3d12.dll"))
        LOGW("BFI Presenter: DX11: %s resolves into %s rather than the system DLL", what, mod);
    char why[96] = "";
    void *u = MakeUnhookedEntry(fn, why, sizeof(why));
    LOGI("BFI Presenter: DX11: %s called directly (%s)", what, u ? why : "hooks stay in the path");
    return u ? u : fn;
}
// The system's own DLL: ReShade is often installed as dxgi.dll in the game's
// folder, and that one is what LoadLibrary("dxgi.dll") returns there.
static HMODULE LoadSystemDll(const char *name)
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (!n || n + strlen(name) + 2 > MAX_PATH) return LoadLibraryA(name);
    strcat(path, "\\");
    strcat(path, name);
    HMODULE m = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    return m ? m : LoadLibraryA(name);
}
typedef HRESULT (WINAPI *PFN_D3D12CreateDev)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *PFN_CreateFactory2)(UINT, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSCForHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
static HRESULT RawCreateFactory(IDXGIFactory4 **out)
{
    static PFN_CreateFactory2 f = nullptr;
    if (!f) f = (PFN_CreateFactory2)RawEntry((void *)GetProcAddress(LoadSystemDll("dxgi.dll"), "CreateDXGIFactory2"), "CreateDXGIFactory2");
    return f ? f(0, IID_PPV_ARGS(out)) : E_FAIL;
}
static HRESULT RawCreateDevice12(IUnknown *adapter, ID3D12Device **out)
{
    static PFN_D3D12CreateDev f = nullptr;
    if (!f) f = (PFN_D3D12CreateDev)RawEntry((void *)GetProcAddress(LoadSystemDll("d3d12.dll"), "D3D12CreateDevice"), "D3D12CreateDevice");
    return f ? f(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(out)) : E_FAIL;
}
static HRESULT RawCreateSwapChain(IDXGIFactory4 *fac, IUnknown *queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *sd, IDXGISwapChain1 **out)
{
    void **vt = *(void ***)fac;
    void *pristine = PristineVtableEntry(vt, 15);                  // IDXGIFactory2::CreateSwapChainForHwnd
    PFN_CreateSCForHwnd f = (PFN_CreateSCForHwnd)RawEntry(pristine ? pristine : vt[15], "CreateSwapChainForHwnd");
    return f((IDXGIFactory2 *)fac, queue, hwnd, sd, nullptr, nullptr, out);
}
static std::atomic<bool> g_creatingOwn{false};

static bool InitPresenter11(reshade::api::swapchain *swapchain, IDXGISwapChain3 *sc3, const DXGI_SWAP_CHAIN_DESC1 &d,
                            HWND hwnd, DXGI_FORMAT ff, LUID luid, const char *what);
static bool InitDx11(reshade::api::swapchain *swapchain)
{
    IDXGISwapChain *native = reinterpret_cast<IDXGISwapChain *>(swapchain->get_native());
    IDXGISwapChain3 *sc3 = nullptr;
    if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&sc3))))
    {
        SetReason(0, "Not managed (DX11): no IDXGISwapChain3 interface.");
        return false;
    }
    sc3->Release();
    DXGI_SWAP_CHAIN_DESC1 d = {};
    sc3->GetDesc1(&d);
    HWND hwnd = nullptr;
    if (FAILED(sc3->GetHwnd(&hwnd)) || !hwnd)
    {
        SetReason(0, "Not managed (DX11): the swap chain has no window.");
        return false;
    }
    const DXGI_FORMAT ff = FlipFormat(d.Format);
    if (ff == DXGI_FORMAT_UNKNOWN)
    {
        SetReason(0, "Not managed (DX11): back-buffer format %u is not supported.", (unsigned)d.Format);
        return false;
    }
    {
        void **vt = *(void ***)sc3;
        char objMod[128], presMod[128];
        ModuleOf(vt, objMod, sizeof(objMod));
        ModuleOf(vt[VT_Present], presMod, sizeof(presMod));
        snprintf(g_scOwner, sizeof(g_scOwner), "%s (Present in %s)", objMod, presMod);
        DetectFrameGen();
    }
    g_gamePresent11.store(GetPrivateProfileIntA("BFI", "DX11GamePresent", 0, g_iniPath));
    LOGI("BFI Presenter: DX11 init 1/5 -- swap chain %p, %ux%u, format %u, %u buffers, swap effect %u, flags 0x%X",
         (void *)sc3, d.Width, d.Height, (unsigned)d.Format, d.BufferCount, (unsigned)d.SwapEffect, (unsigned)d.Flags);

    ID3D11Device *dev = reinterpret_cast<ID3D11Device *>(swapchain->get_device()->get_native());
    ID3D11Device3 *dev3 = nullptr;
    ID3D11DeviceContext3 *ctx3 = nullptr;
    bool ok = dev && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&g_dev11)));
    if (ok && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev3))))
    {
        dev3->GetImmediateContext3(&ctx3);
        if (ctx3) ctx3->QueryInterface(IID_PPV_ARGS(&g_ctx11));
    }
    SafeRelease(ctx3);
    SafeRelease(dev3);
    ok = ok && g_ctx11;
    if (!ok)
    {
        SafeRelease(g_dev11);
        SetReason(0, "Not managed (DX11): the game's device lacks shared-fence support (needs Windows 10 1703 or later).");
        return false;
    }

    // The add-on's D3D12 device, on the game's GPU.
    IDXGIDevice *dxgiDev = nullptr;
    IDXGIAdapter *ad = nullptr;
    DXGI_ADAPTER_DESC ad0 = {};
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev))) && SUCCEEDED(dxgiDev->GetAdapter(&ad))) ad->GetDesc(&ad0);
    SafeRelease(dxgiDev);
    SafeRelease(ad);
    return InitPresenter11(swapchain, sc3, d, hwnd, ff, ad0.AdapterLuid, "DX11");
}

// The presenter side shared by the DX11 and DX9 front ends: the add-on's
// D3D12 device, shared fences, presenter queue, child window, swap chain and
// staging ring. g_dev11 / g_ctx11 are set by the caller. sc3 is the game's
// DXGI swap chain (DX11; hooked here) or null (DX9: hooked by the caller).
static bool InitPresenter11(reshade::api::swapchain *swapchain, IDXGISwapChain3 *sc3, const DXGI_SWAP_CHAIN_DESC1 &d,
                            HWND hwnd, DXGI_FORMAT ff, LUID luid, const char *what)
{
    bool ok = true;
    DXGI_ADAPTER_DESC ad0 = {};
    ad0.AdapterLuid = luid;
    if (g_dev12own && memcmp(&g_dev12Luid, &ad0.AdapterLuid, sizeof(LUID)) != 0)
    {
        SafeRelease(g_presQueue);
        SafeRelease(g_dev12own);
    }
    IDXGIFactory4 *fac = nullptr;
    ok = SUCCEEDED(RawCreateFactory(&fac));
    if (ok && !g_dev12own)
    {
        IDXGIAdapter1 *a1 = nullptr;
        if (SUCCEEDED(fac->EnumAdapterByLuid(ad0.AdapterLuid, IID_PPV_ARGS(&a1))))
            ok = SUCCEEDED(RawCreateDevice12(a1, &g_dev12own));
        else ok = false;
        SafeRelease(a1);
        g_dev12Luid = ad0.AdapterLuid;
        if (ok) LOGI("BFI Presenter: DX11 init 2/5 -- D3D12 device for the presenter created");
    }
    if (!ok || !g_dev12own)
    {
        SafeRelease(fac);
        SafeRelease(g_ctx11);
        SafeRelease(g_dev11);
        SetReason(0, "Not managed (%s): could not create the presenter's D3D12 device.", what);
        return false;
    }
    g_device = g_dev12own;

    // Shared fences: the game's copies signal g_gameFence from DX11, the
    // presenter's reads signal g_presFence from D3D12, and each side waits on
    // the other's.
    auto shareFence = [](ID3D12Fence *f, ID3D11Fence **out) {
        HANDLE h = nullptr;
        bool r = SUCCEEDED(g_device->CreateSharedHandle(f, nullptr, GENERIC_ALL, nullptr, &h)) &&
                 SUCCEEDED(g_dev11->OpenSharedFence(h, IID_PPV_ARGS(out)));
        if (h) CloseHandle(h);
        return r;
    };
    ok = SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_gameFence))) &&
         SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_presFence))) &&
         shareFence(g_gameFence, &g_gameFence11) && shareFence(g_presFence, &g_presFence11);
    if (ok && !g_presQueue)
    {
        int want;
        EnterCriticalSection(&g_cfgLock);
        want = g_shared.queuePriority;
        LeaveCriticalSection(&g_cfgLock);
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ok = false;
        if (want >= 2)
        {
            EnableBasePriorityPrivilege();
            qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
            ok = SUCCEEDED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_presQueue)));
        }
        if (!ok)
        {
            qd.Priority = want >= 1 ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            ok = SUCCEEDED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_presQueue)));
        }
        g_queuePriority = ok ? qd.Priority : 0;
        if (ok) g_presQueue->SetName(L"BFI presenter queue (DX11 mode)");
    }
    LOGI("BFI Presenter: DX11 init 3/5 -- shared fences and presenter queue %s", ok ? "ready" : "FAILED");

    // Hooks on the game's swap chain (Present, resize, statistics, latency,
    // fullscreen and colour space).
    if (ok && sc3 && !InstallHooks(sc3)) { ok = false; LOGW("BFI Presenter: DX11: swap chain vtable differs from the first one hooked"); }
    if (ok && sc3)
    {
        void **vt = *(void ***)sc3;
        PatchSlot(vt, VT_SetFullscreenState, (void *)&hk_SetFS,  (void **)&o_SetFS);
        PatchSlot(vt, VT_GetFullscreenState, (void *)&hk_GetFSt, (void **)&o_GetFSt);
        PatchSlot(vt, VT_SetColorSpace1,     (void *)&hk_SetCS1, (void **)&o_SetCS1);
    }

    // The presenter's window and swap chain.
    g_gameHwnd = hwnd;
    g_watchHwnd.store(hwnd);
    ok = ok && CreateChildWindow();
    IDXGISwapChain1 *own1 = nullptr;
    Settings cur;
    EnterCriticalSection(&g_cfgLock);
    cur = g_shared;
    LeaveCriticalSection(&g_cfgLock);
    if (ok)
    {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width       = d.Width;
        sd.Height      = d.Height;
        sd.Format      = ff;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = (UINT)(std::max)(2, cur.presBuffers);
        sd.Scaling     = DXGI_SCALING_STRETCH;
        sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
        HRESULT hr;
        {
            OwnCall own;
            g_creatingOwn.store(true);
            hr = RawCreateSwapChain(fac, g_presQueue, g_childHwnd.load(), &sd, &own1);
            g_creatingOwn.store(false);
        }
        ok = SUCCEEDED(hr) && own1 && SUCCEEDED(own1->QueryInterface(IID_PPV_ARGS(&g_sc)));
        SafeRelease(own1);
        if (ok) fac->MakeWindowAssociation(g_childHwnd.load(), DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
        LOGI("BFI Presenter: DX11 init 4/5 -- presenter swap chain on its own window: 0x%08lX", (unsigned long)hr);
    }
    SafeRelease(fac);
    if (!ok)
    {
        if (g_sc) { g_sc->Release(); g_sc = nullptr; }
        DestroyChildWindow();
        SafeRelease(g_gameFence11); SafeRelease(g_presFence11); SafeRelease(g_gameFence); SafeRelease(g_presFence);
        SafeRelease(g_ctx11); SafeRelease(g_dev11);
        g_device = nullptr;
        if (sc3) RemoveHooks();
        SetReason(0, "Not managed (%s): setup failed (see ReShade.log).", what);
        return false;
    }
    if (sc3)
    {
        // Already in exclusive fullscreen: leave it (the game is told it still is).
        BOOL fs = FALSE;
        ((PFN_GetFS2)DirectSlot(*(void ***)sc3, VT_GetFullscreenState))(sc3, &fs, nullptr);
        if (fs)
        {
            OwnCall own;
            const HRESULT hr = ((PFN_SetFS)DirectSlot(*(void ***)sc3, VT_SetFullscreenState))(sc3, FALSE, nullptr);
            g_fsWanted = TRUE;
            MakeBorderless(hwnd, nullptr);
            LOGW("BFI Presenter: DX11: the game was in exclusive fullscreen -- switched to windowed (0x%08lX); "
                 "choose borderless/windowed fullscreen in the game for the best result", (unsigned long)hr);
        }
    }

    g_sc11 = sc3;
    g_desc11 = d;
    g_rsSwapchain = swapchain;
    g_dx11.store(true);
    g_ownPresentQueue = true;
    g_gameQueue = nullptr;
    g_gameFenceVal = g_presFenceVal = 0;
    g_frameId = 0;
    g_releasedUpTo.store(0);
    g_shownGameFrames.store(0);
    g_lastPresentT.store(0.0);
    g_stallLogged.store(false);
    g_firstStaged.store(false);
    g_firstPresented.store(false);
    g_presenterFailed.store(false);
    g_deferPresenter.store(false);
    for (DWORD &t : g_presentThreads) t = 0;
    ApplyColorSpace11();
    if (!CreateStage11())
    {
        ReleaseDx11();
        if (sc3) RemoveHooks();
        SetReason(0, "Not managed (%s): shared staging textures could not be created.", what);
        return false;
    }
    g_virtCount = d.BufferCount;
    if (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
    {
        g_maxLatency.store(1);
        g_waitSem = CreateSemaphoreA(nullptr, 1, 16, nullptr);
        LOGI("BFI Presenter: DX11: game uses a frame-latency waitable -- driven by the presenter");
    }
    g_active.store(true);
    g_swapChainsManaged++;
    LOGI("BFI Presenter: %s init 5/5 -- managing the game's %s swap chain; BFI starts with its first frame", what, what);
    SetReason(1, "Managing the %s swap chain (%ux%u) through the add-on's own D3D12 presenter.", what, d.Width, d.Height);
    return true;
}

// One DX11 game frame: copy its back buffer (ReShade's effects already
// applied) into a free staging slot, signal the shared fence, and keep the
// game to the frames-ahead limit and the frame cap. Its own swap chain does
// not present.
// The frame's source comes from getFrame (an AddRef'd texture on g_dev11, or
// null); showOwn presents the game's own frame when the presenter can't run.
template <class GetFrame, class ShowOwn>
static HRESULT SubmitFrame11Core(GetFrame getFrame, ShowOwn showOwn)
{
    const double enter = Now();
    EnterCriticalSection(&g_submitLock);
    g_gtEnter = enter;
    g_gtSerial = Now();
    g_gtSlot = g_gtLock = g_gtCopy = g_gtAhead = 0.0;
    g_gtFallback = 0;
    if (!g_running.load() && !g_presenterFailed.load()) StartPresenter();
    if (!g_running.load())
    {
        // Presenter unavailable: show the game's own frames under a hidden child window.
        static bool logged = false;
        if (!logged) { logged = true; LOGW("BFI Presenter: presenter not running (failed %d, buffers %u, stuck %d) -- the game's own frames are shown",
                                         (int)g_presenterFailed.load(), g_bufCount, (int)g_presenterStuck.load()); }
        HWND h = g_childHwnd.load();
        if (h && IsWindowVisible(h)) ShowWindowAsync(h, SW_HIDE);
        ReleaseGameSlot();               // shown by DXGI directly: the presenter won't release this slot
        LeaveCriticalSection(&g_submitLock);
        GameStage(ST_G_OUTSIDE);
        return showOwn();
    }
    ID3D11Texture2D *bb = getFrame();
    D3D11_TEXTURE2D_DESC bd = {};
    if (bb) bb->GetDesc(&bd);
    if (!bb || bd.Width != g_desc.Width || bd.Height != g_desc.Height)
    {
        if (!g_sizeLogged) { g_sizeLogged = true; LOGW("BFI Presenter: DX11: back buffer %ux%u does not match the presenter (%ux%u)", bd.Width, bd.Height, g_desc.Width, g_desc.Height); }
        SafeRelease(bb);
        ReleaseGameSlot();               // no frame staged: nothing would release the slot it took
        LeaveCriticalSection(&g_submitLock);
        GameStage(ST_G_OUTSIDE);
        return S_OK;
    }
    // Staging slot: the oldest one the presenter is neither showing nor reading.
    UINT s = g_stageNext;
    {
        const UINT64 pdone = g_presFence->GetCompletedValue();
        const UINT64 gdone = g_gameFence->GetCompletedValue();
        int best = -1;
        UINT64 bestId = ~0ull;
        EnterCriticalSection(&g_lock);
        for (UINT i = 0; i < kStage; i++)
        {
            if ((int)i == g_presCurSlot) continue;
            if (g_readFence[i] > pdone || g_stageFence[i] > gdone) continue;
            if (g_stageId[i] < bestId) { best = (int)i; bestId = g_stageId[i]; }
        }
        LeaveCriticalSection(&g_lock);
        if (best >= 0) s = (UINT)best;
        else g_slotWaits.fetch_add(1);
        g_gtFallback = best < 0;
    }
    g_gtSlot = Now();
    EnterCriticalSection(&g_lock);                       // only to mark the slot (see the DX12 path)
    g_gtLock = Now();
    const UINT64 waitRead = g_readFence[s];
    if (g_stageId[s] > g_shownGameFrames.load()) g_lostFrames.fetch_add(1);
    g_stageId[s] = 0;                                    // being rewritten: not selectable
    LeaveCriticalSection(&g_lock);
    if (waitRead) g_ctx11->Wait(g_presFence11, waitRead);   // presenter done reading this slot
    if (bd.SampleDesc.Count > 1) g_ctx11->ResolveSubresource(g_stage11[s], 0, bb, 0, g_desc.Format);
    else                         g_ctx11->CopyResource(g_stage11[s], bb);
    g_ctx11->Signal(g_gameFence11, ++g_gameFenceVal);
    g_ctx11->Flush();
    EnterCriticalSection(&g_lock);                       // publish: selectable from here on
    g_stageFence[s] = g_gameFenceVal;
    g_stageId[s]    = ++g_frameId;
    g_recentFence[g_frameId % 8] = g_gameFenceVal;
    g_stageTime[s]  = Now();
    LeaveCriticalSection(&g_lock);
    LrStaged(g_frameId, g_gameFenceVal);
    g_gtCopy = Now();
    bb->Release();
    g_stageNext = (s + 1) % kStage;
    g_stagedThisSecond.fetch_add(1);
    if (!g_firstStaged.exchange(true)) LOGI("BFI Presenter: DX11: first game frame received");

    // Latency limit and frame cap, as for DX12 games.
    const int ahead = g_maxAhead.load();
    if (ahead > 0 && g_frameId > (UINT64)ahead)
        WaitFence(g_gameFence, g_recentFence[(g_frameId - ahead) % 8], g_gameFenceEvent);
    g_gtAhead = Now();
    if (g_releaseOnSubmit.load()) ReleaseUpTo(g_frameId);   // lowest latency: the game is not paced by the strobe
    const int cap = g_gameCap.load();
    const UINT64 per = cap == 1 ? 1 : cap == 2 ? 2 : cap == 3 ? 4 : 0;
    if (per && g_running.load()) CapWait(per);
    g_gtExit = Now();
    WriteGameLog();
    g_gtPrevExit = g_gtExit;
    LeaveCriticalSection(&g_submitLock);
    GameStage(ST_G_OUTSIDE);
    const HRESULT hr = g_lastPresentHr.load();
    return (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) ? hr : S_OK;
}
static HRESULT SubmitGameFrame11(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    if (g_gamePresent11.load())
    {
        const HRESULT hr = SubmitFrame11Core(
            [&]() -> ID3D11Texture2D * { ID3D11Texture2D *bb = nullptr; OwnCall own; o_GetBuffer(sc, 0, __uuidof(ID3D11Texture2D), (void **)&bb); return bb; },
            [&]() -> HRESULT { OwnCall own; return o_Present(sc, sync, flags); });
        if (g_running.load())
        {
            // DXGI's own Present, past the overlay hooks (Steam, RTSS): the
            // frame is never seen, and Steam's overlay crashed Wreckfest when
            // it saw both the game's DX11 presents and the presenter's D3D12
            // device.
            OwnCall own;
            static PFN_Present direct = nullptr;
            static void **forVt = nullptr;
            void **vt = *(void ***)sc;
            if (forVt != vt) { forVt = vt; direct = (PFN_Present)DirectSlot(vt, VT_Present); }
            const HRESULT ph = (direct && direct != (PFN_Present)&hk_Present) ? direct(sc, 0, DXGI_PRESENT_DO_NOT_WAIT)
                                                                                : o_Present(sc, 0, DXGI_PRESENT_DO_NOT_WAIT);
            static bool logged = false;
            if (!logged) { logged = true; LOGI("BFI Presenter: DX11: the game's own swap chain presents too (DX11GamePresent=1): 0x%08lX", (unsigned long)ph); }
        }
        return hr;
    }
    return SubmitFrame11Core(
        [&]() -> ID3D11Texture2D * { ID3D11Texture2D *bb = nullptr; OwnCall own; o_GetBuffer(sc, 0, __uuidof(ID3D11Texture2D), (void **)&bb); return bb; },
        [&]() -> HRESULT { OwnCall own; return o_Present(sc, sync, flags); });
}

static HRESULT Resize11(IDXGISwapChain *sc, UINT cnt, UINT w, UINT h, DXGI_FORMAT f, UINT fl,
                        const UINT *mask, IUnknown *const *queues, bool one)
{
    EnterCriticalSection(&g_submitLock);
    if (!StopPresenter())
    {
        if (HWND h = g_childHwnd.load()) ShowWindowAsync(h, SW_HIDE);
        LeaveCriticalSection(&g_submitLock);
        return StuckResize(sc, [&] { OwnCall own;
            return one ? o_ResizeBuffers1((IDXGISwapChain3 *)sc, cnt, w, h, f, fl, mask, queues) : o_ResizeBuffers(sc, cnt, w, h, f, fl); });
    }
    ReleaseStage11();
    HRESULT hr;
    {
        OwnCall own;
        hr = one ? o_ResizeBuffers1((IDXGISwapChain3 *)sc, cnt, w, h, f, fl, mask, queues) : o_ResizeBuffers(sc, cnt, w, h, f, fl);
    }
    DXGI_SWAP_CHAIN_DESC1 d = {};
    g_sc11->GetDesc1(&d);
    g_desc11 = d;
    if (cnt) g_virtCount = cnt;
    DXGI_FORMAT ff = FlipFormat(d.Format);
    if (ff == DXGI_FORMAT_UNKNOWN) ff = g_desc.Format;
    HRESULT hr2;
    {
        OwnCall own;
        hr2 = SC_DIRECT(PFN_ResizeBuffers, 13)(g_sc, 0, d.Width, d.Height, ff, 0);
    }
    LOGI("BFI Presenter: DX11: game resized to %ux%u format %u (0x%08lX); presenter swap chain 0x%08lX",
         d.Width, d.Height, (unsigned)d.Format, (unsigned long)hr, (unsigned long)hr2);
    g_sizeLogged = false;
    ApplyColorSpace11();
    if (!CreateStage11()) g_presenterFailed.store(true);
    Sync11Window();
    HWND hw = g_childHwnd.load();
    if (hw && !g_presenterFailed.load() && !IsWindowVisible(hw)) ShowWindowAsync(hw, SW_SHOWNA);
    LeaveCriticalSection(&g_submitLock);
    GameStage(ST_G_OUTSIDE);
    return hr;
}

// ---------------------------------------------------------------- DX9 (feasibility test)
// Direct3D 9 can share a texture with newer APIs only from a 9Ex device.
// ReShade can create the game's device as 9Ex (its create_device event, newer
// ReShade only; it also converts the managed pool). This test build checks
// that the game runs that way and that its frames can be copied into a
// texture the add-on's own D3D11 device reads. No BFI yet: results go to
// ReShade.log ("DX9 test: ...").
static std::atomic<bool> g_dx9Upgrade{false};
static bool g_dx9WantedFs = false;                  // the game asked for exclusive fullscreen (kept windowed)
static bool g_dx9Diag = false;
static bool on_create_device9(reshade::api::device_api api, uint32_t &ver)
{
    if (api != reshade::api::device_api::d3d9 || ver >= 0x9100) return false;
    if (GetPrivateProfileIntA("BFI", "DX9Support", 1, g_iniPath) == 0)
    {
        LOGI("BFI Presenter: DX9: DX9Support=0 -- the game's device is left as it is (no BFI)");
        return false;
    }
    g_dx9Upgrade.store(true);   // also gates the windowed override
    g_dx9Diag = GetPrivateProfileIntA("BFI", "DX9Diag", 0, g_iniPath) != 0;
    if (GetPrivateProfileIntA("BFI", "DX9Ex", 1, g_iniPath) == 0)
    {
        LOGI("BFI Presenter: DX9: DX9Ex=0 -- keeping plain Direct3D 9 (windowed only, no BFI)");
        return false;
    }
    ver = 0x9100;
    LOGI("BFI Presenter: DX9: asking ReShade to create the game's device as Direct3D 9Ex (needed to share its frames)");
    return true;
}
// 9Ex can't create an exclusive-fullscreen device the way the game asks
// (Alan Wake: DEVICE_CREATE_ERROR - Invalid call), and the presenter needs
// a window anyway: keep the swap chain windowed (borderless at full size).
// create_swapchain gained a device_api first argument in newer ReShade; this
// callback reads either form: an API value is small, a pointer is not.
static bool on_create_swapchain9(uintptr_t a, uintptr_t b, uintptr_t)
{
    if (!g_dx9Upgrade.load()) return false;
    reshade::api::swapchain_desc *desc = nullptr;
    if (a == (uintptr_t)reshade::api::device_api::d3d9) desc = (reshade::api::swapchain_desc *)b;   // newer ReShade
    else if (a < 0x100000) return false;                                                           // another API
    else desc = (reshade::api::swapchain_desc *)a;                                                  // older ReShade
    if (!desc || !desc->fullscreen_state) return false;
    desc->fullscreen_state = false;
    desc->fullscreen_refresh_rate = 0;
    g_dx9WantedFs = true;
    LOGI("BFI Presenter: DX9: the game asked for exclusive fullscreen -- kept windowed (9Ex needs it, BFI needs borderless)");
    return true;
}
// Diagnostics: a create_resource with no matching init_resource means the
// D3D9 Create* call failed. Log the description of every failed one.
static std::atomic<bool> g_t9Pending{false};
static reshade::api::resource_desc g_t9PendingDesc{};
static std::atomic<uint32_t> g_t9Creates{0}, g_t9Fails{0};
static void LogT9Pending(const char *when)
{
    if (!g_t9Pending.exchange(false)) return;
    const auto &d = g_t9PendingDesc;
    g_t9Fails++;
    LOGW("BFI Presenter: DX9 diag: resource creation FAILED (%s): type %d, %ux%u x%u, levels %u, format %d, heap %d, usage 0x%x, flags 0x%x",
         when, (int)d.type, d.texture.width, d.texture.height, (unsigned)d.texture.depth_or_layers, (unsigned)d.texture.levels,
         (int)d.texture.format, (int)d.heap, (unsigned)d.usage, (unsigned)d.flags);
}
static bool on_create_resource9(reshade::api::device *dev, reshade::api::resource_desc &desc, reshade::api::subresource_data *, reshade::api::resource_usage)
{
    if (!g_dx9Diag || !dev || dev->get_api() != reshade::api::device_api::d3d9) return false;
    LogT9Pending("next create");
    g_t9PendingDesc = desc;
    g_t9Pending.store(true);
    uint32_t n = ++g_t9Creates;
    if (n <= 150 || (n % 500) == 0)
        LOGI("BFI Presenter: DX9 diag: create #%u type %d, %ux%u, levels %u, format %d, heap %d, usage 0x%x, flags 0x%x",
             n, (int)desc.type, desc.texture.width, desc.texture.height, (unsigned)desc.texture.levels,
             (int)desc.texture.format, (int)desc.heap, (unsigned)desc.usage, (unsigned)desc.flags);
    return false;
}
static void on_init_resource9(reshade::api::device *dev, const reshade::api::resource_desc &, const reshade::api::subresource_data *,
                              reshade::api::resource_usage, reshade::api::resource)
{
    if (dev && dev->get_api() == reshade::api::device_api::d3d9) g_t9Pending.store(false);
}
static void Dx9DiagRemove();
static void Dx9UnhideEx();
static void Dx9UnhookPresent();
static void on_present9(reshade::api::command_queue *, reshade::api::swapchain *, const reshade::api::rect *,
                        const reshade::api::rect *, uint32_t, const reshade::api::rect *);
static void on_reshade_present_gl(reshade::api::effect_runtime *runtime);
static void RegisterDx9Events(bool on)
{
    // (also the OpenGL front end's frame event)
    if (on) reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present_gl);
    else    reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present_gl);
    if (on) reshade::register_event<reshade::addon_event::present>(on_present9);
    else    reshade::unregister_event<reshade::addon_event::present>(on_present9);
    if (!on)
    {
        Dx9UnhookPresent();
        Dx9DiagRemove();
        Dx9UnhideEx();
        LogT9Pending("at shutdown");
        if (g_dx9Diag)
            LOGI("BFI Presenter: DX9 diag: %u resource creations seen, %u failed", g_t9Creates.load(), g_t9Fails.load());
    }
    if (on) { reshade::register_event<reshade::addon_event::create_resource>(on_create_resource9);
              reshade::register_event<reshade::addon_event::init_resource>(on_init_resource9); }
    else    { reshade::unregister_event<reshade::addon_event::create_resource>(on_create_resource9);
              reshade::unregister_event<reshade::addon_event::init_resource>(on_init_resource9); }
    // create_device is event 96 in newer ReShade (API 17+); older ReShade
    // ignores event numbers it doesn't know.
    HMODULE rs = reshade::internal::get_reshade_module_handle();
    typedef void (*PFN_Ev)(reshade::addon_event, void *);
    PFN_Ev f = rs ? (PFN_Ev)(void *)GetProcAddress(rs, on ? "ReShadeRegisterEvent" : "ReShadeUnregisterEvent") : nullptr;
    if (f) f((reshade::addon_event)96, (void *)&on_create_device9);
    // create_swapchain moved from 7 to 97 (with the device_api argument) in
    // newer ReShade; 7 is now unused there, 97 is ignored by older ReShade.
    if (f) f(reshade::addon_event::create_swapchain, (void *)&on_create_swapchain9);
    if (f) f((reshade::addon_event)97, (void *)&on_create_swapchain9);
}

struct Dx9State
{
    reshade::api::swapchain *rsc = nullptr;
    IDirect3DDevice9    *dev = nullptr;           // the game's native device (9Ex)
    IDirect3DSwapChain9 *sc9 = nullptr;           // its implicit swap chain (native)
    IDirect3DTexture9   *tex = nullptr;           // shared render target on the game's device
    IDirect3DSurface9   *surf = nullptr;
    IDirect3DQuery9     *query = nullptr;
    ID3D11Device        *d11 = nullptr;           // the add-on's own D3D11 device
    ID3D11DeviceContext *c11 = nullptr;
    ID3D11Texture2D     *t11 = nullptr;           // the shared texture, opened there
    UINT w = 0, h = 0;
    bool ready = false;
};
static Dx9State g_t9;
static std::atomic<bool> g_dx9{false};
static void ReleaseDx9()
{
    g_t9.ready = false;
    if (g_dx9.exchange(false))
    {
        ReleaseDx11();
        if (g_presenterStuck.load())
        {
            // The presenter thread may still read the staging ring: leave every
            // resource alive (see StopPresenter).
            g_t9 = Dx9State();
            return;
        }
    }
    SafeRelease(g_t9.t11); SafeRelease(g_t9.c11); SafeRelease(g_t9.d11);
    SafeRelease(g_t9.query); SafeRelease(g_t9.surf); SafeRelease(g_t9.tex);
    g_t9.dev = nullptr; g_t9.sc9 = nullptr; g_t9.rsc = nullptr;
}
// ---- DX9 test diagnostics: which call inside the game's texture loading fails.
// Logs failing calls on the game's real (native) D3D9 device and texture
// objects, and every D3DXCreateTextureFromFileInMemoryEx call that fails.
static std::atomic<int> g_d9FailLogs{0};
BFI_TLS(int, t_inD3dx);
static const char *CallerModule(void *ret, char *buf, size_t n)
{
    HMODULE m = nullptr;
    buf[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m) && m)
    {
        char p[MAX_PATH];
        if (GetModuleFileNameA(m, p, MAX_PATH)) { const char *s = strrchr(p, '\\'); snprintf(buf, n, "%s", s ? s + 1 : p); }
    }
    return buf;
}
#define D9FAIL(hr, ...) do { if (FAILED(hr) && g_d9FailLogs.fetch_add(1) < 120) { char cm[64]; \
    LOGW("BFI Presenter: DX9 diag: %s failed 0x%08lX%s (caller %s): " , __func__, (unsigned long)(hr), t_inD3dx ? " inside D3DX" : "", \
         CallerModule(__builtin_return_address(0), cm, sizeof(cm))); LOGW(__VA_ARGS__); } } while (0)

typedef HRESULT (STDMETHODCALLTYPE *PFN9_CreateTex)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_CreateVol)(IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9 **, HANDLE *);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_CreateCube)(IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9 **, HANDLE *);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_CreateOff)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9 **, HANDLE *);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_UpdSurf)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_UpdTex)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_Stretch)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_RTData)(IDirect3DDevice9 *, IDirect3DSurface9 *, IDirect3DSurface9 *);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_TexLock)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_SurfLock)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_CheckFmt)(IDirect3D9 *, UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT);
static PFN9_CreateTex o9_CreateTex; static PFN9_CreateVol o9_CreateVol; static PFN9_CreateCube o9_CreateCube;
static PFN9_CreateOff o9_CreateOff; static PFN9_UpdSurf o9_UpdSurf; static PFN9_UpdTex o9_UpdTex;
static PFN9_Stretch o9_Stretch; static PFN9_RTData o9_RTData; static PFN9_TexLock o9_TexLock; static PFN9_SurfLock o9_SurfLock;
static PFN9_CheckFmt o9_CheckFmt;

static void SurfDesc(IDirect3DSurface9 *s, char *b, size_t n)
{
    D3DSURFACE_DESC d = {};
    if (s && SUCCEEDED(s->GetDesc(&d))) snprintf(b, n, "%ux%u fmt %d pool %d usage 0x%lx", d.Width, d.Height, (int)d.Format, (int)d.Pool, (unsigned long)d.Usage);
    else snprintf(b, n, "null");
}
static HRESULT STDMETHODCALLTYPE h9_CreateTex(IDirect3DDevice9 *d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9 **t, HANDLE *sh)
{ HRESULT hr = o9_CreateTex(d, w, h, l, u, f, p, t, sh);
  D9FAIL(hr, "BFI Presenter:   CreateTexture %ux%u levels %u usage 0x%lx format %d pool %d shared %d", w, h, l, (unsigned long)u, (int)f, (int)p, sh ? 1 : 0); return hr; }
static HRESULT STDMETHODCALLTYPE h9_CreateVol(IDirect3DDevice9 *d, UINT w, UINT h, UINT z, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9 **t, HANDLE *sh)
{ HRESULT hr = o9_CreateVol(d, w, h, z, l, u, f, p, t, sh);
  D9FAIL(hr, "BFI Presenter:   CreateVolumeTexture %ux%ux%u levels %u usage 0x%lx format %d pool %d", w, h, z, l, (unsigned long)u, (int)f, (int)p); return hr; }
static HRESULT STDMETHODCALLTYPE h9_CreateCube(IDirect3DDevice9 *d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9 **t, HANDLE *sh)
{ HRESULT hr = o9_CreateCube(d, e, l, u, f, p, t, sh);
  D9FAIL(hr, "BFI Presenter:   CreateCubeTexture %u levels %u usage 0x%lx format %d pool %d", e, l, (unsigned long)u, (int)f, (int)p); return hr; }
static HRESULT STDMETHODCALLTYPE h9_CreateOff(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9 **s, HANDLE *sh)
{ HRESULT hr = o9_CreateOff(d, w, h, f, p, s, sh);
  D9FAIL(hr, "BFI Presenter:   CreateOffscreenPlainSurface %ux%u format %d pool %d", w, h, (int)f, (int)p); return hr; }
static HRESULT STDMETHODCALLTYPE h9_UpdSurf(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const RECT *r, IDirect3DSurface9 *t, const POINT *pt)
{ HRESULT hr = o9_UpdSurf(d, s, r, t, pt);
  if (FAILED(hr)) { char a[96], b[96]; SurfDesc(s, a, sizeof(a)); SurfDesc(t, b, sizeof(b)); D9FAIL(hr, "BFI Presenter:   UpdateSurface src %s -> dst %s", a, b); } return hr; }
static HRESULT STDMETHODCALLTYPE h9_UpdTex(IDirect3DDevice9 *d, IDirect3DBaseTexture9 *s, IDirect3DBaseTexture9 *t)
{ HRESULT hr = o9_UpdTex(d, s, t);
  D9FAIL(hr, "BFI Presenter:   UpdateTexture src type %d -> dst type %d", s ? (int)s->GetType() : -1, t ? (int)t->GetType() : -1); return hr; }
static HRESULT STDMETHODCALLTYPE h9_Stretch(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *t, const RECT *tr, D3DTEXTUREFILTERTYPE f)
{ HRESULT hr = o9_Stretch(d, s, sr, t, tr, f);
  if (FAILED(hr)) { char a[96], b[96]; SurfDesc(s, a, sizeof(a)); SurfDesc(t, b, sizeof(b)); D9FAIL(hr, "BFI Presenter:   StretchRect src %s -> dst %s filter %d", a, b, (int)f); } return hr; }
static HRESULT STDMETHODCALLTYPE h9_RTData(IDirect3DDevice9 *d, IDirect3DSurface9 *s, IDirect3DSurface9 *t)
{ HRESULT hr = o9_RTData(d, s, t);
  if (FAILED(hr)) { char a[96], b[96]; SurfDesc(s, a, sizeof(a)); SurfDesc(t, b, sizeof(b)); D9FAIL(hr, "BFI Presenter:   GetRenderTargetData src %s -> dst %s", a, b); } return hr; }
static HRESULT STDMETHODCALLTYPE h9_TexLock(IDirect3DTexture9 *t, UINT lv, D3DLOCKED_RECT *lr, const RECT *r, DWORD fl)
{ HRESULT hr = o9_TexLock(t, lv, lr, r, fl);
  if (FAILED(hr)) { D3DSURFACE_DESC d = {}; t->GetLevelDesc(lv, &d);
    D9FAIL(hr, "BFI Presenter:   Texture LockRect level %u flags 0x%lx: %ux%u fmt %d pool %d usage 0x%lx", lv, (unsigned long)fl, d.Width, d.Height, (int)d.Format, (int)d.Pool, (unsigned long)d.Usage); } return hr; }
static HRESULT STDMETHODCALLTYPE h9_SurfLock(IDirect3DSurface9 *s, D3DLOCKED_RECT *lr, const RECT *r, DWORD fl)
{ HRESULT hr = o9_SurfLock(s, lr, r, fl);
  if (FAILED(hr)) { char a[96]; SurfDesc(s, a, sizeof(a)); D9FAIL(hr, "BFI Presenter:   Surface LockRect flags 0x%lx: %s", (unsigned long)fl, a); } return hr; }
static HRESULT STDMETHODCALLTYPE h9_CheckFmt(IDirect3D9 *d, UINT a, D3DDEVTYPE dt, D3DFORMAT af, DWORD u, D3DRESOURCETYPE rt, D3DFORMAT f)
{ HRESULT hr = o9_CheckFmt(d, a, dt, af, u, rt, f);
  if (t_inD3dx) D9FAIL(hr, "BFI Presenter:   CheckDeviceFormat adapter fmt %d usage 0x%lx rtype %d format %d", (int)af, (unsigned long)u, (int)rt, (int)f); return hr; }

#ifndef _WIN64
static BYTE *g_d3dxFn = nullptr; static BYTE g_d3dxOrig[16]; static size_t g_d3dxLen = 0;
typedef HRESULT (WINAPI *PFN_D3DXTexMemEx)(IDirect3DDevice9 *, LPCVOID, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD,
                                           D3DCOLOR, void *, void *, IDirect3DTexture9 **);
static PFN_D3DXTexMemEx o_D3DXTexMemEx;
static HRESULT WINAPI h_D3DXTexMemEx(IDirect3DDevice9 *dev, LPCVOID src, UINT size, UINT w, UINT h, UINT mips, DWORD usage, D3DFORMAT fmt,
                                     D3DPOOL pool, DWORD filter, DWORD mipf, D3DCOLOR ck, void *info, void *pal, IDirect3DTexture9 **out)
{
    t_inD3dx = t_inD3dx + 1;
    HRESULT hr = o_D3DXTexMemEx(dev, src, size, w, h, mips, usage, fmt, pool, filter, mipf, ck, info, pal, out);
    t_inD3dx = t_inD3dx - 1;
    if (FAILED(hr))
    {
        char hdr[16] = {};
        if (src && size >= 4) memcpy(hdr, src, 4);
        for (int i = 0; i < 4; i++) if (hdr[i] < 32 || hdr[i] > 126) hdr[i] = '.';
        LOGW("BFI Presenter: DX9 diag: D3DXCreateTextureFromFileInMemoryEx failed 0x%08lX: %u bytes (starts \"%s\"), %ux%u mips %u usage 0x%lx format %d pool %d filter 0x%lx mipfilter 0x%lx",
             (unsigned long)hr, size, hdr, w, h, mips, (unsigned long)usage, (int)fmt, (int)pool, (unsigned long)filter, (unsigned long)mipf);
    }
    return hr;
}
// Minimal x86 inline detour: whole instructions covering 5 bytes go to a
// trampoline, a jmp to the hook goes over them.
static void *Detour32(void *target, void *hook)
{
    BYTE *code = (BYTE *)target;
    if (!code || code[0] == 0xE9 || code[0] == 0xEB) return nullptr;      // already hooked or a stub
    size_t len = 0;
    while (len < 5)
    {
        int rd;
        const int l = InsnLength(code + len, &rd);
        if (l <= 0) return nullptr;
        len += (size_t)l;
    }
    if (len > sizeof(g_d3dxOrig)) return nullptr;
    BYTE *t = (BYTE *)VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, code, len);
    memcpy(g_d3dxOrig, code, len); g_d3dxFn = code; g_d3dxLen = len;
    t[len] = 0xE9;
    int32_t rel = (int32_t)((intptr_t)(code + len) - (intptr_t)(t + len + 5));
    memcpy(t + len + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, len + 5);
    DWORD old;
    VirtualProtect(code, len, PAGE_EXECUTE_READWRITE, &old);
    code[0] = 0xE9;
    rel = (int32_t)((intptr_t)hook - (intptr_t)(code + 5));
    memcpy(code + 1, &rel, 4);
    for (size_t i = 5; i < len; i++) code[i] = 0x90;
    VirtualProtect(code, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, len);
    return t;
}
#endif

static void **g_d9vtDev = nullptr, **g_d9vtTex = nullptr, **g_d9vtSurf = nullptr, **g_d9vtD3d = nullptr;
static bool g_d9diagDone = false;
static void Dx9DiagRemove()
{
    if (!g_d9diagDone) return;
    g_d9diagDone = false;
    if (void **vt = g_d9vtDev)
    {
        UnpatchSlot(vt, 23, (void *)o9_CreateTex,  (void *)&h9_CreateTex);
        UnpatchSlot(vt, 24, (void *)o9_CreateVol,  (void *)&h9_CreateVol);
        UnpatchSlot(vt, 25, (void *)o9_CreateCube, (void *)&h9_CreateCube);
        UnpatchSlot(vt, 30, (void *)o9_UpdSurf,    (void *)&h9_UpdSurf);
        UnpatchSlot(vt, 31, (void *)o9_UpdTex,     (void *)&h9_UpdTex);
        UnpatchSlot(vt, 32, (void *)o9_RTData,     (void *)&h9_RTData);
        UnpatchSlot(vt, 34, (void *)o9_Stretch,    (void *)&h9_Stretch);
        UnpatchSlot(vt, 36, (void *)o9_CreateOff,  (void *)&h9_CreateOff);
    }
    if (g_d9vtTex)  UnpatchSlot(g_d9vtTex, 19, (void *)o9_TexLock, (void *)&h9_TexLock);
    if (g_d9vtSurf) UnpatchSlot(g_d9vtSurf, 13, (void *)o9_SurfLock, (void *)&h9_SurfLock);
    if (g_d9vtD3d)  UnpatchSlot(g_d9vtD3d, 10, (void *)o9_CheckFmt, (void *)&h9_CheckFmt);
    g_d9vtDev = g_d9vtTex = g_d9vtSurf = g_d9vtD3d = nullptr;
#ifndef _WIN64
    if (g_d3dxFn && g_d3dxLen)
    {
        DWORD old;
        VirtualProtect(g_d3dxFn, g_d3dxLen, PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_d3dxFn, g_d3dxOrig, g_d3dxLen);
        VirtualProtect(g_d3dxFn, g_d3dxLen, old, &old);
        FlushInstructionCache(GetCurrentProcess(), g_d3dxFn, g_d3dxLen);
        // The trampoline stays allocated: a call may still be inside it.
    }
    g_d3dxFn = nullptr; g_d3dxLen = 0; o_D3DXTexMemEx = nullptr;
#endif
}
static void Dx9DiagInstall(IDirect3DDevice9 *dev)
{
    if (g_d9diagDone || !dev) return;
    g_d9diagDone = true;
    void **vt = *(void ***)dev;
    g_d9vtDev = vt;
    PatchSlot(vt, 23, (void *)&h9_CreateTex,  (void **)&o9_CreateTex);
    PatchSlot(vt, 24, (void *)&h9_CreateVol,  (void **)&o9_CreateVol);
    PatchSlot(vt, 25, (void *)&h9_CreateCube, (void **)&o9_CreateCube);
    PatchSlot(vt, 30, (void *)&h9_UpdSurf,    (void **)&o9_UpdSurf);
    PatchSlot(vt, 31, (void *)&h9_UpdTex,     (void **)&o9_UpdTex);
    PatchSlot(vt, 32, (void *)&h9_RTData,     (void **)&o9_RTData);
    PatchSlot(vt, 34, (void *)&h9_Stretch,    (void **)&o9_Stretch);
    PatchSlot(vt, 36, (void *)&h9_CreateOff,  (void **)&o9_CreateOff);
    IDirect3DTexture9 *tx = nullptr;
    if (SUCCEEDED(o9_CreateTex(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &tx, nullptr)) && tx)
    {
        g_d9vtTex = *(void ***)tx;
        PatchSlot(g_d9vtTex, 19, (void *)&h9_TexLock, (void **)&o9_TexLock);
        IDirect3DSurface9 *s = nullptr;
        if (SUCCEEDED(tx->GetSurfaceLevel(0, &s)) && s) { g_d9vtSurf = *(void ***)s; PatchSlot(g_d9vtSurf, 13, (void *)&h9_SurfLock, (void **)&o9_SurfLock); s->Release(); }
        tx->Release();
    }
    IDirect3D9 *d3d = nullptr;
    if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d) { g_d9vtD3d = *(void ***)d3d; PatchSlot(g_d9vtD3d, 10, (void *)&h9_CheckFmt, (void **)&o9_CheckFmt); d3d->Release(); }
    int dx = 0;
#ifndef _WIN64
    for (int v = 43; v >= 24 && !o_D3DXTexMemEx; v--)
    {
        char name[32];
        snprintf(name, sizeof(name), "d3dx9_%d.dll", v);
        HMODULE m = GetModuleHandleA(name);
        if (!m) continue;
        void *fn = (void *)GetProcAddress(m, "D3DXCreateTextureFromFileInMemoryEx");
        o_D3DXTexMemEx = (PFN_D3DXTexMemEx)Detour32(fn, (void *)&h_D3DXTexMemEx);
        LOGI("BFI Presenter: DX9 diag: %s found, D3DXCreateTextureFromFileInMemoryEx %s", name, o_D3DXTexMemEx ? "hooked" : "NOT hooked");
        dx = v;
    }
#endif
    LOGI("BFI Presenter: DX9 diag: watching the game's D3D9 calls for failures (d3dx9_%d)", dx);
}

// D3DX (d3dx9_NN.dll) refuses D3DPOOL_MANAGED once it sees an
// IDirect3DDevice9Ex (Alan Wake: D3DXCreateTextureFromFileInMemoryEx ->
// D3DERR_INVALIDCALL, pool 1). ReShade already turns MANAGED into
// MANAGED_EX for the runtime, so D3DX only needs to not see Ex: deny the Ex
// interfaces to callers inside a d3dx9 DLL.
typedef HRESULT (STDMETHODCALLTYPE *PFN_QI)(IUnknown *, REFIID, void **);
static PFN_QI o_ProxyDevQI = nullptr, o_NativeD3dQI = nullptr;
static void **g_proxyDevVt = nullptr, **g_nativeD3dVt = nullptr;
static std::atomic<int> g_hideExLogs{0};
static bool CallerIsD3dx(void *ret)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m) || !m) return false;
    char p[MAX_PATH];
    if (!GetModuleFileNameA(m, p, MAX_PATH)) return false;
    const char *s = strrchr(p, '\\');
    s = s ? s + 1 : p;
    return _strnicmp(s, "d3dx9_", 6) == 0;
}
static HRESULT STDMETHODCALLTYPE h_ProxyDevQI(IUnknown *self, REFIID riid, void **pp)
{
    if (IsEqualIID(riid, IID_IDirect3DDevice9Ex) && CallerIsD3dx(__builtin_return_address(0)))
    {
        if (g_hideExLogs.fetch_add(1) == 0) LOGI("BFI Presenter: DX9: hid Direct3D 9Ex from D3DX so its D3DPOOL_MANAGED loads keep working");
        if (pp) *pp = nullptr;
        return E_NOINTERFACE;
    }
    return o_ProxyDevQI(self, riid, pp);
}
static HRESULT STDMETHODCALLTYPE h_NativeD3dQI(IUnknown *self, REFIID riid, void **pp)
{
    if (IsEqualIID(riid, IID_IDirect3D9Ex) && CallerIsD3dx(__builtin_return_address(0)))
    {
        if (g_hideExLogs.fetch_add(1) == 0) LOGI("BFI Presenter: DX9: hid Direct3D 9Ex from D3DX so its D3DPOOL_MANAGED loads keep working");
        if (pp) *pp = nullptr;
        return E_NOINTERFACE;
    }
    return o_NativeD3dQI(self, riid, pp);
}
static void Dx9HideExFromD3dx(reshade::api::device *apiDev, IDirect3DDevice9 *native)
{
    HMODULE rs = reshade::internal::get_reshade_module_handle();
    // ReShade's proxy is "class Direct3DDevice9 : public IDirect3DDevice9Ex,
    // public reshade::d3d9::device_impl": the api::device sits one vtable
    // pointer after the COM interface the game holds. Check before using it.
    IUnknown *proxy = reinterpret_cast<IUnknown *>(reinterpret_cast<char *>(apiDev) - sizeof(void *));
    bool ok = false;
    if (rs && apiDev)
    {
        HMODULE vm = nullptr;
        void **vt = *reinterpret_cast<void ***>(proxy);
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[0], &vm) && vm == rs)
        {
            IUnknown *same = nullptr;
            if (SUCCEEDED(proxy->QueryInterface(IID_IDirect3DDevice9, (void **)&same)) && same)
            {
                ok = (same == proxy);
                same->Release();
            }
            if (ok) { g_proxyDevVt = vt; PatchSlot(vt, 0, (void *)&h_ProxyDevQI, (void **)&o_ProxyDevQI); }
        }
    }
    IDirect3D9 *d3d = nullptr;
    if (native && SUCCEEDED(native->GetDirect3D(&d3d)) && d3d)
    {
        g_nativeD3dVt = *reinterpret_cast<void ***>(d3d);
        PatchSlot(g_nativeD3dVt, 0, (void *)&h_NativeD3dQI, (void **)&o_NativeD3dQI);
        d3d->Release();
    }
    LOGI("BFI Presenter: DX9: D3DX will see a plain Direct3D 9 device (%s)", ok ? "ReShade device proxy found" : "proxy NOT found -- D3DX may still fail");
}
static void Dx9UnhideEx()
{
    if (g_proxyDevVt)  UnpatchSlot(g_proxyDevVt, 0, (void *)o_ProxyDevQI, (void *)&h_ProxyDevQI);
    if (g_nativeD3dVt) UnpatchSlot(g_nativeD3dVt, 0, (void *)o_NativeD3dQI, (void *)&h_NativeD3dQI);
    g_proxyDevVt = g_nativeD3dVt = nullptr;
}

// ---- DX9 front end: the game's Present (native device / swap chain).
typedef HRESULT (STDMETHODCALLTYPE *PFN9_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_PresentEx)(IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN9_ScPresent)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
static PFN9_ScPresent o9_ScPresent = nullptr;
static void **g_9vtSc = nullptr;
// The runtime switches the device object between several vtables after it is
// created (Alan Wake: the slot hooked at init was no longer the one in use a
// few frames later). Every vtable seen gets hooked, each with its own
// originals; ReShade's present event re-checks before each Present.
struct Dx9DevVt { void **vt; PFN9_Present present; PFN9_PresentEx presentEx; };
static Dx9DevVt g_9devVts[8];
static int g_9devVtN = 0;
static CRITICAL_SECTION g_9vtLock;
static bool g_9vtLockInit = false;
static const Dx9DevVt *Dx9FindVt(void **vt)
{
    for (int i = 0; i < g_9devVtN; i++) if (g_9devVts[i].vt == vt) return &g_9devVts[i];
    return g_9devVtN ? &g_9devVts[0] : nullptr;
}
BFI_TLS(int, t_in9Present);                 // the runtime's Present may go through another of the hooked ones
static std::atomic<bool> g_9firstLogged{false};

// One DX9 game frame: copy the back buffer (ReShade's effects already
// applied) into the shared texture, wait until the game's GPU has written it,
// and hand it to the DX11 core, which copies it into the staging ring on the
// add-on's own D3D11 device. The game's own Present is skipped.
template <class ShowOwn>
static HRESULT SubmitGameFrame9(ShowOwn showOwn)
{
    struct Depth { Depth() { t_in9Present++; } ~Depth() { t_in9Present--; } } depth;
    return SubmitFrame11Core(
        [&]() -> ID3D11Texture2D * {
            // The previous frame's D3D11 copy must be done reading the shared
            // texture before the game's device writes it again.
            if (g_gameFenceVal && g_gameFence->GetCompletedValue() < g_gameFenceVal)
                WaitFence(g_gameFence, g_gameFenceVal, g_gameFenceEvent);
            IDirect3DSurface9 *bb = nullptr;
            if (FAILED(g_t9.dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return nullptr;
            const HRESULT hr = g_t9.dev->StretchRect(bb, nullptr, g_t9.surf, nullptr, D3DTEXF_NONE);
            bb->Release();
            if (FAILED(hr)) return nullptr;
            // Direct3D 9 has no shared fences: wait on the CPU for the copy.
            if (g_t9.query)
            {
                g_t9.query->Issue(D3DISSUE_END);
                const double t0 = Now();
                int spins = 0;
                while (g_t9.query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
                {
                    const double el = Now() - t0;
                    if (el > 0.25) { LOGW("BFI Presenter: DX9: the game's frame took over 250 ms on the GPU -- not waited for"); break; }
                    if (el < 0.002 || (++spins & 15)) YieldProcessor(); else SwitchToThread();
                }
            }
            if (!g_9firstLogged.exchange(true)) LOGI("BFI Presenter: DX9: first game frame copied");
            g_t9.t11->AddRef();
            return g_t9.t11;
        },
        showOwn);
}
static std::atomic<uint32_t> g_9hits{0}, g_9owned{0}, g_9rsPresents{0};
static void Dx9LogHit(const char *which, void *obj, void *expect)
{
    if (g_9hits.fetch_add(1) == 0)
        LOGI("BFI Presenter: DX9: first %s reached the add-on (object %s, dx9 %d, active %d, ready %d, nested %d)", which,
             obj == expect ? "matches" : "does NOT match", (int)g_dx9.load(), (int)g_active.load(), (int)g_t9.ready, (int)t_in9Present);
}
static bool Dx9OwnsDevice(void *dev) { return !t_in9Present && g_dx9.load() && g_active.load() && g_t9.ready && dev == (void *)g_t9.dev; }
static bool Dx9OwnsSc(void *sc)      { return !t_in9Present && g_dx9.load() && g_active.load() && g_t9.ready && sc == (void *)g_t9.sc9; }
static HRESULT STDMETHODCALLTYPE h9_Present(IDirect3DDevice9 *d, const RECT *sr, const RECT *dr, HWND w, const RGNDATA *rg)
{
    const PFN9_Present o9_Present = Dx9FindVt(*(void ***)d)->present;
    Dx9LogHit("device Present", d, g_t9.dev);
    if (!Dx9OwnsDevice(d)) return o9_Present(d, sr, dr, w, rg);
    g_9owned++;
    return SubmitGameFrame9([&]() -> HRESULT { t_in9Present++; HRESULT hr = o9_Present(d, sr, dr, w, rg); t_in9Present--; return hr; });
}
static HRESULT STDMETHODCALLTYPE h9_PresentEx(IDirect3DDevice9Ex *d, const RECT *sr, const RECT *dr, HWND w, const RGNDATA *rg, DWORD fl)
{
    const PFN9_PresentEx o9_PresentEx = Dx9FindVt(*(void ***)d)->presentEx;
    Dx9LogHit("device PresentEx", d, g_t9.dev);
    if (!Dx9OwnsDevice(d)) return o9_PresentEx(d, sr, dr, w, rg, fl);
    g_9owned++;
    return SubmitGameFrame9([&]() -> HRESULT { t_in9Present++; HRESULT hr = o9_PresentEx(d, sr, dr, w, rg, fl); t_in9Present--; return hr; });
}
static HRESULT STDMETHODCALLTYPE h9_ScPresent(IDirect3DSwapChain9 *sc, const RECT *sr, const RECT *dr, HWND w, const RGNDATA *rg, DWORD fl)
{
    Dx9LogHit("swap chain Present", sc, g_t9.sc9);
    if (!Dx9OwnsSc(sc)) return o9_ScPresent(sc, sr, dr, w, rg, fl);
    g_9owned++;
    return SubmitGameFrame9([&]() -> HRESULT { t_in9Present++; HRESULT hr = o9_ScPresent(sc, sr, dr, w, rg, fl); t_in9Present--; return hr; });
}
static void Dx9HookDeviceVt(IDirect3DDevice9 *dev)
{
    void **vt = *(void ***)dev;
    if (vt[17] == (void *)&h9_Present && vt[121] == (void *)&h9_PresentEx) return;
    if (!g_9vtLockInit) { InitializeCriticalSection(&g_9vtLock); g_9vtLockInit = true; }
    EnterCriticalSection(&g_9vtLock);
    Dx9DevVt *e = nullptr;
    for (int i = 0; i < g_9devVtN; i++) if (g_9devVts[i].vt == vt) e = &g_9devVts[i];
    if (!e && g_9devVtN < 8)
    {
        e = &g_9devVts[g_9devVtN];
        e->vt = vt; e->present = nullptr; e->presentEx = nullptr;
        char m[64];
        ModuleOf(vt[17], m, sizeof(m));
        LOGI("BFI Presenter: DX9: hooking device vtable %u (Present in %s)", (unsigned)g_9devVtN + 1, m);
    }
    if (e)
    {
        DWORD old;
        if (vt[17] != (void *)&h9_Present)
        {
            VirtualProtect(&vt[17], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
            e->present = (PFN9_Present)vt[17];
            vt[17] = (void *)&h9_Present;
            VirtualProtect(&vt[17], sizeof(void *), old, &old);
        }
        if (vt[121] != (void *)&h9_PresentEx)
        {
            VirtualProtect(&vt[121], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
            e->presentEx = (PFN9_PresentEx)vt[121];
            vt[121] = (void *)&h9_PresentEx;
            VirtualProtect(&vt[121], sizeof(void *), old, &old);
        }
        if (e == &g_9devVts[g_9devVtN]) g_9devVtN++;
    }
    LeaveCriticalSection(&g_9vtLock);
}
static void Dx9HookPresent(IDirect3DDevice9 *dev, IDirect3DSwapChain9 *sc9)
{
    Dx9HookDeviceVt(dev);
    void **vs = *(void ***)sc9;
    if (g_9vtSc != vs)
    {
        g_9vtSc = vs;
        PatchSlot(vs, 3, (void *)&h9_ScPresent, (void **)&o9_ScPresent);
    }
}
static void Dx9UnhookPresent()
{
    for (int i = 0; i < g_9devVtN; i++)
    {
        Dx9DevVt &e = g_9devVts[i];
        if (e.present)   UnpatchSlot(e.vt, 17,  (void *)e.present,   (void *)&h9_Present);
        if (e.presentEx) UnpatchSlot(e.vt, 121, (void *)e.presentEx, (void *)&h9_PresentEx);
    }
    g_9devVtN = 0;
    if (g_9vtSc) UnpatchSlot(g_9vtSc, 3, (void *)o9_ScPresent, (void *)&h9_ScPresent);
    g_9vtSc = nullptr;
}

// ReShade's present event: a check that the game's Present reaches the
// add-on's hooks (logged every 5 seconds while they don't).
static void on_present9(reshade::api::command_queue *, reshade::api::swapchain *swapchain, const reshade::api::rect *,
                        const reshade::api::rect *, uint32_t, const reshade::api::rect *)
{
    if (!g_dx9.load() || swapchain != g_t9.rsc) return;
    Dx9HookDeviceVt(g_t9.dev);                  // the runtime may have switched the device's vtable
    static double last = 0.0;
    static uint32_t lastOwned = 0;
    g_9rsPresents++;
    const double now = Now();
    if (last == 0.0) { last = now; return; }
    if (now - last < 5.0) return;
    last = now;
    const uint32_t owned = g_9owned.load();
    if (owned != lastOwned) { lastOwned = owned; return; }
    char m17[64] = "", m3[64] = "";
    void **vt = *(void ***)g_t9.dev, **vs = *(void ***)g_t9.sc9;
    ModuleOf(vt[17], m17, sizeof(m17));
    ModuleOf(vs[3], m3, sizeof(m3));
    LOGW("BFI Presenter: DX9: %u Presents seen by ReShade, %u reached the add-on, %u taken over; device Present slot %s (%s), "
         "swap chain Present slot %s (%s)", g_9rsPresents.load(), g_9hits.load(), owned,
         vt[17] == (void *)&h9_Present ? "is ours" : "is NOT ours", m17, vs[3] == (void *)&h9_ScPresent ? "is ours" : "is NOT ours", m3);
}
static void InitDx9(reshade::api::swapchain *swapchain)
{
    ReleaseDx9();
    IDirect3DDevice9 *dev = reinterpret_cast<IDirect3DDevice9 *>(swapchain->get_device()->get_native());
    IDirect3DSwapChain9 *sc9 = reinterpret_cast<IDirect3DSwapChain9 *>(swapchain->get_native());
    if (!dev || !sc9) return;
    if (GetPrivateProfileIntA("BFI", "DX9Diag", 0, g_iniPath)) Dx9DiagInstall(dev);
    IDirect3DDevice9Ex *ex = nullptr;
    const bool isEx = SUCCEEDED(dev->QueryInterface(IID_IDirect3DDevice9Ex, (void **)&ex)) && ex;
    if (ex) ex->Release();
    D3DPRESENT_PARAMETERS pp = {};
    sc9->GetPresentParameters(&pp);
    LOGI("BFI Presenter: DX9 init 1/5 -- device %s, back buffer %ux%u format %u, %s, swap effect %u, interval 0x%X",
         isEx ? "is Direct3D 9Ex" : "is plain Direct3D 9 (NOT 9Ex)", pp.BackBufferWidth, pp.BackBufferHeight, (unsigned)pp.BackBufferFormat,
         pp.Windowed ? "windowed" : "exclusive fullscreen", (unsigned)pp.SwapEffect, (unsigned)pp.PresentationInterval);
    if (!isEx)
    {
        LOGW("BFI Presenter: DX9: frames can't be shared from a plain Direct3D 9 device. ReShade 6.8 or newer is needed "
             "(it creates the game's device as Direct3D 9Ex for the add-on).");
        SetReason(0, "Not managed (DX9): the game's device is not Direct3D 9Ex -- update ReShade to 6.8 or newer.");
        return;
    }
    if (!g_proxyDevVt) Dx9HideExFromD3dx(swapchain->get_device(), dev);
    HWND hwnd = (HWND)swapchain->get_hwnd();
    if (!hwnd) hwnd = pp.hDeviceWindow;
    if (!hwnd) { SetReason(0, "Not managed (DX9): the swap chain has no window."); return; }
    // Shared render target on the game's device.
    HANDLE share = nullptr;
    HRESULT hr = dev->CreateTexture(pp.BackBufferWidth, pp.BackBufferHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &g_t9.tex, &share);
    if (FAILED(hr) || !share)
    {
        LOGW("BFI Presenter: DX9: shared texture not created (0x%08lX)", (unsigned long)hr);
        SetReason(0, "Not managed (DX9): the shared texture could not be created (see ReShade.log).");
        ReleaseDx9();
        return;
    }
    g_t9.tex->GetSurfaceLevel(0, &g_t9.surf);
    dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_t9.query);
    // The add-on's own D3D11 device on the same adapter, past ReShade's hooks.
    LUID luid = {};
    {
        IDirect3D9 *d3d = nullptr;
        IDirect3D9Ex *d3dex = nullptr;
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        dev->GetCreationParameters(&cp);
        if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d)
        {
            if (SUCCEEDED(d3d->QueryInterface(IID_IDirect3D9Ex, (void **)&d3dex)) && d3dex) { d3dex->GetAdapterLUID(cp.AdapterOrdinal, &luid); d3dex->Release(); }
            d3d->Release();
        }
    }
    typedef HRESULT (WINAPI *PFN_CF1)(REFIID, void **);
    typedef HRESULT (WINAPI *PFN_D3D11CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                            const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
                                            ID3D11DeviceContext **);
    static PFN_CF1 cf1 = nullptr;
    static PFN_D3D11CDSC cd = nullptr;
    if (!cf1) { HMODULE m = LoadSystemDll("dxgi.dll"); if (m) cf1 = (PFN_CF1)RawEntry((void *)GetProcAddress(m, "CreateDXGIFactory1"), "CreateDXGIFactory1"); }
    // D3D11CreateDevice goes through the D3D11CreateDeviceAndSwapChain export,
    // which ReShade hooks: call that one past its hook, so ReShade doesn't
    // wrap the add-on's device.
    if (!cd) { HMODULE m = LoadSystemDll("d3d11.dll"); if (m) cd = (PFN_D3D11CDSC)RawEntry((void *)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain"), "D3D11CreateDeviceAndSwapChain"); }
    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *ad = nullptr;
    if (cf1 && SUCCEEDED(cf1(__uuidof(IDXGIFactory1), (void **)&fac)) && fac)
    {
        IDXGIAdapter1 *a = nullptr;
        for (UINT i = 0; !ad && fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++)
        {
            DXGI_ADAPTER_DESC1 d = {};
            a->GetDesc1(&d);
            if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) ad = a; else a->Release();
        }
        fac->Release();
    }
    if (!ad) LOGW("BFI Presenter: DX9: the game's adapter was not found among DXGI's; using the default one");
    {
        OwnCall own;
        hr = cd ? cd(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                     nullptr, nullptr, &g_t9.d11, nullptr, &g_t9.c11) : E_FAIL;
    }
    if (ad) ad->Release();
    if (FAILED(hr))
    {
        LOGW("BFI Presenter: DX9: D3D11 device not created (0x%08lX)", (unsigned long)hr);
        SetReason(0, "Not managed (DX9): the add-on's D3D11 device could not be created.");
        ReleaseDx9();
        return;
    }
    hr = g_t9.d11->OpenSharedResource(share, __uuidof(ID3D11Texture2D), (void **)&g_t9.t11);
    if (FAILED(hr) || !g_t9.t11)
    {
        LOGW("BFI Presenter: DX9: D3D11 could not open the shared texture (0x%08lX)", (unsigned long)hr);
        SetReason(0, "Not managed (DX9): the game's frames could not be shared (see ReShade.log).");
        ReleaseDx9();
        return;
    }
    D3D11_TEXTURE2D_DESC td = {};
    g_t9.t11->GetDesc(&td);
    LOGI("BFI Presenter: DX9 init 2/5 -- game frames shared with the add-on's D3D11 device (%ux%u, DXGI format %u)",
         td.Width, td.Height, (unsigned)td.Format);
    // The presenter, as for DX11 games, fed from the add-on's D3D11 device.
    ID3D11Device3 *dev3 = nullptr;
    ID3D11DeviceContext3 *ctx3 = nullptr;
    bool ok = SUCCEEDED(g_t9.d11->QueryInterface(IID_PPV_ARGS(&g_dev11)));
    if (ok && SUCCEEDED(g_t9.d11->QueryInterface(IID_PPV_ARGS(&dev3))))
    {
        dev3->GetImmediateContext3(&ctx3);
        if (ctx3) ctx3->QueryInterface(IID_PPV_ARGS(&g_ctx11));
    }
    SafeRelease(ctx3);
    SafeRelease(dev3);
    if (!ok || !g_ctx11)
    {
        SafeRelease(g_ctx11);
        SafeRelease(g_dev11);
        SetReason(0, "Not managed (DX9): no shared-fence support (needs Windows 10 1703 or later).");
        ReleaseDx9();
        return;
    }
    g_t9.dev = dev;
    g_t9.sc9 = sc9;
    g_t9.rsc = swapchain;
    g_t9.w = pp.BackBufferWidth; g_t9.h = pp.BackBufferHeight;
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = td.Width;
    d.Height = td.Height;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferCount = pp.BackBufferCount ? pp.BackBufferCount : 1;
    g_9firstLogged.store(false);
    if (!InitPresenter11(swapchain, nullptr, d, hwnd, DXGI_FORMAT_B8G8R8A8_UNORM, luid, "DX9"))
    {
        SafeRelease(g_ctx11);
        SafeRelease(g_dev11);
        g_dx11.store(false);
        ReleaseDx9();
        return;
    }
    if (g_dx9WantedFs) MakeBorderless(hwnd, nullptr);
    g_dx9.store(true);
    g_t9.ready = true;
    Dx9HookPresent(dev, sc9);
}

// ---------------------------------------------------------------- OpenGL front end
// The game's frames (ReShade's effects and overlay applied) are blitted, at
// ReShade's reshade_present event, into a Direct3D 11 texture the game's GL
// context sees through WGL_NV_DX_interop2; from there they take the DX11
// path to the presenter. The game's own SwapBuffers still runs (under the
// presenter's window, swap interval 0), so nothing below ReShade is hooked.
#define BFI_GL_READ_FRAMEBUFFER          0x8CA8
#define BFI_GL_DRAW_FRAMEBUFFER          0x8CA9
#define BFI_GL_READ_FRAMEBUFFER_BINDING  0x8CAA
#define BFI_GL_DRAW_FRAMEBUFFER_BINDING  0x8CA6
#define BFI_GL_COLOR_ATTACHMENT0         0x8CE0
#define BFI_GL_FRAMEBUFFER_COMPLETE      0x8CD5
#define BFI_GL_FRAMEBUFFER_SRGB          0x8DB9
#define BFI_GL_SCISSOR_TEST              0x0C11
#define BFI_GL_RASTERIZER_DISCARD        0x8C89
#define BFI_GL_COLOR_BUFFER_BIT          0x00004000
#define BFI_GL_NEAREST                   0x2600
#define BFI_GL_BACK                      0x0405
#define BFI_GL_READ_BUFFER               0x0C02
#define BFI_GL_TEXTURE_2D                0x0DE1
#define BFI_WGL_ACCESS_WRITE_DISCARD_NV  0x0002
typedef unsigned int GLuintB; typedef int GLintB; typedef unsigned int GLenumB; typedef unsigned char GLbooleanB;
struct GlFns
{
    PROC   (WINAPI *GetProcAddress)(LPCSTR) = nullptr;
    HGLRC  (WINAPI *GetCurrentContext)() = nullptr;
    void   (WINAPI *GetIntegerv)(GLenumB, GLintB *) = nullptr;
    GLbooleanB (WINAPI *IsEnabled)(GLenumB) = nullptr;
    void   (WINAPI *Enable)(GLenumB) = nullptr;
    void   (WINAPI *Disable)(GLenumB) = nullptr;
    void   (WINAPI *GenTextures)(int, GLuintB *) = nullptr;
    void   (WINAPI *DeleteTextures)(int, const GLuintB *) = nullptr;
    void   (WINAPI *ReadBuffer)(GLenumB) = nullptr;
    GLenumB (WINAPI *GetError)() = nullptr;
    void   (APIENTRY *GenFramebuffers)(int, GLuintB *) = nullptr;
    void   (APIENTRY *DeleteFramebuffers)(int, const GLuintB *) = nullptr;
    void   (APIENTRY *BindFramebuffer)(GLenumB, GLuintB) = nullptr;
    void   (APIENTRY *FramebufferTexture2D)(GLenumB, GLenumB, GLenumB, GLuintB, GLintB) = nullptr;
    GLenumB (APIENTRY *CheckFramebufferStatus)(GLenumB) = nullptr;
    void   (APIENTRY *BlitFramebuffer)(GLintB, GLintB, GLintB, GLintB, GLintB, GLintB, GLintB, GLintB, unsigned, GLenumB) = nullptr;
    HANDLE (WINAPI *DXOpenDeviceNV)(void *) = nullptr;
    BOOL   (WINAPI *DXCloseDeviceNV)(HANDLE) = nullptr;
    HANDLE (WINAPI *DXRegisterObjectNV)(HANDLE, void *, GLuintB, GLenumB, GLenumB) = nullptr;
    BOOL   (WINAPI *DXUnregisterObjectNV)(HANDLE, HANDLE) = nullptr;
    BOOL   (WINAPI *DXLockObjectsNV)(HANDLE, GLintB, HANDLE *) = nullptr;
    BOOL   (WINAPI *DXUnlockObjectsNV)(HANDLE, GLintB, HANDLE *) = nullptr;
    BOOL   (WINAPI *SwapIntervalEXT)(int) = nullptr;
    int    (WINAPI *GetSwapIntervalEXT)() = nullptr;
};
struct GlState
{
    reshade::api::effect_runtime *rt = nullptr;   // ReShade's runtime for the game's window
    HWND   hwnd = nullptr;
    HGLRC  ctx = nullptr;                          // the game's context (GL objects below belong to it)
    ID3D11Device        *d11 = nullptr;
    ID3D11DeviceContext *c11 = nullptr;
    ID3D11Texture2D     *tex = nullptr;            // interop texture, the frame for the presenter
    HANDLE glDev = nullptr, glObj = nullptr;
    GLuintB glTex = 0, fbo = 0;
    UINT   w = 0, h = 0;
    bool   presenter = false;                      // InitPresenter11 succeeded
    bool   glReady = false, glFailed = false;
    bool   swapLogged = false;
};
static GlState g_gl;
static GlFns   g_glf;
static std::atomic<bool> g_glActive{false};

static bool LoadGlFns()
{
    if (g_glf.GetProcAddress) return true;
    HMODULE m = GetModuleHandleA("opengl32.dll");
    if (!m) return false;
    auto core = [&](const char *n) { return (void *)GetProcAddress(m, n); };
    g_glf.GetProcAddress    = (decltype(g_glf.GetProcAddress))core("wglGetProcAddress");
    g_glf.GetCurrentContext = (decltype(g_glf.GetCurrentContext))core("wglGetCurrentContext");
    g_glf.GetIntegerv       = (decltype(g_glf.GetIntegerv))core("glGetIntegerv");
    g_glf.IsEnabled         = (decltype(g_glf.IsEnabled))core("glIsEnabled");
    g_glf.Enable            = (decltype(g_glf.Enable))core("glEnable");
    g_glf.Disable           = (decltype(g_glf.Disable))core("glDisable");
    g_glf.GenTextures       = (decltype(g_glf.GenTextures))core("glGenTextures");
    g_glf.DeleteTextures    = (decltype(g_glf.DeleteTextures))core("glDeleteTextures");
    g_glf.ReadBuffer        = (decltype(g_glf.ReadBuffer))core("glReadBuffer");
    g_glf.GetError          = (decltype(g_glf.GetError))core("glGetError");
    return g_glf.GetProcAddress && g_glf.GetCurrentContext && g_glf.GetIntegerv && g_glf.IsEnabled && g_glf.Enable &&
           g_glf.Disable && g_glf.GenTextures && g_glf.DeleteTextures && g_glf.ReadBuffer && g_glf.GetError;
}
// Extension functions: per context in theory; loaded with the game's context current.
static bool LoadGlExtFns()
{
    auto ext = [&](const char *n) { return (void *)g_glf.GetProcAddress(n); };
    g_glf.GenFramebuffers        = (decltype(g_glf.GenFramebuffers))ext("glGenFramebuffers");
    g_glf.DeleteFramebuffers     = (decltype(g_glf.DeleteFramebuffers))ext("glDeleteFramebuffers");
    g_glf.BindFramebuffer        = (decltype(g_glf.BindFramebuffer))ext("glBindFramebuffer");
    g_glf.FramebufferTexture2D   = (decltype(g_glf.FramebufferTexture2D))ext("glFramebufferTexture2D");
    g_glf.CheckFramebufferStatus = (decltype(g_glf.CheckFramebufferStatus))ext("glCheckFramebufferStatus");
    g_glf.BlitFramebuffer        = (decltype(g_glf.BlitFramebuffer))ext("glBlitFramebuffer");
    g_glf.DXOpenDeviceNV         = (decltype(g_glf.DXOpenDeviceNV))ext("wglDXOpenDeviceNV");
    g_glf.DXCloseDeviceNV        = (decltype(g_glf.DXCloseDeviceNV))ext("wglDXCloseDeviceNV");
    g_glf.DXRegisterObjectNV     = (decltype(g_glf.DXRegisterObjectNV))ext("wglDXRegisterObjectNV");
    g_glf.DXUnregisterObjectNV   = (decltype(g_glf.DXUnregisterObjectNV))ext("wglDXUnregisterObjectNV");
    g_glf.DXLockObjectsNV        = (decltype(g_glf.DXLockObjectsNV))ext("wglDXLockObjectsNV");
    g_glf.DXUnlockObjectsNV      = (decltype(g_glf.DXUnlockObjectsNV))ext("wglDXUnlockObjectsNV");
    g_glf.SwapIntervalEXT        = (decltype(g_glf.SwapIntervalEXT))ext("wglSwapIntervalEXT");
    g_glf.GetSwapIntervalEXT     = (decltype(g_glf.GetSwapIntervalEXT))ext("wglGetSwapIntervalEXT");
    const bool fbo = g_glf.GenFramebuffers && g_glf.DeleteFramebuffers && g_glf.BindFramebuffer && g_glf.FramebufferTexture2D &&
                     g_glf.CheckFramebufferStatus && g_glf.BlitFramebuffer;
    const bool nv  = g_glf.DXOpenDeviceNV && g_glf.DXCloseDeviceNV && g_glf.DXRegisterObjectNV && g_glf.DXUnregisterObjectNV &&
                     g_glf.DXLockObjectsNV && g_glf.DXUnlockObjectsNV;
    if (!fbo) LOGW("BFI Presenter: OpenGL: framebuffer-object functions missing (OpenGL 3.0 or newer needed)");
    if (!nv)  LOGW("BFI Presenter: OpenGL: WGL_NV_DX_interop2 not available (NVIDIA drivers have it; some AMD drivers don't)");
    return fbo && nv;
}

// GL objects: only with the game's context current on this thread.
static void GlReleaseObjects()
{
    if (!g_gl.ctx || !LoadGlFns() || g_glf.GetCurrentContext() != g_gl.ctx)
    {
        if (g_gl.glDev || g_gl.fbo) LOGI("BFI Presenter: OpenGL: the game's context isn't current here -- its interop objects are left to it");
        g_gl.glObj = g_gl.glDev = nullptr; g_gl.glTex = g_gl.fbo = 0; g_gl.glReady = false;
        return;
    }
    if (g_gl.glObj && g_glf.DXUnregisterObjectNV) g_glf.DXUnregisterObjectNV(g_gl.glDev, g_gl.glObj);
    if (g_gl.glDev && g_glf.DXCloseDeviceNV)      g_glf.DXCloseDeviceNV(g_gl.glDev);
    if (g_gl.fbo && g_glf.DeleteFramebuffers)     g_glf.DeleteFramebuffers(1, &g_gl.fbo);
    if (g_gl.glTex)                               g_glf.DeleteTextures(1, &g_gl.glTex);
    g_gl.glObj = g_gl.glDev = nullptr; g_gl.glTex = g_gl.fbo = 0; g_gl.glReady = false;
}
static void ReleaseGl()
{
    const bool wasActive = g_glActive.exchange(false);
    GlReleaseObjects();
    if (wasActive || g_gl.presenter)
    {
        ReleaseDx11();
        if (g_presenterStuck.load()) { g_gl = GlState(); return; }       // leave the rest alive (see StopPresenter)
    }
    SafeRelease(g_gl.tex); SafeRelease(g_gl.c11); SafeRelease(g_gl.d11);
    g_gl = GlState();
}

// The add-on's own D3D11 device, past ReShade's hooks: the first hardware
// adapter (OpenGL gives no adapter LUID; this is the GPU the desktop runs on).
static HRESULT CreateAddonD3D11(ID3D11Device **dev, ID3D11DeviceContext **ctx, LUID *luid)
{
    typedef HRESULT (WINAPI *PFN_CF1)(REFIID, void **);
    typedef HRESULT (WINAPI *PFN_D3D11CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                            const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
                                            ID3D11DeviceContext **);
    static PFN_CF1 cf1 = nullptr;
    static PFN_D3D11CDSC cd = nullptr;
    if (!cf1) { HMODULE m = LoadSystemDll("dxgi.dll"); if (m) cf1 = (PFN_CF1)RawEntry((void *)GetProcAddress(m, "CreateDXGIFactory1"), "CreateDXGIFactory1"); }
    if (!cd)  { HMODULE m = LoadSystemDll("d3d11.dll"); if (m) cd = (PFN_D3D11CDSC)RawEntry((void *)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain"), "D3D11CreateDeviceAndSwapChain"); }
    if (!cf1 || !cd) return E_FAIL;
    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *ad = nullptr;
    if (SUCCEEDED(cf1(__uuidof(IDXGIFactory1), (void **)&fac)) && fac)
    {
        IDXGIAdapter1 *a = nullptr;
        for (UINT i = 0; !ad && fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++)
        {
            DXGI_ADAPTER_DESC1 d = {};
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { ad = a; *luid = d.AdapterLuid; } else a->Release();
        }
        fac->Release();
    }
    HRESULT hr;
    {
        OwnCall own;
        hr = cd(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                nullptr, nullptr, dev, nullptr, ctx);
    }
    if (ad) ad->Release();
    return hr;
}

// Device side, from init_swapchain: the add-on's D3D11 device, the frame
// texture and the presenter. The GL side follows on the first present.
static void InitGl(reshade::api::swapchain *swapchain)
{
    ReleaseGl();
    if (GetPrivateProfileIntA("BFI", "OpenGLSupport", 1, g_iniPath) == 0)
    {
        SetReason(0, "Not managed: OpenGL support is off (OpenGLSupport=0).");
        return;
    }
    HWND hwnd = (HWND)swapchain->get_hwnd();
    RECT rc = {};
    if (!hwnd || !GetClientRect(hwnd, &rc) || rc.right - rc.left < 64 || rc.bottom - rc.top < 64)
    {
        LOGI("BFI Presenter: OpenGL: ignoring a window of %ldx%ld", rc.right - rc.left, rc.bottom - rc.top);
        return;
    }
    const UINT w = (UINT)(rc.right - rc.left), h = (UINT)(rc.bottom - rc.top);
    LUID luid = {};
    HRESULT hr = CreateAddonD3D11(&g_gl.d11, &g_gl.c11, &luid);
    if (FAILED(hr))
    {
        LOGW("BFI Presenter: OpenGL: the add-on's D3D11 device was not created (0x%08lX)", (unsigned long)hr);
        SetReason(0, "Not managed (OpenGL): the add-on's D3D11 device could not be created.");
        ReleaseGl();
        return;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = g_gl.d11->CreateTexture2D(&td, nullptr, &g_gl.tex);
    if (FAILED(hr))
    {
        LOGW("BFI Presenter: OpenGL: frame texture not created (0x%08lX)", (unsigned long)hr);
        ReleaseGl();
        return;
    }
    ID3D11Device3 *dev3 = nullptr;
    ID3D11DeviceContext3 *ctx3 = nullptr;
    bool ok = SUCCEEDED(g_gl.d11->QueryInterface(IID_PPV_ARGS(&g_dev11)));
    if (ok && SUCCEEDED(g_gl.d11->QueryInterface(IID_PPV_ARGS(&dev3))))
    {
        dev3->GetImmediateContext3(&ctx3);
        if (ctx3) ctx3->QueryInterface(IID_PPV_ARGS(&g_ctx11));
    }
    SafeRelease(ctx3);
    SafeRelease(dev3);
    if (!ok || !g_ctx11)
    {
        SafeRelease(g_ctx11); SafeRelease(g_dev11);
        SetReason(0, "Not managed (OpenGL): no shared-fence support (needs Windows 10 1703 or later).");
        ReleaseGl();
        return;
    }
    LOGI("BFI Presenter: OpenGL init 1/5 -- window %ux%u; frames go through the add-on's D3D11 device", w, h);
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.BufferCount = 2;
    if (!InitPresenter11(swapchain, nullptr, d, hwnd, DXGI_FORMAT_R8G8B8A8_UNORM, luid, "OpenGL"))
    {
        SafeRelease(g_ctx11); SafeRelease(g_dev11);
        g_dx11.store(false);
        ReleaseGl();
        return;
    }
    g_gl.presenter = true;
    g_gl.hwnd = hwnd;
    g_gl.w = w; g_gl.h = h;
    g_glActive.store(true);
}

// GL side, on the game's render thread with its context current.
static bool GlSetup(HGLRC ctx)
{
    if (!LoadGlFns() || !LoadGlExtFns()) return false;
    g_gl.ctx = ctx;
    g_gl.glDev = g_glf.DXOpenDeviceNV(g_gl.d11);
    if (!g_gl.glDev) { LOGW("BFI Presenter: OpenGL: wglDXOpenDeviceNV failed (%lu)", GetLastError()); return false; }
    g_glf.GenTextures(1, &g_gl.glTex);
    g_gl.glObj = g_glf.DXRegisterObjectNV(g_gl.glDev, g_gl.tex, g_gl.glTex, BFI_GL_TEXTURE_2D, BFI_WGL_ACCESS_WRITE_DISCARD_NV);
    if (!g_gl.glObj) { LOGW("BFI Presenter: OpenGL: wglDXRegisterObjectNV failed (%lu)", GetLastError()); return false; }
    GLintB oldRead = 0, oldDraw = 0;
    g_glf.GetIntegerv(BFI_GL_READ_FRAMEBUFFER_BINDING, &oldRead);
    g_glf.GetIntegerv(BFI_GL_DRAW_FRAMEBUFFER_BINDING, &oldDraw);
    g_glf.GenFramebuffers(1, &g_gl.fbo);
    g_glf.DXLockObjectsNV(g_gl.glDev, 1, &g_gl.glObj);
    g_glf.BindFramebuffer(BFI_GL_DRAW_FRAMEBUFFER, g_gl.fbo);
    g_glf.FramebufferTexture2D(BFI_GL_DRAW_FRAMEBUFFER, BFI_GL_COLOR_ATTACHMENT0, BFI_GL_TEXTURE_2D, g_gl.glTex, 0);
    const GLenumB st = g_glf.CheckFramebufferStatus(BFI_GL_DRAW_FRAMEBUFFER);
    g_glf.DXUnlockObjectsNV(g_gl.glDev, 1, &g_gl.glObj);
    g_glf.BindFramebuffer(BFI_GL_READ_FRAMEBUFFER, (GLuintB)oldRead);
    g_glf.BindFramebuffer(BFI_GL_DRAW_FRAMEBUFFER, (GLuintB)oldDraw);
    if (st != BFI_GL_FRAMEBUFFER_COMPLETE) { LOGW("BFI Presenter: OpenGL: the frame texture's framebuffer is incomplete (0x%X)", st); return false; }
    LOGI("BFI Presenter: OpenGL init 2/5 -- the game's context shares the frame texture through WGL_NV_DX_interop2");
    return true;
}

// One OpenGL game frame: blit the default framebuffer (bottom-up) into the
// interop texture (top-down), then hand it to the DX11 core.
static void on_reshade_present_gl(reshade::api::effect_runtime *runtime)
{
    if (!g_glActive.load() || !g_gl.presenter || g_gl.glFailed) return;
    if (runtime->get_device()->get_api() != reshade::api::device_api::opengl) return;
    if ((HWND)runtime->get_hwnd() != g_gl.hwnd) return;
    if (!LoadGlFns()) return;
    HGLRC ctx = g_glf.GetCurrentContext();
    if (!ctx) return;
    if (!g_gl.glReady)
    {
        if (!GlSetup(ctx))
        {
            g_gl.glFailed = true;
            SetReason(0, "Not managed (OpenGL): the game's frames could not be shared with the presenter (see ReShade.log).");
            GlReleaseObjects();
            if (HWND h = g_childHwnd.load()) ShowWindowAsync(h, SW_HIDE);
            return;
        }
        g_gl.glReady = true;
    }
    if (ctx != g_gl.ctx) return;                                    // another context of the game
    RECT rc = {};
    GetClientRect(g_gl.hwnd, &rc);
    if ((UINT)(rc.right - rc.left) != g_gl.w || (UINT)(rc.bottom - rc.top) != g_gl.h) return;   // resized: init_swapchain follows
    // The game's swap must not wait for vblank: the presenter paces the game.
    if (g_glf.SwapIntervalEXT && g_glf.GetSwapIntervalEXT && GetPrivateProfileIntA("BFI", "OpenGLSwapInterval0", 1, g_iniPath))
    {
        const int si = g_glf.GetSwapIntervalEXT();
        if (si != 0)
        {
            g_glf.SwapIntervalEXT(0);
            if (!g_gl.swapLogged) { g_gl.swapLogged = true; LOGI("BFI Presenter: OpenGL: the game's swap interval %d set to 0 (the presenter paces the game)", si); }
        }
    }
    SubmitFrame11Core(
        [&]() -> ID3D11Texture2D * {
            GLintB oldRead = 0, oldDraw = 0, oldReadBuf = 0;
            g_glf.GetIntegerv(BFI_GL_READ_FRAMEBUFFER_BINDING, &oldRead);
            g_glf.GetIntegerv(BFI_GL_DRAW_FRAMEBUFFER_BINDING, &oldDraw);
            const bool scissor = g_glf.IsEnabled(BFI_GL_SCISSOR_TEST) != 0;
            const bool srgb    = g_glf.IsEnabled(BFI_GL_FRAMEBUFFER_SRGB) != 0;
            const bool discard = g_glf.IsEnabled(BFI_GL_RASTERIZER_DISCARD) != 0;
            if (!g_glf.DXLockObjectsNV(g_gl.glDev, 1, &g_gl.glObj)) return nullptr;
            g_glf.BindFramebuffer(BFI_GL_READ_FRAMEBUFFER, 0);
            g_glf.GetIntegerv(BFI_GL_READ_BUFFER, &oldReadBuf);
            g_glf.ReadBuffer(BFI_GL_BACK);
            g_glf.BindFramebuffer(BFI_GL_DRAW_FRAMEBUFFER, g_gl.fbo);
            if (scissor) g_glf.Disable(BFI_GL_SCISSOR_TEST);
            if (srgb)    g_glf.Disable(BFI_GL_FRAMEBUFFER_SRGB);
            if (discard) g_glf.Disable(BFI_GL_RASTERIZER_DISCARD);
            g_glf.BlitFramebuffer(0, 0, (GLintB)g_gl.w, (GLintB)g_gl.h, 0, (GLintB)g_gl.h, (GLintB)g_gl.w, 0,
                                  BFI_GL_COLOR_BUFFER_BIT, BFI_GL_NEAREST);
            if (scissor) g_glf.Enable(BFI_GL_SCISSOR_TEST);
            if (srgb)    g_glf.Enable(BFI_GL_FRAMEBUFFER_SRGB);
            if (discard) g_glf.Enable(BFI_GL_RASTERIZER_DISCARD);
            g_glf.ReadBuffer((GLenumB)oldReadBuf);
            g_glf.BindFramebuffer(BFI_GL_READ_FRAMEBUFFER, (GLuintB)oldRead);
            g_glf.BindFramebuffer(BFI_GL_DRAW_FRAMEBUFFER, (GLuintB)oldDraw);
            g_glf.DXUnlockObjectsNV(g_gl.glDev, 1, &g_gl.glObj);
            static bool first = true;
            if (first) { first = false; LOGI("BFI Presenter: OpenGL: first game frame copied (GL error 0x%X)", g_glf.GetError()); }
            g_gl.tex->AddRef();
            return g_gl.tex;
        },
        []() -> HRESULT { return S_OK; });                         // the game's own SwapBuffers follows anyway
}

static void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
    if (g_sc) return;                                            // resizes of ours are handled in the hook
    // Direct3D 9 is handled above; 11 and 12 below. In an OpenGL or Vulkan game the native
    // handle is not a DXGI swap chain (Heretic II, OpenGL: crashed reading
    // the HDC as one in the probe check below).
    {
        const reshade::api::device_api a = swapchain->get_device()->get_api();
        if (a == reshade::api::device_api::d3d9) { if (!g_dx9.load()) InitDx9(swapchain); return; }
        if (a == reshade::api::device_api::opengl) { if (!g_glActive.load()) InitGl(swapchain); return; }
        if (a != reshade::api::device_api::d3d11 && a != reshade::api::device_api::d3d12)
        {
            SetReason(0, "Not managed: this game renders with %s; the add-on handles Direct3D 12, 11 and 9.",
                      a == reshade::api::device_api::opengl ? "OpenGL" : a == reshade::api::device_api::vulkan ? "Vulkan (use the Vulkan layer)" :
                      a == reshade::api::device_api::d3d9 ? "Direct3D 9" : a == reshade::api::device_api::d3d10 ? "Direct3D 10" : "another API");
            return;
        }
    }
    // Overlays and injectors (Special K, Steam) create tiny throw-away swap
    // chains on hidden windows to find DXGI's functions. Taking one of those
    // left the game's real swap chain unmanaged and the presenter pointed at
    // a swap chain about to be destroyed (Guacamelee 2 crashed). Skip them;
    // if a skipped one is later resized to a real size, take it then.
    static void *probeSc = nullptr;
    if (resize && swapchain != probeSc) return;
    {
        // Size from DXGI itself: ReShade's back-buffer accessors are not
        // usable yet inside init_swapchain (ReShade 6.5.1 crashed in
        // get_back_buffer there, Guacamelee 2).
        IDXGISwapChain *nsc = reinterpret_cast<IDXGISwapChain *>(swapchain->get_native());
        DXGI_SWAP_CHAIN_DESC sd = {};
        if (nsc && SUCCEEDED(nsc->GetDesc(&sd)) && (sd.BufferDesc.Width < 64 || sd.BufferDesc.Height < 64))
        {
            if (probeSc != swapchain)
                LOGI("BFI Presenter: ignoring a %ux%u swap chain (a probe by an overlay or injector, not the game's)",
                     sd.BufferDesc.Width, sd.BufferDesc.Height);
            probeSc = swapchain;
            return;
        }
    }
    if (swapchain == probeSc) probeSc = nullptr;
    if (!g_watchHwnd.load()) g_watchHwnd.store((HWND)swapchain->get_hwnd());
    const reshade::api::device_api api = swapchain->get_device()->get_api();
    if (api == reshade::api::device_api::d3d11 && !g_dx11.load())
    {
        int dx11 = 1;
        LoadShared();                                            // read the ini first: DX11Support decides below
        EnterCriticalSection(&g_cfgLock);
        dx11 = g_shared.dx11;
        LeaveCriticalSection(&g_cfgLock);
        if (!dx11) { SetReason(0, "Not managed: Direct3D 11 support is off (DX11Support=0)."); return; }
        if (GetModuleHandleA("EOSOVH-Win64-Shipping.dll") && GetPrivateProfileIntA("BFI", "AllowEosOverlay", 0, g_iniPath) == 0)
        {
            DetectPresentHooks();                                // logs the overlay's full path
            LOGW("BFI Presenter: the DX11 presenter stays off while the Epic Online Services overlay is loaded "
                 "(it crashed the presenter's present in Silt). Rename EOSOVH-Win64-Shipping.dll (path above) to use BFI.");
            SetReason(0, "Not managed: the Epic Online Services overlay is loaded (crashes the DX11 presenter). "
                         "Rename EOSOVH-Win64-Shipping.dll -- ReShade.log has its path.");
            return;
        }
        if (SmoothMotionActive() && GetPrivateProfileIntA("BFI", "AllowSmoothMotion", 0, g_iniPath) == 0)
        {
            LOGW("BFI Presenter: NVIDIA Smooth Motion is active in this game -- the DX11 presenter stays off "
                 "(it conflicts with Smooth Motion's own presentation). Turn Smooth Motion off for this game in the NVIDIA app.");
            SetReason(0, "Not managed: NVIDIA Smooth Motion is on for this game. Turn it off in the NVIDIA app "
                         "(BFI shows 60 images a second at 120 Hz, so it adds nothing).");
            return;
        }
        if (DetectPresentHooks() && !GetPrivateProfileIntA("BFI", "AllowRTSS", 1, g_iniPath))
        {
            SetReason(0, "Not managed: RTSS is hooked into this game (AllowRTSS=0).");
            return;
        }
        LoadShared();
        InitDx11(swapchain);
        return;
    }
    if (g_dx11.load() || g_creatingOwn.load()) return;            // the DX11 presenter's own swap chain
    if (api != reshade::api::device_api::d3d12)
    {
        SetReason(0, "Not managed: the game uses %s. BFI Presenter supports Direct3D 12, 11 and 9 (Vulkan: use the Vulkan layer).",
                  api == reshade::api::device_api::d3d11 ? "Direct3D 11" :
                  api == reshade::api::device_api::d3d10 ? "Direct3D 10" :
                  api == reshade::api::device_api::d3d9  ? "Direct3D 9"  :
                  api == reshade::api::device_api::vulkan ? "Vulkan" :
                  api == reshade::api::device_api::opengl ? "OpenGL" : "an unsupported API");
        return;
    }

    if (DetectPresentHooks() && !GetPrivateProfileIntA("BFI", "AllowRTSS", 1, g_iniPath))
    {
        SetReason(0, "Not managed: RivaTuner Statistics Server (RTSS) is hooked into this game. Its OSD and frame "
                     "limiter run inside the presenter and crashed it in testing. Exclude the game in RTSS "
                     "(Application detection level: None) and use this add-on's frame cap, or set AllowRTSS=1.");
        return;
    }

    IDXGISwapChain *native = reinterpret_cast<IDXGISwapChain *>(swapchain->get_native());
    IDXGISwapChain3 *sc3 = nullptr;
    if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&sc3))))
    {
        SetReason(0, "Not managed: no IDXGISwapChain3 interface.");
        return;
    }
    sc3->Release();                                              // keep a plain pointer

    DXGI_SWAP_CHAIN_DESC1 d = {};
    sc3->GetDesc1(&d);
    {
        // Who implements the swap chain object below ReShade: dxgi.dll means
        // the real one; anything else is a proxy (frame generation, overlays).
        void **vt = *(void ***)sc3;
        const void *present = (g_hookedVtbl == vt && o_Present) ? (const void *)o_Present : vt[VT_Present];
        char objMod[128], presMod[128];
        ModuleOf(vt, objMod, sizeof(objMod));
        ModuleOf(present, presMod, sizeof(presMod));
        snprintf(g_scOwner, sizeof(g_scOwner), "%s (Present in %s)", objMod, presMod);
        LOGI("BFI Presenter: swap chain below ReShade is implemented by %s", g_scOwner);
        DetectFrameGen();
    }
    LOGI("BFI Presenter: init 1/6 -- swap chain %p, %ux%u, format %u, %u buffers, flags 0x%X",
         (void *)sc3, d.Width, d.Height, (unsigned)d.Format, d.BufferCount, (unsigned)d.Flags);
    if (d.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && d.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
    {
        SetReason(0, "Not managed: the swap chain is not flip-model.");
        return;
    }

    g_device = reinterpret_cast<ID3D12Device *>(swapchain->get_device()->get_native());
    if (g_diagnostics >= 2 && !g_infoQueue &&
        SUCCEEDED(g_device->QueryInterface(__uuidof(ID3D12InfoQueue), (void **)&g_infoQueue)) && g_infoQueue)
    {
        g_infoQueue->SetMessageCountLimit(4096);
        g_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        g_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        LOGI("BFI Presenter: collecting D3D12 debug-layer messages");
    }
    else if (g_diagnostics >= 2 && !g_infoQueue)
        LOGW("BFI Presenter: the device has no debug layer (it was created before the layer was enabled, or Graphics Tools is missing)");
    // D3D12 swap chains return their command queue from GetDevice. If that
    // fails, the queue is taken from ReShade's effect runtime right after.
    if (FAILED(sc3->GetDevice(IID_PPV_ARGS(&g_gameQueue)))) g_gameQueue = nullptr;
    LOGI("BFI Presenter: init 2/6 -- game queue %s", g_gameQueue ? "from the swap chain" : "pending (effect runtime)");
    if (!InstallHooks(sc3))
    {
        SafeRelease(g_gameQueue);
        SetReason(0, "Not managed: swap chain class differs from the first one hooked.");
        return;
    }

    LOGI("BFI Presenter: init 3/6 -- hooks in place");
    g_sc = sc3;
    g_rsSwapchain = swapchain;
    g_lastPresentT.store(0.0);
    g_stallLogged.store(false);
    g_firstStaged.store(false);
    g_firstPresented.store(false);
    for (DWORD &t : g_presentThreads) t = 0;
    LoadShared();

    bool ok = SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_gameFence))) &&
              SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_presFence)));
    for (UINT s = 0; s < kStage && ok; s++)
        ok = SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_copyAlloc[s])));
    if (ok)
        ok = SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_copyAlloc[0], nullptr,
                                                   IID_PPV_ARGS(&g_copyList)));
    if (ok)
    {
        g_copyList->SetName(L"BFI frame copy list");
        g_copyList->Close();
    }
    if (ok && g_presQueue)
    {
        // Reuse the queue from an earlier swap chain if it is on this device.
        ID3D12Device *qdev = nullptr;
        if (SUCCEEDED(g_presQueue->GetDevice(IID_PPV_ARGS(&qdev))) && qdev == g_device)
            LOGI("BFI Presenter: reusing the presenter queue from the previous swap chain");
        else
            SafeRelease(g_presQueue);
        SafeRelease(qdev);
    }
    if (ok && !g_presQueue)
    {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        // Global realtime priority needs the "increase scheduling priority"
        // privilege (usually only when the game runs as administrator); fall
        // back to high otherwise.
        // PresenterQueuePriority: 0 normal, 1 high, 2 global realtime when
        // Windows allows it, else high. A lower priority avoids preempting the
        // game's GPU work (a possible factor in Resident Evil 4's GPU hangs),
        // at the cost of more late refreshes when the GPU is busy.
        int want;
        EnterCriticalSection(&g_cfgLock);
        want = g_shared.queuePriority;
        LeaveCriticalSection(&g_cfgLock);
        ok = false;
        if (want >= 2)
        {
            EnableBasePriorityPrivilege();
            qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
            ok = SUCCEEDED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_presQueue)));
        }
        if (!ok)
        {
            qd.Priority = want >= 1 ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            ok = SUCCEEDED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_presQueue)));
        }
        g_queuePriority = ok ? qd.Priority : 0;
        if (ok)
        {
            g_presQueue->SetName(L"BFI presenter queue");
            LOGI("BFI Presenter: presenter queue priority: %s",
                 qd.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME ? "global realtime" :
                 qd.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH ? "high" : "normal");
        }
    }
    for (UINT i = 0; i < kMaxBuf && ok; i++)
    {
        ok = SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_fbAlloc[i])));
        g_fbFence[i] = 0;
    }
    if (ok)
        ok = SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_fbAlloc[0], nullptr,
                                                   IID_PPV_ARGS(&g_fbList)));
    if (ok)
    {
        g_fbList->SetName(L"BFI fallback copy list");
        g_fbList->Close();
    }
    g_gameFenceVal = g_presFenceVal = 0;
    g_frameId = 0;
    g_releasedUpTo.store(0);
    g_shownGameFrames.store(0);
    LOGI("BFI Presenter: init 4/6 -- GPU objects %s", ok ? "created" : "FAILED");
    // Present on the presenter's own queue. Nothing holds the real buffers
    // yet (the game has not seen the swap chain), so a same-size
    // ResizeBuffers1 can rebind the present queue.
    g_ownPresentQueue = false;
    Settings cur;
    EnterCriticalSection(&g_cfgLock);
    cur = g_shared;
    LeaveCriticalSection(&g_cfgLock);
    g_needRebind.store(ok && cur.ownQueue != 0);
    g_presenterFailed.store(false);
    g_swapChainsManaged++;
    // Presenting on the game's own queue before its first resize deadlocked
    // God of War Ragnarok (recreated swap chain) and Pragmata (first swap
    // chain: the game's ResizeBuffers waited for a presenter stuck on the
    // game's queue). By default the game presents its own frames until it
    // resizes the swap chain -- nearly every game does at start-up -- and BFI
    // starts inside that resize, on the presenter's own queue.
    g_deferPresenter.store(ok && cur.ownQueue != 0 &&
                           (cur.waitResizeAll || (cur.deferRecreated && g_swapChainsManaged > 1)));
    if (g_deferPresenter.load())
        LOGI("BFI Presenter: %s -- the game presents its own frames (no BFI) until it resizes the swap chain; "
             "if BFI doesn't start, change resolution or display mode once",
             g_swapChainsManaged > 1 ? "swap chain recreated by the game" : "waiting for the game's first resize");
    if (ok)
        LOGI("BFI Presenter: init 5/6 -- %s", cur.ownQueue ? "presentation moves to the presenter's own queue on the first frame"
                                                        : "PresentOnOwnQueue=0, presenting on the game's queue");
    ok = ok && CreateFrameResources();
    LOGI("BFI Presenter: init 6/6 -- stand-in back buffers %s", ok ? "created" : "FAILED");
    if (!ok)
    {
        SetReason(0, "Not managed: setup failed (see ReShade.log).");
        ReleaseAll();
        return;
    }

    g_virtCount = d.BufferCount;
    if (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
    {
        g_maxLatency.store(1);                                   // DXGI's default for waitable swap chains
        g_waitSem = CreateSemaphoreA(nullptr, 1, 16, nullptr);
        LOGI("BFI Presenter: game uses a frame-latency waitable -- driven by the presenter");
    }
    g_active.store(true);                                        // from here GetBuffer etc. are virtual
    // The presenter starts with the game's first frame (see SubmitGameFrame).
    SetReason(1, "Managing the swap chain (%u buffers, %ux%u).", g_bufCount, g_desc.Width, g_desc.Height);
}

// Views the game creates on the stand-in back buffers: give typeless
// 8-bit buffers a typed format when the game did not ask for one
// (sRGB views are passed through untouched).
static bool on_create_resource_view(reshade::api::device *, reshade::api::resource resource,
                                    reshade::api::resource_usage, reshade::api::resource_view_desc &desc)
{
    if (!g_active.load() || VirtualFormat(g_desc.Format) == g_desc.Format) return false;
    bool ours = false;
    for (UINT i = 0; i < g_bufCount; i++)
        if (resource.handle == (uint64_t)(uintptr_t)g_virt[i]) { ours = true; break; }
    if (!ours) return false;
    const reshade::api::format typeless = (reshade::api::format)VirtualFormat(g_desc.Format);
    if (desc.format != reshade::api::format::unknown && desc.format != typeless) return false;
    desc.format = (reshade::api::format)g_desc.Format;
    if (desc.type == reshade::api::resource_view_type::unknown)
    {
        desc.type = reshade::api::resource_view_type::texture_2d;
        desc.texture.first_level = 0;
        desc.texture.level_count = 1;
        desc.texture.first_layer = 0;
        desc.texture.layer_count = 1;
    }
    return true;
}

// ---------------------------------------------------------------- overlay
static const char *kOutputNames = "Auto\0SDR (sRGB)\0HDR scRGB\0HDR10 (PQ)\0";

static void draw_overlay(reshade::api::effect_runtime *)
{
    Status st;
    Settings s;
    EnterCriticalSection(&g_cfgLock);
    st = g_status;
    s = g_shared;
    LeaveCriticalSection(&g_cfgLock);
    const Settings before = s;
    const ImVec4 kRed(1.0f, 0.4f, 0.3f, 1.0f), kAmber(1.0f, 0.75f, 0.2f, 1.0f);

    // ================================================================ header
    ImGui::TextDisabled("BFI Presenter v" BFI_ADDON_VERSION " (Direct3D 12 / 11 / 9)");
    if (st.deviceLost)
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Device removed -- report written to BFIPresenter_dred.txt");
    if (st.managed) ImGui::TextWrapped("%s", st.reason);
    else            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", st.reason);
    bool en = s.enabled != 0;
    if (ImGui::Checkbox("BFI enabled (Ctrl+Shift+B)", &en)) s.enabled = en ? 1 : 0;

    // ================================================================ warnings (only when something needs attention)
    if (strstr(g_hooksSeen, "RTSS"))
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "RTSS is hooked: keep its frame limiter off and don't toggle its OSD while playing.");
    if (g_hooksSeen[0]) ImGui::TextColored(kAmber, "Other Present hooks: %s", g_hooksSeen);
    if (g_fgSeen[0])    ImGui::TextColored(kAmber, "Frame generation: %s", g_fgSeen);
    if (st.managed)
    {
        if (g_deferPresenter.load() && !g_ownPresentQueue)
            ImGui::TextColored(kAmber, "BFI starts when the game resizes its swap chain.\nIf it doesn't, change resolution or display mode once (and back).");
        if (st.vrrSuspect)
            ImGui::TextColored(kRed, "The display is refreshing slower than it can: VRR (G-SYNC) is active.\nTurn G-SYNC off for this game in the NVIDIA profile, or turn on Queue one refresh ahead.");
        if (st.limitSuspect)
            ImGui::TextColored(kRed, "A frame limiter is holding the presenter below the display's refresh rate.\nSet the RTSS frame limit to 0 for this game (and check NVIDIA Profile Inspector).");
        if (st.vsyncOverridden)
            ImGui::TextColored(kRed, "The driver is ignoring VSync (NVIDIA profile: Force off / Fast Sync?).\nPacing to vblank instead; tearing is likely. Set Vertical Sync to Force on.");
        if (st.repeats > 0 || st.late > 0)
            ImGui::TextColored(kAmber, "Missed refreshes break the strobe: check VRR is off and nothing caps the presenter.");
        if (st.hitchPaused)
            ImGui::TextColored(kAmber, "Hitch guard: BFI paused (loading or missed refreshes)");
        // Only when it persists: a stray repeat now and then is not worth a warning.
        if (st.unevenRep + st.unevenSkip >= 3)
        {
            const double capRate = st.refreshHz / (s.gameCap == 2 ? 2.0 : s.gameCap == 3 ? 4.0 : 1.0);
            if (s.gameCap >= 1 && st.gameFps < 0.97 * capRate)
                ImGui::TextColored(kAmber, "The game is below the cap's frame rate (%.0f of %.0f fps): motion stutters.\n"
                                   "Lower its settings or resolution, or use a lower cap (Half refresh).", st.gameFps, capRate);
            else
                ImGui::TextColored(kAmber, "Game frames reach the strobe unevenly: motion may stutter. If it persists, the GPU\n"
                                   "is probably finishing frames right at the refresh -- lower settings or try Smooth pacing.");
        }
    }

    // ================================================================ 1. strobe
    if (ImGui::CollapsingHeader("Strobe", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Combo("Method (Ctrl+Shift+M)", &s.method, "BFI (black frames + highlight roll-off)\0Variable MPRT (brightness budget)\0");
        ImGui::SliderInt("Frames per cycle", &s.frames, 2, 8);
        ImGui::SliderFloat("Gain vs blur", &s.gain, 0.3f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0.5 = lit frame unchanged (half average brightness).\n1.0 = full brightness: BFI rolls off highlights, MPRT spills them into the dark frame.");
        ImGui::SliderFloat("Dark frame floor", &s.dfloor, 0.0f, 0.5f, "%.2f");
        if (s.method == 0)
        {
            bool ro = s.rolloff != 0;
            if (ImGui::Checkbox("Highlight roll-off", &ro)) s.rolloff = ro ? 1 : 0;
            if (s.rolloff) ImGui::SliderFloat("Roll-off knee", &s.knee, 0.5f, 0.95f, "%.2f");
        }
        bool sine = s.sine != 0, inv = s.invert != 0;
        if (ImGui::Checkbox("Sine BFI", &sine)) s.sine = sine ? 1 : 0;
        ImGui::SameLine();
        if (ImGui::Checkbox("Invert cycle (frames > 2)", &inv)) s.invert = inv ? 1 : 0;

        bool rsc = s.rolling != 0;
        if (ImGui::Checkbox("Rolling scan (Ctrl+Shift+S)", &rsc)) s.rolling = rsc ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("One horizontal band per sub-frame, lit in turn from the top down, like a CRT beam.\n"
                                                      "The screen as a whole never goes dark: less visible flicker. Fast pans can show\n"
                                                      "a step at the seam -- use phosphor decay to hide it.");
        if (s.rolling)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderFloat("Phosphor decay (sub-frames, 0 = off)", &s.rollDecay, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each row lights when the simulated beam passes it and fades out over this long,\n"
                                                          "like CRT phosphor (Blur Busters CRT simulator style). No seam: the band edge\n"
                                                          "moves with the beam and fades instead of switching. Longer = smoother and less\n"
                                                          "flicker, but more blur. 0.2-0.4 is a good start; 0 = hard bands with a seam.");
            if (s.rollDecay <= 0.0f)
                ImGui::SliderFloat("Seam softness (share of screen height)", &s.rollSeam, 0.0f, 0.3f, "%.2f");
            ImGui::Unindent(16.0f);
        }
    }

    // ================================================================ 2. brightness and output
    if (ImGui::CollapsingHeader("Brightness and output", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int out = s.output + 1;
        if (ImGui::Combo("Output", &out, kOutputNames)) s.output = out - 1;
        ImGui::SliderFloat("Display peak (nits)", &s.peakNits, 100.0f, 4000.0f, "%.0f");
        bool ag = s.ablGuard != 0;
        if (ImGui::Checkbox("ABL guard (keep bright scenes within the panel's budget)", &ag)) s.ablGuard = ag ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("OLED panels dim the picture when a refresh puts out too much light overall (area x brightness).\nWith BFI the lit refresh carries the boost, so bright skies and white screens can trigger it.\nThe lit-refresh ceiling is lowered just enough to stay within the budget (the rest spills into\nthe dark refresh), and returns to full when the scene allows. HDR only.");
        if (s.ablGuard)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderInt("Panel budget (average nits of a lit refresh)", &s.ablBudget, 100, 1500);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The highest average brightness a lit refresh may have. The ceiling is lowered just enough\nto stay within it: large bright areas get a low ceiling, small highlights keep the full one.\nLower it if bright scenes still darken.");
            if (st.hotArea >= 0.0)
                ImGui::Text("Scene asks for: %.0f nits average   After ceiling: %.0f   Ceiling in use: %.0f nits", st.rawAvg, st.hotArea, st.ceilInUse);
            ImGui::Unindent(16.0f);
        }
    }

    // ================================================================ 3. frame pacing and latency
    if (ImGui::CollapsingHeader("Frame pacing and latency", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Combo("Game frame cap", &s.gameCap, "Off\0Refresh rate\0Half refresh (one game frame per lit + dark pair)\0Quarter refresh (30 fps at 120 Hz)\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh rate: lowest latency when the game holds it.\nHalf refresh = 60 fps at 120 Hz: every game frame is shown once lit, the smoothest motion\nwith 2-frame BFI, and the best choice when the game can't hold 120. With frame generation use Refresh rate:\nHalf throws away half the generated frames.");
        ImGui::Combo("Frame pacing", &s.pacingMode, "Smooth (every game frame once, in order)\0Lowest latency (newest frame)\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lowest latency shows the newest frame -- best at the Refresh cap.\nSmooth holds up to one finished frame in reserve (about one refresh more latency) so every\nframe is lit once and in order -- it only helps at the Half or Quarter cap.");
        if (s.pacingMode == 0)
        {
            bool cu = s.smoothCatchUp != 0;
            if (ImGui::Checkbox("Smooth: catch up after a hitch", &cu)) s.smoothCatchUp = cu ? 1 : 0;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off (default): after a game hitch the presenter stays one frame behind (about one cycle\n"
                                                          "more latency) and shows every frame once, in order.\n"
                                                          "On: when two finished frames keep waiting, one is skipped to get back to the lowest\n"
                                                          "latency -- a second stutter shortly after the first.");
        }
        bool cj = s.capJit != 0;
        if (ImGui::Checkbox("Just-in-time release at Half / Quarter cap", &cj)) s.capJit = cj ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Releases the game for each frame at the lit refresh minus its measured frame time and a\n"
                                                      "margin, so every frame lands on the same lit refresh: no repeat-then-skip at Half, no\n"
                                                      "1-then-3 cycle frames at Quarter. Light games also get fresher frames.");
        bool lr = s.laterRelease != 0;
        if (ImGui::Checkbox("Later release at Refresh cap (Lowest latency, no frame generation)", &lr)) s.laterRelease = lr ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lets the game start each frame just in time for the refresh that shows it, instead of\na full refresh earlier, so its input is read later -- up to ~5 ms less latency in games\nthat render well within a refresh. Does nothing while frames take nearly a whole refresh.");
        if (s.laterRelease || (s.capJit && s.gameCap >= 2))
        {
            ImGui::Indent(16.0f);
            ImGui::SliderFloat("Just-in-time safety margin (ms)", &s.laterReleaseMs, 0.5f, 5.0f, "%.1f");
            static const char *kLrNames[] = { "off (needs Wait for vblank; at Refresh cap also Lowest latency + Later release)", "measuring", "active", "at its limit (game frames take nearly the whole interval)" };
            ImGui::Text("Just-in-time release: %s", kLrNames[st.lrState & 3]);
            if (st.lrState >= 2)
                ImGui::Text("Frames take %.1f ms, margin %.1f ms: released %.1f ms before the refresh (misses last second: %d)",
                            st.lrEst, st.lrMargin, st.lrEst + st.lrMargin, st.lrMisses);
            ImGui::Unindent(16.0f);
        }
        bool aa = s.autoAhead != 0;
        if (ImGui::Checkbox("Automatic frames ahead (1, or 2 while the game misses cycles)", &aa)) s.autoAhead = aa ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many frames the game may queue on the GPU ahead of the screen.\n1 = lowest input lag (like a low-latency mode); 2 if the frame rate drops.");
        ImGui::Indent(16.0f);
        if (!s.autoAhead) ImGui::SliderInt("Max frames ahead (0 = off)", &s.maxAhead, 0, 3);
        else ImGui::Text("Frames ahead in use: %d", st.aheadInUse);
        ImGui::Unindent(16.0f);
    }

    // ================================================================ 4. status
    if (st.managed && ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
    {
        static const char *outs[] = { "SDR", "scRGB", "HDR10" };
        ImGui::Text("Game: %.1f fps   Refreshes: %.1f/s (display %.1f Hz)   Frame age: %.1f ms",
                    st.gameFps, st.refreshHz, st.displayHz, st.frameAgeMs);
        ImGui::Text("Uneven strobe cycles: %d/s (a frame repeated %d, frames skipped %d)",
                    st.unevenRep + st.unevenSkip, st.unevenRep, st.unevenSkip);
        ImGui::Text("Late refreshes: %d/s   Display repeats: %d/s", st.late, st.repeats);
        ImGui::Text("Presenter: %s   Output: %s   Format: %u   %ux%u, %u buffers",
                    st.running ? "running" : "stopped",
                    (st.outType >= 0 && st.outType <= 2) ? outs[st.outType] : "-", st.format,
                    st.width, st.height, st.buffers);
        ImGui::Text("Presenter queue: %s", g_queuePriority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME ? "global realtime" :
                                            g_queuePriority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH ? "high (run the launcher as administrator for realtime)" : "normal");
        if (g_scOwner[0]) ImGui::TextDisabled("Swap chain implemented by %s", g_scOwner);
    }

    // ================================================================ 5. advanced
    if (ImGui::CollapsingHeader("Advanced"))
    {
        bool vb = s.pacing != 0, oq = s.ownQueue != 0;
        if (ImGui::Checkbox("Wait for vblank", &vb)) s.pacing = vb ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("One present per refresh, nothing queued: lowest latency. Needed by just-in-time release.");
        bool qa = s.queueAhead != 0;
        if (ImGui::Checkbox("Queue one refresh ahead (needs 3 presenter buffers, Wait for vblank on)", &qa)) s.queueAhead = qa ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps exactly one extra present queued, so an occasional late slot is covered\nwithout a flash. +1 refresh (8 ms) of latency. Unlike Wait for vblank off, the\nqueue never grows, so the driver doesn't throttle the game.");
        ImGui::SliderInt("Presenter buffers (applies at the game's next resize)", &s.presBuffers, 2, 4);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Real swap chain buffers behind the presenter. With 3 or 4 AND 'Wait for vblank' off,\nthe presenter queues frames ahead, so a late slot no longer holds a lit or dark frame\ntwice (a flash) -- at the cost of 1-2 refreshes (8-17 ms) of extra latency.");
        if (ImGui::Checkbox("Present on own queue (applies when the game next creates its swap chain)", &oq)) s.ownQueue = oq ? 1 : 0;
        bool hg = s.hitchGuard != 0;
        if (ImGui::Checkbox("Hitch guard (pause BFI during loading and missed refreshes)", &hg)) s.hitchGuard = hg ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("While the presenter misses refreshes (3 within half a second) or the game delivers no\nnew frame for the loading time, every refresh is shown at the strobe's average brightness\ninstead of strobing (no brightness change). BFI returns after one second without either.");
        if (s.hitchGuard)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderInt("Loading: no new frame for (ms)", &s.hitchGapMs, 150, 5000);
            ImGui::Unindent(16.0f);
        }
        ImGui::Text("Game frames that waited for the presenter: %d/s (should be 0)", st.slotWaits);
        if (st.queued >= 0) ImGui::Text("Presents queued: %d", st.queued);
        ImGui::Text("Refreshes reusing an already-drawn buffer: %d/s", st.reused);
        ImGui::Text("Last present: %.0f ms ago", g_lastPresentT.load() > 0.0 ? (Now() - g_lastPresentT.load()) * 1000.0 : -1.0);
    }

    // ================================================================ footer
    ImGui::Separator();
    bool lg = s.log != 0;
    if (ImGui::Checkbox("Timing log (new BFIPresenter_log_<date>_<time>.csv each time it is switched on)", &lg)) s.log = lg ? 1 : 0;

    if (memcmp(&s, &before, sizeof(Settings)) != 0)
    {
        EnterCriticalSection(&g_cfgLock);
        g_shared = s;
        LeaveCriticalSection(&g_cfgLock);
        g_gameCap.store(s.gameCap);
        if (!s.autoAhead) g_maxAhead.store(s.maxAhead);
        g_virtualStats.store(s.virtualStats);
    }
    if (ImGui::Button("Save to BFIPresenter.ini")) SaveSettings(s);
    ImGui::SameLine();
    if (ImGui::Button("Reload ini")) LoadShared();
    ImGui::TextDisabled("Use ENABLE_DECAY=0 in CRT-Standalone. Fixed 120 Hz, VRR off.");
}

static void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
    if (!g_sc || g_gameQueue) return;
    if (reinterpret_cast<void *>(runtime->get_native()) != (void *)g_sc) return;
    g_gameQueue = reinterpret_cast<ID3D12CommandQueue *>(runtime->get_command_queue()->get_native());
    if (!g_gameQueue) return;
    g_gameQueue->AddRef();
    LOGI("BFI Presenter: command queue taken from the effect runtime");
}

static void on_destroy_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
    if (g_glActive.load() && swapchain->get_device()->get_api() == reshade::api::device_api::opengl &&
        (HWND)swapchain->get_hwnd() == g_gl.hwnd)
    {
        LOGI("BFI Presenter: OpenGL swap chain %s -- stopping the presenter", resize ? "resized" : "destroyed");
        ReleaseGl();
        return;
    }
    if (g_t9.rsc && swapchain == g_t9.rsc)
    {
        LOGI("BFI Presenter: DX9 swap chain %s -- stopping the presenter", resize ? "reset" : "destroyed");
        ReleaseDx9();
        return;
    }
    if (!resize && g_dx11.load() && reinterpret_cast<void *>(swapchain->get_native()) == (void *)g_sc11)
    {
        LOGI("BFI Presenter: DX11 swap chain %p destroyed -- stopping the presenter", (void *)g_sc11);
        ReleaseDx11();
        RemoveHooks();
        return;
    }
    if (resize || !g_sc || g_dx11.load()) return;
    if (reinterpret_cast<void *>(swapchain->get_native()) != (void *)g_sc) return;
    LOGI("BFI Presenter: swap chain %p destroyed -- stopping the presenter", (void *)g_sc);
    ReleaseAll();
    // Take the vtable hooks off before the swap chain itself goes away, and
    // put them back on the next one. Overlays that hook Present too (Steam's
    // gameoverlayrenderer64, RTSS) clean up and re-hook when the swap chain
    // is destroyed and recreated; with our hook left on top, the Steam hook
    // was left half-reset and called a null pointer on the new swap chain's
    // first frame (God of War Ragnarok, on loading).
    if (g_hookedVtbl)
    {
        void **vt = g_hookedVtbl;
        const bool ours = vt[VT_Present] == (void *)&hk_Present;
        RemoveHooks();
        char mod[64];
        ModuleOf(vt[VT_Present], mod, sizeof(mod));
        LOGI("BFI Presenter: swap chain hooks removed (%s); Present now goes to %s",
             ours ? "they were on top" : "another hook had been placed on top of them", mod);
    }
    LOGI("BFI Presenter: presenter stopped, resources released");
}

// ---------------------------------------------------------------- entry
// Unregister from ReShade. The module is pinned (the watchdog thread runs for
// the life of the process), so ReShade's FreeLibrary never reaches
// DLL_PROCESS_DETACH; ReShade calls the AddonUninit export first instead.
static bool g_unregistered = false;
static void UnregisterAll(HMODULE hModule)
{
    if (g_unregistered) return;
    g_unregistered = true;
    if (g_crashHandler) { RemoveVectoredExceptionHandler(g_crashHandler); g_crashHandler = nullptr; }
    reshade::unregister_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
    reshade::unregister_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::unregister_event<reshade::addon_event::create_resource_view>(on_create_resource_view);
    RegisterDx9Events(false);
    reshade::unregister_overlay("BFI Presenter", draw_overlay);
    RemoveHooks();
    reshade::unregister_addon(hModule);
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE)
{
    UnregisterAll(addon_module);
}
// ReShade unloads and reloads add-ons with every device the game creates and
// destroys (Unreal Engine makes several short-lived D3D11 devices at start).
// The module is pinned, so a reload does not run DllMain again: register
// again here, or ReShade reports "No add-on was registered" and drops it
// (Grip, v0.64).
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE)
{
    if (!g_unregistered) return true;                  // first load: DllMain registered already
    if (!reshade::register_addon(addon_module)) return false;
    g_unregistered = false;
    reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
    reshade::register_event<reshade::addon_event::create_resource_view>(on_create_resource_view);
    RegisterDx9Events(true);
    reshade::register_overlay("BFI Presenter", draw_overlay);
    if (!g_crashHandler && g_crashLogOn) g_crashHandler = AddVectoredExceptionHandler(0, CrashLogger);
    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        if (!reshade::register_addon(hModule)) return FALSE;
        g_module = hModule;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = (double)f.QuadPart;
        GetModuleFileNameA(hModule, g_iniPath, MAX_PATH);
        char *slash = strrchr(g_iniPath, '\\');
        if (slash) slash[1] = '\0';
        strcpy(g_logPath, g_iniPath);
        strcpy(g_dredPath, g_iniPath);
        strncat(g_iniPath, "BFIPresenter.ini", MAX_PATH - strlen(g_iniPath) - 1);
        {
            char mod[MAX_PATH];
            GetModuleFileNameA(hModule, mod, MAX_PATH);
            LOGI("BFI Presenter: v" BFI_ADDON_VERSION " loaded from %s", mod);
        }
        strncat(g_logPath, "BFIPresenter_log.csv", MAX_PATH - strlen(g_logPath) - 1);
        strncat(g_dredPath, "BFIPresenter_dred.txt", MAX_PATH - strlen(g_dredPath) - 1);
        strcpy(g_crashPath, g_iniPath);
        if (char *cs = strrchr(g_crashPath, '\\')) cs[1] = '\0';
        strncat(g_crashPath, "BFIPresenter_crash.txt", MAX_PATH - strlen(g_crashPath) - 1);
        DeleteFileA(g_crashPath);                       // one record per game session
        // Registered LAST: runtimes that use access violations on purpose
        // (Mono in Unity games turns them into NullReferenceExceptions) handle
        // theirs first, and only exceptions nobody resumed reach the logger.
        // CrashLog=0 in the ini leaves it out entirely.
        g_crashLogOn = GetPrivateProfileIntA("BFI", "CrashLog", 1, g_iniPath) != 0;
        if (g_crashLogOn) g_crashHandler = AddVectoredExceptionHandler(0, CrashLogger);
        g_diagnostics = (int)GetPrivateProfileIntA("BFI", "Diagnostics", 1, g_iniPath);
        InitializeCriticalSection(&g_infoLock);
        strcpy(g_debugPath, g_crashPath);
        if (char *ds = strrchr(g_debugPath, '\\')) ds[1] = '\0';
        strncat(g_debugPath, "BFIPresenter_debug.txt", MAX_PATH - strlen(g_debugPath) - 1);
        if (g_diagnostics >= 2) DeleteFileA(g_debugPath);
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_cfgLock);
        InitializeCriticalSection(&g_submitLock);
        g_tickEvent      = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g_gameFenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::create_resource_view>(on_create_resource_view);
        RegisterDx9Events(true);
        reshade::register_overlay("BFI Presenter", draw_overlay);
        // DRED must be switched on before the game creates its device; a
        // thread avoids calling into D3D12 under the loader lock.
        {
            // The watchdog runs for the life of the process: pin the module so
            // an early FreeLibrary can never unmap code it is executing.
            HMODULE pinned = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               (LPCSTR)(void *)&WatchdogThread, &pinned);
            HANDLE w = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
            if (w) CloseHandle(w);
        }
        if (g_diagnostics)
        {
            HANDLE t = CreateThread(nullptr, 0, EnableDredThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        UnregisterAll(hModule);
        break;
    }
    return TRUE;
}
