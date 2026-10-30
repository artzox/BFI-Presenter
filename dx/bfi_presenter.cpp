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
#include <string>
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

#define BFI_ADDON_VERSION "1.0.27-e20"

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
    int   lcdSwap  = 0;       // LCD: shift the strobe by one refresh now and then (even cycles), so the lit
                              // refreshes alternate between the panel's + and - drive polarity (no image retention)
    int   lcdSwapSec = 5;    // seconds of full strobe between phase shifts (1-300)
    int   lcdGradual = 0;    // LCD saver mode: 0 Immediate (fade, shift, fade back), 1 Gradual (the lit window drifts slowly)
    int   lcdGradSec = 1000; // Gradual: seconds for the lit window to drift through the whole cycle (20-1200)
    int   lcdHoldSec = 120;  // Gradual at 2 frames: seconds held sharp between drifts (0 = drift continuously; 0-600)
    int   lcdDriftSec = 10;  // Gradual at 2 frames: seconds one drift to the other refresh takes (2-60)
    float rollSeam = 0.0f;    // blended zone between bands, fraction of the screen height
    float rollDecay = 0.2f;   // rolling scan phosphor decay, in sub-frames (0 = off: hard bands + seam)
    float knee     = 0.75f;
    float peakNits = 1400.0f; // display peak (HDR ceiling)
    int   output   = -1;      // -1 auto, 0 SDR, 1 scRGB, 2 HDR10
    int   gameCap  = 1;       // 0 off, 1 = refresh rate, 2 = half refresh, 3 = quarter refresh (30 fps at 120 Hz),
                              // 4 = one game frame per strobe cycle (refresh / Frames), 5 = refresh / CapDivisor
    int   capDivisor = 3;    // cap 5: release the game every this many refreshes (1-8)
    int   ownQueue = 1;       // present on the add-on's own queue (read when a swap chain is set up)
    int   pacingMode = 1;     // 0 = smooth (every game frame shown once, in order), 1 = lowest latency (newest)
    int   maxAhead = 1;       // game may run this many frames ahead of the GPU (0 = no limit)
    int   smoothCatchUp = 0;  // Smooth: 1 = skip a frame when two have waited for 3 cycles (standing latency)
    int   autoAhead = 1;      // automatic: 1 frame ahead, 2 while the game keeps missing strobe cycles
    int   presBuffers = 3;    // real swap chain buffers for the presenter (read when presentation moves)
    int   queueAhead = 0;     // with vblank pacing: keep this many extra presents queued (0 or 1)
    int   laterRelease = 0;   // Lowest latency + Refresh cap: release the game just in time for the pick
    int   capJit = 1;         // Half / Quarter cap: release just in time for the lit refresh (both pacing modes)
    int   autoCap = 1;        // Refresh cap: one game frame per strobe cycle while the game can't hold the refresh rate
    int   reflexAlign = 0;    // Reflex frame limit at the sleep point lined up with the lit refresh's pick (test)
    int   reflexAlignCoarse = 1; // ReflexAlignCoarse: frames ready far ahead of the pick are moved closer once (ReflexAlign off)
    int   capReflex = 0;      // the game frame cap applied with Reflex's frame limit in games that use Reflex (opt-in)
    int   autoCapOwn = 0;     // AutoCapOwnWithoutFg: 0 = Reflex games step down with Reflex's limit; 1 = the presenter's own cap while frame generation is off
    int   learnPerGame = 1;      // LearnPerGame: the automatic frame cap starts from what it found for the game last time
    int   autoCapFrameStart = 1; // AutoCapAtFrameStart: the automatic frame cap without Reflex holds the game before its next frame starts
    int   fgLimitNoReflex = 1; // FrameGenLimitWithoutReflex: frame generation without Reflex limited to one presented frame per strobe cycle
    int   reflexAtSleep = 1;  // ReflexLimitAtSleep: pace the game's own frames in its Reflex sleep call instead of the driver's limit
                              // (with frame generation: Reflex's frame limit at one presented frame per cycle)
    float laterReleaseMs = 1.5f; // safety margin on top of the measured build time (widened automatically)
    int   deferRecreated = 1; // recreated swap chains: no BFI until the game resizes (safe)
    int   virtualStats = 1;   // report frame statistics in terms of the game's own frames
    int   reuseBuffers = 1;   // present an already-drawn buffer again instead of redrawing it
    int   flashHints = 1;     // overlay: suggest the next step when refreshes keep being missed (flashes)
    int   waitResizeAll = 1;  // every swap chain: no BFI until the game first resizes it
    int   bypassResizeHooks = 1; // move to the presenter queue with DXGI's own ResizeBuffers1
    int   showOverlays = 0;   // 1 = the presenter's presents go through overlay hooks (RTSS OSD, Steam overlay
                              // draw on the strobed frames); read once, at the presenter's first present
    int   ablGuard = 1;       // lower the boost while a lit refresh would be too bright overall (panel ABL)
    int   ablNits = 700;      // lit-refresh ceiling (nits) while the guard is engaged
    int   ablTrigNits = 400;  // (v0.51 area mode, kept for the ini only)
    int   ablBudget = 250;    // panel budget: average brightness (nits) of a lit refresh after capping
    char  ablCurve[128] = {}; // AblCurve: the panel's window curve, e.g. "10:1400,25:600,50:400,100:250" (area %:nits); empty = from DisplayPeakNits and the budget
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
// LCD saver in Gradual mode: even cycles only (odd ones alternate by themselves), plain strobe only
// (sine, inverted and rolling-scan cycles keep the Immediate swap).
static bool GradualSaverOn(const Settings &s)
{
    return s.lcdSwap && s.lcdGradual && s.enabled && (s.frames % 2) == 0 && !s.sine && !s.invert && !s.rolling;
}
static std::atomic<int> g_gameCap{1};
static std::atomic<int> g_capPer{1};          // refreshes per game frame for the cap (0 = no cap)
// Refreshes per game frame for a cap setting: Refresh 1, Half 2, Quarter 4,
// per strobe cycle = Frames, custom = CapDivisor.
static int CapPeriod(int cap, int frames, int divisor)
{
    return cap == 1 ? 1 : cap == 2 ? 2 : cap == 3 ? 4 : cap == 4 ? frames : cap == 5 ? divisor : 0;
}
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
static double              g_lrDoneT[kLr] = {};    // when game frame id's copy finished on the GPU (ReflexAlign)
static UINT64              g_lrDoneId[kLr] = {};
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
    s.lcdSwap  = IniInt("LcdPhaseSwap", 0) ? 1 : 0;
    s.lcdSwapSec = IniInt("LcdPhaseSwapSeconds", 5);
    s.lcdGradual = IniInt("LcdSaverGradual", 0) ? 1 : 0;
    s.lcdGradSec = IniInt("LcdGradualSeconds", 1000);
    if (s.lcdGradSec < 20) s.lcdGradSec = 20;
    if (s.lcdGradSec > 1200) s.lcdGradSec = 1200;
    s.lcdHoldSec = std::min(600, std::max(0, IniInt("LcdGradualHoldSeconds", 120)));
    s.lcdDriftSec = std::min(60, std::max(2, IniInt("LcdGradualDriftSeconds", 10)));
    if (s.lcdSwapSec < 1) s.lcdSwapSec = 1;
    if (s.lcdSwapSec > 300) s.lcdSwapSec = 300;
    s.rollSeam = IniFloat("RollingSeam", 0.0f);
    if (s.rollSeam < 0.0f) s.rollSeam = 0.0f;
    if (s.rollSeam > 0.5f) s.rollSeam = 0.5f;
    s.rollDecay = IniFloat("RollingPhosphorDecay", 0.2f);
    if (s.rollDecay < 0.0f) s.rollDecay = 0.0f;
    if (s.rollDecay > 1.0f) s.rollDecay = 1.0f;
    s.knee     = IniFloat("RolloffKnee", 0.75f);
    s.peakNits = IniFloat("DisplayPeakNits", 1400.0f);
    s.gameCap  = IniInt("CapGameToRefresh", 1);
    if (s.gameCap < 0 || s.gameCap > 5) s.gameCap = 1;
    s.capDivisor = IniInt("CapDivisor", 3);
    if (s.capDivisor < 1) s.capDivisor = 1;
    if (s.capDivisor > 8) s.capDivisor = 8;
    s.ownQueue = IniInt("PresentOnOwnQueue", 1);
    s.pacingMode = IniInt("FramePacing", 1) ? 1 : 0;
    s.maxAhead = IniInt("MaxFramesAhead", 1);
    s.autoAhead = IniInt("AutoFramesAhead", 1) ? 1 : 0;
    s.smoothCatchUp = IniInt("SmoothCatchUp", 0) ? 1 : 0;
    s.presBuffers = IniInt("PresenterBuffers", 3);
    s.queueAhead = IniInt("QueueAhead", 0) ? 1 : 0;
    s.laterRelease = IniInt("LaterRelease", 0) ? 1 : 0;
    s.capJit = IniInt("CapJustInTime", 1) ? 1 : 0;
    s.autoCap = IniInt("AutoFrameCap", 1) ? 1 : 0;
    s.capReflex = IniInt("CapWithReflex", 0) ? 1 : 0;
    s.reflexAlign = IniInt("ReflexAlign", 0) ? 1 : 0;
    s.reflexAlignCoarse = IniInt("ReflexAlignCoarse", 1) ? 1 : 0;
    s.reflexAtSleep = IniInt("ReflexLimitAtSleep", 1) ? 1 : 0;
    s.fgLimitNoReflex = IniInt("FrameGenLimitWithoutReflex", 1) ? 1 : 0;
    s.autoCapFrameStart = IniInt("AutoCapAtFrameStart", 1) ? 1 : 0;
    s.learnPerGame = IniInt("LearnPerGame", 1) ? 1 : 0;
    s.autoCapOwn = IniInt("AutoCapOwnWithoutFg", 0) ? 1 : 0;
    s.laterReleaseMs = IniFloat("LaterReleaseMarginMs", 1.5f);
    if (s.laterReleaseMs < 0.5f) s.laterReleaseMs = 0.5f;
    if (s.laterReleaseMs > 5.0f) s.laterReleaseMs = 5.0f;
    s.deferRecreated = IniInt("WaitForResizeOnRecreatedSwapChain", 1) ? 1 : 0;
    s.virtualStats = IniInt("GameFrameStatistics", 1) ? 1 : 0;
    s.reuseBuffers = IniInt("ReuseDrawnBuffers", 1) ? 1 : 0;
    s.flashHints = IniInt("FlashHints", 1) ? 1 : 0;
    s.waitResizeAll = IniInt("StartBFIAfterGameResize", 1) ? 1 : 0;
    s.bypassResizeHooks = IniInt("BypassOverlayResizeHooks", 1) ? 1 : 0;
    s.showOverlays = IniInt("ShowOverlays", 0) ? 1 : 0;
    s.reportTyped = IniInt("TypedBackBufferDesc", 1) ? 1 : 0;
    s.hitchGuard = IniInt("HitchGuard", 1) ? 1 : 0;
    s.ablGuard = IniInt("AblGuard", 1) ? 1 : 0;
    s.ablNits = IniInt("AblGuardCapNits", 700);
    if (s.ablNits < 200) s.ablNits = 200;
    if (s.ablNits > 2000) s.ablNits = 2000;
    s.ablBudget = IniInt("AblGuardBudgetNits", 250);
    if (s.ablBudget < 100) s.ablBudget = 100;
    if (s.ablBudget > 1500) s.ablBudget = 1500;
    GetPrivateProfileStringA("BFI", "AblCurve", "", s.ablCurve, sizeof(s.ablCurve), g_iniPath);
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
    g_capPer.store(CapPeriod(s.gameCap, s.frames, s.capDivisor));
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
    static std::atomic<bool> upgraded{false};
    if (!upgraded.exchange(true)) UpgradeIni(s);
    EnterCriticalSection(&g_cfgLock);
    g_shared = s;
    LeaveCriticalSection(&g_cfgLock);
}
static DWORD WINAPI LoadIniThread(LPVOID) { LoadShared(); return 0; }

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
    IniWriteInt("LcdPhaseSwap", s.lcdSwap);
    IniWriteInt("LcdPhaseSwapSeconds", s.lcdSwapSec);
    IniWriteInt("LcdSaverGradual", s.lcdGradual);
    IniWriteInt("LcdGradualSeconds", s.lcdGradSec);
    IniWriteInt("LcdGradualHoldSeconds", s.lcdHoldSec);
    IniWriteInt("LcdGradualDriftSeconds", s.lcdDriftSec);
    IniWriteFloat("RollingSeam", s.rollSeam);
    IniWriteFloat("RollingPhosphorDecay", s.rollDecay);
    IniWriteFloat("RolloffKnee", s.knee);
    IniWriteFloat("DisplayPeakNits", s.peakNits);
    IniWrite("Output", s.output == 0 ? "sdr" : s.output == 1 ? "scrgb" : s.output == 2 ? "hdr10" : "auto");
    IniWriteInt("CapGameToRefresh", s.gameCap);
    IniWriteInt("CapDivisor", s.capDivisor);
    IniWriteInt("PresentOnOwnQueue", s.ownQueue);
    IniWriteInt("FramePacing", s.pacingMode);
    IniWriteInt("MaxFramesAhead", s.maxAhead);
    IniWriteInt("AutoFramesAhead", s.autoAhead);
    IniWriteInt("SmoothCatchUp", s.smoothCatchUp);
    IniWriteInt("PresenterBuffers", s.presBuffers);
    IniWriteInt("QueueAhead", s.queueAhead);
    IniWriteInt("LaterRelease", s.laterRelease);
    IniWriteInt("CapJustInTime", s.capJit);
    IniWriteInt("AutoFrameCap", s.autoCap);
    IniWriteInt("CapWithReflex", s.capReflex);
    IniWriteInt("ReflexAlign", s.reflexAlign);
    IniWriteInt("ReflexAlignCoarse", s.reflexAlignCoarse);
    IniWriteInt("FrameGenLimitWithoutReflex", s.fgLimitNoReflex);
    IniWriteInt("AutoCapAtFrameStart", s.autoCapFrameStart);
    IniWriteInt("LearnPerGame", s.learnPerGame);
    IniWriteFloat("LaterReleaseMarginMs", s.laterReleaseMs);
    IniWriteInt("WaitForResizeOnRecreatedSwapChain", s.deferRecreated);
    IniWriteInt("GameFrameStatistics", s.virtualStats);
    IniWriteInt("ReuseDrawnBuffers", s.reuseBuffers);
    IniWriteInt("FlashHints", s.flashHints);
    IniWriteInt("AutoCapOwnWithoutFg", s.autoCapOwn);
    IniWriteInt("StartBFIAfterGameResize", s.waitResizeAll);
    IniWriteInt("BypassOverlayResizeHooks", s.bypassResizeHooks);
    IniWriteInt("ShowOverlays", s.showOverlays);
    IniWriteInt("PresenterQueuePriority", s.queuePriority);
    IniWriteInt("TypedBackBufferDesc", s.reportTyped);
    IniWriteInt("HitchGuard", s.hitchGuard);
    IniWriteInt("AblGuard", s.ablGuard);
    IniWriteInt("AblGuardCapNits", s.ablNits);
    IniWriteInt("AblGuardArea", s.ablArea);
    IniWriteInt("AblGuardBudgetNits", s.ablBudget);
    IniWrite("AblCurve", s.ablCurve);
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
    double alignSlackMs = -1.0, alignShiftMs = 0.0;   // ReflexAlign: frames ready before the pick (10th percentile), total shift
    int    unevenRep = 0, unevenSkip = 0;           // per second: strobe cycles one or more frames short / ahead of the expected step
    int    slotWaits = 0;                       // per second: game copies that waited for the presenter
    bool   vsyncOverridden = false;             // presents far faster than the display: driver ignores VSync
    int    queued = -1;                         // presents queued but not yet on screen (-1 = unknown)
    bool   hitchPaused = false;                 // hitch guard has BFI paused
    double displayHz = 0.0;                     // the display's own refresh rate (frame statistics)
    bool   vrrSuspect = false;
    bool   limitSuspect = false;
    bool   glPresentBlocked = false;            // OpenGL: the presenter's own presents block (NVIDIA present method)
    int    lrState = 0;         // later release: 0 off, 1 measuring, 2 active, 3 paused (frames take too long)
    double lrEst = 0.0, lrMargin = 0.0;
    int    lrMisses = 0;
    int    aheadInUse = 1;
    int    fgLimitUs = 0;                       // frame limit for frame generation without Reflex: per rendered frame (0 = off)
    int    feLimitUs = 0;                       // automatic frame cap held at the frame start (no Reflex, no frame generation; 0 = off)
    int    autoCapDown = 0;                     // automatic frame cap: one game frame per strobe cycle in use
    int    autoCapProbe = 0;                    // automatic frame cap: trying the Refresh rate again
    int    fgActive = 0;                        // frame generation looks active (presented well above rendered frames)
    int    rfxGameUses = 0;                     // the game uses Reflex (latency reports)
    int    rfxLimitUs = 0;                      // Reflex frame limit applied by the presenter (0 = none)
    double rfxSimGpuMs = -1.0;                  // Reflex report: simulation start -> GPU finished (average)
    double relFlashMs = -1.0;                   // game released -> lit flash showing that frame (average)
    double rawAvg  = -1.0;                      // ABL guard: unclipped lit-refresh average (nits)
    double hotArea = -1.0;                      // ABL guard: percent of the screen whose lit refresh exceeds the cap
    double ceilInUse = 0.0;                     // ABL guard: lit-refresh ceiling in effect (nits)                  // display refreshing slower than it can, with no missed refreshes
    int    reused = 0;                          // per second: refreshes that reused an already-drawn buffer   // per second: strobe cycles with no new game frame / frames never shown
    int    deviceLost = 0;
    int    flashHint = 0;                       // missed refreshes keep coming back (3 of the last 10 s; off after 30 s without)
    int    presBufs = 0;                        // real swap chain buffers behind the presenter
    bool   vramOver = false;                    // the game uses more video memory than its budget (DXGI)
    double vramUse = 0.0, vramBudget = 0.0;     // MB
};
static Status g_status;
static char   g_hooksSeen[256] = "";               // other Present hooks found in the process
static char   g_fgSeen[256] = "";                  // frame-generation components found in the process
static char   g_scOwner[128] = "";                 // module that implements the swap chain ReShade sees
static std::atomic<bool> g_firstStaged{false}, g_firstPresented{false};
static std::atomic<bool> g_glActive{false};       // an OpenGL game is managed
static std::atomic<double> g_lastPresentT{0.0};   // presenter heartbeat
static std::atomic<bool>   g_stallLogged{false};
static std::atomic<int> g_stagedThisSecond{0};

// The game's presents per thread, counted in the swap-chain hook: frame generation without Reflex presents its
// generated frames from a thread of its own, so the game's own thread against all presents gives its factor.
struct ThreadCount { DWORD tid[6] = {}; long n[6] = {}; long other = 0; };
static SRWLOCK g_presCountLock = SRWLOCK_INIT;
static ThreadCount g_presCount;
// The game's own presents on the swap chain it holds, when that is a wrapper over the real one (OptiScaler's
// swap chain, frame generation behind it). FSR frame generation presents every frame from its own thread to the
// real swap chain, so only here are the game's frames still apart from the generated ones.
static ThreadCount g_outerCount;
// Game frames per present thread, every API, for the automatic frame cap at the frame start: it holds the thread that
// presented most in the last second (Obduction, Unreal Engine: thread 52480 presented at start, 50748 in game, and the
// hold stayed on the first -- 60-65 fps, held at the present by frames ahead instead, ~34 ms frame age against ~20).
static ThreadCount g_feCount;
static void **g_outerVtbl = nullptr;
// 1 = the game's frames are counted on FSR frame generation's swap chain (its own timings below); XeSS frame generation
// and anything else keep the hook on the real swap chain and the timings tested with it (Riven).
static std::atomic<int> g_fgOuterFsr{0};
// Frame generation without Reflex (FrameGenLimitWithoutReflex): the game's own presents (its first present thread) are paced after they
// return, one rendered frame per (strobe cycle x frame-generation factor) -- a frame-start sleep where Reflex would
// sleep; frame generation's presents (its own thread) pass. 0 = off.
static std::atomic<int>   g_fgPaceUs{0};
// What g_fgPaceUs paces: 1 = frame generation without Reflex; 2 = the automatic frame cap at one game frame per strobe
// cycle without Reflex or frame generation, held before the game's next frame starts instead of at its present
// (God of War Ragnarok at 60 of 120 Hz: about 65 ms from frame start to flash held at the present, about 33 ms with
// Reflex's limit at the frame start).
static std::atomic<int>   g_fgPaceKind{0};
static std::atomic<DWORD> g_fgGameTid{0};
static std::atomic<int>   g_fgShiftUs{0};     // phase step requested by the presenter (one refresh)
static void CountTo(ThreadCount &c)
{
    const DWORD tid = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_presCountLock);
    int k = 0;
    while (k < 6 && c.tid[k] && c.tid[k] != tid) k++;
    if (k < 6) { if (!c.tid[k]) c.tid[k] = tid; c.n[k]++; }
    else c.other++;
    ReleaseSRWLockExclusive(&g_presCountLock);
}
static void CountPresent()
{
    const DWORD tid = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_presCountLock);
    int k = 0;
    while (k < 6 && g_presCount.tid[k] && g_presCount.tid[k] != tid) k++;
    if (k < 6) { if (!g_presCount.tid[k]) g_presCount.tid[k] = tid; g_presCount.n[k]++; }
    else g_presCount.other++;
    ReleaseSRWLockExclusive(&g_presCountLock);
}



// The game's video memory use against the budget Windows gives it (DXGI, per process, the adapter with the most
// dedicated memory), in MB.
static bool VramMB(double &use, double &budget)
{
    static IDXGIAdapter3 *ad = nullptr; static bool tried = false;
    if (!tried)
    {
        tried = true;
        IDXGIFactory1 *fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac)) && fac)
        {
            IDXGIAdapter1 *a = nullptr; SIZE_T best = 0;
            for (UINT k = 0; fac->EnumAdapters1(k, &a) == S_OK; k++)
            {
                DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
                IDXGIAdapter3 *a3 = nullptr;
                if (d.DedicatedVideoMemory > best && SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&a3)) && a3)
                {
                    if (ad) ad->Release();
                    ad = a3; best = d.DedicatedVideoMemory;
                }
                a->Release();
            }
            fac->Release();
        }
    }
    if (!ad) return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
    if (FAILED(ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) return false;
    use = mi.CurrentUsage / 1048576.0; budget = mi.Budget / 1048576.0;
    return true;
}

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
static PFN_Present        o_OuterPresent = nullptr;    // the game-facing (outer) swap chain, see hk_OuterPresent
static PFN_Present1       o_OuterPresent1 = nullptr;
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
BFI_TLS(int, t_inOuter);            // inside the game-facing (outer) swap chain's Present
BFI_TLS(int, t_scCreateDepth);      // nested CreateSwapChain* calls through the factory hook (outermost = 1)
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
    int limitMissSecs = 0, limitMisses = 0, limitFitSecs = 0, limitTrips = 0; bool limitOff = false;   // just-in-time release switched off at its limit
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
            g_lrDoneT[k] = fin; g_lrDoneId[k] = id;
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
        if (state != prev && (state == 2 || state == 3) && !limitOff)
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
        // ... unless frames still miss their lit refresh that way: then
        // release at the refresh, as with just-in-time release off (a game
        // tested showed repeat-then-skip at the limit and was even
        // without it). It stays off until game frames have fitted the
        // interval for 30 seconds -- switching straight back on brought the
        // repeats back within a second -- and for the session once that has
        // happened twice.
        if (state == 3 && capMode && !limitOff)
        {
            // Misses counted over 20-second windows: the repeat-then-skip
            // hiccups come a few a minute, rarely in consecutive seconds.
            limitMisses += misses;
            if (++limitMissSecs >= 20) { limitMissSecs = 0; limitMisses = 0; }
            if (limitMisses >= 3)
            {
                limitOff = true; limitMisses = 0; limitMissSecs = 0; limitFitSecs = 0; limitTrips++;
                LOGI("BFI Presenter: just-in-time release off -- at its limit frames still missed their lit refresh; "
                     "released at the refresh, as with it off, %s",
                     limitTrips >= 2 ? "for the rest of this session" : "until game frames fit the interval for 30 s");
            }
        }
        else if (limitOff && limitTrips < 2)
        {
            limitFitSecs = (state != 0 && est + margin < period - 0.002) ? limitFitSecs + 1 : 0;
            if (limitFitSecs >= 30)
            {
                limitOff = false; limitFitSecs = 0;
                LOGI("BFI Presenter: just-in-time release back on -- game frames take %.1f ms", est * 1000.0);
            }
        }
        if (!allowed) { limitOff = false; limitTrips = 0; limitMisses = limitMissSecs = limitFitSecs = 0; }
        if (state == 3 && capMode && !limitOff) lead = period - 0.001;
        if (limitOff) lead = 0.0;
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
    float soften;     // 0..1: blend toward the steady average picture (LCD phase swap fade)
    float phase;      // LCD gradual saver: start of the lit window within the first sub-frame, 0..1
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

// LCD gradual saver (after the Blur Busters CRT simulator's slewing LCD Saver): the pixel's light above
// the floor is a box of height h and width w (w = 1 for BFI; wider when Variable MPRT spreads a bright
// pixel over several sub-frames) that starts at 'phase' and drifts slowly through the cycle. Sub-frame j
// gets the part of the box that overlaps it (wrapping within the cycle), so the light per cycle is unchanged.
float Overlap(float a, float w, float j, float n)
{
    float o = max(0.0, min(a + w, j + 1.0) - max(a, j));
    o += max(0.0, min(a + w, j + 1.0 + n) - max(a, j + n));
    return o;
}
float GradualGain(uint j, float litGain, float fl, float S)
{
    float E = litGain - fl;
    if (E <= 0.0) return fl;
    float n = float(frames);
    float h = min(E, max(S - fl, 1e-4));
    float w = E / h;
    if (w >= n) { w = n; h = E / n; }
    return fl + h * Overlap(phase, w, float(j), n);
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
        // Luminance weights of the output's primaries: BT.2020 for HDR10, BT.709 for SDR and scRGB.
        sum += dot(lin, outType == 2 ? float3(0.2627, 0.6780, 0.0593) : float3(0.2126, 0.7152, 0.0722)) * min(litGain, S);
    }
    return sum / 16.0;
}

// One pixel's light in sub-frame j of the cycle.
float3 Strobe(float3 lin, uint j, float litGain)
{
    if ((flags & 16) != 0)
    {
        if (method == 0)
        {
            // Roll the highlights off once, for the whole lit image, then share that light out: rolling off
            // each refresh's share separately compresses less as the light splits, and highlights pulsed
            // brighter with every drift step.
            float3 full = ((flags & 1) != 0) ? Rolloff(lin, litGain, ceilLin, knee) : lin * litGain;
            float3 base = lin * dfloor;
            return base + (full - base) * Overlap(phase, 1.0, float(j), float(frames));
        }
        float mg = max(max(lin.r, lin.g), lin.b);
        float Sg = (mg > 1e-6) ? max(ceilLin / mg, 1.0) : 1e6;
        return lin * GradualGain(j, litGain, dfloor, Sg);
    }
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
    if (soften > 0.0 && method < 2)
    {
        // LCD phase swap: fade toward the steady average picture (the hitch guard's), which keeps the light
        // of every cycle the same while the strobe's depth goes down and back up.
        float a = 0.0;
        [unroll] for (uint j = 0; j < 8; j++) if (j < frames) a += Target(j, litGain, dfloor);
        o = lerp(o, lin * (a / float(frames)), soften);
    }
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
    float    soften;      // 0..1: blend toward the steady average picture (LCD phase swap fade)
    float    phase;       // LCD gradual saver: start of the lit window within the first sub-frame, 0..1
};

// ABL guard: the screen average of the brightest refresh in the cycle when
// every 32x18 cell's lit value is capped at C. Plain strobe: the lit refresh.
// Rolling scan: refresh j lights band j only (plus, with Variable MPRT, what
// the other bands spill above C into their dark refreshes), so each refresh
// carries roughly 1/frames of the light. NOT used: on the A95L the panel's
// own ABL still dimmed bright areas with rolling scan on, so its limiter does
// not go by each refresh's average -- the guard keeps measuring as if the
// whole screen were lit, rolling or not.
// ABL guard window curve (#71): the brightness the panel sustains for a white area of a given size. An OLED's
// limit depends on how much of the screen is bright, not only on the average: a panel holding 250 nits full-screen
// may hold 600 on 25% of it and 1400 on 10%, so a 25% area at 800 nits trips its limiter at an average of only
// 200. Each cell counts as its brightness against the largest area the panel holds at that brightness (a 600-nit
// cell counts 4 on that curve), and the lit refresh may average at most 1. Without AblCurve the curve runs from
// DisplayPeakNits at 10% to AblGuardBudgetNits full-screen.
struct AblCurveT { int n = 0; double a[8] = {}, L[8] = {}; };   // area fraction (ascending), nits (non-increasing)

static AblCurveT AblMakeCurve(const char *spec, double peak, double budget)
{
    AblCurveT c;
    for (const char *p = spec; p && *p && c.n < 8;)
    {
        double ar = 0.0, nt = 0.0;
        if (sscanf(p, " %lf : %lf", &ar, &nt) == 2 && ar > 0.0 && ar <= 100.0 && nt >= 50.0 && nt <= 10000.0)
        {
            c.a[c.n] = ar / 100.0;
            c.L[c.n] = nt;
            c.n++;
        }
        const char *q = strchr(p, ',');
        if (!q) break;
        p = q + 1;
    }
    if (c.n == 0)
    {
        c.a[0] = 1.0; c.L[0] = budget; c.n = 1;
        if (peak > budget) { c.a[0] = 0.10; c.L[0] = peak; c.a[1] = 1.0; c.L[1] = budget; c.n = 2; }
        return c;
    }
    for (int i = 1; i < c.n; i++)                       // sort by area
        for (int j = i; j > 0 && c.a[j] < c.a[j - 1]; j--)
        {
            std::swap(c.a[j], c.a[j - 1]);
            std::swap(c.L[j], c.L[j - 1]);
        }
    for (int i = 1; i < c.n; i++) c.L[i] = std::min(c.L[i], c.L[i - 1]);   // a larger area never holds more
    if (c.a[c.n - 1] < 1.0)
    {
        if (c.n < 8) c.n++;
        c.a[c.n - 1] = 1.0;
        c.L[c.n - 1] = std::min(budget, c.L[c.n - 2]);   // no full-screen point: the budget
    }
    return c;
}

// How much of the panel's budget one cell at L nits uses: L / full-screen level up to that level, then 1 / (the
// largest area the panel holds at L), interpolated in log-log between the curve's points.
static double AblWeight(const AblCurveT &c, double L)
{
    const double full = c.L[c.n - 1];
    if (L <= full || c.n < 2) return L / full;
    int i = c.n - 2;
    while (i > 0 && L > c.L[i]) i--;
    double la;
    if (c.L[i] <= c.L[i + 1] * 1.0001) la = (L <= c.L[i]) ? log(c.a[i + 1]) : log(1e-3);
    else
    {
        const double t = (log(L) - log(c.L[i])) / (log(c.L[i + 1]) - log(c.L[i]));
        la = log(c.a[i]) + t * (log(c.a[i + 1]) - log(c.a[i]));
    }
    return 1.0 / std::max(exp(la), 1e-3);
}

// Average budget use of a lit refresh with every cell capped at C (1 = at the panel's limit).
static double AblLoadAt(const float *cells, double C, const AblCurveT &curve)
{
    double a = 0.0;
    for (int i = 0; i < 32 * 18; i++) a += AblWeight(curve, std::min((double)cells[i], C));
    return a / (32.0 * 18.0);
}

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

#ifdef _WIN64
// True when the stack from rsp up is committed, readable memory; top is where
// that memory ends (the stack's base for a real stack). A thread's stack walk
// only reads between rsp and its base, so a context pointing anywhere else (a
// thread that exited and freed its stack, a corrupt frame) stops the walk
// instead of faulting: the watchdog once crashed a UE4 game reading a stack
// that was no longer there.
static bool StackReadable(DWORD64 rsp, DWORD64 &top)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!rsp || (rsp & 7) || VirtualQuery((const void *)(uintptr_t)rsp, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    top = (DWORD64)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return rsp + 8 <= top;
}

// x64 stack walk with the unwind tables, kept to readable stack memory: each
// frame must sit higher on the stack than the one before (a walk that goes
// round in circles or jumps off the stack stops there).
static int UnwindStack(CONTEXT ctx, DWORD64 *frames, int max)
{
    int nf = 0;
    DWORD64 top = 0;
    if (ctx.Rip < 0x10000 && StackReadable(ctx.Rsp, top))
    {
        // Call through a null (or tiny) pointer: the caller's return address
        // is on top of the stack.
        frames[nf++] = ctx.Rip;
        ctx.Rip = *(DWORD64 *)ctx.Rsp;
        ctx.Rsp += 8;
    }
    for (; nf < max && ctx.Rip; nf++)
    {
        frames[nf] = ctx.Rip;
        if (!InImage(ctx.Rip)) { nf++; break; }               // JIT code: don't ask the runtime to unwind
        if (!StackReadable(ctx.Rsp, top)) { nf++; break; }
        const DWORD64 rsp = ctx.Rsp;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!fn) { ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8; }
        else
        {
            PVOID hd = nullptr;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &hd, &est, nullptr);
        }
        if (ctx.Rsp <= rsp || ctx.Rsp > top) { nf++; break; }
    }
    return nf;
}
#endif

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
    DWORD code = 0;
    if (!GetExitCodeThread(th, &code) || code != STILL_ACTIVE) { CloseHandle(th); return; }   // gone: its stack may be freed
    if (SuspendThread(th) != (DWORD)-1)
    {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx))
        {
#ifndef _WIN64
            nf = WalkFramePointers(ctx, frames, 32);
#else
            nf = UnwindStack(ctx, frames, 32);
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
    nf = UnwindStack(ctx, frames, 24);
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

// ---------------------------------------------------------------- learned per game
// LearnPerGame: what the automatic frame cap finds for a game -- one game frame per strobe cycle or the Refresh rate,
// and the frame start time -- is kept in BFIPresenter_learned.ini next to BFIPresenter.ini, one section per game, and
// the next session starts from it instead of finding it again (seconds of uneven frames before the cap steps down,
// about 27 s for the frame start). The section is the executable and the window title without digits, so each game
// of an emulator gets its own and fps counters or version numbers in a title don't make new ones. Used only at the same
// refresh rate and frames per strobe cycle; what is learned is still checked as usual (tries at Refresh rate, a new
// search after lost frames) and saved again when it changes. The file and the window title are read on a helper
// thread: file access on the presenter's thread could miss a refresh, and GetWindowText waits on a hung window.
struct LearnRec
{
    int    down = -1;       // 1 = one game frame per strobe cycle, 0 = Refresh rate, -1 = not known
    double fsMs = 0.0;      // frame start: > 0 the start time found (ms), -2 not needed, -3 the game runs slower than the strobe, 0 not known
    double bestMs = 0.0;    // release -> flash at that start time
    double hz = 0.0;        // the refresh rate and frames per strobe cycle it was learned at
    int    frames = 0;
};
static SRWLOCK     g_learnLock = SRWLOCK_INIT;
static std::string g_learnKey;          // section in use ("" = none read yet)
static LearnRec    g_learnRead;         // what the file had for it
static int         g_learnGen = 0;      // bumped each time the section is read
static LearnRec    g_learnOut;          // to be written to it
static bool        g_learnDirty = false;
// "Forget this game" in the overlay: the helper thread deletes the game's section, the presenter starts over.
static std::atomic<bool> g_learnForgetFile{false}, g_learnForgetNow{false};

static void LearnPost(const LearnRec &r)
{
    AcquireSRWLockExclusive(&g_learnLock);
    g_learnOut = r;
    g_learnDirty = true;
    ReleaseSRWLockExclusive(&g_learnLock);
}

// The section read last, if newer than seenGen; returns its generation.
static int LearnFetch(int seenGen, LearnRec &r)
{
    AcquireSRWLockShared(&g_learnLock);
    const int g = g_learnGen;
    if (g != seenGen) r = g_learnRead;
    ReleaseSRWLockShared(&g_learnLock);
    return g;
}

static std::string LearnKeyFor(const char *exe, HWND wnd)
{
    typedef int (WINAPI *PFN_InternalGetWindowText)(HWND, LPWSTR, int);
    static PFN_InternalGetWindowText igwt =
        (PFN_InternalGetWindowText)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "InternalGetWindowText");
    char title[512] = "";
    if (wnd)
    {
        wchar_t w[256] = L"";
        int n = igwt ? igwt(wnd, w, 256) : 0;
        // Fallback (InternalGetWindowText missing or empty): WM_GETTEXT, given up on a hung window.
        DWORD_PTR got = 0;
        if (n <= 0 && SendMessageTimeoutW(wnd, WM_GETTEXT, 256, (LPARAM)w, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &got)) n = (int)got;
        if (n > 0) { w[255] = 0; WideCharToMultiByte(CP_UTF8, 0, w, -1, title, sizeof(title), nullptr, nullptr); }
    }
    std::string t;
    for (const unsigned char *p = (const unsigned char *)title; *p; p++)
    {
        const unsigned char ch = *p;
        if (ch >= 0x80 || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '\'' || ch == '&' || ch == '-') t += (char)ch;
        else if (!t.empty() && t.back() != ' ') t += ' ';
    }
    while (!t.empty() && (t.back() == ' ' || t.back() == '-')) t.pop_back();
    if (t.size() > 100) t.resize(100);
    std::string k = exe;
    if (!t.empty()) k += " - " + t;
    return k;
}

// Helper thread, every 2 s: writes what the presenter posted, then reads the section again when the game (its window
// title) changed.
static void LearnTick(const char *path, const char *exe, HWND wnd)
{
    const std::string key = LearnKeyFor(exe, wnd);
    const bool forget = g_learnForgetFile.exchange(false);
    AcquireSRWLockExclusive(&g_learnLock);
    const bool dirty = g_learnDirty && !forget;
    const LearnRec out = g_learnOut;
    const std::string cur = g_learnKey;
    g_learnDirty = false;
    ReleaseSRWLockExclusive(&g_learnLock);
    char b[64];
    if (dirty && !cur.empty())
    {
        const char *sec = cur.c_str();
        // Whole numbers (us, mHz): a game that sets its own locale reads "120.000" as 120 thousand or 120.
        snprintf(b, sizeof(b), "%d", out.down);                               WritePrivateProfileStringA(sec, "OneFramePerCycle", b, path);
        snprintf(b, sizeof(b), "%ld", (long)std::lround(out.fsMs * 1000.0));   WritePrivateProfileStringA(sec, "FrameStartUs", b, path);
        snprintf(b, sizeof(b), "%ld", (long)std::lround(out.bestMs * 1000.0)); WritePrivateProfileStringA(sec, "ReleaseToFlashUs", b, path);
        snprintf(b, sizeof(b), "%ld", (long)std::lround(out.hz * 1000.0));     WritePrivateProfileStringA(sec, "RefreshMilliHz", b, path);
        snprintf(b, sizeof(b), "%d", out.frames);                             WritePrivateProfileStringA(sec, "Frames", b, path);
    }
    if (forget && !cur.empty())
    {
        WritePrivateProfileStringA(cur.c_str(), nullptr, nullptr, path);     // the whole section
        AcquireSRWLockExclusive(&g_learnLock);
        g_learnRead = LearnRec();
        g_learnGen++;
        ReleaseSRWLockExclusive(&g_learnLock);
    }
    if (key == cur) return;
    LearnRec r;
    const char *sec = key.c_str();
    // GetPrivateProfileInt returns 0 for negative values: read as text.
    auto get = [&](const char *name, long def) -> long
    {
        char v[32];
        GetPrivateProfileStringA(sec, name, "", v, sizeof(v), path);
        char *end = nullptr;
        const long x = strtol(v, &end, 10);
        return end && end != v ? x : def;
    };
    r.frames = (int)get("Frames", 0);
    if (r.frames > 0)
    {
        r.down   = (int)get("OneFramePerCycle", -1);
        r.fsMs   = get("FrameStartUs", 0) / 1000.0;
        r.bestMs = get("ReleaseToFlashUs", 0) / 1000.0;
        r.hz     = get("RefreshMilliHz", 0) / 1000.0;
    }
    AcquireSRWLockExclusive(&g_learnLock);
    g_learnKey = key;
    g_learnRead = r;
    g_learnGen++;
    g_learnDirty = false;     // posted for the previous game: the presenter posts again for this one
    ReleaseSRWLockExclusive(&g_learnLock);
}

// Last time the watchdog saw the game's window not responding (Windows' 5 s test): the frame-start search and its
// held start time don't count lost frames then or for 10 s after (Obduction: streaming stalls set off two searches in a
// minute, both finding the start time already in use).
static std::atomic<double> g_windowHungT{-1e9};

static DWORD WINAPI WatchdogThread(void *)
{
    int lastPres = -1, lastGame = -1;
    double lastPresT = 0.0, lastGameT = 0.0;
    int learnTicks = 0;
    char exe[MAX_PATH] = "";
    {
        char full[MAX_PATH] = "";
        GetModuleFileNameA(nullptr, full, MAX_PATH);
        const char *b = strrchr(full, '\\');
        strncpy(exe, b ? b + 1 : full, MAX_PATH - 1);
    }
    for (;;)
    {
        Sleep(500);
        DrainInfoQueue();
        const double t = Now();
        // Learned per game: written and read here, every 2 s, once the game has a window.
        if (++learnTicks % 4 == 0 && g_iniPath[0])
        {
            EnterCriticalSection(&g_cfgLock);
            const bool on = g_shared.learnPerGame && g_shared.autoCap;
            LeaveCriticalSection(&g_cfgLock);
            HWND gw = g_watchHwnd.load();
            if (on && gw)
            {
                char path[MAX_PATH];
                strncpy(path, g_iniPath, MAX_PATH - 1); path[MAX_PATH - 1] = 0;
                if (char *sl = strrchr(path, '\\')) sl[1] = 0; else path[0] = 0;
                strncat(path, "BFIPresenter_learned.ini", MAX_PATH - strlen(path) - 1);
                LearnTick(path, exe, GetAncestor(gw, GA_ROOT));
            }
        }
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
            if (hung) g_windowHungT.store(Now());
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

// When each game frame was released to the game (it starts rendering it then):
// the presenter logs release -> lit flash for every frame it shows.
static const int     kRel = 64;
static UINT64        g_relId[kRel] = {};
static double        g_relT[kRel] = {};

// NVIDIA Reflex (NvAPI): see the Reflex section further down.
struct ReflexSecond
{
    int    frames = 0;           // latency reports this second (0 = the game doesn't use Reflex)
    double simGpuMs = -1.0;      // simulation start -> GPU finished
    double simPresentMs = -1.0;  // simulation start -> present call finished
    double inputGpuMs = -1.0;    // input sampled -> GPU finished (games that mark input)
    double gpuFrameMs = -1.0;    // GPU frame time
    double gpuActiveMs = -1.0;   // GPU busy with the frame
    double simMs = -1.0;         // simulation (CPU) time
    double submitMs = -1.0;      // render submission (CPU) time
    double queueMs = -1.0;       // time in the OS render queue
    double renderedFps = -1.0;   // rendered frames a second (frame numbers)
    int    limitUs = 0;          // Reflex frame limit in effect
};
static void ReflexTick(int limitUs, ReflexSecond &out);
static std::atomic<int> g_sleepShiftUs{0};    // ReflexAlign: move the next wake-up by this much (+ later), taken once
static std::atomic<int> g_sleepLimitUs{0};    // frame limit at the game's Reflex sleep, per rendered frame (0 = off)
static std::atomic<bool> g_sleepActive{false};        // the sleep-point limit is in charge: the game's own limit is taken from the driver
static std::atomic<int>  g_gameOwnUs{0};              // the game's own Reflex limit, applied at the sleep point instead
static std::atomic<bool> g_sleepFgStrip{false};  // frame generation: the game's own Reflex limit is taken from the driver and not applied
static std::atomic<int> g_sleepFgFactor{1};   // presents per rendered frame (frame generation), for the sleep-point limit
static IUnknown *ReflexOwnDevice();
// The game collapsed under a Reflex frame limit (Returnal: 60 -> 7.5 fps, ours or its own).
// For the rest of the process: no limit from the presenter, and the game's own limit is
// taken out too (kept across swap chain changes, which recreate the presenter's state).
static std::atomic<bool> g_rfxLimitBad{false};
static int ReflexGameLimitUs();

// Frame generation that presents its own frames (DLSS-G, FSR 3 FG, XeSS FG): its
// DLL is loaded and it has set the game's frame latency to 2 (DLSS-G does when it
// takes over presenting, Control: Resonant). The automatic frame cap stays at
// Refresh then: holding DLSS-G's presents back broke its pacing (Onimusha).
static std::atomic<double> g_fgPresentT{0.0};   // last time a frame-generation DLL was seen presenting
static std::atomic<bool>   g_fgCallerDlss{false};  // ... and it was DLSS-G's (also on the stack without frame generation)
static std::atomic<double> g_rfxRenderedFps{0.0}, g_rfxRenderedT{0.0};   // rendered frames/s from Reflex frame numbers
// Every quarter second of game presents: is a frame-generation DLL on the call stack? DLSS-G presents its
// real and generated frames from its own thread (sl.dlss_g / nvngx_dlssg); without frame
// generation the game's own thread presents. The DLL being loaded alone said nothing: God of
// War loads DLSS-G and sets a frame latency of 2 with frame generation off.
static void FgCheckCaller()
{
    // Every quarter second, not every 30th present: at the ~8 fps Returnal fell to with frame generation,
    // 30 presents took ~4 s, longer than the 2 s the result counts for, so frame generation read on, off,
    // on... every two seconds, the frame cap and the limit with it.
    static std::atomic<double> lastT{0.0};
    const double now = Now();
    if (now - lastT.load() < 0.25) return;
    lastT.store(now);
    static const char *k[] = { "nvngx_dlssg.dll", "sl.dlss_g.dll", "dlssg_to_fsr3_amd_is_better.dll",
                               "ffx_frameinterpolation_x64.dll", "libxess_fg.dll", "amd_fidelityfx_dx12.dll" };
    HMODULE mods[6] = {};
    bool any = false;
    for (int i = 0; i < 6; i++) { mods[i] = GetModuleHandleA(k[i]); any = any || mods[i]; }
    if (!any) return;
    void *frames[16];
    const USHORT got = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
    for (USHORT f = 0; f < got; f++)
    {
        HMODULE m = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[f], &m)) continue;
        for (int i = 0; i < 6; i++)
            if (mods[i] && m == mods[i]) { g_fgCallerDlss.store(i < 3); g_fgPresentT.store(Now()); return; }
    }
}
static bool FgModuleLoaded()
{
    static const char *k[] = { "nvngx_dlssg.dll", "sl.dlss_g.dll", "dlssg_to_fsr3_amd_is_better.dll",
                               "ffx_frameinterpolation_x64.dll", "libxess_fg.dll" };
    for (const char *d : k) if (GetModuleHandleA(d)) return true;
    return false;
}

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
    // LCD phase swap: refreshes held back from the strobe count. Each one repeats the cycle's last (dark)
    // refresh once, so with an even cycle the lit refresh moves to the panel's other drive polarity.
    uint64_t phaseShift = 0;
    double   lastPhaseSwapT = 0.0;
    int      swapState = 0;          // 0 idle, 1 fading to the steady picture, 2 fading back
    float    soften = 0.0f;          // current fade (0 = full strobe, 1 = steady average)
    double   gradPhase = 0.0;        // LCD gradual saver: lit window start within the first sub-frame, 0..1
    double   gradHold = -1.0;        // LCD gradual saver at 2 frames: refreshes left to hold sharp (-1 = start a hold)
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
    // Uneven strobe cycles over the last 30 seconds (one count per second), so
    // scattered repeat-then-skip hiccups bring 2 frames ahead back as well as a
    // run of bad seconds (e.g. ~16 a minute at 1, none at 2).
    int repWin[30] = {}, repIdx = 0, repSum = 0, repSecsRun = 0;   // repSecsRun: the first seconds (loading) don't count
    int lastOutType = -1;
    double ceilNow = -1.0, ceilTarget = -1.0, hotArea = -1.0, ablSolve = -1.0, ablT = Now();
    double rawAvg = -1.0, ablLowT = 0.0;      // unclipped lit average; last time the guard was pulling down
    double aheadDropT = -1.0;      // when automatic frames ahead last went back to 1
    bool   aheadLocked = false;    // settled at 2 for this swap chain (no more switching)
    // Automatic frame cap: at the Refresh cap, one game frame per strobe cycle
    // while the game can't hold the refresh rate (frames between the strobe rate
    // and the refresh rate were dropped unevenly: judder). Back to Refresh when
    // the game's Reflex reports show its frames fit a refresh again, or, without
    // them, after a short try at Refresh that holds the refresh rate (tries 30 s,
    // 60 s, then every 120 s while they keep failing).
    bool   autoCapDown = false, capProbe = false;
    int    capDownSecs = 0, capUpSecs = 0, probeSecs = 0, probeGood = 0, userCap = 1;
    int    statPer = 1;           // refreshes per game frame of the cap in effect (for the uneven-cycle counts)
    double capSwitchT = -100.0, capUpT = -100.0, probeWait = 30.0, stayMin = 10.0;
    bool   fgWas = false;
    bool   capByReflex = false;
    bool   reflexCap = false;     // the game frame cap applied with Reflex's frame limit (CapWithReflex)
    double rcOnT = -100.0, fpsAtRc = 0.0;
    double rfxChkT = -100.0;      // start of the collapse check after a step-down with Reflex's limit (moved on by stalls)
    int    rcBadSecs = 0;
    int    rfxBadSecs = 0, gameLimBadSecs = 0;   // seconds collapsed under our Reflex limit / the game's own
    double fpsGoodT = -100.0;     // last second the game held at least half the strobe rate
    // Learned per game (LearnPerGame): read on the watchdog thread, applied after 5 s with the automatic cap in use
    // (so frame generation and Reflex are known first).
    LearnRec learnRead, learnMine;
    // A remembered one-frame-per-strobe-cycle cap waiting to be applied: the game runs at the Refresh cap meanwhile and
    // misses strobe cycles, which aren't counted toward frames ahead (Shadow of the Tomb Raider: frames ahead went to 2
    // a second before the remembered cap came on).
    bool learnHoldAhead = false;
    // The frame-start search is trying start times (some lose frames on purpose): not counted toward frames ahead,
    // as in the Vulkan layer (Shadow of the Tomb Raider went to 2 during one and stayed there for the session).
    bool swSearching = false;
    int    learnGen = 0, learnWarm = 0, learnSavedDown = -1;
    bool   learnApply = false, learnSwNow = false, learnSwReset = false;
    double fpsAtDown = 0.0;       // game fps when the automatic frame cap stepped down   // the automatic frame cap steps down with Reflex's frame limit (game uses Reflex)
    int    fgVotes = 0;           // seconds in a row the counts said the frame-generation state changed
    double fpsPrevSec = -1.0;
    // ReflexAlign: how long before the lit refresh's pick each new game frame was ready, and the controller.
    double alignSlack[256]; int alignN = 0;
    double alignTarget = 0.002, alignShiftTot = 0.0, alignCoolT = 0.0, alignLastLog = -1.0;
    int    alignCleanSecs = 0, alignExcessSecs = 0;
    double alignLogSlack = -1.0, alignPrevP10 = -1.0, alignLastStep = 0.0;
    double phaseUntil = 0.0, phaseNextT = 0.0;   // Reflex limit phase nudge (see the per-second block)
    int    phaseNudges = 0;     // last second's presents (goes with the rendered count taken then)
    ReflexSecond rsLast;           // last second's Reflex reports (frames == 0: the game doesn't use Reflex)
    double rsEverT = -1.0;         // last time this swap chain had Reflex reports
    bool   fgEarlyLogged = false;  // the "Reflex counts not in yet" line was written
    double rsLastT = -100.0;
    // Release -> lit flash of the frames shown this second (timing log, Reflex tests).
    bool   pickNew = false;
    double pickRelT = 0.0, relSum = 0.0, relMax = 0.0;
    int    relN = 0;
    FILE  *rfxf = nullptr;         // BFIPresenter_reflex_*.csv, one line a second while the timing log is on
    bool refillNow = false;          // present again without waiting for the next vblank
    bool vsyncOverridden = false;    // the driver is not honouring the presenter's VSync
    int  vsyncFastSecs = 0;          // seconds in a row with presents well above the refresh count
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
    int    secSlowPres = 0, glBlockSecs = 0;   // presents that blocked for most of a refresh
    bool   glBlockLogged = false;
    bool   limitLogged = false;
    bool   lostLogged = false;
    bool   hitchPaused = false;
    bool   secStalled = false;                 // the game stalled (loading) during this second
    int    flashWin[10] = {}, flashIdx = 0;    // seconds with missed refreshes, outside loading
    double flashLastT = -1e9;
    bool   flashOn = false;
    UINT lastSync = 0, lastPC = 0;
    bool haveStats = false;
    bool kb = false, km = false, kr = false, krs = false, kk = false;

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
        // Ctrl+Shift+K: a marker in the log with the last second's state, to find a moment the player noticed (a
        // stutter, a lost frame limit) in a long session.
        {
            const bool k = KeyCombo('K');
            if (k && !kk)
            {
                EnterCriticalSection(&g_cfgLock);
                const Status ms = g_status;
                LeaveCriticalSection(&g_cfgLock);
                LOGI("BFI Presenter: ---- Ctrl+Shift+K marker ---- game %.1f fps, refreshes %.1f/s, %d missed, frame age %.1f ms, "
                     "frame generation %s, Reflex limit %d us, frames ahead %d%s, video memory %.0f of %.0f MB",
                     ms.gameFps, ms.refreshHz, ms.late, ms.frameAgeMs, ms.fgActive ? "active" : "off", ms.rfxLimitUs,
                     ms.aheadInUse, ms.hitchPaused ? ", hitch guard paused" : "", ms.vramUse, ms.vramBudget);
            }
            kk = k;
        }
        kb = b; km = m; kr = r;

        EnterCriticalSection(&g_cfgLock);
        s = g_shared;
        // BFI off: a plain presenter, a new game frame every refresh. Kept at
        // the strobe's cycle, every second frame of a 120 fps game was dropped
        // and each shown twice (60 Hz motion), which also made a CRT shader's
        // own BFI flicker (Twelve Minutes, Vulkan layer).
        if (!s.enabled) s.frames = 1;
        userCap = s.gameCap;
        // one game frame per strobe cycle (games with Reflex: Reflex's frame limit does it instead, see below)
        // (held at the frame start instead, without Reflex or frame generation: the presenter stays at Refresh rate)
        if (s.autoCap && userCap == 1 && autoCapDown && !capByReflex && s.frames >= 2 && g_fgPaceKind.load() != 2) s.gameCap = 4;
        // Cap applied with Reflex's frame limit (see the per-second block): the presenter itself then
        // takes every frame, like at Refresh, and the driver paces the game.
        statPer = CapPeriod(s.gameCap, s.frames, s.capDivisor);
        if (reflexCap && s.capReflex && userCap >= 2 && s.enabled) s.gameCap = 1;
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
            const int per = std::max(1, CapPeriod(s.gameCap, s.frames, s.capDivisor));
            const bool capOk = s.capJit && per >= 2 && s.frames >= 2 && per % s.frames == 0;
            const bool llOk  = s.laterRelease && s.pacingMode == 1 && per == 1 && s.gameCap >= 1 && g_fgPaceKind.load() != 2;
            g_lrAllowed.store((capOk || llOk) && s.pacing && !vsyncOverridden && output);
            g_lrWindow.store(per / (modeHz > 0.0 ? modeHz : 120.0));
        }
        g_lrBaseMs.store(s.laterReleaseMs);
        g_lrPeriod.store(1.0 / (modeHz > 0.0 ? modeHz : 120.0));
        g_gameCap.store(s.gameCap);
    g_capPer.store(CapPeriod(s.gameCap, s.frames, s.capDivisor));

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
            snprintf(path, sizeof(path), "%.*sBFIPresenter_reflex_%04u%02u%02u_%02u%02u%02u.csv",
                     (int)(strlen(g_logPath) - strlen("BFIPresenter_log.csv")), g_logPath,
                     lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
            rfxf = fopen(path, "w");
            if (rfxf)
                fprintf(rfxf, "time_s,game_fps,cap_set,cap_period,auto_cap,auto_cap_probe,fg_active,pacing,jit,jit_state,jit_est_ms,ahead,"
                              "reflex_limit_us,release_to_flash_ms,release_to_flash_max_ms,frames_shown,"
                              "rfx_reports,rfx_sim_to_gpu_ms,rfx_sim_to_present_ms,rfx_input_to_gpu_ms,rfx_gpu_frame_ms,"
                              "rfx_gpu_active_ms,rfx_sim_ms,rfx_submit_ms,rfx_queue_ms,rendered_fps,align_slack_p10_ms,align_shift_ms\n");
        }
        else if (!s.log && logf)
        {
            fclose(logf);
            logf = nullptr;
            if (rfxf) { fclose(rfxf); rfxf = nullptr; }
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
            g_tickSlot.store((int)((presents - phaseShift) % (uint64_t)s.frames));
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
        const uint32_t slotNow = (uint32_t)((presents - phaseShift) % (uint64_t)s.frames);
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
        {
            pick = (s.pacingMode == 0 && lastShown >= 0 && g_stageId[lastShown] == lastShownId) ? lastShown : newest;
            // Frame generation paced at the game's frame start (no Reflex): XeSS frame generation submits its
            // generated frame and the game's frame together (0.4 ms apart, Riven). When both are ready at one
            // flash, Lowest latency showed only the newer -- 30 of 60 frames a second. Show the generated one now
            // and the game's frame at the next flash.
            if (cycleStart && g_fgPaceUs.load() > 0 && g_fgPaceKind.load() == 1 && unseen == 2 && oldestNew >= 0 && oldestNewId + 1 == newestId &&
                std::fabs(g_stageTime[newest] - g_stageTime[oldestNew]) < 0.004)
                pick = oldestNew;
        }
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
            if (statPer > s.frames) { if (step >= 2) secSkipU++; }   // frame lit over several cycles: 0/1 steps are normal
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
        pickNew = cycleStart && pickId != lastShownId;
        pickRelT = (pickNew && g_relId[pickId % kRel] == pickId) ? g_relT[pickId % kRel] : 0.0;
        // Measured from when the frame's copy finished on the GPU, not from its Present: in God of War Ragnarok
        // the GPU still needed 10-13 ms after Present, so a frame "ready" 12 ms before the pick by its Present
        // was really ready just in time, and moving it later wrapped it round to the next cycle. Not recorded
        // yet means it has only just finished: 0.
        if (pickNew && alignN < 256)
        {
            const int kd = (int)(pickId % kLr);
            const double sl = g_lrDoneId[kd] == pickId ? Now() - g_lrDoneT[kd] : 0.0;
            if (sl >= 0.0 && sl < 0.1) alignSlack[alignN++] = sl;
        }

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
        c.slot    = slotNow;
        c.method  = !s.enabled ? 2u : hitchPaused ? 3u : (uint32_t)s.method;
        c.soften = (c.method < 2u) ? soften : 0.0f;   // LCD phase swap fade
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
                const double peak = s.peakNits;
                const AblCurveT curve = AblMakeCurve(s.ablCurve, peak, (double)s.ablBudget);
                auto loadAt = [&](double C) { return AblLoadAt(cells, C, curve); };
                if (loadAt(peak) <= 1.0) ablSolve = peak;
                else
                {
                    double lo = 100.0, hi = peak;
                    for (int it = 0; it < 24; it++)
                    {
                        const double mid = 0.5 * (lo + hi);
                        if (loadAt(mid) <= 1.0) lo = mid; else hi = mid;
                    }
                    ablSolve = lo;
                }
                hotArea = avgAt(ablSolve);                     // lit-refresh average at that ceiling (nits)
                rawAvg  = avgAt(1.0e9);                        // what the scene would ask for with no ceiling at all
                static bool firstMeas = true;
                if (firstMeas) { firstMeas = false; LOGI("BFI Presenter: ABL guard measuring (budget %d nits full-screen, window curve %s)", s.ablBudget, s.ablCurve[0] ? s.ablCurve : "from peak brightness"); }
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
        if (GradualSaverOn(s))
        {
            c.flags |= 16u;
            c.phase = (float)(floor(gradPhase * 256.0) / 256.0);   // steps of 1/256 refresh: buffers can still be reused
        }
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
        if (tPres - tSub > 0.75 / (modeHz > 0.0 ? modeHz : 120.0)) secSlowPres++;
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
        // LCD phase swap: an LCD drives each refresh with the opposite polarity of the one before. With an
        // even cycle (lit, dark) every lit refresh gets the same polarity, the panel sees a DC bias and ions
        // build up in the liquid crystal: image retention ("burn-in") on IPS/VA. Every N seconds the strobe
        // fades to the steady average picture (as the hitch guard shows) over about 0.4 s, repeats one refresh
        // while every refresh looks the same -- invisible -- and fades back; the lit refreshes then sit on
        // the other polarity. Fading keeps the light of every cycle the same, so nothing flickers.
        // LCD gradual saver: instead of fading and jumping, the lit window drifts later by a tiny fraction of
        // a refresh every refresh. When it has moved a whole refresh, the cycle's last (dark) refresh is
        // repeated once and the window starts again at the cycle's first refresh -- the light keeps its
        // even spacing, and over LcdGradualSeconds it has visited every refresh of the cycle.
        if (GradualSaverOn(s))
        {
            soften = 0.0f; swapState = 0;
            const double hzG = modeHz > 0.0 ? modeHz : 120.0;
            if (s.frames == 2 && s.lcdHoldSec > 0)
            {
                // 2 frames per cycle: hold, drift, hold. Mid-drift the light is split across both refreshes
                // and motion softens, so the window sits sharp on one refresh for LcdGradualHoldSeconds and
                // only then drifts to the other over LcdGradualDriftSeconds.
                if (gradPhase <= 0.0 && gradHold < 0.0) gradHold = (double)s.lcdHoldSec * hzG;
                if (gradHold > 0.0) gradHold -= 1.0;
                else gradPhase = std::min(1.0, gradPhase + 1.0 / ((double)s.lcdDriftSec * hzG));
            }
            else gradPhase = std::min(1.0, gradPhase + (double)s.frames / ((double)s.lcdGradSec * hzG));
            if (gradPhase >= 1.0 && slotNow == (uint32_t)s.frames - 1)
            {
                phaseShift++;
                gradPhase = 0.0;
                gradHold = -1.0;
            }
        }
        else if (s.lcdSwap && s.enabled && (s.frames % 2) == 0)
        {
            const double tNow = Now();
            const float step = 1.0f / (float)std::max(8, (int)(0.4 * (modeHz > 0.0 ? modeHz : 120.0)));
            if (lastPhaseSwapT <= 0.0) lastPhaseSwapT = tNow;
            if (swapState == 0 && tNow - lastPhaseSwapT >= (double)s.lcdSwapSec) swapState = 1;
            if (swapState == 1)
            {
                soften = std::min(1.0f, soften + step);
                if (soften >= 1.0f && slotNow == (uint32_t)s.frames - 1)
                {
                    phaseShift++;              // the next refresh repeats this slot, at the steady average
                    swapState = 2;
                }
            }
            else if (swapState == 2)
            {
                soften = std::max(0.0f, soften - step);
                if (soften <= 0.0f) { swapState = 0; lastPhaseSwapT = tNow; }   // interval counts full-strobe time
            }
        }
        else { soften = 0.0f; swapState = 0; gradPhase = 0.0; gradHold = -1.0; }
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
        if (pickNew && pickRelT > 0.0 && t > pickRelT && t - pickRelT < 0.5)
        {
            const double ms = (t - pickRelT) * 1000.0;
            relSum += ms; relN++;
            if (ms > relMax) relMax = ms;
        }
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
            if (stalled) secStalled = true;
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
            {
                // Frame generation without Reflex: the game's thread (its first present thread) against all presents.
                static int fgCand = 1, fgRun = 0, fgFact = 1;
                long tot = 0, gt = 0;
                DWORD gtid = g_presentThreads[0];
                AcquireSRWLockExclusive(&g_presCountLock);
                // The outer swap chain's presents (the game's own frames), from its busiest thread.
                long outerN = 0; DWORD outerTid = 0;
                for (int k = 0; k < 6 && g_outerCount.tid[k]; k++)
                {
                    if (g_outerCount.n[k] > outerN) { outerN = g_outerCount.n[k]; outerTid = g_outerCount.tid[k]; }
                    g_outerCount.n[k] = 0;
                }
                g_outerCount.other = 0;
                for (int k = 0; k < 6 && g_presCount.tid[k]; k++)
                {
                    tot += g_presCount.n[k];
                    if (g_presCount.tid[k] == gtid) gt = g_presCount.n[k];
                    g_presCount.n[k] = 0;
                }
                tot += g_presCount.other; g_presCount.other = 0;
                ReleaseSRWLockExclusive(&g_presCountLock);
                if (outerN > 0) { gt = outerN; gtid = outerTid; }
                long feN = 0; DWORD feTid = 0;
                AcquireSRWLockExclusive(&g_presCountLock);
                for (int k = 0; k < 6 && g_feCount.tid[k]; k++)
                {
                    if (g_feCount.n[k] > feN) { feN = g_feCount.n[k]; feTid = g_feCount.tid[k]; }
                    g_feCount.n[k] = 0;
                }
                g_feCount.other = 0;
                ReleaseSRWLockExclusive(&g_presCountLock);
                g_fgOuterFsr.store(outerN > 0 ? 1 : 0);
                int cnd = 1;
                if (g_fgSeen[0] && gt > 0 && tot >= 1.5 * gt) cnd = std::max(2, std::min(4, (int)((double)tot / gt + 0.5)));
                fgRun = cnd == fgCand ? fgRun + 1 : 1; fgCand = cnd;
                if (fgRun >= 3 && fgFact != cnd)
                {
                    fgFact = cnd;
                    LOGI("BFI Presenter: frame generation without Reflex: %dx (%ld presents a second, %ld from the game's thread %lu%s)",
                         fgFact, tot, gt, (unsigned long)gtid, outerN > 0 ? ", counted on the outer swap chain" : "");
                }
                const double hzP = modeHz > 0.0 ? modeHz : peakHz;
                int want = 0;
                if (s.fgLimitNoReflex && fgFact > 1 && !g_status.rfxGameUses && s.enabled && s.frames >= 2 && userCap == 1 && hzP > 0.0 &&
                    hzP / s.frames / fgFact >= 28.0)
                    want = (int)(1e6 * s.frames / hzP * fgFact + 0.5);
                // The automatic frame cap without Reflex and without frame generation: the same pacing after the game's
                // present, at one game frame per strobe cycle, instead of holding the finished frame at the present.
                int feWant = 0;
                if (want == 0 && s.autoCapFrameStart && s.autoCap && userCap == 1 && autoCapDown && !capByReflex && !fgWas &&
                    fgFact <= 1 && s.enabled && s.frames >= 2 && s.pacingMode == 1 && hzP > 0.0 && feTid != 0)
                    feWant = (int)(1e6 * s.frames / hzP + 0.5);
                {
                    static int feWas = 0;
                    if ((feWas > 0) != (feWant > 0))
                    {
                        const char *why = feWant > 0 ? "" : !s.autoCapFrameStart ? ": switched off" : !autoCapDown ? ": the cap is at Refresh rate" :
                                          capByReflex ? ": Reflex's frame limit holds the game" : (fgWas || fgFact > 1) ? ": frame generation" :
                                          s.pacingMode != 1 ? ": needs Lowest latency pacing" : feTid == 0 ? ": the game's present thread isn't known" : ": BFI off or cap changed";
                        LOGI("BFI Presenter: automatic frame cap at the frame start %s%s (%d us per game frame, game thread %lu)",
                             feWant > 0 ? "on" : "off", why, feWant, (unsigned long)feTid);
                    }
                    else if (feWant > 0 && want == 0)
                    {
                        static DWORD feTidWas = 0;
                        if (feTidWas && feTid != feTidWas)
                            LOGI("BFI Presenter: automatic frame cap at the frame start: the game now presents from thread %lu (was %lu)",
                                 (unsigned long)feTid, (unsigned long)feTidWas);
                        feTidWas = feTid;
                    }
                    feWas = feWant;
                }
                g_fgGameTid.store(want == 0 && feWant > 0 ? feTid : gtid);
                g_fgPaceKind.store(want > 0 ? 1 : feWant > 0 ? 2 : 0);
                g_fgPaceUs.store(want > 0 ? want : feWant);
                g_status.feLimitUs = feWant;
                static int fgWasOn = 0;
                const int was = fgWasOn; fgWasOn = want;
                if ((was > 0) != (want > 0))
                {
                    const char *why = want > 0 ? "" : !s.fgLimitNoReflex ? ": switched off" : fgFact <= 1 ? ": no frame generation" :
                                      g_status.rfxGameUses ? ": the game reports Reflex" : !s.enabled ? ": BFI off" :
                                      userCap != 1 ? ": needs the Refresh rate cap" : s.frames < 2 ? ": needs 2+ frames per cycle" :
                                      ": under 28 rendered fps";
                    LOGI("BFI Presenter: frame limit for frame generation without Reflex %s%s (%d us per rendered frame, "
                         "game thread %lu, %dx)", want > 0 ? "on" : "off", why, want, (unsigned long)gtid, fgFact);
                }
                g_status.fgLimitUs = want;
            }
            DXGI_FRAME_STATISTICS st = {};
            const bool sok = SUCCEEDED(ScStats(&st));
            int rep = 0;
            const double rate = secPresents / (t - secStart);
            if (!vsyncOverridden)
            {
                bool fast = (sok && haveStats && st.SyncRefreshCount > lastSync)
                                ? rate > 1.4 * (double)(st.SyncRefreshCount - lastSync) / (t - secStart)
                                : rate > 300.0;
                // Two seconds in a row, and above the display mode's rate when it
                // is known: DXGI's refresh count can lag for a second (frame
                // generation starting up in Control: Resonant showed 120
                // presents/s at 120 Hz as "ignoring VSync").
                if (fast && modeHz > 0.0 && rate < 1.15 * modeHz) fast = false;
                vsyncFastSecs = fast ? vsyncFastSecs + 1 : 0;
                if (vsyncFastSecs >= 2 && presents > 240)
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
            {
                // Flash hint: missed refreshes or display repeats in 3 of the last 10 seconds, not counting
                // seconds the game stalled (loading); it goes away after 30 s without any.
                const bool fl = s.enabled && !secStalled && (secLate > 0 || rep > 0);
                flashWin[flashIdx++ % 10] = fl ? 1 : 0;
                if (fl) flashLastT = t;
                int nfl = 0;
                for (int k = 0; k < 10; k++) nfl += flashWin[k];
                if (!flashOn && nfl >= 3) flashOn = true;
                else if (flashOn && t - flashLastT > 30.0) flashOn = false;
                secStalled = false;
                g_status.flashHint = flashOn ? 1 : 0;
                g_status.presBufs  = (int)n;
            }
            g_status.hotArea      = (s.ablGuard && psoMeas) ? hotArea : -1.0;
            g_status.rawAvg       = (s.ablGuard && psoMeas) ? rawAvg : -1.0;
            g_status.ceilInUse    = c.ceilLin * 80.0;
            g_status.displayHz    = displayHz;
            g_status.vrrSuspect   = vrrSlowSecs >= 3;
            g_status.limitSuspect = limitSecs >= 3;
            // Video memory over the game's budget: Windows moves part of it to system memory and the GPU stalls now
            // and then (Indiana Jones at 4K with frame generation, Vulkan: presentation slowed, generated frames
            // dropped, BFI paused). Over budget 3 of the last 5 seconds; clears after 10 s under.
            {
                static int overWin = 0, underSecs = 0;
                double vu = 0.0, vbud = 0.0;
                const bool have = VramMB(vu, vbud) && vbud > 0.0;
                const bool over = have && vu > vbud;
                overWin = ((overWin << 1) | (over ? 1 : 0)) & 31;
                int nOver = 0; for (int b = 0; b < 5; b++) nOver += (overWin >> b) & 1;
                underSecs = over ? 0 : underSecs + 1;
                bool on = g_status.vramOver;
                if (!on && nOver >= 3)
                {
                    on = true;
                    LOGW("BFI Presenter: the game uses more video memory than Windows gives it (%.0f of %.0f MB). Part of it is "
                         "moved to system memory and the GPU stalls now and then: missed refreshes, frame generation drops "
                         "frames, BFI pauses. Lower the game's texture or memory settings.", vu, vbud);
                }
                else if (on && underSecs >= 10)
                {
                    on = false;
                    LOGI("BFI Presenter: video memory back under the budget (%.0f of %.0f MB)", vu, vbud);
                }
                g_status.vramOver = on; g_status.vramUse = vu; g_status.vramBudget = vbud;
            }
            // OpenGL games that present by exchange (Need for Speed: Porsche
            // Unleashed, DOOM 3 BFG) block the presenter's own presents for
            // several refreshes until the game's NVIDIA profile presents
            // OpenGL through DXGI ("Prefer layered on DXGI swapchain").
            {
                const double hzNow = secPresents / (t - secStart), want = modeHz > 0.0 ? modeHz : peakHz;
                glBlockSecs = (g_glActive.load() && want > 0.0 && hzNow < 0.8 * want && secSlowPres * 2 >= secPresents) ? glBlockSecs + 1 : 0;
                if (glBlockSecs == 3 && !glBlockLogged)
                {
                    glBlockLogged = true;
                    LOGW("BFI Presenter: OpenGL: the presenter's own presents block (%.0f of %.0f per second) -- set "
                         "'Vulkan/OpenGL present method' to 'Prefer layered on DXGI swapchain' in this game's NVIDIA profile "
                         "(the BFI Presenter Manager or NVIDIA Profile Inspector)", hzNow, want);
                }
                g_status.glPresentBlocked = glBlockSecs >= 3;
            }
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
                    repSum -= repWin[repIdx];
                    // Not in the first 8 s after the cap changes (the switch itself and the frame-start hold coming on make uneven
                    // cycles: Shadow of the Tomb Raider went to 2 frames ahead 6 s after the remembered cap came on, for good).
                    const bool aheadCount = !hitchPaused && !learnHoldAhead && !swSearching && t - capSwitchT > 8.0;
                    repWin[repIdx] = (s.gameCap >= 1 && autoAhead == 1 && aheadCount && ++repSecsRun > 5) ? secRepU : 0;   // only what happened at 1 frame ahead, not while paused (loading)
                    repSum += repWin[repIdx];
                    repIdx = (repIdx + 1) % 30;
                    if (autoAhead == 1)
                    {
                        missSecs = (secRepU >= 2 && s.gameCap >= 1 && aheadCount) ? missSecs + 1 : 0;
                        if (missSecs >= 3 || repSum >= 6)
                        {
                            if (missSecs < 3)
                                LOGI("BFI Presenter: %d uneven strobe cycles in the last 30 s at 1 frame ahead", repSum);
                            autoAhead = 2; missSecs = 0; spareSecs = 0;
                            for (int r = 0; r < 30; r++) repWin[r] = 0;
                            repSum = 0;
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
                        for (int r = 0; r < 30; r++) repWin[r] = 0;
                        repSum = 0;
                            LOGI("BFI Presenter: automatic frames ahead: 1 (headroom: %.1f ms spare per frame)", capWaitMs);
                        }
                    }
                    // (Forcing 1 frame ahead under Reflex's limit was tried: ~21 ms instead of ~30 ms release
                    // -> flash, but DLSS-G then dropped to 52-55 presented a second at times; at 2 it held
                    // 60 of 60. Automatic frames ahead decides, as before.)
                    g_maxAhead.store(autoAhead);
                }
                else g_maxAhead.store(s.maxAhead);
                g_status.aheadInUse = g_maxAhead.load();

                // Automatic frame cap (see autoCapDown).
                const double hzA = modeHz > 0.0 ? modeHz : peakHz;
                // Frame generation: with Reflex frame numbers (every DLSS-G game has them), presents
                // well above the rendered frames. Without them, an FSR / XeSS frame-generation DLL
                // presenting. DLSS-G's DLL is on the present path in God of War with it off, so its
                // presence alone doesn't count.
                // Compared over the same second: the rendered count was taken at the end of the last
                // second, so it goes with last second's presents. Comparing it with this second's made
                // the state flip every second once the Reflex limit changed the present rate (God of
                // War Ragnarok: limit on, off, on... and heavy stutter). A change also needs 3 seconds
                // in a row (on) or 5 (off).
                bool fg = fgWas;
                if (Now() - g_rfxRenderedT.load() < 3.0 && g_rfxRenderedFps.load() > 1.0)
                {
                    if (fpsPrevSec > 0.0 && !hitchPaused)
                    {
                        const double ratio = fpsPrevSec / g_rfxRenderedFps.load();
                        const bool says = fgWas ? ratio > 1.25 : ratio > 1.5;
                        fgVotes = says != fgWas ? fgVotes + 1 : 0;
                        if (fgVotes >= (fgWas ? 5 : 3)) { fg = says; fgVotes = 0; }
                    }
                }
                else
                {
                    // DLSS-G presenting counts too once the game has stopped giving Reflex reports it gave
                    // before (Returnal with frame generation on): the presenter's cap would halve its frames.
                    // Stopped, not just started: God of War Ragnarok has DLSS-G on its present path with frame
                    // generation off, and in its first second of Reflex reports there were no rendered counts yet
                    // -- frame generation 'active', a 2x limit, 30 fps, and Reflex's limit dropped for the session.
                    const bool dlssOnPath = Now() - g_fgPresentT.load() < 2.0;
                    const bool rfxStopped = rsEverT > 0.0 && t - rsEverT >= 3.0;
                    // ... and only at a frame rate frame generation would give: The Witcher 3 with frame generation off
                    // stopped its reports in loading stalls at 27-29 fps and was read as frame generation for ~20 s
                    // (the cap at Refresh, Reflex's limit off). Returnal with frame generation presented 50-60.
                    const bool fgRate = hzA <= 0.0 || g_status.gameFps >= 0.35 * hzA;
                    fg = dlssOnPath && (!g_fgCallerDlss.load() || (rfxStopped && (fgWas || fgRate)));
                    if (dlssOnPath && g_fgCallerDlss.load() && rsEverT > 0.0 && !rfxStopped && !fgEarlyLogged)
                    {
                        fgEarlyLogged = true;
                        LOGI("BFI Presenter: frame generation check: DLSS-G is on the present path but the game's Reflex frame "
                             "counts aren't in yet -- not counted as frame generation");
                    }
                    fgVotes = 0;
                }
                fpsPrevSec = g_status.gameFps;
                if (fg != fgWas)
                {
                    fgWas = fg;
                    LOGI("BFI Presenter: frame generation %s (%.0f presented, %.0f rendered a second)", fg ? "active" : "not active",
                         fpsPrevSec, g_rfxRenderedFps.load());
                }
                const double fpsA = g_status.gameFps;
                // Games with Reflex (every DLSS frame-generation game): the cap steps down with Reflex's
                // frame limit, with frame generation or without. The driver paces the game (and DLSS-G),
                // which is quicker than holding the game at Present: God of War Ragnarok with frame
                // generation switching itself off had ~57 ms release -> flash with the presenter's cap.
                // Other games: the presenter's own cap, never while frame generation is active.
                // Safety net: a game whose own Reflex setup makes the limit collapse its frame rate
                // (Returnal: 60 fps -> 7.5 fps the moment the limit went on) -- the limit is dropped for
                // the session and the presenter's own cap used instead.
                // A stall is not a collapse: Until Dawn's startup load (1-12 fps, the hitch guard paused from
                // 50 ms after the step-down, the window not responding) set it off. A collapsed game keeps
                // drawing steadily with the hitch guard running (Returnal ~7 fps, Pragmata ~8), so a second
                // with the hitch guard paused or below 3 fps restarts the check instead of counting.
                const bool rfxLimitBad = g_rfxLimitBad.load();
                const bool stallSec = fpsA < 3.0 || hitchPaused;
                // Nor is frame generation switching off: until 5 seconds agree it stays 'active' and the limit keeps
                // its 2x per rendered frame, so the game runs at half the cap for those seconds (Returnal with frame
                // generation turned off in the options: "fell to 30 fps", Reflex's limit gone for the session).
                const bool fgChanging = fgVotes > 0;
                if (stallSec) { rfxChkT = t; rcOnT = t; }
                if (autoCapDown && capByReflex && !rfxLimitBad && !stallSec && !fgChanging && t - rfxChkT >= 1.5 && t - rfxChkT < 6.0 &&
                    fpsAtDown > 0.8 * hzA / s.frames && fpsA < 0.5 * hzA / s.frames)
                {
                    if (++rfxBadSecs >= 2)
                    {
                        g_rfxLimitBad.store(true); capByReflex = false;
                        LOGW("BFI Presenter: Reflex frame limit: the game fell to %.0f fps under it (%.0f before) -- no Reflex frame "
                             "limit for the rest of this session (the game's own is taken out too); the automatic frame cap uses the "
                             "presenter's own cap", fpsA, fpsAtDown);
                    }
                }
                else rfxBadSecs = 0;
                // The same collapse under the game's own Reflex limit (Returnal sets 16,666 us now and then:
                // 60 fps -> 7.9 the same second, back to 60 when it clears it).
                if (hzA > 0.0 && fpsA >= 0.5 * hzA / s.frames) fpsGoodT = t;
                const int gameLimUs = ReflexGameLimitUs();
                if (!rfxLimitBad && s.enabled && gameLimUs > 0 && !hitchPaused && fpsA > 0.0 && fpsA < 0.2 * hzA / s.frames && t - fpsGoodT < 6.0)
                {
                    if (++gameLimBadSecs >= 2)
                    {
                        g_rfxLimitBad.store(true); capByReflex = false;
                        LOGW("BFI Presenter: the game's own Reflex frame limit (%d us): the game fell to %.0f fps under it -- taken out "
                             "for the rest of this session", gameLimUs, fpsA);
                    }
                }
                else gameLimBadSecs = 0;
                const bool rfxUse = t - rsLastT < 3.0 && rsLast.frames > 0 && !g_rfxLimitBad.load();
                // Game frame cap with Reflex's frame limit (CapWithReflex, off by default): in games with
                // Reflex, a cap below the refresh rate (Half, Quarter, one per strobe cycle, custom) is applied
                // by the driver where the game starts each frame, instead of holding the game at Present --
                // what a game's own Reflex fps limit does. For engines that hand frames over irregularly
                // (The Midnight Walk at Half: 1.7% of flashes off-cadence against 15.6%); elsewhere just-in-
                // time release is as even with less latency (Silent Hill 2 at Half: 1.3% against 2.2%, 25 ms
                // against 34 ms release -> flash; Alan Wake 2 at Quarter: 35 ms against 56 ms).
                {
                    const bool autoDown = s.autoCap && userCap == 1 && autoCapDown;
                    const bool want = s.capReflex && userCap >= 2 && s.enabled && rfxUse && hzA > 0.0 && !autoDown;
                    const int rcPer = CapPeriod(userCap, s.frames, s.capDivisor);
                    if (want && !reflexCap)
                    {
                        rcOnT = t; fpsAtRc = fpsA; rcBadSecs = 0;
                        LOGI("BFI Presenter: game frame cap with Reflex's frame limit: %.1f fps (%.0f us)", hzA / rcPer, 1e6 * rcPer / hzA);
                    }
                    else if (!want && reflexCap && !autoDown)
                        LOGI("BFI Presenter: game frame cap: the presenter's own cap%s", g_rfxLimitBad.load() ? " (the game collapsed under Reflex's limit)" :
                             !rfxUse && s.capReflex ? " (no Reflex reports from the game)" : "");
                    reflexCap = want;
                    if (reflexCap)
                    {
                        const double capFps = hzA / rcPer;
                        if (!stallSec && !fgChanging && t - rcOnT >= 1.5 && t - rcOnT < 6.0 && fpsAtRc > 0.8 * capFps && fpsA < 0.5 * capFps)
                        {
                            if (++rcBadSecs >= 2)
                            {
                                g_rfxLimitBad.store(true); reflexCap = false;
                                LOGW("BFI Presenter: Reflex frame limit: the game fell to %.0f fps under it (%.0f before) -- no Reflex frame "
                                     "limit for the rest of this session; the game frame cap uses the presenter's own cap", fpsA, fpsAtRc);
                            }
                        }
                        else rcBadSecs = 0;
                    }
                }
                // Which cap steps down in a Reflex game: Reflex's limit (applied at the game's sleep). With frame
                // generation the presenter's cap would hold frame generation's present thread (Alan Wake 2 3x ~140 ms
                // frame start -> GPU against ~42 ms); without it, frame start -> flash was about the same or a little
                // quicker with Reflex's limit (Onimusha ~54 ms against ~58, God of War 54-64 against ~70).
                // AutoCapOwnWithoutFg=1: the presenter's own cap (just-in-time release) while frame generation is
                // off. Follows frame generation going on and off, also while down.
                {
                    const bool reflexOk = !s.autoCapOwn || fg;
                    if (!autoCapDown) capByReflex = rfxUse && reflexOk;
                    else if (capByReflex && !reflexOk) capByReflex = false;
                    else if (!capByReflex && reflexOk && rfxUse) capByReflex = true;
                }
                // The frame limit for frame generation without Reflex holds the game itself: the presenter's own cap
                // on top held frame generation's presents too, and the game's window stopped responding for
                // seconds (The Callisto Protocol, Riven), so the cap stays at Refresh while that limit is in use.
                const bool fgLimitOn = g_fgPaceUs.load() > 0 && g_fgPaceKind.load() == 1;
                const bool capOn = s.autoCap && userCap == 1 && s.frames >= 2 && s.enabled && hzA > 0.0 && (rfxUse || !fg) && !fgLimitOn;
                // "Forget this game": the cap and the frame start start over (the section is deleted on the watchdog thread).
                if (g_learnForgetNow.exchange(false))
                {
                    learnMine = LearnRec(); learnMine.hz = hzA; learnMine.frames = s.frames;
                    learnSavedDown = -1; learnApply = false; learnSwNow = false; learnSwReset = true;
                    if (autoCapDown || capProbe) { autoCapDown = capProbe = false; capDownSecs = capUpSecs = 0; capSwitchT = t; probeWait = 30.0; }
                    LOGI("BFI Presenter: remembered data for this game cleared -- the automatic frame cap starts over");
                }
                learnHoldAhead = false;
                if (s.learnPerGame)
                {
                    const int g = LearnFetch(learnGen, learnRead);
                    if (g != learnGen)
                    {
                        // A game (window title) read: start from what it learned once its refresh rate is known.
                        learnGen = g; learnApply = true; learnSavedDown = -1; learnMine = LearnRec();
                    }
                    learnWarm = capOn ? learnWarm + 1 : 0;
                    learnHoldAhead = learnApply && capOn && learnRead.down == 1 && learnRead.frames == s.frames && fabs(learnRead.hz - hzA) < 0.05;
                    if (learnApply && capOn && learnWarm >= 5)
                    {
                        learnApply = false; learnHoldAhead = false;
                        if (learnRead.frames == s.frames && fabs(learnRead.hz - hzA) < 0.05)
                        {
                            learnMine = learnRead;
                            learnSavedDown = learnRead.down;
                            if (learnRead.down == 1 && !autoCapDown && !capProbe)
                            {
                                autoCapDown = true; capDownSecs = capUpSecs = 0; capSwitchT = t; rfxChkT = t; fpsAtDown = fpsA;
                                probeWait = 60.0;
                                LOGI("BFI Presenter: automatic frame cap: one game frame per strobe cycle (%.0f fps, %s) -- learned for this "
                                     "game in an earlier session", hzA / s.frames, capByReflex ? "Reflex frame limit" : "the presenter's cap");
                            }
                        }
                        learnMine.hz = hzA; learnMine.frames = s.frames;
                        learnSwNow = true;    // a frame-start search already running (the cap stepped down by itself first): learned start instead
                    }
                }
                if (capOn)
                {
                    const double refMs = 1000.0 / hzA;
                    if (capProbe)
                    {
                        // Trying Refresh: the first second still counts frames from the cap.
                        probeSecs++;
                        if (probeSecs >= 2 && fpsA >= 0.95 * hzA && !hitchPaused) probeGood++;
                        if (probeSecs >= 4)
                        {
                            capProbe = false; capDownSecs = capUpSecs = 0; capSwitchT = t;
                            if (probeGood >= 2)
                            {
                                // probeWait is kept: it is reset only after a stay at Refresh rate of 30 s or more (below)
                                LOGI("BFI Presenter: automatic frame cap: Refresh rate -- the game holds %.0f fps", fpsA);
                            }
                            else
                            {
                                autoCapDown = true;
                                probeWait = probeWait * 2.0 > 120.0 ? 120.0 : probeWait * 2.0;
                                // Far from the refresh rate (under 80%): each try is seconds of uneven frames and the
                                // longest delay (Soul Reaver at 64 of 120: 53 ms from release to flash for 4 s), so the
                                // next one waits 5 minutes.
                                if (fpsA < 0.8 * hzA) probeWait = 300.0;
                                LOGI("BFI Presenter: automatic frame cap: still one game frame per strobe cycle (%.0f fps of %.0f "
                                     "at Refresh); next try in %.0f s", fpsA, hzA, probeWait);
                            }
                        }
                    }
                    else if (!autoCapDown)
                    {
                        capDownSecs = (!hitchPaused && fgVotes == 0 && fpsA > 0.3 * hzA / s.frames && fpsA < 0.95 * hzA) ? capDownSecs + 1 : 0;   // not while frame generation may be starting
                        if (capDownSecs >= 3 && t - capSwitchT > 3.0)
                        {
                            // Down again soon after going back up: the game is close to the limit, so the
                            // next return waits longer.
                            if (t - capUpT < 30.0) stayMin = stayMin * 2.0 > 120.0 ? 120.0 : stayMin * 2.0;
                            else stayMin = 10.0;
                            // The same for the try at Refresh rate without Reflex reports: a game that holds the refresh
                            // rate only in lighter scenes went up and down every 30 s (Alan Wake: 120 fps for 3 s, then 96),
                            // and each time the frame start was looked for again.
                            if (t - capUpT < 30.0) probeWait = probeWait * 2.0 > 120.0 ? 120.0 : probeWait * 2.0;
                            else probeWait = 30.0;
                            if (fpsA < 0.8 * hzA && probeWait < 60.0) probeWait = 60.0;   // well under the refresh rate: first try after a minute
                            autoCapDown = true; capDownSecs = capUpSecs = 0; capSwitchT = t; rfxChkT = t; fpsAtDown = fpsA;
                            LOGI("BFI Presenter: automatic frame cap: one game frame per strobe cycle (%.0f fps, %s) -- the game held "
                                 "%.0f fps of %.0f, so frames were dropped unevenly", hzA / s.frames,
                                 capByReflex ? "Reflex frame limit" : "the presenter's cap", fpsA, hzA);
                        }
                    }
                    else if (!fg && t - rsLastT < 3.0 && rsLast.frames > 0 && rsLast.gpuActiveMs > 0.0)
                    {
                        // Reflex reports: back up when the game's GPU and CPU work per frame fit well
                        // within one refresh.
                        double needMs = rsLast.gpuActiveMs;
                        if (rsLast.simMs > needMs) needMs = rsLast.simMs;
                        if (rsLast.submitMs > needMs) needMs = rsLast.submitMs;
                        capUpSecs = (!hitchPaused && needMs < 0.85 * refMs) ? capUpSecs + 1 : 0;
                        if (capUpSecs >= 10 && t - capSwitchT >= stayMin)
                        {
                            autoCapDown = false; capDownSecs = capUpSecs = 0; capSwitchT = t; capUpT = t;
                            LOGI("BFI Presenter: automatic frame cap: Refresh rate -- game frames take %.1f ms (GPU %.1f, "
                                 "simulation %.1f, submit %.1f), within a refresh", needMs, rsLast.gpuActiveMs, rsLast.simMs, rsLast.submitMs);
                        }
                    }
                    // With frame generation, a try means 3 s without the limit, and DLSS-G re-pacing twice:
                    // a visible stutter each time (God of War Ragnarok, every 30-120 s). Frame generation that
                    // could fill the refresh rate is rare, so try only every 5 minutes then.
                    else if (t - capSwitchT >= (fg && capByReflex ? 300.0 : probeWait) && !hitchPaused)
                    {
                        // No Reflex reports (or frame generation, whose reports are for rendered frames):
                        // try Refresh for a few seconds.
                        autoCapDown = false; capProbe = true; probeSecs = probeGood = 0; capSwitchT = t; capUpT = t;
                    }
                }
                else
                {
                    if ((autoCapDown || capProbe) && (fg || fgLimitOn) && s.autoCap && userCap == 1)
                        LOGI("BFI Presenter: automatic frame cap: Refresh rate (frame generation without Reflex%s)",
                             fgLimitOn ? " -- the frame limit for it holds the game" : "");
                    autoCapDown = capProbe = false; capDownSecs = capUpSecs = 0;
                }
                // Learned per game: the cap's state once it has stayed 30 s (a try at Refresh rate that fails isn't a change).
                if (s.learnPerGame && learnMine.frames > 0 && capOn && !capProbe && t - capSwitchT > 30.0 && (autoCapDown ? 1 : 0) != learnSavedDown)
                {
                    learnSavedDown = autoCapDown ? 1 : 0;
                    learnMine.down = learnSavedDown;
                    LearnPost(learnMine);
                }
                g_status.autoCapDown = autoCapDown ? 1 : 0;
                g_status.autoCapProbe = capProbe ? 1 : 0;
                g_status.fgActive = fg ? 1 : 0;
                g_status.relFlashMs = relN ? relSum / relN : -1.0;
            }
            secFrameRepeats = secFrameSkips = 0;
            secRepU = secSkipU = 0;
            g_status.outType    = (int)c.outType;
            const Status stNow = g_status;
            LeaveCriticalSection(&g_cfgLock);
            {
                ReflexSecond rs;
                const int perNow = CapPeriod(s.gameCap, s.frames, s.capDivisor);
                // Frame generation at the Refresh cap: Reflex's frame limit at one presented frame
                // per strobe cycle (e.g. 16,667 us at 120 Hz with 2 frames per cycle).
                const double hzL = modeHz > 0.0 ? modeHz : peakHz;
                int limitUs = (s.autoCap && userCap == 1 && s.enabled && s.frames >= 2 && stNow.autoCapDown && capByReflex && hzL > 0.0)
                                  ? (int)(1e6 * s.frames / hzL + 0.5) : 0;
                if (limitUs == 0 && reflexCap && s.capReflex && userCap >= 2 && s.enabled && hzL > 0.0)
                    limitUs = (int)(1e6 * CapPeriod(userCap, s.frames, s.capDivisor) / hzL + 0.5);
                // Phase: the limiter paces the game on its own clock, so its frames can settle right at
                // the moment the presenter picks a frame for the lit refresh, where a little jitter
                // repeats one frame and skips the next, again and again (God of War Ragnarok: 10-18
                // pairs a second for a whole minute, against none in another run). Then the limit is
                // shortened by 0.15 ms for one second, which moves the frames ~9 ms earlier against
                // the strobe, and checked again 3 s later.
                if (!s.reflexAlign && limitUs > 0 && rsLast.limitUs > 0 && !hitchPaused && stNow.frameRepeats >= 5 && stNow.frameSkips >= 5 && t >= phaseNextT)
                {
                    phaseUntil = t + 1.0; phaseNextT = t + 4.0;
                    if (++phaseNudges <= 5)
                        LOGI("BFI Presenter: Reflex frame limit: game frames arrive at the lit refresh's pick (%d repeated, %d skipped "
                             "last second) -- moving them earlier", stNow.frameRepeats, stNow.frameSkips);
                }
                if (limitUs > 0 && t < phaseUntil) limitUs -= 150;
                // ReflexAlign (test): the limiter paces the game on its own clock, so a game frame is ready
                // anywhere up to a whole cap interval before the presenter picks it for the lit refresh -- time
                // it only waits. Line the wake-up up so frames are ready just before the pick: each second the
                // 10th percentile of that wait is brought down to a small target (half the excess at a time,
                // at most 3 ms), and a frame repeated at the pick moves the wake-up 2 ms earlier and widens
                // the target by 1 ms (at most 8 ms; back down by 0.25 ms every 10 clean seconds). Only at the
                // game's sleep without frame generation, and only while the game holds the cap.
                {
                    bool alignIdle = false;
                    double p10 = -1.0;
                    if (alignN >= 10)
                    {
                        std::sort(alignSlack, alignSlack + alignN);
                        p10 = alignSlack[alignN / 10];
                    }
                    // The limit has to be a whole number of strobe cycles, or its frames drift past the pick by design.
                    // Also for the frame limit with frame generation without Reflex: one presented frame
                    // per strobe cycle, moved through g_fgShiftUs.
                    const bool fgOn = g_fgPaceUs.load() > 0;
                    std::atomic<int> &shiftUs = fgOn ? g_fgShiftUs : g_sleepShiftUs;
                    const int sleepUs = fgOn ? g_fgPaceUs.load() : g_sleepLimitUs.load();
                    const double capFps = fgOn ? (hzL > 0.0 ? hzL / s.frames : 0.0) : sleepUs > 0 ? 1e6 / sleepUs : 0.0;
                    const double cyc = hzL > 0.0 ? s.frames / hzL : 0.0;
                    const double cycles = cyc > 0.0 && sleepUs > 0 ? sleepUs * 1e-6 / cyc : 0.0;
                    const bool locked = cycles >= 0.9 && fabs(cycles - std::floor(cycles + 0.5)) * cyc < 0.0003;
                    // Lowest-latency pacing only: Smooth shows the oldest waiting frame, so with two waiting a late frame
                    // just leaves the older one to be shown -- no repeat -- and the wait jumps a whole cycle instead.
                    // In Dead Space (Smooth) the wake-up chased that round and round (318 ms in 6 minutes).
                    // Not with the presenter's own pacing after the present (frame generation without Reflex, the frame
                    // cap at the frame start): its start-time search steers that, and the alignment steering it as well
                    // made the start jump 3-5 ms every few seconds (Resident Evil 4 with ReflexAlign on: 33-36 ms from
                    // release to flash every 4-5 s between 22-25 ms).
                    const bool atSleepNoFg = sleepUs > 0 && !fgOn && locked && g_sleepFgFactor.load() == 1 && !stNow.fgActive && s.pacingMode == 1;
                    const bool holds = capFps > 0.0 && stNow.gameFps > 0.95 * capFps && stNow.gameFps < 1.05 * capFps;
                    // Frame generation without Reflex: the start time is chosen by trying it out. The controller below
                    // hunted there: frames a pair apart wait a refresh more by design, so the 10th percentile it steers
                    // by has no steady target (Riven: the wake-up moved 3 ms later every second, 89 -> 248 ms in two
                    // minutes, and motion stuttered; in another run it settled at once). Instead, start times across
                    // one strobe cycle are tried about 2 ms apart, 3 s each (1 s to settle, 2 s measured): the one
                    // with every frame reaching a flash and the shortest release -> flash is kept. Tried again only
                    // after 10 s in a row with under 90% of the frames reaching a flash or 6 ms slower, at most once
                    // a minute (FSR: 5 s, every 20 s, see below). Riven: 29 ms (start +8.3 ms in the cycle) against 34-45 ms at the other start times.
                    static int swIdx = -1, swN = 0, swSecs = 0, swBad = 0, swShown = 0, swRelN = 0, swRep = 0;
                    static double swPhase = 0.0, swStart = 0.0, swDoneT = -1.0, swBestMs = 0.0, swRel = 0.0, swScore[16] = {}, swRelMs[16] = {},
                                  swSlack = 1.0, swSlackK[16] = {};
                    static int swTight = 0;
                    static int swShownK[16] = {};
                    static bool swWasOn = false;
                    // The automatic frame cap at the frame start keeps the start time it found while the cap goes up to
                    // Refresh rate and back down (within 10 minutes): looking again each time showed its bad start
                    // times every 30 s (Alan Wake: 30 of 60 frames for seconds at a time). Frame generation looks again.
                    {
                        // Learned for this game in an earlier session (LearnPerGame): its place in the strobe cycle.
                        const bool learnSwOk = s.learnPerGame && learnMine.frames > 0 && learnMine.fsMs > 0.0 && cyc > 0.0 &&
                                               learnMine.fsMs < cyc * 1000.0;
                        auto learnSwUse = [&]()
                        {
                            double ph = std::fmod(swPhase, cyc);
                            if (ph < 0.0) ph += cyc;
                            const int d = (int)((learnMine.fsMs * 0.001 - ph) * 1e6);
                            if (d) { shiftUs.fetch_add(d); alignShiftTot += d * 1e-6; swPhase += d * 1e-6; }
                            swIdx = -1; swBad = 0; swDoneT = t;
                            swBestMs = learnMine.bestMs > 0.0 ? learnMine.bestMs : 1e3;
                            LOGI("BFI Presenter: automatic frame cap at the frame start: start time learned for this game in an earlier session "
                                 "(start %+.1f ms in the cycle, %.1f ms from release to flash)", learnMine.fsMs, learnMine.bestMs);
                        };
                        static int swKind = 0; static double swOffT = -1e9;
                        const int kindNow = g_fgPaceKind.load();
                        if (!fgOn && swWasOn) swOffT = t;
                        if (fgOn && !swWasOn)
                        {
                            const bool keep = kindNow == 2 && swKind == 2 && swIdx < 0 && swDoneT >= 0.0 && t - swOffT < 600.0;
                            if (keep) { swBad = 0; swDoneT = t; LOGI("BFI Presenter: automatic frame cap at the frame start: start time kept from before"); }
                            else if (kindNow == 2 && learnSwOk) learnSwUse();
                            else { swDoneT = -1.0; swIdx = -1; swBad = 0; }
                        }
                        if (learnSwReset) { learnSwReset = false; swDoneT = -1.0; swIdx = -1; swBad = 0; }
                        if (learnSwNow)
                        {
                            learnSwNow = false;
                            if (fgOn && kindNow == 2 && swIdx >= 0 && learnSwOk) learnSwUse();
                        }
                        if (fgOn) swKind = kindNow;
                    }
                    swWasOn = fgOn;
                    swSearching = fgOn && swIdx >= 0 && g_fgPaceKind.load() == 2;
                    if (fgOn && locked && s.pacingMode == 1 && cyc > 0.0)
                    {
                        // FSR frame generation paces its own output and follows a moved frame start with a lag, and the
                        // start times that work moved between searches (Riven): 2 s to settle per start time, 8 ms to
                        // spare preferred, no 2 ms steps while held (they walked it into the bad start times), looked
                        // for again after 5 s of lost frames (every 20 s at most). XeSS frame generation keeps the
                        // timings tested with it.
                        const bool fsr = g_fgOuterFsr.load() != 0;
                        const int settle = fsr ? 3 : 2;
                        // Learned per game: the frame start's place in the strobe cycle (the automatic frame cap only).
                        auto learnSw = [&]()
                        {
                            if (!s.learnPerGame || learnMine.frames <= 0 || g_fgPaceKind.load() != 2) return;
                            double ph = std::fmod(swPhase, cyc);
                            if (ph < 0.0) ph += cyc;
                            learnMine.fsMs = ph * 1000.0; learnMine.bestMs = swBestMs;
                            LearnPost(learnMine);
                        };
                        auto moveTo = [&](double ph)
                        {
                            const int d = (int)((ph - swPhase) * 1e6);
                            if (d) { shiftUs.fetch_add(d); alignShiftTot += d * 1e-6; }
                            swPhase += d * 1e-6;
                        };
                        // The automatic frame cap at the frame start: only while the game holds the cap. Below it (a scene
                        // too heavy for 60) every start time loses frames and a search only shows its bad ones (Days Gone:
                        // four searches in four minutes, each started in a scene at 55-59 fps).
                        const bool ok = holds && !hitchPaused && (g_fgPaceKind.load() != 2 || stNow.gameFps >= 0.98 * capFps) &&
                                        t - g_windowHungT.load() > 10.0;
                        const double base = swPhase - std::fmod(swPhase, cyc);   // stay within the same cycle's worth of moves
                        // The automatic frame cap at the frame start looks again as soon as FSR does: when a heavier
                        // scene makes frames miss the pick they were timed for, each waits a whole cycle more (Obduction:
                        // 23 ms from release to flash, then 40 ms for the 20 s the next search was still held back).
                        const bool quick = fsr || g_fgPaceKind.load() == 2;
                        if (swIdx < 0 && ok && (swDoneT < 0.0 || (quick ? (swBad >= 5 && t - swDoneT > 20.0) : (swBad >= 10 && t - swDoneT > 60.0))))
                        {
                            swN = std::max(4, std::min(16, (int)(cyc / 0.002 + 0.5)));
                            swIdx = 0; swSecs = 0; swShown = swRelN = swRep = 0; swRel = 0.0; swBad = 0;
                            swStart = base;
                            moveTo(swStart);
                            LOGI("BFI Presenter: %s: trying %d start times across the strobe cycle (%.0f s)",
                                 g_fgPaceKind.load() == 2 ? "automatic frame cap at the frame start" : "frame limit for frame generation",
                                 swN, swN * (settle + 1.0));
                        }
                        else if (swIdx >= 0)
                        {
                            if (!ok) { swSecs = 0; swShown = swRelN = swRep = 0; swRel = 0.0; swSlack = 1.0; }
                            else if (++swSecs >= settle)
                            {
                                swShown += relN; swRel += relSum; swRelN += relN; swRep += stNow.frameRepeats;
                                if (p10 >= 0.0) swSlack = std::min(swSlack, p10);
                                // The automatic frame cap at the frame start: a start time that loses frames in its first
                                // measured second is left at once (it showed for 3 s each before).
                                const bool dropEarly = g_fgPaceKind.load() == 2 && swSecs == settle && relN < (int)(0.9 * capFps);
                                if (swSecs >= settle + 1 || dropEarly)
                                {
                                    const double relMs = swRelN ? swRel / swRelN : 1e3;
                                    const bool full = swShown >= (int)(2.0 * capFps * 0.97) && swRep <= 2;
                                    // Every frame shown, and frames ready at least 3 ms before their flash: the quickest start
                                    // right at the edge lost frames as soon as the GPU work grew (Riven: 29 ms, then 40-50 ms
                                    // with 53-58 of 60 shown while OptiScaler's denoiser went from 2.6 to 7 ms a frame).
                                    // FSR frame generation (Riven): next to start times that lost frames, one that had shown every
                                    // frame with 4.6 ms to spare lost half of them a minute later (30 of 60 shown: the generated
                                    // frames, finished on the GPU later than the game's, missed the pick). So 8 ms to spare first,
                                    // then 3 ms, then any start time that showed every frame.
                                    swScore[swIdx] = !full ? 1e3 + (2.0 * capFps - swShown) :
                                                     fsr ? (swSlack >= 0.008 ? relMs : swSlack >= 0.003 ? 50.0 + relMs : 100.0 + relMs)
                                                         : (swSlack < 0.003 ? 100.0 + relMs : relMs);
                                    swRelMs[swIdx] = relMs; swShownK[swIdx] = swShown; swSlackK[swIdx] = swSlack;
                                    swIdx++; swSecs = 0; swShown = swRelN = swRep = 0; swRel = 0.0; swSlack = 1.0;
                                    const double start = swStart;
                                    if (swIdx < swN) moveTo(start + swIdx * cyc / swN);
                                    else
                                    {
                                        int best = 0;
                                        for (int k = 1; k < swN; k++) if (swScore[k] < swScore[best]) best = k;
                                        moveTo(start + best * cyc / swN);
                                        swIdx = -1; swDoneT = t; swBestMs = swRelMs[best];
                                        learnSw();
                                        char list[400]; int len = 0;
                                        for (int k = 0; k < swN && len < (int)sizeof(list) - 32; k++)
                                            len += snprintf(list + len, sizeof(list) - len, " %.1f:%d/%.0f/%.1f", k * cyc / swN * 1000.0,
                                                            swShownK[k], swRelMs[k], swSlackK[k] < 1.0 ? swSlackK[k] * 1000.0 : -1.0);
                                        LOGI("BFI Presenter: %s%s lined up with the flash: start %+.1f ms "
                                             "in the cycle, %.1f ms from release to flash, %d of %.0f frames shown (start ms:shown/ms/ready ms before the flash --%s)",
                                             g_fgPaceKind.load() == 2 ? "automatic frame cap at the frame start" : "frame limit for frame generation",
                                             fsr ? " (FSR)" : "", best * cyc / swN * 1000.0, swRelMs[best], swShownK[best], 2.0 * capFps, list);
                                    }
                                }
                            }
                        }
                        // Held: tried again after 10 s (FSR 5 s) in a row with under 90% of the frames shown, or 6 ms or more
                        // slower than the start time found.
                        else
                        {
                            swBad = (ok && (relN < (int)(0.9 * capFps) || (relN > 0 && relSum / relN > swBestMs + 6.0))) ? swBad + 1 : 0;
                            // Held, frames repeating close to their flash (the GPU work grew): 2 ms earlier, once per 3 s.
                            swTight = (!fsr && ok && stNow.frameRepeats >= 2 && p10 >= 0.0 && p10 < 0.0015) ? swTight + 1 : 0;
                            if (swTight >= 3)
                            {
                                swTight = 0;
                                moveTo(swPhase - 0.002);
                                learnSw();
                                LOGI("BFI Presenter: %s: frames repeated close to their flash -- 2 ms earlier",
                                     g_fgPaceKind.load() == 2 ? "automatic frame cap at the frame start" : "frame limit for frame generation");
                            }
                        }
                    }
                    if (s.reflexAlign && atSleepNoFg && holds && !hitchPaused && t >= alignCoolT)
                    {
                        if (alignLastStep > 0.0 && p10 >= 0.0 && alignPrevP10 >= 0.0 && p10 - alignPrevP10 > 0.5 * cyc)
                        {
                            // Moved past the pick: the frames now wait a whole cycle more. Back to before that step, a
                            // little earlier, and a wider target.
                            const int back = (int)((alignLastStep + 0.002) * 1e6);
                            shiftUs.fetch_sub(back); alignShiftTot -= back * 1e-6;
                            alignTarget = std::min(0.005, alignTarget + 0.001);
                            alignCleanSecs = 0; alignCoolT = t + 2.0; alignLastStep = 0.0;
                            LOGI("BFI Presenter: Reflex frame limit alignment went past the pick (%.1f -> %.1f ms) -- moved back, "
                                 "target %.1f ms", alignPrevP10 * 1000.0, p10 * 1000.0, alignTarget * 1000.0);
                        }
                        // Repeats only count against the alignment when frames were close to the pick (under 3 ms): Dead
                        // Space's own stutters repeat frames with 4-9 ms to spare. Measured against the target instead, the
                        // target climbed to 8 ms and from then on every stutter counted (-56 ms in 6 minutes).
                        else if (stNow.frameRepeats >= 2 && (p10 < 0.0 || p10 < 0.003))
                        {
                            alignLastStep = 0.0;
                            shiftUs.fetch_sub(2000); alignShiftTot -= 0.002;
                            alignTarget = std::min(0.005, alignTarget + 0.001);
                            alignCleanSecs = 0; alignCoolT = t + 2.0;
                        }
                        else if (p10 >= 0.0 && alignN >= (int)(0.8 * capFps))
                        {
                            // Back down slowly: at 1.5 ms and 10 s per step God of War Ragnarok missed the pick about once a
                            // minute (frames ready 0-0.6 ms before it), each time soon after the target had come back down.
                            if (stNow.frameRepeats == 0 && ++alignCleanSecs >= 30 && alignTarget > 0.002)
                            { alignTarget = std::max(0.002, alignTarget - 0.00025); alignCleanSecs = 0; }
                            const double excess = p10 - alignTarget;
                            alignLastStep = 0.0;
                            // Two seconds in a row over the target: one second can be the tail of a cap change or a hitch.
                            alignExcessSecs = excess > 0.001 ? alignExcessSecs + 1 : 0;
                            if (alignExcessSecs >= 2)
                            {
                                const double step = std::min(0.003, 0.5 * excess);
                                shiftUs.fetch_add((int)(step * 1e6)); alignShiftTot += step; alignLastStep = step;
                            }
                            if (alignLastLog < 0.0 || t - alignLastLog > 30.0)
                            {
                                alignLastLog = t;
                                LOGI("BFI Presenter: Reflex frame limit aligned to the flash: game frames ready %.1f ms before the "
                                     "lit refresh's pick (10th percentile; target %.1f ms), wake-up moved %+.1f ms in all",
                                     p10 * 1000.0, alignTarget * 1000.0, alignShiftTot * 1000.0);
                            }
                        }
                    }
                    else { alignCleanSecs = 0; alignLastStep = 0.0; alignExcessSecs = 0; alignIdle = true; }
                    // Coarse alignment (ReflexAlign off): each time the Reflex frame limit comes on, its frames land anywhere
                    // in the strobe cycle. Ready 10-13 ms before the pick they only wait there (Dead Space: 29-33 ms from frame
                    // start to flash, against 24-25 ms in stretches where they landed 1-5 ms before it). After 3 s in a row
                    // with the 10th percentile above 8 ms the wake-up is moved later once, to 5 ms before the pick -- far
                    // enough from it that varying frame times don't repeat frames (Spider-Man 2 with the fine alignment).
                    // A frame repeated close to the pick soon after a move: 3 ms back and 2 ms more to spare (at most 8 ms).
                    // The frames' readiness also moves with the scene's GPU load (Dead Space: 3-4 ms either way within a minute), so
                    // 5 ms to spare. Not within 20 s of the "moving them earlier" nudge, at most once per 20 s.
                    {
                        static int coarseSecs = 0;
                        static double coarseT = -1e9, coarseTarget = 0.005, coarseHoldT = -1e9;
                        // The nudge moves the frames ~9 ms earlier itself: no step of this one on top of it (Dead Space,
                        // both in the same second: 40 of 60 frames shown for 3 s, until the next nudge).
                        const bool nudging = t < phaseUntil;
                        if (nudging) coarseHoldT = t + 20.0;
                        if (!nudging && !s.reflexAlign && s.reflexAlignCoarse && atSleepNoFg && holds && !hitchPaused && p10 >= 0.0 && alignN >= (int)(0.8 * capFps))
                        {
                            if (t - coarseT < 10.0 && stNow.frameRepeats >= 2 && p10 < 0.002)
                            {
                                shiftUs.fetch_sub(3000); alignShiftTot -= 0.003;
                                coarseTarget = std::min(0.008, coarseTarget + 0.002);
                                coarseT = -1e9; coarseSecs = 0;
                                LOGI("BFI Presenter: Reflex frame limit: frames repeated after moving them later -- 3 ms back, "
                                     "%.0f ms to spare from now on", coarseTarget * 1000.0);
                            }
                            else
                            {
                                coarseSecs = (p10 > coarseTarget + 0.003 && stNow.frameRepeats == 0) ? coarseSecs + 1 : 0;
                                if (coarseSecs >= 3 && t - coarseT >= 20.0 && t >= coarseHoldT)
                                {
                                    const double step = std::min(p10 - coarseTarget, 0.9 * cyc);
                                    shiftUs.fetch_add((int)(step * 1e6)); alignShiftTot += step;
                                    coarseT = t; coarseSecs = 0;
                                    LOGI("BFI Presenter: Reflex frame limit: game frames ready %.1f ms before the lit refresh's pick "
                                         "-- moving them %.1f ms later", p10 * 1000.0, step * 1000.0);
                                }
                            }
                        }
                        else coarseSecs = 0;
                    }
                    EnterCriticalSection(&g_cfgLock);
                    g_status.alignSlackMs = p10 >= 0.0 ? p10 * 1000.0 : -1.0;
                    g_status.alignShiftMs = alignShiftTot * 1000.0;
                    LeaveCriticalSection(&g_cfgLock);
                    alignLogSlack = p10; alignPrevP10 = alignIdle ? -1.0 : p10;
                    alignN = 0;
                }
                {
                    // Frame-generation factor for the sleep-point limit: presents per rendered frame, over the same
                    // second (the rendered count was taken at the end of the last second, so it goes with last
                    // second's presents), and changed only after 3 seconds in a row agree. Comparing it with this
                    // second's presents swung it between 2 and 4 after a hitch in Pragmata (3x frame generation:
                    // 50 / 77 presented, every other second, until the limit was switched off and on).
                    static double presPrev = 0.0;
                    static int candPrev = 0, candRun = 0;
                    const double rf = Now() - g_rfxRenderedT.load() < 3.0 ? g_rfxRenderedFps.load() : 0.0;
                    int cand = 1;
                    if (stNow.fgActive && rf > 1.0 && presPrev > 1.0) cand = std::max(2, std::min(4, (int)(presPrev / rf + 0.5)));
                    if (!stNow.fgActive) cand = 1;
                    candRun = (cand == candPrev) ? candRun + 1 : 1;
                    candPrev = cand;
                    const int cur = g_sleepFgFactor.load();
                    // Up only after 3 seconds in a row, also from 1x: the second the limit goes on pairs the presents
                    // from before it with the rendered frames under it (God of War Ragnarok: 109 / 55 read as 2x).
                    // Back to 1x at once.
                    if (cand != cur && (candRun >= 3 || cand == 1))
                    {
                        g_sleepFgFactor.store(cand);
                        if (g_sleepLimitUs.load() > 0)
                            LOGI("BFI Presenter: frame limit at the game's Reflex sleep: frame generation %dx", cand);
                    }
                    presPrev = stNow.gameFps;
                }
                ReflexTick(limitUs, rs);
                rsLast = rs; rsLastT = t;
                if (rs.frames > 0) rsEverT = t;
                EnterCriticalSection(&g_cfgLock);
                g_status.rfxGameUses = rs.frames > 0 ? 1 : 0;
                g_status.rfxLimitUs = rs.limitUs;
                g_status.rfxSimGpuMs = rs.simGpuMs;
                LeaveCriticalSection(&g_cfgLock);
                if (rfxf)
                {
                    fprintf(rfxf, "%.3f,%.1f,%d,%d,%d,%d,%d,%d,%d,%d,%.2f,%d,%d,%.2f,%.2f,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f\n",
                            t, stNow.gameFps, userCap, perNow, stNow.autoCapDown, stNow.autoCapProbe, stNow.fgActive, s.pacingMode,
                            s.capJit, stNow.lrState, stNow.lrEst, stNow.aheadInUse, rs.limitUs,
                            relN ? relSum / relN : -1.0, relN ? relMax : -1.0, relN, rs.frames, rs.simGpuMs, rs.simPresentMs,
                            rs.inputGpuMs, rs.gpuFrameMs, rs.gpuActiveMs, rs.simMs, rs.submitMs, rs.queueMs,
                            Now() - g_rfxRenderedT.load() < 3.0 ? g_rfxRenderedFps.load() : -1.0,
                            alignLogSlack >= 0.0 ? alignLogSlack * 1000.0 : -1.0, alignShiftTot * 1000.0);
                    fflush(rfxf);
                }
                relSum = relMax = 0.0; relN = 0;
            }
            secStart = t; secPresents = 0; secLate = 0; ageSum = 0.0; secSlowPres = 0;
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
    if (rfxf) fclose(rfxf);
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
                // Keep everything it may touch alive and leave it be. Where it is stuck goes to the log first.
                LogThreadStack(GetThreadId(g_thread), "presenter thread (not stopping)");
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
    const UINT64 per = cap >= 1 ? (UINT64)std::max(0, g_capPer.load()) : 0;
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
// The game gets control back and starts its next frame: Reflex's sleep (forced
// mode), then the release time is noted for the release -> lit flash log.
static void GameReleased(UINT64 lastId)
{
    const UINT64 next = lastId + 1;
    g_relT[next % kRel] = Now();
    g_relId[next % kRel] = next;
}

static void FgPaceAfterPresent()
{
    if (t_inOuter) return;   // the outer swap chain's Present paces, after it returns
    const int us = g_fgPaceUs.load();
    if (us <= 0 || GetCurrentThreadId() != g_fgGameTid.load()) return;
    static thread_local HANDLE timer = nullptr;
    static double nextT = 0.0, phase = 0.0;   // phase: where on the vblank grid frames start (moved by the alignment)
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    const double iv = us * 1e-6;
    const double now = Now();
    if (nextT <= 0.0 || now > nextT + iv || nextT - now > 2.0 * iv) nextT = now;   // start, a hitch, or a changed limit
    const int sh = g_fgShiftUs.exchange(0);
    if (sh != 0) { nextT += sh * 1e-6; phase += sh * 1e-6; }
    // On the strobe-cycle grid (the last refresh the presenter drew, less its slot in the cycle): the interval is a
    // whole number of cycles, so frame generation's two frames keep the same place against the flashes instead of
    // drifting until both land in one strobe cycle (Riven: then only 30 of 60 frames a second were shown). On the
    // cycle and not just the refresh grid: after a hitch the start time is taken up again from the clock, and on the
    // refresh grid alone it came back a refresh or two off (Riven: 28 ms from release to flash before a hitch,
    // 37, 42 and 50 ms after the next ones).
    {
        UINT64 c = 0; int sl = 0; double tick = 0.0;
        for (int i = 0; i < 4; i++)
        {
            c = g_tickCount.load(); sl = g_tickSlot.load(); tick = g_tickT.load();
            if (g_tickCount.load() == c) break;
        }
        const double per = g_lrPeriod.load();
        const int frames = std::max(1, g_strobeFrames.load());
        const double cyc = per * frames;
        if (tick > 0.0 && per > 0.0 && now - tick < 0.1)
        {
            const double cycStart = tick - sl * per;
            double ph = std::fmod(phase, cyc);
            if (ph < 0.0) ph += cyc;
            nextT = cycStart + ph + std::floor((nextT - cycStart - ph) / cyc + 0.5) * cyc;
        }
    }
    const double held0 = Now();
    while (nextT - Now() > 0.015) SleepUntil(Now() + 0.015, timer);
    SleepUntil(nextT, timer);
    nextT += iv;
    // The automatic frame cap at the frame start: the game waits here instead of in the presenter's cap, so this is its
    // spare time for automatic frames ahead (counted only in the cap before, about 0 here: it stayed at 2 for good).
    if (g_fgPaceKind.load() == 2)
    {
        g_capWaitUs.fetch_add((long long)((Now() - held0) * 1e6));
        g_capWaits.fetch_add(1);
    }
    // The automatic frame cap at the frame start: the game's next frame starts now, so release -> flash is measured
    // from here (from the return of its Present it counted the hold too: Soul Reaver showed 34.7 ms where frame start
    // -> flash was about 21 ms, and the start-time search preferred the shorter holds).
    if (g_fgPaceKind.load() == 2)
    {
        const UINT64 next = g_frameId + 1;
        if (g_relId[next % kRel] == next) g_relT[next % kRel] = Now();
    }
}

static HRESULT SubmitGameFrame()
{
    CountPresent();
    CountTo(g_feCount);
    const DWORD tid = GetCurrentThreadId();
    const double enter = Now();
    FgCheckCaller();
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
    const UINT64 lastId = g_frameId;
    LeaveCriticalSection(&g_submitLock);
    GameReleased(lastId);
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

// DX11 games: the game's present thread, for the pacing after its present (FgPaceAfterPresent).
static void CountTo(ThreadCount &c);
static void NoteGameThread11()
{
    CountTo(g_feCount);
    const DWORD tid = GetCurrentThreadId();
    if (g_presentThreads[0] == tid) return;
    EnterCriticalSection(&g_submitLock);
    for (DWORD &t : g_presentThreads)
    {
        if (t == tid) break;
        if (t == 0)
        {
            t = tid;
            LOGI("BFI Presenter: game frames presented from thread %lu (DX11)", (unsigned long)tid);
            break;
        }
    }
    LeaveCriticalSection(&g_submitLock);
}

static HRESULT Present11Paced(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    NoteGameThread11();
    const HRESULT hr = SubmitGameFrame11(sc, sync, flags);
    FgPaceAfterPresent();
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    if (!t_ownCall && IsGame11(sc) && !(flags & DXGI_PRESENT_TEST)) return Present11Paced(sc, sync, flags);
    if (StuckPassThrough(sc)) { const HRESULT hr = o_Present(sc, sync, flags); ReleaseGameSlot(); return hr; }
    if (t_ownCall || !IsManaged(sc) || (flags & DXGI_PRESENT_TEST))
        return o_Present(sc, sync, flags);
    const HRESULT hr = SubmitGameFrame();
    FgPaceAfterPresent();
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *p)
{
    if (!t_ownCall && IsGame11(sc) && !(flags & DXGI_PRESENT_TEST)) return Present11Paced(sc, sync, flags);
    if (StuckPassThrough(sc)) { const HRESULT hr = o_Present1(sc, sync, flags, p); ReleaseGameSlot(); return hr; }
    if (t_ownCall || !IsManaged(sc) || (flags & DXGI_PRESENT_TEST))
        return o_Present1(sc, sync, flags, p);
    const HRESULT hr = SubmitGameFrame();
    FgPaceAfterPresent();
    return hr;
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
    // Not hooked yet: still build the copy over the first 16 bytes, so a hook
    // written later (RTSS hooks Present whenever its OSD turns on, even in a
    // game that started without it -- it crashed Alan Wake 2) is skipped too.
    // If those bytes can't be relocated, the function itself is still fine.
    const bool notYet = !patched;
    if (notYet) patched = 16;
    // Whole original instructions covering the patched bytes.
    int ins[16], rip[16], n = 0;
    size_t len = 0;
    while (len < patched)
    {
        int rd;
        const int l = InsnLength(orig + len, &rd);
        if (l <= 0 || len + l > 48 || n >= 16)
        {
            if (notYet) { snprintf(why, whyLen, "not hooked (start not relocatable: later hooks stay in the path)"); return target; }
            snprintf(why, whyLen, "unsupported instruction at +%zu (%02X)", len, orig[len]); return nullptr;
        }
        ins[n] = (int)len; rip[n] = rd; n++;
        len += (size_t)l;
    }
#ifndef _WIN64
    // x86: no RIP-relative operands; copy the instructions and jmp rel32 back.
    {
        BYTE *t = (BYTE *)VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!t) { snprintf(why, whyLen, "no memory"); return notYet ? target : nullptr; }
        memcpy(t, orig, len);
        t[len] = 0xE9;
        const int32_t rel = (int32_t)((intptr_t)(code + len) - (intptr_t)(t + len + 5));
        memcpy(t + len + 1, &rel, 4);
        DWORD o;
        VirtualProtect(t, 4096, PAGE_EXECUTE_READ, &o);
        FlushInstructionCache(GetCurrentProcess(), t, len + 5);
        if (notYet) snprintf(why, whyLen, "not hooked (copy of its start in place, later hooks skipped)");
        else snprintf(why, whyLen, "%zu patched bytes skipped", patched);
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
    if (!tramp) { snprintf(why, whyLen, "no memory near the function"); return notYet ? target : nullptr; }
    memcpy(tramp, orig, len);
    for (int k = 0; k < n; k++)
    {
        if (rip[k] < 0) continue;
        const int at = ins[k] + rip[k];
        int32_t disp;
        memcpy(&disp, orig + at, 4);
        const int64_t abs = (int64_t)(uintptr_t)code + at + 4 + disp;     // what it pointed at
        const int64_t nd  = abs - ((int64_t)(uintptr_t)tramp + at + 4);
        if (nd < INT32_MIN || nd > INT32_MAX) { VirtualFree(tramp, 0, MEM_RELEASE); snprintf(why, whyLen, "displacement out of range"); return notYet ? target : nullptr; }
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
    if (notYet) snprintf(why, whyLen, "not hooked (copy of its start in place, later hooks skipped)");
    else snprintf(why, whyLen, "%zu patched bytes skipped", patched);
    return tramp;
}

// ---------------------------------------------------------------- NVIDIA Reflex (NvAPI)
// Two uses, both for the automatic frame cap:
//  * The driver's latency reports (every game that uses Reflex has them, and every DLSS-G
//    game uses Reflex): rendered frames a second from the game's own frame numbers (frame
//    generation presents about twice as many), the game's GPU and CPU time per frame (can it
//    hold the refresh rate again?), and the latency columns of the timing log.
//  * With frame generation: Reflex's frame limit at one presented frame per strobe cycle.
//    The driver then paces DLSS-G itself, so every lit refresh gets the next frame (God of
//    War Ragnarok: 60 of 60, against 57 at ~76 fps without it). Holding DLSS-G's presents back
//    with the presenter's own cap instead broke its pacing: only 30 of its 60 frames a second
//    reached the screen.
// The game's own sleep mode is watched (an inline hook on NvAPI_D3D_SetSleepMode, set up
// only while the limit is wanted), so its low-latency and Boost choices are kept, and the
// limit is sent again every 2 s in case the game or the driver resets it.
#ifdef _WIN64
struct NvSleepParams   { uint32_t version; uint8_t lowLatency, boost, pad0[2]; uint32_t minIntervalUs; uint8_t useMarkers, rsvd[31]; };
struct NvSleepStatus   { uint32_t version; uint8_t lowLatency, rsvd[127]; };
struct NvFrameReport
{
    uint64_t frameID, inputSample, simStart, simEnd, submitStart, submitEnd, presentStart, presentEnd,
             driverStart, driverEnd, queueStart, queueEnd, gpuStart, gpuEnd;
    uint32_t gpuActiveUs, gpuFrameUs;
    uint8_t  rsvd[120];
};
struct NvLatencyResult { uint32_t version; uint32_t pad0; NvFrameReport r[64]; uint8_t rsvd[32]; };
static_assert(sizeof(NvSleepParams) == 44 && sizeof(NvSleepStatus) == 132 && sizeof(NvFrameReport) == 240 &&
              sizeof(NvLatencyResult) == 15400, "NvAPI struct layout");
#define BFI_NVVER(T) ((uint32_t)sizeof(T) | (1u << 16))
typedef void *(__cdecl *PFN_NvQI)(unsigned int);
typedef int   (__cdecl *PFN_NvInit)();
typedef int   (__cdecl *PFN_NvSetSleepMode)(IUnknown *, NvSleepParams *);
typedef int   (__cdecl *PFN_NvGetSleepStatus)(IUnknown *, NvSleepStatus *);
typedef int   (__cdecl *PFN_NvGetLatency)(IUnknown *, NvLatencyResult *);
static PFN_NvSetSleepMode   g_nvSetSleepMode = nullptr;     // the function itself (until hooked)
static PFN_NvSetSleepMode   o_NvSetSleepMode = nullptr;     // original (trampoline) once hooked
static PFN_NvGetSleepStatus g_nvGetSleepStatus = nullptr;
static PFN_NvGetLatency     g_nvGetLatency = nullptr;
static SRWLOCK              g_rfxLock = SRWLOCK_INIT;
static IUnknown            *g_rfxDev = nullptr;              // device the game passed to SetSleepMode
static NvSleepParams        g_rfxGame = {}, g_rfxApplied = {};
static bool                 g_rfxGameSeen = false, g_rfxAppliedValid = false;
static int                  g_rfxApi = 0;                    // 0 not tried, 1 available, -1 unavailable
static int                  g_rfxHooked = 0;                 // 0 not tried, 1 hooked, -1 couldn't
static std::atomic<int>     g_rfxWantLimitUs{0};             // 0 = no limit from the presenter
static UINT64               g_rfxLastReport = 0;

static int __cdecl hk_NvSetSleepMode(IUnknown *dev, NvSleepParams *p)
{
    if (!p) return o_NvSetSleepMode(dev, p);
    AcquireSRWLockExclusive(&g_rfxLock);
    const bool changed = !g_rfxGameSeen || g_rfxGame.lowLatency != p->lowLatency || g_rfxGame.boost != p->boost ||
                         g_rfxGame.minIntervalUs != p->minIntervalUs;
    g_rfxGame = *p;
    g_rfxGameSeen = true;
    g_rfxDev = dev;
    const int us = g_rfxWantLimitUs.load();
    NvSleepParams q = *p;
    g_gameOwnUs.store((int)p->minIntervalUs);
    // The game's own limit also counts the presenter's presents in the driver (Returnal: 16,666 us -> 8 fps):
    // with the sleep-point limit in charge it is applied there instead, and the driver gets none.
    const bool strip = us <= 0 && (g_rfxLimitBad.load() || g_sleepActive.load() || g_sleepLimitUs.load() > 0 || g_sleepFgStrip.load()) && p->minIntervalUs > 0;
    if (us > 0) q.minIntervalUs = (uint32_t)us;
    if (strip) q.minIntervalUs = 0;
    if (us > 0) { g_rfxApplied = q; g_rfxAppliedValid = true; }
    ReleaseSRWLockExclusive(&g_rfxLock);
    if (changed)
        LOGI("BFI Presenter: game Reflex: low latency %s%s, frame limit %u us%s", p->lowLatency ? "on" : "off",
             p->boost ? " + Boost" : "", p->minIntervalUs, us > 0 ? " -- the presenter's frame limit applies on top" :
             strip ? (g_sleepActive.load() ? " -- applied at the game's Reflex sleep instead" : " -- taken out (the game collapsed under it)") : "");
    return o_NvSetSleepMode(dev, (us > 0 || strip) ? &q : p);
}

// ---------------------------------------------------------------- frame limit at the game's Reflex sleep
// The driver's frame limit (Reflex's minimum interval, and NVIDIA's Max Frame Rate while Reflex is on)
// paces the game by the presents it sees on the swap chain. With the presenter those are the presenter's
// own, 120 a second whatever the game does, so the driver keeps sleeping the game longer and it ends at
// ~7 rendered fps (The Witcher 3 under NVPI's 60 fps limit and under the presenter's Reflex limit; Returnal
// ~7.5). Instead the presenter paces the game where Reflex would: in the game's own NvAPI_D3D_Sleep call at
// the start of each frame, one rendered frame per (limit x frame-generation factor), and the driver gets
// no limit of its own.
typedef int (__cdecl *PFN_NvSleep)(IUnknown *);
static PFN_NvSleep       g_nvSleep = nullptr, o_NvSleep = nullptr;
static int               g_sleepHooked = 0;           // 0 not tried, 1 hooked, -1 couldn't
static std::atomic<long> g_sleepCalls{0};
static int __cdecl hk_NvSleep(IUnknown *dev)
{
    g_sleepCalls.fetch_add(1);
    const int us = g_sleepLimitUs.load();
    if (us > 0)
    {
        static thread_local HANDLE timer = nullptr;
        static double nextT = 0.0;
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        const double iv = us * 1e-6;
        double now = Now();
        if (nextT <= 0.0 || now > nextT + iv || nextT - now > 2.0 * iv) nextT = now;   // start, a hitch, or a changed limit
        const int sh = g_sleepShiftUs.exchange(0);
        if (sh != 0) nextT += sh * 1e-6;
        while (nextT - Now() > 0.015) SleepUntil(Now() + 0.015, timer);
        SleepUntil(nextT, timer);
        nextT += iv;
    }
    return o_NvSleep(dev);
}

// A function that already starts with a jump (someone else's inline hook, or a jump stub: Pragmata's
// NvAPI_D3D_Sleep and Streamline's slReflexSetOptions start with a 2-byte nop and a jmp rel32): the jump's
// target is changed to a relay to hook, and the returned "original" runs the bytes before the jump and
// then jumps where it used to go, so the earlier hook keeps working behind ours.
static void *HookByJumpChain(BYTE *code, size_t at, void *hook, char *why, size_t whyLen)
{
    int32_t rel;
    memcpy(&rel, code + at + 1, 4);
    BYTE *dest = code + at + 5 + rel;
    BYTE *page = nullptr;
    const uintptr_t base = (uintptr_t)code & ~(uintptr_t)0xFFFF;
    for (uintptr_t d = 0x10000; d < 0x40000000 && !page; d += 0x10000)
    {
        page = (BYTE *)VirtualAlloc((void *)(base - d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!page) page = (BYTE *)VirtualAlloc((void *)(base + d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    if (!page) { snprintf(why, whyLen, "no memory near the function"); return nullptr; }
    // page+0: the original -- the bytes before the jump, then jmp [rip] -> dest
    memcpy(page, code, at);
    BYTE *j = page + at;
    j[0] = 0xFF; j[1] = 0x25; j[2] = j[3] = j[4] = j[5] = 0;
    const uint64_t d64 = (uint64_t)(uintptr_t)dest;
    memcpy(j + 6, &d64, 8);
    // page+64: relay to the hook
    BYTE *relay = page + 64;
    relay[0] = 0xFF; relay[1] = 0x25; relay[2] = relay[3] = relay[4] = relay[5] = 0;
    const uint64_t h64 = (uint64_t)(uintptr_t)hook;
    memcpy(relay + 6, &h64, 8);
    const int64_t nrel = (int64_t)(uintptr_t)relay - ((int64_t)(uintptr_t)code + (int64_t)at + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) { VirtualFree(page, 0, MEM_RELEASE); snprintf(why, whyLen, "relay out of range"); return nullptr; }
    DWORD old;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 128);
    if (!VirtualProtect(code + at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { snprintf(why, whyLen, "can't write the function"); return nullptr; }
    const int32_t n32 = (int32_t)nrel;
    InterlockedExchange((volatile LONG *)(code + at + 1), (LONG)n32);   // not always aligned; x64 tolerates it
    VirtualProtect(code + at + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, at + 5);
    snprintf(why, whyLen, "chained behind the jump at +%zu", at);
    return page;
}

// Patches target's first instructions with a jump to hook; returns a callable copy
// of the original start (nullptr if the start can't be relocated).
static void *InlineHook64(void *target, void *hook, char *why, size_t whyLen)
{
    BYTE *code = (BYTE *)target;
    int ins[16], rip[16], n = 0;
    size_t len = 0;
    while (len < 14)
    {
        int rd;
        // A jump already at the start (only plain instructions before it, no RIP-relative ones): chain behind it.
        if (code[len] == 0xE9 && len <= 8)
        {
            bool plain = true;
            for (int k = 0; k < n; k++) if (rip[k] >= 0) plain = false;
            if (plain) return HookByJumpChain(code, len, hook, why, whyLen);
        }
        const int l = InsnLength(code + len, &rd);
        if (l <= 0 || n >= 16) { snprintf(why, whyLen, "unsupported instruction at +%zu (%02X)", len, code[len]); return nullptr; }
        ins[n] = (int)len; rip[n] = rd; n++;
        len += (size_t)l;
    }
    BYTE *tramp = nullptr;
    const uintptr_t base = (uintptr_t)target & ~(uintptr_t)0xFFFF;
    for (uintptr_t d = 0x10000; d < 0x40000000 && !tramp; d += 0x10000)
    {
        tramp = (BYTE *)VirtualAlloc((void *)(base - d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!tramp) tramp = (BYTE *)VirtualAlloc((void *)(base + d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    if (!tramp) { snprintf(why, whyLen, "no memory near the function"); return nullptr; }
    memcpy(tramp, code, len);
    for (int k = 0; k < n; k++)
    {
        if (rip[k] < 0) continue;
        const int at = ins[k] + rip[k];
        int32_t disp;
        memcpy(&disp, code + at, 4);
        const int64_t abs = (int64_t)(uintptr_t)code + at + 4 + disp;
        const int64_t nd  = abs - ((int64_t)(uintptr_t)tramp + at + 4);
        if (nd < INT32_MIN || nd > INT32_MAX) { VirtualFree(tramp, 0, MEM_RELEASE); snprintf(why, whyLen, "displacement out of range"); return nullptr; }
        const int32_t nd32 = (int32_t)nd;
        memcpy(tramp + at, &nd32, 4);
    }
    BYTE *j = tramp + len;
    j[0] = 0xFF; j[1] = 0x25; j[2] = j[3] = j[4] = j[5] = 0;
    const uint64_t back = (uint64_t)(uintptr_t)(code + len);
    memcpy(j + 6, &back, 8);
    DWORD old;
    VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 14);
    BYTE patch[48];
    patch[0] = 0xFF; patch[1] = 0x25; patch[2] = patch[3] = patch[4] = patch[5] = 0;
    const uint64_t to = (uint64_t)(uintptr_t)hook;
    memcpy(patch + 6, &to, 8);
    for (size_t i = 14; i < len; i++) patch[i] = 0x90;
    if (!VirtualProtect(code, len, PAGE_EXECUTE_READWRITE, &old)) { snprintf(why, whyLen, "can't write the function"); return nullptr; }
    memcpy(code, patch, len);
    VirtualProtect(code, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, len);
    return tramp;
}

// NvAPI functions, once nvapi64.dll is in the game (it isn't loaded by the presenter).
static bool ReflexApi()
{
    if (g_rfxApi != 0) return g_rfxApi == 1;
    HMODULE nv = GetModuleHandleA("nvapi64.dll");
    if (!nv) return false;                                         // tried again each second
    PFN_NvQI qi = (PFN_NvQI)(void *)GetProcAddress(nv, "nvapi_QueryInterface");
    if (qi)
    {
        if (PFN_NvInit init = (PFN_NvInit)qi(0x0150E828)) init();
        g_nvSetSleepMode   = (PFN_NvSetSleepMode)qi(0xAC1CA9E0);
        g_nvGetSleepStatus = (PFN_NvGetSleepStatus)qi(0xAEF96CA1);
        g_nvGetLatency     = (PFN_NvGetLatency)qi(0x1A587F9C);
        g_nvSleep          = (PFN_NvSleep)qi(0x852CD1D2);
    }
    g_rfxApi = (g_nvSetSleepMode && g_nvGetLatency) ? 1 : -1;
    if (g_rfxApi < 0) LOGW("BFI Presenter: Reflex: NvAPI latency functions not available");
    return g_rfxApi == 1;
}

static void ReflexWatch()
{
    if (g_rfxHooked != 0 || !g_nvSetSleepMode) return;
    char why[96] = "";
    void *orig = InlineHook64((void *)g_nvSetSleepMode, (void *)&hk_NvSetSleepMode, why, sizeof(why));
    if (orig) { o_NvSetSleepMode = (PFN_NvSetSleepMode)orig; g_rfxHooked = 1; }
    else
    {
        o_NvSetSleepMode = g_nvSetSleepMode;
        g_rfxHooked = -1;
        LOGW("BFI Presenter: Reflex: the game's own sleep mode can't be watched (%s); the frame limit is sent every 2 s", why);
    }
}

// Once a second, from the presenter thread. limitUs: the frame limit wanted (0 = none).
static int ReflexGameLimitUs()
{
    AcquireSRWLockShared(&g_rfxLock);
    const int us = g_rfxGameSeen ? (int)g_rfxGame.minIntervalUs : 0;
    ReleaseSRWLockShared(&g_rfxLock);
    return us;
}

static void ReflexTick(int limitUs, ReflexSecond &out)
{
    out = ReflexSecond();
    if (!ReflexApi()) return;
    if (g_rfxLimitBad.load()) limitUs = 0;
    const int limitAll = limitUs;
    // The game's own frames paced in its Reflex sleep call (see hk_NvSleep): nothing for the driver.
    bool atSleep = false;
    {
        EnterCriticalSection(&g_cfgLock);
        const bool want = g_shared.reflexAtSleep != 0;
        LeaveCriticalSection(&g_cfgLock);
        if (want && g_nvSleep && g_sleepHooked == 0)
        {
            char why[96] = "";
            void *orig = InlineHook64((void *)g_nvSleep, (void *)&hk_NvSleep, why, sizeof(why));
            if (orig) { o_NvSleep = (PFN_NvSleep)orig; g_sleepHooked = 1; LOGI("BFI Presenter: frame limit at the game's Reflex sleep: hooked%s%s%s", why[0] ? " (" : "", why, why[0] ? ")" : ""); }
            else { g_sleepHooked = -1; LOGW("BFI Presenter: the game's Reflex sleep can't be hooked (%s) -- the driver's frame limit is used", why); }
        }
        static long lastCalls = 0;
        static double lastCallsT = 0.0;
        static int perFrame = 1, pfCand = 1, pfRun = 0;
        const long calls = g_sleepCalls.load();
        const bool sleeping = calls != lastCalls;     // the game calls it (each frame)
        {
            // Sleep calls per rendered frame (DLSS 5 injectors with frame generation in Onimusha make two). The
            // sleep-point limit is used only with one call per frame; changes need 3 seconds in a row.
            const double tn = Now(), dt = tn - lastCallsT;
            const double rf = tn - g_rfxRenderedT.load() < 3.0 ? g_rfxRenderedFps.load() : 0.0;
            if (lastCallsT > 0.0 && dt > 0.5 && dt < 3.0 && rf > 5.0)
            {
                const int c = std::max(1, std::min(4, (int)((calls - lastCalls) / dt / rf + 0.5)));
                pfRun = c == pfCand ? pfRun + 1 : 1;
                pfCand = c;
                if (c != perFrame && pfRun >= 3)
                {
                    LOGI("BFI Presenter: frame limit at the game's Reflex sleep: %d sleep call(s) per rendered frame", c);
                    perFrame = c;
                }
            }
            lastCallsT = tn;
        }
        lastCalls = calls;
        // The game's own limit is taken over only without frame generation (Returnal).
        // More than one sleep call per rendered frame (DLSS 5 injectors with frame generation in Onimusha: two):
        // pacing the calls held the game at 15 rendered fps or made frames uneven (3-7 a second not shown), while the
        // driver's limit held 60 evenly there, so the driver gets the limit; the collapse check still guards it.
        const bool oneCall = perFrame == 1;
        {
            static bool multiLogged = false;
            if (!oneCall && want && g_sleepHooked == 1 && !multiLogged)
            {
                multiLogged = true;
                LOGI("BFI Presenter: the game calls its Reflex sleep %d times per rendered frame -- the driver's frame limit is used", perFrame);
            }
        }
        g_sleepActive.store(want && oneCall && g_sleepHooked == 1 && sleeping && !g_rfxLimitBad.load() && g_sleepFgFactor.load() == 1);
        // With frame generation a game's (or injector's) own Reflex limit would collapse behind the presenter in the
        // driver, so it is taken out, as 1.0.18 ended up doing after its collapse check: frame generation then runs
        // freely and the automatic frame cap's own limit (at the sleep point) paces it.
        g_sleepFgStrip.store(want && oneCall && g_sleepHooked == 1 && sleeping && g_sleepFgFactor.load() > 1);
        if (want && oneCall && g_sleepHooked == 1 && (sleeping || limitAll <= 0))
        {
            const int f = std::max(1, std::min(4, g_sleepFgFactor.load()));
            const int own = (g_rfxLimitBad.load() || f > 1) ? 0 : g_gameOwnUs.load();   // the game's own limit, if any (not with frame generation)
            const int base = std::max(limitAll, own);
            const int us = base > 0 ? base * f : 0;
            const int before = g_sleepLimitUs.exchange(us);
            if ((before > 0) != (us > 0))
                LOGI("BFI Presenter: frame limit at the game's Reflex sleep %s (%d us per rendered frame%s)", us > 0 ? "on" : "off", us,
                     f > 1 ? ", frame generation" : "");
            atSleep = us > 0;
        }
        else if (g_sleepLimitUs.load() > 0) g_sleepLimitUs.store(0);
    }
    if (atSleep) limitUs = 0;
    g_rfxWantLimitUs.store(limitUs);
    if (limitAll > 0 || g_rfxLimitBad.load() || g_sleepActive.load() || g_sleepFgStrip.load()) ReflexWatch();
    IUnknown *dev = nullptr;
    AcquireSRWLockShared(&g_rfxLock);
    dev = g_rfxDev;
    const bool seen = g_rfxGameSeen;
    const NvSleepParams game = g_rfxGame, applied = g_rfxApplied;
    const bool appliedValid = g_rfxAppliedValid;
    ReleaseSRWLockShared(&g_rfxLock);
    if (!dev) dev = ReflexOwnDevice();
    if (!dev) return;

    static NvLatencyResult res;
    memset(&res, 0, sizeof(res));
    res.version = BFI_NVVER(NvLatencyResult);
    bool markers = false;
    if (g_nvGetLatency(dev, &res) == 0)
    {
        double a = 0, b = 0, c = 0, g = 0, q = 0, ga = 0, sim = 0, sub = 0;
        int n = 0, ni = 0;
        UINT64 newest = g_rfxLastReport;
        for (const NvFrameReport &r : res.r)
        {
            if (!r.frameID || r.frameID <= g_rfxLastReport || !r.simStart || r.gpuEnd < r.simStart) continue;
            if (r.gpuEnd - r.simStart > 1000000) continue;                 // not a plausible frame
            a += (r.gpuEnd - r.simStart) / 1000.0;
            b += r.presentEnd >= r.simStart ? (r.presentEnd - r.simStart) / 1000.0 : 0.0;
            if (r.inputSample && r.gpuEnd >= r.inputSample) { c += (r.gpuEnd - r.inputSample) / 1000.0; ni++; }
            g += r.gpuFrameUs / 1000.0;
            ga += r.gpuActiveUs / 1000.0;
            q += r.queueEnd >= r.queueStart ? (r.queueEnd - r.queueStart) / 1000.0 : 0.0;
            sim += r.simEnd >= r.simStart ? (r.simEnd - r.simStart) / 1000.0 : 0.0;
            sub += r.submitEnd >= r.submitStart ? (r.submitEnd - r.submitStart) / 1000.0 : 0.0;
            n++;
            if (r.frameID > newest) newest = r.frameID;
        }
        if (n)
        {
            markers = true;
            out.frames = n;
            out.simGpuMs = a / n; out.simPresentMs = b / n; out.gpuFrameMs = g / n; out.queueMs = q / n;
            out.gpuActiveMs = ga / n; out.simMs = sim / n; out.submitMs = sub / n;
            if (ni) out.inputGpuMs = c / ni;
        }
        // Rendered frames a second, from the game's own frame numbers.
        static UINT64 prevNewest = 0;
        static double prevT = 0.0;
        const double tn = Now();
        if (prevNewest && newest > prevNewest && newest - prevNewest < 2000 && tn - prevT > 0.5 && tn - prevT < 3.0)
        {
            out.renderedFps = (double)(newest - prevNewest) / (tn - prevT);
            g_rfxRenderedFps.store(out.renderedFps);
            g_rfxRenderedT.store(tn);
        }
        prevNewest = newest; prevT = tn;
        g_rfxLastReport = newest;
    }

    // The frame limit: on top of the game's own mode (as seen, or as the driver reports it).
    const bool gameUses = seen || markers;
    if (gameUses && (limitUs > 0 || appliedValid))
    {
        NvSleepParams base = {};
        if (seen) base = game;
        else if (g_nvGetSleepStatus)
        {
            NvSleepStatus st = {};
            st.version = BFI_NVVER(NvSleepStatus);
            if (g_nvGetSleepStatus(dev, &st) == 0) base.lowLatency = st.lowLatency;
            base.useMarkers = 1;
        }
        base.version = BFI_NVVER(NvSleepParams);
        NvSleepParams want = base;
        if (limitUs > 0) want.minIntervalUs = (uint32_t)limitUs;
        static double lastSent = 0.0;
        const bool same = appliedValid && applied.minIntervalUs == want.minIntervalUs && applied.lowLatency == want.lowLatency &&
                          applied.boost == want.boost;
        const bool need = limitUs > 0 ? (!same || Now() - lastSent > 2.0) : appliedValid;
        if (need)
        {
            PFN_NvSetSleepMode set = o_NvSetSleepMode ? o_NvSetSleepMode : g_nvSetSleepMode;
            const int r = set(dev, &want);
            lastSent = Now();
            AcquireSRWLockExclusive(&g_rfxLock);
            g_rfxApplied = want;
            g_rfxAppliedValid = limitUs > 0;
            ReleaseSRWLockExclusive(&g_rfxLock);
            const bool onOff = !appliedValid || (applied.minIntervalUs == 0) != (want.minIntervalUs == 0) || limitUs == 0;
            if ((!same && onOff) || r != 0)
                LOGI("BFI Presenter: Reflex frame limit %s (%d)", limitUs > 0 ? "on: one presented frame per strobe cycle" :
                     "off: the game's own setting", r);
        }
    }
    // The game's own limit taken out (g_rfxLimitBad): its setting again without the limit, every
    // 2 s while it has one (the hook strips it from the game's own calls).
    if ((g_rfxLimitBad.load() || g_sleepActive.load() || g_sleepLimitUs.load() > 0 || g_sleepFgStrip.load()) && limitUs <= 0 && seen && game.minIntervalUs > 0)
    {
        static double lastStrip = 0.0;
        if (Now() - lastStrip > 2.0)
        {
            NvSleepParams want = game;
            want.version = BFI_NVVER(NvSleepParams);
            want.minIntervalUs = 0;
            PFN_NvSetSleepMode set = o_NvSetSleepMode ? o_NvSetSleepMode : g_nvSetSleepMode;
            set(dev, &want);
            lastStrip = Now();
        }
    }
    out.limitUs = (limitAll > 0 && (gameUses || atSleep)) ? limitAll : 0;
}
#else
static int ReflexGameLimitUs() { return 0; }
static void ReflexTick(int, ReflexSecond &out) { out = ReflexSecond(); }
#endif

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

// ShowOverlays, as it was when the presenter first presented: overlays set themselves up on the first
// present they see and remember things such as the queue (God of War Ragnarok crashed when that changed),
// so the path is not switched while the game runs.
static int g_showOverlaysBoot = -1;

static HRESULT OwnPresent()
{
    OwnCall own;
    if (g_showOverlaysBoot < 0)
    {
        g_showOverlaysBoot = g_shared.showOverlays ? 1 : 0;
#ifndef _WIN64
        // 32-bit games crashed or still showed no overlay in testing (Heretic II).
        if (g_showOverlaysBoot) { g_showOverlaysBoot = 0; LOGW("BFI Presenter: ShowOverlays ignored -- not supported in 32-bit games (crashes or no overlay)"); }
#endif
        // OpenGL: RTSS and Steam hook the game's OpenGL presents, which the presenter replaces; going through
        // DXGI's hooks doesn't bring their overlay back.
        if (g_showOverlaysBoot && g_glActive.load()) { g_showOverlaysBoot = 0; LOGW("BFI Presenter: ShowOverlays ignored -- not supported in OpenGL games"); }
        if (g_showOverlaysBoot)
            LOGI("BFI Presenter: ShowOverlays=1 -- the presenter's presents go through overlay hooks (RTSS OSD, Steam "
                 "overlay); keep RTSS's frame limiter off");
    }
    if (g_showOverlaysBoot == 1)
    {
        // Through the hooks: the swap chain's vtable (this add-on's hook passes own calls straight on) for
        // the DX11-mode swap chain, the original Present below this add-on's hook for a DX12 game's.
        if (g_dx11.load()) return g_sc->Present(1, 0);
        return o_Present(g_sc, 1, 0);
    }
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

// ---------------------------------------------------------------- outer (game-facing) swap chain
// OptiScaler hands the game a wrapper swap chain; with FSR frame generation behind it every real present comes
// from FSR's own thread, so the presenter's hook on the real swap chain can't tell the game's frames apart and
// can't hold the game where it starts a frame. The wrapper's Present is still called by the game, once per
// rendered frame: counted there, and the frame limit for frame generation without Reflex sleeps right after it.
// Found by hooking the factory the game gets (through its own dxgi.dll import) before it creates its swap chain.
static HRESULT STDMETHODCALLTYPE hk_OuterPresent(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST) return o_OuterPresent(sc, sync, flags);
    CountTo(g_outerCount);
    t_inOuter++;
    const HRESULT hr = o_OuterPresent(sc, sync, flags);
    t_inOuter--;
    FgPaceAfterPresent();
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_OuterPresent1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *p)
{
    if (flags & DXGI_PRESENT_TEST) return o_OuterPresent1(sc, sync, flags, p);
    CountTo(g_outerCount);
    t_inOuter++;
    const HRESULT hr = o_OuterPresent1(sc, sync, flags, p);
    t_inOuter--;
    FgPaceAfterPresent();
    return hr;
}
static void OuterCheck(void *obj)
{
    if (!obj) return;
    void **vt = *(void ***)obj;
    if (vt == g_hookedVtbl || vt == g_outerVtbl) return;
    // DXGI's own swap chain (no wrapper): the hook on the real swap chain already sees the game's presents.
    static HMODULE sysDxgi = nullptr;
    if (!sysDxgi)
    {
        wchar_t path[MAX_PATH];
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n && n < MAX_PATH - 12) { wcscat(path, L"\\dxgi.dll"); sysDxgi = GetModuleHandleW(path); }
    }
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[VT_Present], &m);
    char mod[64];
    ModuleOf(vt[VT_Present], mod, sizeof(mod));
    if (!m || m == sysDxgi)
    {
        LOGI("BFI Presenter: the game's swap chain is DXGI's own (%s) -- no outer swap chain to hook", mod);
        return;
    }
    if (g_outerVtbl) { LOGW("BFI Presenter: another outer swap chain type (%s) -- not hooked", mod); return; }
    {
        char l[64]; size_t i = 0;
        for (; mod[i] && i < sizeof(l) - 1; i++) l[i] = (char)tolower((unsigned char)mod[i]);
        l[i] = 0;
        if (!strstr(l, "fidelityfx") && !strstr(l, "ffx"))
        {
            LOGI("BFI Presenter: the game presents to a wrapper swap chain (%s) -- not FSR frame generation, so the game's "
                 "presents are counted on the real swap chain as before", mod);
            return;
        }
    }
    PatchSlot(vt, VT_Present,  (void *)&hk_OuterPresent,  (void **)&o_OuterPresent);
    PatchSlot(vt, VT_Present1, (void *)&hk_OuterPresent1, (void **)&o_OuterPresent1);
    g_outerVtbl = vt;
    LOGI("BFI Presenter: the game presents to FSR frame generation's swap chain (%s) -- its presents are counted there "
         "(FSR presents every frame from a thread of its own)", mod);
}
typedef HRESULT (STDMETHODCALLTYPE *PFN_FCreateSC)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_FCreateSCHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_FCreateSCCore)(IDXGIFactory2 *, IUnknown *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *,
                                                        IDXGIOutput *, IDXGISwapChain1 **);
static PFN_FCreateSC     o_FCreateSC = nullptr;
static PFN_FCreateSCHwnd o_FCreateSCHwnd = nullptr;
static PFN_FCreateSCCore o_FCreateSCCore = nullptr;
static HRESULT STDMETHODCALLTYPE hk_FCreateSC(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out)
{
    // Only the outermost call: wrappers create the real swap chain (and ReShade its own) inside it, through the
    // same factory methods, and the game gets the object the outermost call returns.
    t_scCreateDepth++;
    const HRESULT hr = o_FCreateSC(f, dev, d, out);
    const int depth = t_scCreateDepth--;
    if (SUCCEEDED(hr) && out && !t_ownCall && depth == 1) OuterCheck(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_FCreateSCHwnd(IDXGIFactory2 *f, IUnknown *dev, HWND w, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *o, IDXGISwapChain1 **out)
{
    // Only the outermost call: wrappers create the real swap chain (and ReShade its own) inside it, through the
    // same factory methods, and the game gets the object the outermost call returns.
    t_scCreateDepth++;
    const HRESULT hr = o_FCreateSCHwnd(f, dev, w, d, fs, o, out);
    const int depth = t_scCreateDepth--;
    if (SUCCEEDED(hr) && out && !t_ownCall && depth == 1) OuterCheck(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_FCreateSCCore(IDXGIFactory2 *f, IUnknown *dev, IUnknown *win, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                  IDXGIOutput *o, IDXGISwapChain1 **out)
{
    // Only the outermost call: wrappers create the real swap chain (and ReShade its own) inside it, through the
    // same factory methods, and the game gets the object the outermost call returns.
    t_scCreateDepth++;
    const HRESULT hr = o_FCreateSCCore(f, dev, win, d, o, out);
    const int depth = t_scCreateDepth--;
    if (SUCCEEDED(hr) && out && !t_ownCall && depth == 1) OuterCheck(*out);
    return hr;
}
// The function the game itself imports from dxgi.dll (its own import table): with OptiScaler that is
// OptiScaler's dxgi.dll, so the factory made here is the kind the game gets.
static void *GameDxgiImport(const char *fn)
{
    BYTE *base = (BYTE *)GetModuleHandleW(nullptr);
    if (!base) return nullptr;
    auto *dos = (IMAGE_DOS_HEADER *)base;
    auto *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++)
    {
        if (_stricmp((const char *)(base + imp->Name), "dxgi.dll") != 0) continue;
        auto *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto *addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, fn) == 0) return (void *)addrs->u1.Function;
        }
    }
    return nullptr;
}
static void InstallOuterFactoryHook()
{
    static bool done = false;
    if (done) return;
    done = true;
    if (!GetPrivateProfileIntA("BFI", "HookOuterSwapChain", 1, g_iniPath)) { LOGI("BFI Presenter: outer swap chain hook off (ini)"); return; }
    typedef HRESULT (WINAPI *PFN_CF2)(UINT, REFIID, void **);
    typedef HRESULT (WINAPI *PFN_CF1)(REFIID, void **);
    const char *from = "the game's import";
    PFN_CF2 cf2 = (PFN_CF2)GameDxgiImport("CreateDXGIFactory2");
    PFN_CF1 cf1 = (PFN_CF1)GameDxgiImport("CreateDXGIFactory1");
    if (!cf2 && !cf1)
    {
        from = "dxgi.dll";
        HMODULE d = GetModuleHandleA("dxgi.dll");
        if (d) { cf2 = (PFN_CF2)GetProcAddress(d, "CreateDXGIFactory2"); cf1 = (PFN_CF1)GetProcAddress(d, "CreateDXGIFactory1"); }
    }
    IDXGIFactory2 *f = nullptr;
    {
        OwnCall own;
        if (cf2) cf2(0, __uuidof(IDXGIFactory2), (void **)&f);
        if (!f && cf1) cf1(__uuidof(IDXGIFactory2), (void **)&f);
    }
    if (!f) { LOGW("BFI Presenter: outer swap chain hook: no DXGI factory"); return; }
    void **vt = *(void ***)f;
    char mod[64];
    ModuleOf(vt[15], mod, sizeof(mod));
    PatchSlot(vt, 10, (void *)&hk_FCreateSC,     (void **)&o_FCreateSC);
    PatchSlot(vt, 15, (void *)&hk_FCreateSCHwnd, (void **)&o_FCreateSCHwnd);
    PatchSlot(vt, 16, (void *)&hk_FCreateSCCore, (void **)&o_FCreateSCCore);
    f->Release();
    LOGI("BFI Presenter: outer swap chain hook on the game's DXGI factory (from %s, CreateSwapChainForHwnd in %s)", from, mod);
}
static void on_init_device_outer(reshade::api::device *dev)
{
    if (dev && (dev->get_api() == reshade::api::device_api::d3d12 || dev->get_api() == reshade::api::device_api::d3d11))
        InstallOuterFactoryHook();
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
        { "amd_fidelityfx_framegeneration_dx12.dll", "FSR Frame Generation" },
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


// ReLimiter (a ReShade add-on, relimiter.addon64) paces the game with its own frame limiter, Present hooks and
// Reflex markers. In user reports (#26, #27) its limit stacked with the presenter's cap, and its G-SYNC detection
// kept switching DLSS-G's mode, locking frame generation at ~30 fps (Control) until G-SYNC was off for the game. ReShade may load it after this
// add-on, so it is looked for every few seconds until found; the overlay and the log then say so.
static bool RelimiterLoaded()
{
    static bool found = false;
    static double nextT = 0.0;
    if (found) return true;
    const double t = Now();
    if (t < nextT) return false;
    nextT = t + 3.0;
    static const char *k[] = { "relimiter.addon64", "relimiter.addon32", "relimiter.addon", "relimiter.dll" };
    for (const char *n : k)
    {
        HMODULE m = GetModuleHandleA(n);
        if (!m) continue;
        char path[MAX_PATH] = "";
        GetModuleFileNameA(m, path, MAX_PATH);
        LOGW("BFI Presenter: ReLimiter is loaded (%s). Its frame limit stacks with the presenter's frame cap, its "
             "G-SYNC handling can switch DLSS frame generation's mode back and forth (Control: a ~30 fps lock until "
             "G-SYNC was turned off for the game), and its smoothness reading looks worse than the screen because the "
             "presenter holds each frame for its flash. Remove it from this game's folder, or turn its limiter off, and "
             "use the presenter's frame cap.", path);
        found = true;
        return true;
    }
    return false;
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
// The game's device, for Reflex calls the presenter makes itself.
static IUnknown *ReflexOwnDevice()
{
    if (g_dx11.load()) return (IUnknown *)g_dev11;
    return (IUnknown *)g_device;
}
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
    FgCheckCaller();
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
    const UINT64 per = cap >= 1 ? (UINT64)std::max(0, g_capPer.load()) : 0;
    if (per && g_running.load()) CapWait(per);
    g_gtExit = Now();
    WriteGameLog();
    g_gtPrevExit = g_gtExit;
    const UINT64 lastId = g_frameId;
    LeaveCriticalSection(&g_submitLock);
    GameStage(ST_G_OUTSIDE);
    GameReleased(lastId);
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
static HRESULT SubmitGameFrame9Core(ShowOwn showOwn)
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
template <class ShowOwn>
static HRESULT SubmitGameFrame9(ShowOwn showOwn)
{
    NoteGameThread11();
    const HRESULT hr = SubmitGameFrame9Core(showOwn);
    FgPaceAfterPresent();   // after the frame is handed over (the automatic frame cap at the frame start)
    return hr;
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
    LoadShared();                       // reads (and on first start creates) BFIPresenter.ini
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
    LoadShared();                       // reads (and on first start creates) BFIPresenter.ini
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
    // The automatic frame cap at the frame start: the game's thread is held here, right before its own SwapBuffers
    // (with swap interval 0 that returns at once).
    NoteGameThread11();
    FgPaceAfterPresent();
}

static void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
    // A DX11 game that makes a new swap chain for its window while the old one still exists (DARQ, 32-bit
    // Unity: 4x MSAA at start, a second swap chain with 2x MSAA a second later, then the old one released):
    // the presenter stayed on the old one, the game presented to the new one under the presenter's window,
    // and it hung at start. Move the presenter to the new swap chain.
    if (!resize && g_dx11.load() && swapchain->get_device()->get_api() == reshade::api::device_api::d3d11 &&
        reinterpret_cast<void *>(swapchain->get_native()) != (void *)g_sc11 && (HWND)swapchain->get_hwnd() == g_gameHwnd)
    {
        IDXGISwapChain *nsc = reinterpret_cast<IDXGISwapChain *>(swapchain->get_native());
        DXGI_SWAP_CHAIN_DESC sd = {};
        if (nsc && SUCCEEDED(nsc->GetDesc(&sd)) && sd.BufferDesc.Width >= 64 && sd.BufferDesc.Height >= 64)
        {
            LOGI("BFI Presenter: DX11: the game made a new swap chain for its window (%ux%u, %u samples) -- moving the presenter to it",
                 sd.BufferDesc.Width, sd.BufferDesc.Height, sd.SampleDesc.Count);
            ReleaseDx11();
            RemoveHooks();
            if (g_dx11.load() || g_sc) return;                   // the presenter couldn't be stopped: stay off
        }
    }
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

// Width of a text in the overlay font. ReShade is built with MSVC, which returns ImVec2 (it has
// constructors) through a hidden pointer; MinGW expects it in registers, so a plain
// ImGui::CalcTextSize call through ReShade's function table crashes. Call it the MSVC way.
static float OverlayTextWidth(const char *text)
{
    typedef ImVec2 *(*CalcTextSizeMsvc)(ImVec2 *, const char *, const char *, bool, float);
    ImVec2 r(0.0f, 0.0f);
    reinterpret_cast<CalcTextSizeMsvc>(imgui_function_table_instance()->CalcTextSize)(&r, text, nullptr, false, -1.0f);
    return r.x;
}
// Width left on the current line, the same way (GetContentRegionAvail returns ImVec2 too).
static float OverlayAvailWidth()
{
    typedef ImVec2 *(*GetContentRegionAvailMsvc)(ImVec2 *);
    ImVec2 r(0.0f, 0.0f);
    reinterpret_cast<GetContentRegionAvailMsvc>(imgui_function_table_instance()->GetContentRegionAvail)(&r);
    return r.x;
}

// Ko-fi button at the bottom of the overlay. shell32 is loaded on the click only, so the add-on links nothing new.
static void OpenKofi()
{
    typedef HINSTANCE (WINAPI *PFN_ShellExecuteW)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
    HMODULE sh = LoadLibraryW(L"shell32.dll");
    PFN_ShellExecuteW se = sh ? (PFN_ShellExecuteW)(void *)GetProcAddress(sh, "ShellExecuteW") : nullptr;
    if (se) se(nullptr, L"open", L"https://ko-fi.com/artzox", nullptr, nullptr, SW_SHOWNORMAL);
}
static void KofiButton()
{
    const char *kf = "Support on Ko-fi";
    const float w = OverlayTextWidth(kf) + ImGui::GetStyle().FramePadding.x * 2;   // not ImGui::CalcTextSize: MSVC return ABI (see OverlayTextWidth)
    ImGui::SameLine();
    const float room = OverlayAvailWidth();      // right-aligned when it fits on the line
    if (room > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - w);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.50f, 0.78f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.58f, 0.88f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.42f, 0.68f, 1));
    if (ImGui::Button(kf)) OpenKofi();
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("BFI Presenter is free. If it helps you, you can support its development on Ko-fi\n(ko-fi.com/artzox). Opens in your browser.");
}

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
    bool en = s.enabled != 0;
    if (ImGui::Checkbox("BFI enabled (Ctrl+Shift+B)", &en)) s.enabled = en ? 1 : 0;
    // The status line and the warnings sit in a box of fixed height (kStatusLines lines; it scrolls if more are
    // showing), so the controls below don't move up and down when they change.
    {
        const int kStatusLines = 7;
        const float h = ImGui::GetTextLineHeightWithSpacing() * kStatusLines + ImGui::GetStyle().WindowPadding.y;
        const ImVec2 box(0.0f, h);
        ImGui::BeginChild("##status", box, 0, 0);
    if (st.deviceLost)
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Device removed -- report written to BFIPresenter_dred.txt");
    if (st.managed) ImGui::TextWrapped("%s", st.reason);
    else            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", st.reason);

    // ================================================================ warnings (only when something needs attention)
    if (RelimiterLoaded())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "ReLimiter is loaded. Its frame limit stacks with the presenter's cap, its G-SYNC handling can switch\n"
                           "frame generation's mode back and forth (a ~30 fps lock), and its smoothness reading looks worse than\n"
                           "the screen (the presenter holds each frame for its flash). Remove relimiter.addon64 from the game's\n"
                           "folder, or turn its limiter off, and use the presenter's frame cap.");
    if (strstr(g_hooksSeen, "RTSS"))
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "RTSS is hooked: keep its frame limiter off and don't toggle its OSD while playing.");
    if (g_hooksSeen[0]) ImGui::TextColored(kAmber, "Other Present hooks: %s", g_hooksSeen);
    // The DLLs being loaded says nothing about frame generation being on (God of War Ragnarok loads DLSS-G
    // with it off), so the line also says what the presenter found: active (with the factor once it is
    // known) or off.
    if (g_fgSeen[0])
    {
        // State first and wrapped: with four components loaded (Onimusha with OptiScaler) the state at the
        // end of one line was past the window's edge.
        const int fgF = g_sleepFgFactor.load();
        char fgState[24];
        if (st.fgActive && fgF >= 2) snprintf(fgState, sizeof(fgState), "active, %dx", fgF);
        else snprintf(fgState, sizeof(fgState), "%s", st.fgActive ? "active" : "off");
        ImGui::PushStyleColor(ImGuiCol_Text, st.fgActive ? kAmber : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("Frame generation: %s (loaded: %s)", fgState, g_fgSeen);
        ImGui::PopStyleColor();
    }
    if (st.managed)
    {
        if (g_deferPresenter.load() && !g_ownPresentQueue)
            ImGui::TextColored(kAmber, "BFI starts when the game resizes its swap chain.\nIf it doesn't, change resolution or display mode once (and back).");
        if (st.vrrSuspect)
            ImGui::TextColored(kRed, "The display is refreshing slower than it can: VRR (G-SYNC) is active.\nTurn G-SYNC off for this game in the NVIDIA profile, or turn on Queue one refresh ahead.");
        if (st.limitSuspect)
            ImGui::TextColored(kRed, "A frame limiter is holding the presenter below the display's refresh rate.\nSet the RTSS frame limit to 0 for this game (and check NVIDIA Profile Inspector).");
        if (st.vramOver)
            ImGui::TextColored(kRed, "The game uses more video memory than Windows gives it (%.0f of %.0f MB): the GPU stalls,\n"
                               "refreshes are missed, frame generation drops frames. Lower its texture or memory settings.",
                               st.vramUse, st.vramBudget);
        if (st.glPresentBlocked)
            ImGui::TextColored(kRed, "The presenter's presents are blocked (OpenGL). In this game's NVIDIA profile set\n"
                               "Vulkan/OpenGL present method = Prefer layered on DXGI swapchain (BFI Presenter Manager does it).");
        if (st.vsyncOverridden)
            ImGui::TextColored(kRed, "The driver is ignoring VSync (NVIDIA profile: Force off / Fast Sync?).\nPacing to vblank instead; tearing is likely. Set Vertical Sync to Force on.");
        // Flash hint: when missed refreshes keep coming back, the first step that isn't done yet (the red
        // warnings above come first: nothing below helps while one of them applies).
        const bool redWarn = st.vrrSuspect || st.limitSuspect || st.glPresentBlocked || st.vsyncOverridden;
        // Not 4 presenter buffers when video memory is tight: each is a full-size image (about 33 MB at 4K).
        const bool vramTight = st.vramOver || (st.vramBudget > 0.0 && st.vramUse > st.vramBudget - 256.0);
        if (st.flashHint && s.flashHints && s.enabled && !redWarn)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            if (s.queuePriority >= 2 && g_queuePriority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH)
                ImGui::TextWrapped("Flashes? Start Steam (or the game's launcher) as administrator, then the game: the presenter's "
                                   "GPU queue then runs at realtime priority, so the game's GPU work can't hold up the flashes.");
            else if (s.reuseBuffers && (s.frames == 2 || s.frames == 4) && st.presBufs > 0 && st.presBufs < 4 && s.presBuffers == 4)
                ImGui::TextWrapped("4 presenter buffers apply at the game's next resize: change the resolution or display mode "
                                   "once (and back), or restart the game.");
            else if (s.reuseBuffers && (s.frames == 2 || s.frames == 4) && st.presBufs > 0 && st.presBufs < 4 && !vramTight)
            {
                ImGui::TextWrapped("Flashes? Try 4 presenter buffers: more slack for the strobe, and refreshes that show the same "
                                   "picture again need no drawing. No extra latency.");
                if (ImGui::SmallButton("Use 4 presenter buffers")) { s.presBuffers = 4; IniWriteInt("PresenterBuffers", 4); }
                ImGui::SameLine();
            }
            else if (s.pacing && !s.queueAhead)
            {
                ImGui::TextWrapped("Still flashing? Queue one refresh ahead keeps a finished refresh ready, so a late one doesn't "
                                   "flash. It adds one refresh of latency (about %.0f ms).", st.refreshHz > 1.0 ? 1000.0 / st.refreshHz : 8.0);
                if (ImGui::SmallButton("Queue one refresh ahead")) { s.queueAhead = 1; IniWriteInt("QueueAhead", 1); }
                ImGui::SameLine();
            }
            else
                ImGui::TextWrapped("Still flashing: the GPU is at its limit. Lower the game's settings or resolution, or use the "
                                   "Half refresh cap.");
            ImGui::PopStyleColor(1);
            if (ImGui::SmallButton("Hide flash hints")) { s.flashHints = 0; IniWriteInt("FlashHints", 0); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("For this game. Advanced > Flash hints brings them back.");
        }
        else if (!s.flashHints && (st.repeats > 0 || st.late > 0))
            ImGui::TextColored(kAmber, "Missed refreshes break the strobe: check VRR is off and nothing caps the presenter.");
        if (st.hitchPaused)
            ImGui::TextColored(kAmber, "Hitch guard: BFI paused (loading or missed refreshes)");
        // Only when it persists: a stray repeat now and then is not worth a warning.
        if (st.unevenRep + st.unevenSkip >= 3)
        {
            const double capRate = st.refreshHz / std::max(1, CapPeriod(s.gameCap, s.frames, s.capDivisor));
            if (s.gameCap >= 1 && st.gameFps < 0.97 * capRate)
                ImGui::TextColored(kAmber, "The game is below the cap's frame rate (%.0f of %.0f fps): motion stutters.\n"
                                   "Lower its settings or resolution, or use a lower cap (Half refresh).", st.gameFps, capRate);
            else
                ImGui::TextColored(kAmber, "Game frames reach the strobe unevenly: motion may stutter. If it persists, the GPU\n"
                                   "is probably finishing frames right at the refresh -- lower settings or try Smooth pacing.");
        }
    }
        ImGui::EndChild();
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

        bool lcd = s.lcdSwap != 0;
        if (ImGui::Checkbox("LCD: prevent image retention (phase swap)", &lcd)) s.lcdSwap = lcd ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("For LCD monitors (IPS, VA, TN) -- not needed on OLED.\n"
                                                      "An LCD drives each refresh with the opposite voltage of the one before. With an even\n"
                                                      "number of frames per cycle every lit refresh gets the same voltage, and the panel can\n"
                                                      "keep a ghost of the image (temporary image retention). Every N seconds this fades the\n"
                                                      "strobe to a steady picture for about half a second, shifts it by one refresh and fades\n"
                                                      "it back, so the lit refreshes alternate between the two voltages. No flicker; motion is\n"
                                                      "briefly a little softer.");
        if (s.lcdSwap)
        {
            ImGui::Indent(16.0f);
            const char *modes[] = { "Immediate: fade, shift, fade back", "Gradual: the lit window drifts slowly" };
            int md = s.lcdGradual ? 1 : 0;
            ImGui::SetNextItemWidth(OverlayTextWidth(modes[1]) + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f);
            if (ImGui::Combo("Mode", &md, modes, 2)) s.lcdGradual = md;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Immediate: every N seconds the strobe fades to a steady picture, shifts by one refresh and\n"
                                                          "fades back. Best at 2 frames per cycle (e.g. 120 Hz).\n"
                                                          "Gradual: the lit window drifts later by a tiny fraction of a refresh every refresh, so it\n"
                                                          "visits every refresh of the cycle once per period, with no fade and no stutter. At 4 or\n"
                                                          "more frames per cycle (e.g. 240 Hz and up) it drifts continuously. At 2 frames it holds\n"
                                                          "sharp on one refresh, then drifts to the other over a few seconds and holds again, since\n"
                                                          "mid-drift motion is softer. Plain strobe only: with Sine BFI, Invert cycle or Rolling scan,\n"
                                                          "Immediate is used.\n"
                                                          "Design after the Blur Busters CRT beam simulator's LCD Saver.");
            if (s.lcdGradual)
            {
                if (s.frames == 2)
                {
                    ImGui::SliderInt("Hold seconds (0 = drift continuously)", &s.lcdHoldSec, 0, 600);
                    if (s.lcdHoldSec > 0) ImGui::SliderInt("Drift seconds", &s.lcdDriftSec, 2, 60);
                    else ImGui::SliderInt("Seconds per full drift", &s.lcdGradSec, 20, 1200);
                    ImGui::TextDisabled("2 frames per cycle: holds sharp, drifts briefly to the other refresh, holds again.");
                }
                else ImGui::SliderInt("Seconds per full drift", &s.lcdGradSec, 20, 1200);
                if (s.sine || s.invert || s.rolling) ImGui::TextDisabled("Sine BFI / Invert cycle / Rolling scan on: Immediate is used.");
            }
            else ImGui::SliderInt("Seconds between phase swaps", &s.lcdSwapSec, 1, 300);
            if (s.frames % 2) ImGui::TextDisabled("Odd frames per cycle: the lit refresh already alternates, nothing to do.");
            ImGui::Unindent(16.0f);
        }

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
            if (s.rollDecay <= 0.0f && ImGui::IsItemHovered()) ImGui::SetTooltip("Blended zone where two bands meet. Pixels in it share their light between both\n"
                                                                  "sub-frames, so they are lit longer: visible blur along the seam in motion.\n"
                                                                  "0 = hard edge, no blur. Phosphor decay (above) hides the edge without a seam.");
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
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your panel's full-screen (100%% window) white brightness. Bright areas are held to what the\npanel sustains at their size: from Peak brightness at 10%% of the screen down to this budget at\n100%% (AblCurve in the ini takes your panel's measured curve). The ceiling is lowered just enough;\nsmall highlights keep the full one. Lower it if bright scenes still darken.");
            if (st.hotArea >= 0.0)
                ImGui::Text("Scene asks for: %.0f nits average   After ceiling: %.0f   Ceiling in use: %.0f nits", st.rawAvg, st.hotArea, st.ceilInUse);
            ImGui::Unindent(16.0f);
        }
    }

    // ================================================================ 3. frame pacing and latency
    if (ImGui::CollapsingHeader("Frame pacing and latency", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Combo("Game frame cap", &s.gameCap, "Off\0Refresh rate\0Half refresh (one game frame per lit + dark pair)\0Quarter refresh (one game frame per two lit + dark pairs)\0One game frame per strobe cycle (refresh / Frames)\0Custom (refresh / divisor)\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh rate: lowest latency when the game holds it.\nHalf refresh = 60 fps at 120 Hz: every game frame is shown once lit, the smoothest motion\nwith 2-frame BFI, and the best choice when the game can't hold 120. With frame generation use Refresh rate:\nHalf throws away half the generated frames.\nOne game frame per strobe cycle: refresh / Frames per cycle -- 60 fps at 360 Hz with 6 frames,\n90 fps at 360 Hz with 4, 80 fps at 240 Hz with 3.\nCustom: the game gets a new frame every N refreshes.");
        if (s.gameCap == 5)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderInt("Refreshes per game frame", &s.capDivisor, 1, 8);
            ImGui::Unindent(16.0f);
        }
        {
            const int per = CapPeriod(s.gameCap, s.frames, s.capDivisor);
            if (per >= 1)
            {
                const double hz = st.refreshHz > 1.0 ? st.refreshHz : 0.0;
                if (hz > 0.0) ImGui::Text("Cap: %.0f fps (a new game frame every %d refresh%s)", hz / per, per, per == 1 ? "" : "es");
                if (per >= 2)
                {
                    bool cr = s.capReflex != 0;
                    if (ImGui::Checkbox("Apply the cap with Reflex's frame limit (irregular frame delivery only)", &cr)) s.capReflex = cr ? 1 : 0;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Half / Quarter cap in games with NVIDIA Reflex: the cap is applied by Reflex's own frame limit,\n"
                                                                  "like a game's own Reflex fps limit, instead of the presenter releasing the game just in time.\n"
                                                                  "Only for games whose frames still arrive irregularly at Half or Quarter -- just-in-time release\n"
                                                                  "keeps switching itself off and motion stutters (The Midnight Walk: 1.7%% of flashes off-cadence\n"
                                                                  "with it, 15.6%% without).\n"
                                                                  "Adds latency compared with just-in-time release (about 10-20 ms: Silent Hill 2 34 ms against\n"
                                                                  "25 ms) and is no smoother where just-in-time release works. Off by default.");
                    if (s.capReflex)
                    {
                        ImGui::SameLine();
                        if (st.rfxLimitUs > 0) ImGui::TextDisabled("(in use: %.1f ms)", st.rfxLimitUs / 1000.0);
                        else ImGui::TextDisabled("(not in use: no Reflex in this game)");
                    }
                }
                {
                    bool ra = s.reflexAlign != 0;
                    if (ImGui::Checkbox("Line Reflex's frame limit up with the flash", &ra)) s.reflexAlign = ra ? 1 : 0;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Games with Reflex, capped with Reflex's frame limit (e.g. the automatic frame cap) and without frame\n"
                                                                  "generation: the presenter moves the moment the game starts each frame so the frame is ready just\n"
                                                                  "before the lit refresh picks it, instead of waiting there for up to a whole strobe cycle.\n"
                                                                  "Needs Lowest latency pacing. Best in games with steady frame times (God of War Ragnarok: about\n"
                                                                  "7 ms less from release to flash); in games whose frame times vary it gains little and can\n"
                                                                  "repeat a frame now and then (Spider-Man 2). Off by default. With it off, frames ready more than\n"
                                                                  "8 ms before the pick are still moved to 5 ms before it once (ReflexAlignCoarse=1 in the ini).");
                    if (s.reflexAlign)
                    {
                        ImGui::SameLine();
                        if (st.alignSlackMs >= 0.0) ImGui::TextDisabled("(frames ready %.1f ms before the pick, moved %+.1f ms)", st.alignSlackMs, st.alignShiftMs);
                        else ImGui::TextDisabled(s.pacingMode == 0 ? "(not in use: needs Lowest latency pacing)" : "(not in use)");
                    }
                }
                if (per >= 2 && st.fgActive && st.rfxLimitUs <= 0)
                    ImGui::TextColored(kAmber, "Frame generation with a cap below the refresh rate: half its frames never reach the screen\n"
                                       "and latency doubles. Use Refresh rate.");
                if (per >= 2 && per % s.frames != 0)
                    ImGui::TextColored(kAmber, "%d refreshes per game frame doesn't fit %d frames per cycle: some game frames start\n"
                                       "on a dark refresh and motion judders. Use a multiple of %d.", per, s.frames, s.frames);
            }
        }
        {
            bool ac = s.autoCap != 0;
            if (ImGui::Checkbox("Automatic frame cap", &ac)) s.autoCap = ac ? 1 : 0;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("With the Refresh rate cap: while the game can't hold the refresh rate, the cap drops to one\n"
                                                          "game frame per strobe cycle (e.g. 60 fps at 120 Hz with 2 frames), so every flash shows the\n"
                                                          "next frame instead of frames dropped unevenly (judder). Back to Refresh rate when the game's\n"
                                                          "frames fit a refresh again (its Reflex reports, or a short try every 30 to 120 s).\n"
                                                          "In games with NVIDIA Reflex (every DLSS frame-generation game) it uses Reflex's own frame limit\n"
                                                          "for this, which also paces frame generation. Frame generation without Reflex: see the frame limit below.");
            if (s.autoCap)
            {
                bool aco = s.autoCapOwn != 0;
                ImGui::Indent(16.0f);
                if (ImGui::Checkbox("Presenter's own cap without frame generation", &aco)) s.autoCapOwn = aco ? 1 : 0;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off (default): in games with Reflex, the cap steps down with Reflex's frame limit, with\n"
                                                              "frame generation or without.\n"
                                                              "On: the presenter's own cap with just-in-time release while frame generation is off\n"
                                                              "(each frame timed to its flash); Reflex's frame limit while it is on. Follows frame\n"
                                                              "generation being switched on and off.");
                bool fe = s.autoCapFrameStart != 0;
                if (ImGui::Checkbox("Hold at the frame start without Reflex", &fe)) s.autoCapFrameStart = fe ? 1 : 0;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Games without Reflex or frame generation (or with the presenter's own cap above): when the\n"
                                                              "cap steps down to one game frame per strobe cycle, the game is held before it starts its\n"
                                                              "next frame, like Reflex's limit, instead of its finished frame waiting at the present.\n"
                                                              "It reads your input later, so less delay to the flash. For about 25 s it tries start\n"
                                                              "times across the strobe cycle and keeps the quickest that shows every frame.\n"
                                                              "Needs Lowest latency pacing.");
                bool lp = s.learnPerGame != 0;
                if (ImGui::Checkbox("Remember per game", &lp)) s.learnPerGame = lp ? 1 : 0;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the automatic frame cap finds for a game (one game frame per strobe cycle, the frame\n"
                                                              "start time) is kept in BFIPresenter_learned.ini, and the next session starts from it instead\n"
                                                              "of finding it again. Still checked as usual: tries at Refresh rate, a new search after lost\n"
                                                              "frames.");
                if (s.learnPerGame)
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Forget this game")) { g_learnForgetFile.store(true); g_learnForgetNow.store(true); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Deletes what was remembered for this game and starts over now: the cap goes back to\n"
                                                                  "Refresh rate and steps down again if needed, and the frame start is searched for again.\n"
                                                                  "E.g. after changing the game's graphics settings or the display mode.");
                }
                ImGui::Unindent(16.0f);
                ImGui::SameLine();
                if (s.gameCap != 1) ImGui::TextColored(kAmber, "needs the Refresh rate cap");
                else if (st.rfxLimitUs > 0) ImGui::TextDisabled("(in use: Reflex frame limit %.1f ms, one frame per strobe cycle)", st.rfxLimitUs / 1000.0);
                else if (st.fgLimitUs > 0) ImGui::TextDisabled("(Refresh rate: the frame limit for frame generation without Reflex holds the game)");
                else if (st.feLimitUs > 0) ImGui::TextDisabled("(in use: one game frame per strobe cycle, held at the frame start)");
                else if (st.fgActive && !st.rfxGameUses) ImGui::TextDisabled("(frame generation without Reflex: Refresh rate)");
                else ImGui::TextDisabled("%s", st.autoCapProbe ? "(trying Refresh rate)" :
                                               st.autoCapDown ? "(in use: one game frame per strobe cycle)" : "(in use: Refresh rate)");
            }
            {
                bool fl = s.fgLimitNoReflex != 0;
                if (ImGui::Checkbox("Frame limit for frame generation without Reflex", &fl)) s.fgLimitNoReflex = fl ? 1 : 0;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame generation in games without NVIDIA Reflex (e.g. XeSS frame generation through\n"
                                                              "OptiScaler): the presenter holds the game's own frames to one per (strobe cycle x\n"
                                                              "frame-generation factor) -- e.g. 30 rendered fps at 120 Hz with 2 frames and 2x -- so every\n"
                                                              "flash shows the next frame; for about 25 s after it comes on it tries start times to find the\n"
                                                              "quickest one that shows every frame.\n"
                                                              "Needs the Refresh rate cap; best with Lowest latency pacing. Turn off any frame limit in the\n"
                                                              "game or in OptiScaler (FramerateLimit = 0). On by default.");
                if (s.fgLimitNoReflex && st.fgLimitUs > 0)
                {
                    ImGui::SameLine();
                    if (st.alignSlackMs >= 0.0) ImGui::TextDisabled("(in use: %.1f ms per rendered frame, ready %.1f ms before the pick)", st.fgLimitUs / 1000.0, st.alignSlackMs);
                    else ImGui::TextDisabled("(in use: %.1f ms per rendered frame)", st.fgLimitUs / 1000.0);
                }
            }
            if (st.relFlashMs >= 0.0) ImGui::TextDisabled("Game released -> lit flash: %.1f ms", st.relFlashMs);
        }
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
        if (ImGui::Checkbox("Just-in-time release at caps below the refresh rate", &cj)) s.capJit = cj ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Releases the game for each frame at the lit refresh minus its measured frame time and a\n"
                                                      "margin, so every frame lands on the same lit refresh: no repeat-then-skip at Half, no\n"
                                                      "1-then-3 cycle frames at Quarter. Light games also get fresher frames.");
        bool lr = s.laterRelease != 0;
        if (ImGui::Checkbox("Later release at Refresh cap (Lowest latency, no frame generation)", &lr)) s.laterRelease = lr ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lets the game start each frame just in time for the refresh that shows it, instead of\na full refresh earlier, so its input is read later -- up to ~5 ms less latency in games\nthat render well within a refresh. Does nothing while frames take nearly a whole refresh.");
        if (s.laterRelease || (s.capJit && CapPeriod(s.gameCap, s.frames, s.capDivisor) >= 2))
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
        bool fh = s.flashHints != 0;
        if (ImGui::Checkbox("Flash hints (suggest the next step when the strobe keeps flashing)", &fh)) s.flashHints = fh ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("When refreshes are missed in 3 of 10 seconds (loading doesn't count), the status box\n"
                                                      "suggests one step at a time: run the launcher as administrator, 4 presenter buffers,\n"
                                                      "Queue one refresh ahead, then lighter game settings.");
        bool hg = s.hitchGuard != 0;
        if (ImGui::Checkbox("Hitch guard (pause BFI during loading and missed refreshes)", &hg)) s.hitchGuard = hg ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("While the presenter misses refreshes (3 within half a second) or the game delivers no\nnew frame for the loading time, every refresh is shown at the strobe's average brightness\ninstead of strobing (no brightness change). BFI returns after one second without either.");
        if (s.hitchGuard)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderInt("Loading: no new frame for (ms)", &s.hitchGapMs, 150, 5000);
            ImGui::Unindent(16.0f);
        }
        bool so = s.showOverlays != 0;
        if (ImGui::Checkbox("Show overlays (RTSS OSD, Steam) on the strobed frames", &so)) s.showOverlays = so ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sends the presenter's presents through overlay hooks, so RivaTuner's OSD and the Steam\n"
                                                      "overlay draw on what you see.\n"
                                                      "64-bit DirectX 12 / 11 / 9 games only: not supported in 32-bit games (they crashed or\n"
                                                      "still showed no overlay) or OpenGL games, where it is ignored. Some 64-bit games crash\n"
                                                      "with it too (Steam's hook: Pragmata, Resident Evil 4) -- turn it off if the game crashes.\n"
                                                      "Keep RTSS's frame limiter off: it would hold back the presenter and break the strobe.\n"
                                                      "Applies at the next game start. Vulkan games show overlays without it.");
#ifndef _WIN64
        if (s.showOverlays) { ImGui::SameLine(); ImGui::TextColored(kAmber, "not supported in 32-bit games"); }
#endif
        if (s.showOverlays && g_glActive.load()) { ImGui::SameLine(); ImGui::TextColored(kAmber, "not supported in OpenGL games"); }
        if (g_showOverlaysBoot >= 0 && s.showOverlays != g_showOverlaysBoot)
        {
            ImGui::SameLine();
            ImGui::TextColored(kAmber, "restart the game to apply");
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
    g_capPer.store(CapPeriod(s.gameCap, s.frames, s.capDivisor));
        if (!s.autoAhead) g_maxAhead.store(s.maxAhead);
        g_virtualStats.store(s.virtualStats);
    }
    if (ImGui::Button("Save to BFIPresenter.ini")) SaveSettings(s);
    ImGui::SameLine();
    if (ImGui::Button("Reload ini")) LoadShared();
    KofiButton();
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
    reshade::unregister_event<reshade::addon_event::init_device>(on_init_device_outer);
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
    reshade::register_event<reshade::addon_event::init_device>(on_init_device_outer);
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
        reshade::register_event<reshade::addon_event::init_device>(on_init_device_outer);
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
        // Read (and on the first start create) BFIPresenter.ini as soon as the
        // add-on loads, whatever the API -- not under the loader lock.
        {
            HANDLE t = CreateThread(nullptr, 0, LoadIniThread, nullptr, 0, nullptr);
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
