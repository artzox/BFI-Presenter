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
//           Ctrl+Shift+R  reload ini        Ctrl+Shift+W  frame warp off / on
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
#include <map>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "../common/bfi_syscheck.h"
#include "../common/bfi_vram.h"
#include "../common/bfi_osd.h"
#include "../common/bfi_clockdrift.h"
#include "../common/bfi_warpclock.h"
#ifdef _WIN64
namespace slp { static void Tick(double t); }   // Streamline probe (test), defined after InlineHook64
namespace ngx { static void Tick(double t); }   // DLSS called directly (NGX, test), defined after gmo
namespace gmo                                    // frame warp from the game's own motion (test), defined after slp
{
static void AtPresent(ID3D12GraphicsCommandList *list, UINT s);
static void Published(UINT s, UINT64 id);
static int  SlotFor(int s, UINT64 id);
static float SlotLag(int s);
static ID3D12Resource *FlowRes(int s);
static UINT WriteFlowSrvs(D3D12_CPU_DESCRIPTOR_HANDLE base, UINT stride);
static ID3D12Resource *FlowFor(int s, UINT gen);
static bool PauseNow(UINT64 id, bool letterbox, const char **why);
static int  CutsTake();
static UINT FlowGenNow();
static int  StateNow(char *why, size_t n);
static int  StateFast();
static int  ViaNow();
static int  HudNow();
static void HudBad(double pct);
static void Release();
}
#else
namespace gmo                                    // 32-bit games: no Streamline, the picture's own motion only
{
static inline void AtPresent(ID3D12GraphicsCommandList *, UINT) {}
static inline void Published(UINT, UINT64) {}
static inline int  SlotFor(int, UINT64) { return 0; }
static inline float SlotLag(int) { return 0.0f; }
static inline ID3D12Resource *FlowRes(int) { return nullptr; }
static inline UINT WriteFlowSrvs(D3D12_CPU_DESCRIPTOR_HANDLE, UINT) { return 0; }
static inline ID3D12Resource *FlowFor(int, UINT) { return nullptr; }
static inline bool PauseNow(UINT64, bool, const char **) { return false; }
static inline int  CutsTake() { return 0; }
static inline UINT FlowGenNow() { return 0; }
static inline int  StateNow(char *why, size_t n) { if (n) why[0] = 0; return 0; }
static inline int  StateFast() { return 0; }
static inline int  ViaNow() { return 0; }
static inline int  HudNow() { return 0; }
static inline void HudBad(double) {}
static inline void Release() {}
}
#endif
#include "bfi_crt_gen.h"    // CRT-Standalone's pattern stage, copied from the shader (tools/crt-handoff)
namespace crt
{
static UINT SlotGen();
static void CopyAtPresent(ID3D12GraphicsCommandList *list, UINT s);
static ID3D12Resource *SlotTex(UINT s, int k);
static bool SlotReady(UINT s);
static void ReleaseSlots();
static const int kBlobN = 2 + kCrtPostN;
static bool TakeBlobs(UINT *gen, ID3DBlob *out[kBlobN]);
static bool PostIsGrainRaw(int k);
static bool PostOn(int k, const float *cp);
static bool Bake(ID3D12GraphicsCommandList *list, UINT s, ID3D12Resource *stage, UINT64 waitRead, UINT64 *presWait);
static void BakeSubmitted(UINT s, UINT64 fence);
static int GrainFor(UINT s, UINT64 pres);
static ID3D12Resource *GrainTex(int g);
static UINT RingGen();
static ID3D12Resource *BakeOut(UINT s);
static bool BakeReady(UINT s);
static UINT BakeGen();
static DXGI_FORMAT PassFormat();
static void SetBakeSkip(bool skip);
static void SetMemoryTight(bool tight);
static void ParamsFor(float *out, float w, float h);
static void Release();
static void Register(bool on);
static int StateNow();
}

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
static SysCheck g_sysCheck;      // power plan, GPUs, displays (checked on the watchdog thread)
#define LOGE(...) Log(reshade::log::level::error, __VA_ARGS__)

static double g_qpcFreq = 1.0;
static double Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / g_qpcFreq;
}

template <class T> static void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

#define BFI_ADDON_VERSION "1.0.50"

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
    int   split    = 0;       // SplitCompare: one side of the screen without BFI, for comparison (idea: unrefined803, #111)
    int   splitSide = 0;      // 0 = the left side without BFI, 1 = the right side
    int   splitPos = 50;      // the seam, percent of the width from the left
    int   splitSeam = 0;      // soft seam width in pixels (0 = hard edge)
    int   frameWarp = 0;      // FrameWarp: a strobe cycle without a new game frame shows the last one moved ahead
                              // along the camera's motion instead of lit again (no frame is held back: no added latency)
    float warpMax = 0.8f;     // FrameWarpMaxAhead: never further ahead than this many game frames (0.25-1.5; 1.0 before 1.0.45)
    int   warpMinRate = 75;   // FrameWarpMinRate: only while the game makes at least this percent of the strobe rate (40-95)
    int   warpGame = 1;       // FrameWarpGameMotion: use the game's own motion vectors and depth where it hands them to
                              // its upscaler (Streamline / DLSS, D3D12, 64-bit); otherwise the motion measured from the picture
    int   warpGameHud = 1;    // FrameWarpGameHud: keep the HUD still with the game's HUD-less picture, where it gives one
    int   warpPauseLb = 1;    // FrameWarpPauseCutscenes: no frame warp while a cutscene is letterboxed through DLSS (its output
                              // smaller than the screen, e.g. Townfall)
    int   warpEven = 0;       // FrameWarpEvenPacing: every strobe cycle moved to where the game's motion is, where the game's
                              // own motion is used; elsewhere missing frames are filled as usual
    int   warpEdge = 0;       // FrameWarpEdge: the strip at the screen edge a moved flash has no picture for: 0 the move faded
                              // out toward the edge, 1 the picture mirrored at the edge, 2 the edge repeated (stretched, before 1.0.45)
    float warpSnapX = 0.0f, warpSnapY = 0.0f;   // FrameWarpSnapX / FrameWarpSnapY: moves rounded to whole steps of this many
                              // pixels of the game frame (0 = off) -- a CRT shader's scanlines and mask stay in place
    int   warpGameFg = 0;     // FrameWarpGameMotionWithFG: the game's motion also while frame generation is on. Off by default:
                              // the game's vectors are for its rendered frames, so on generated ones edges double up (outlines
                              // around characters, e.g. 007 First Light, Directive 8020); the measured motion is used instead
    int   warpFgLag = 1;      // FrameWarpGameMotionFGLag (test, ini only): with the game's motion and frame generation, its
                              // vectors read where each point is in the rendered frame: 1 = generated frames come before
                              // the rendered one (DLSS frame generation), -1 = after it, 0 = off (read where shown, ep7)
    int   warpGuard = 1;      // FrameWarpGuard (ini only): filled repeats that keep making their refresh late are shown as
                              // they are for a while (30 s, then longer)
    int   warpEaseNoFg = 1;   // FrameWarpGameMotionEaseNoFG (1.0.45): the ease below without frame generation too
    float warpEaseStr = 1.0f; // FrameWarpGameMotionEaseStrength (1.0.49): how hard the ease below is, 0-3 (1 = as before 1.0.49;
                              // the move past the rendered frame is scaled by (motion now / motion before) ^ this)
    int   crtHandoff = 0;     // CrtStandaloneHandoff (1.0.49, DX12 64-bit): CRT-Standalone's mask, scanlines and glow drawn by
                              // the presenter after frame warp (with BFI_PRESENTER_HANDOFF=1 in the shader; dx/bfi_crt.h)
    int   crtTexState = 0xC0; // CrtTextureState (ini only): the state ReShade's effect textures are in after its effects
    int   warpStillPre = 1;   // FrameWarpStillBeforeEffects (1.0.48): what stands still (a HUD) is found in the frame from
                              // before ReShade's effects, while an effect is on (dx/bfi_prefx.h)
    int   warpDilate = 1;     // FrameWarpGameDilate (1.0.48): the game's vectors dilated by depth -- each point takes the
                              // nearest point's vector within this many depth texels (0 off, 1-3)
    int   hudFromDlss = 1;    // FrameWarpHudFromDlss (1.0.47, ini only): without a HUD-less picture from the game, DLSS's own
                              // output (the scene before post-processing and the HUD) is compared with the frame instead
    int   dlssOutState = 8;   // DlssOutputState (ini only): the state DLSS's output is in after its call (8 = unordered access)
    int   warpEase = 1;       // FrameWarpGameMotionEase (test, ini only): with the game's motion and frame generation, the
                              // part of a move past the rendered frame eases off as the game's motion slows (1 = on)
    float warpMaxGameFg = 0.10f;   // FrameWarpGameMotionMaxAheadFG: with the game's motion and frame generation, at most this
                              // many shown frames ahead (it overshot visibly from about 0.5 in 007 First Light)
    int   warpGameCheck = 1;  // FrameWarpGameMotionCheck (test, ini only): every 2 s the game's motion and the measured one in the log
    int   slHooks = 1;        // StreamlineHooks (test, ini only): 0 = Streamline is not hooked at all (no probe, frame warp
                              // with the picture's own motion only) -- to compare a game's frame rate with and without
    int   dlssHooks = 1;      // DlssHooks (test, ini only): 0 = DLSS called directly (NGX) is not hooked
    int   dlssInState = 64;   // DlssInputState (test, ini only): the state DLSS's inputs are in at its call (64 = non-pixel
                              // shader resource, as DLSS asks; 192 = all shader resource)
    int   warpGameSign = 1;   // FrameWarpGameMotionSign (test, ini only): -1 turns the game's vectors round, in case a game's
                              // scale has the other sign
    int   osd = 0;            // Osd: the in-game on-screen display (common/bfi_osd.h), Ctrl+Shift+F
    int   osdCorner = 1;      // OsdCorner: 0 top left, 1 top right, 2 bottom left, 3 bottom right
    int   osdGraph = 1;       // OsdGraph: frame-time graph
    int   osdLoads = 1;       // OsdLoads: CPU / GPU load
    float osdScale = 1.0f;    // OsdScale: text size
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
    int   queueDark = 1;      // QueueDarkRefresh: the cycle's second refresh is drawn and queued right after the lit one
    int   laterRelease = 0;   // Lowest latency + Refresh cap: release the game just in time for the pick
    int   capJit = 1;         // Half / Quarter cap: release just in time for the lit refresh (both pacing modes)
    int   autoCap = 1;        // Refresh cap: one game frame per strobe cycle while the game can't hold the refresh rate
    int   reflexAlign = 1;    // Reflex frame limit at the sleep point lined up with the lit refresh's pick (default on since 1.0.39)
    int   sleepGrid = 1;         // SleepLimitGrid: the limit at the game's Reflex sleep keeps its place in the strobe cycle (test)
    int   reflexAlignCoarse = 1; // ReflexAlignCoarse: frames ready far ahead of the pick are moved closer once (ReflexAlign off)
    int   capReflex = 0;      // the game frame cap applied with Reflex's frame limit in games that use Reflex (opt-in)
    int   autoCapOwn = 0;     // AutoCapOwnWithoutFg: 0 = Reflex games step down with Reflex's limit; 1 = the presenter's own cap while frame generation is off
    int   learnPerGame = 1;      // LearnPerGame: the automatic frame cap starts from what it found for the game last time
    int   learnReflex = 1;       // LearnReflexStart: the Reflex frame limit starts where its alignment settled last time
    int   threadBoost = 1;       // PresenterThreadBoost: the presenter thread registered with MMCSS (applies at the next start)
    int   autoCapFrameStart = 1; // AutoCapAtFrameStart: the automatic frame cap without Reflex holds the game before its next frame starts
    int   fgLimitNoReflex = 1; // FrameGenLimitWithoutReflex: frame generation without Reflex limited to one presented frame per strobe cycle
    int   reflexAtSleep = 1;  // ReflexLimitAtSleep: pace the game's own frames in its Reflex sleep call instead of the driver's limit
                              // (with frame generation: Reflex's frame limit at one presented frame per cycle)
    float laterReleaseMs = 1.5f; // safety margin on top of the measured build time (widened automatically)
    int   deferRecreated = 1; // recreated swap chains: no BFI until the game resizes (safe)
    int   virtualStats = 1;   // report frame statistics in terms of the game's own frames
    int   reuseBuffers = 1;   // present an already-drawn buffer again instead of redrawing it
    int   fitWindow = 1;      // DX11/DX9/OpenGL: game frames larger than the window are scaled down by the presenter
    int   flashHints = 1;     // overlay: suggest the next step when refreshes keep being missed (flashes)
    int   waitResizeAll = 1;  // every swap chain: no BFI until the game first resizes it
    int   selfResize = 0;     // ... and the game hasn't after a few seconds: the presenter resizes it (same size; first swap chain)
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
    // SDR to HDR (DX11, DX9, OpenGL: the presenter's own swap chain): an SDR game on an HDR display is presented as
    // HDR10, so the lit refresh has headroom above SDR white (as the Vulkan layer). Applies when the swap chain is next created.
    int   sdrToHdr = 1;
    float sdrWhite = 200.0f;  // nits of SDR white (Windows' SDR content brightness: 80 + 4 x the slider)
    float sdrHiNits = 800.0f; // highlights stretch toward this (0 = off)
    float sdrKnee  = 0.65f;   // expansion starts at this SDR signal level
    int   sdrGamma = 0;       // SdrGamma: SDR decoding: 0 sRGB (as Windows), 1 gamma 2.2 (deeper shadows), 2 gamma 2.4
    int   sdrMethod = 0;      // SdrHighlightMethod: highlights stretched by 0 the brightest channel, 1 luminance, 2 RenoFX HDR Boost
    float sdrBoostStart = 0.15f;  // SdrBoostStart (RenoFX): where the boost begins, linear (1 = SDR white), 0.001-0.99
    float sdrBoostRamp  = 1.0f;   // SdrBoostRamp (RenoFX Ramp Intensity), 0.25-4
    int   sdrBoostSpace = 0;      // SdrBoostSpace (RenoFX): 0 LMS with hue restore, 1 by brightness (Yf)
    float sdrBoostColor = 50.0f;  // SdrBoostColor (RenoFX gamut expansion of the boost), 0-100
    int   sdrBoostApl = 1;        // SdrBoostAplLimiter (RenoFX APL Limiter): less boost in bright scenes (average above 30%)
    float sdrGamut = 0.0f;    // SdrGamutExpansion: 0..1 (0 = off)
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
    s.split    = IniInt("SplitCompare", 0) ? 1 : 0;
    s.splitSide = IniInt("SplitCompareSide", 0) ? 1 : 0;
    s.splitPos = std::max(5, std::min(95, IniInt("SplitComparePosition", 50)));
    s.splitSeam = std::max(0, std::min(400, IniInt("SplitCompareSeamPx", 0)));
    s.frameWarp = IniInt("FrameWarp", 0) != 0 ? 1 : 0;      // 2 (even pacing, test builds t1-t2) is gone: fill
    s.warpMax = std::max(0.25f, std::min(1.5f, IniFloat("FrameWarpMaxAhead", 0.8f)));
    s.warpMinRate = std::max(40, std::min(95, IniInt("FrameWarpMinRate", 75)));
    s.warpGame = IniInt("FrameWarpGameMotion", 1) != 0 ? 1 : 0;
    s.warpGameSign = IniInt("FrameWarpGameMotionSign", 1) < 0 ? -1 : 1;
    s.slHooks = IniInt("StreamlineHooks", 1) != 0 ? 1 : 0;
    s.dlssHooks = IniInt("DlssHooks", 1) != 0 ? 1 : 0;
    s.dlssInState = IniInt("DlssInputState", 64);
    s.warpGameCheck = IniInt("FrameWarpGameMotionCheck", 1) != 0 ? 1 : 0;
    s.warpGameHud = IniInt("FrameWarpGameHud", 1) != 0 ? 1 : 0;
    s.warpGameFg = IniInt("FrameWarpGameMotionWithFG", 0) ? 1 : 0;
    s.warpFgLag = std::max(-1, std::min(1, IniInt("FrameWarpGameMotionFGLag", 1)));
    s.warpEase = IniInt("FrameWarpGameMotionEase", 1) ? 1 : 0;
    s.warpEaseNoFg = IniInt("FrameWarpGameMotionEaseNoFG", 1) ? 1 : 0;
    s.warpEaseStr = std::max(0.0f, std::min(3.0f, IniFloat("FrameWarpGameMotionEaseStrength", 1.0f)));
    s.hudFromDlss = IniInt("FrameWarpHudFromDlss", 1) ? 1 : 0;
    s.warpDilate = std::max(0, std::min(3, IniInt("FrameWarpGameDilate", 1)));
    s.warpStillPre = IniInt("FrameWarpStillBeforeEffects", 1) ? 1 : 0;
    s.crtHandoff = IniInt("CrtStandaloneHandoff", 0) ? 1 : 0;
    s.crtTexState = IniInt("CrtTextureState", 0xC0);
    s.dlssOutState = IniInt("DlssOutputState", 8);
    s.warpGuard = IniInt("FrameWarpGuard", 1) ? 1 : 0;
    s.warpMaxGameFg = std::max(0.1f, std::min(1.5f, IniFloat("FrameWarpGameMotionMaxAheadFG", 0.10f)));
    s.warpEven = IniInt("FrameWarpEvenPacing", 0) ? 1 : 0;
    s.warpPauseLb = IniInt("FrameWarpPauseCutscenes", 1) ? 1 : 0;
    s.warpSnapX = std::max(0.0f, std::min(16.0f, IniFloat("FrameWarpSnapX", 0.0f)));
    s.warpSnapY = std::max(0.0f, std::min(16.0f, IniFloat("FrameWarpSnapY", 0.0f)));
    s.warpEdge = std::max(0, std::min(2, IniInt("FrameWarpEdge", 0)));
    s.osd = IniInt("Osd", 0) ? 1 : 0;
    s.osdCorner = std::max(0, std::min(3, IniInt("OsdCorner", 1)));
    s.osdGraph = IniInt("OsdGraph", 1) ? 1 : 0;
    s.osdLoads = IniInt("OsdLoads", 1) ? 1 : 0;
    s.osdScale = std::max(0.75f, std::min(2.5f, IniFloat("OsdScale", 1.0f)));
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
    if (s.peakNits < 50.0f) s.peakNits = 50.0f;
    if (s.peakNits > 10000.0f) s.peakNits = 10000.0f;
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
    s.queueDark = IniInt("QueueDarkRefresh", 1) ? 1 : 0;
    s.laterRelease = IniInt("LaterRelease", 0) ? 1 : 0;
    s.capJit = IniInt("CapJustInTime", 1) ? 1 : 0;
    s.autoCap = IniInt("AutoFrameCap", 1) ? 1 : 0;
    s.capReflex = IniInt("CapWithReflex", 0) ? 1 : 0;
    s.reflexAlign = IniInt("ReflexAlign", 1) ? 1 : 0;
    s.reflexAlignCoarse = IniInt("ReflexAlignCoarse", 1) ? 1 : 0;
    s.sleepGrid = IniInt("SleepLimitGrid", 1) ? 1 : 0;
    s.reflexAtSleep = IniInt("ReflexLimitAtSleep", 1) ? 1 : 0;
    s.fgLimitNoReflex = IniInt("FrameGenLimitWithoutReflex", 1) ? 1 : 0;
    s.autoCapFrameStart = IniInt("AutoCapAtFrameStart", 1) ? 1 : 0;
    s.learnPerGame = IniInt("LearnPerGame", 1) ? 1 : 0;
    s.learnReflex = IniInt("LearnReflexStart", 1) ? 1 : 0;
    s.threadBoost = IniInt("PresenterThreadBoost", 1) ? 1 : 0;
    s.autoCapOwn = IniInt("AutoCapOwnWithoutFg", 0) ? 1 : 0;
    s.laterReleaseMs = IniFloat("LaterReleaseMarginMs", 1.5f);
    if (s.laterReleaseMs < 0.5f) s.laterReleaseMs = 0.5f;
    if (s.laterReleaseMs > 5.0f) s.laterReleaseMs = 5.0f;
    s.deferRecreated = IniInt("WaitForResizeOnRecreatedSwapChain", 1) ? 1 : 0;
    s.virtualStats = IniInt("GameFrameStatistics", 1) ? 1 : 0;
    s.reuseBuffers = IniInt("ReuseDrawnBuffers", 1) ? 1 : 0;
    s.fitWindow = IniInt("ScaleToWindow", 1) ? 1 : 0;
    s.flashHints = IniInt("FlashHints", 1) ? 1 : 0;
    s.waitResizeAll = IniInt("StartBFIAfterGameResize", 1) ? 1 : 0;
    s.selfResize = IniInt("PresenterResize", 0);
    if (s.selfResize < 0 || s.selfResize > 2) s.selfResize = 1;
    s.bypassResizeHooks = IniInt("BypassOverlayResizeHooks", 1) ? 1 : 0;
    s.showOverlays = IniInt("ShowOverlays", 0) ? 1 : 0;
    s.reportTyped = IniInt("TypedBackBufferDesc", 1) ? 1 : 0;
    s.hitchGuard = IniInt("HitchGuard", 1) ? 1 : 0;
    s.ablGuard = IniInt("AblGuard", 1) ? 1 : 0;
    s.ablNits = IniInt("AblGuardCapNits", 700);
    if (s.ablNits < 200) s.ablNits = 200;
    if (s.ablNits > 2000) s.ablNits = 2000;
    s.ablBudget = IniInt("AblGuardBudgetNits", 250);
    if (s.ablBudget < 50) s.ablBudget = 50;
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
    s.sdrToHdr = IniInt("SdrToHdr", 1) ? 1 : 0;
    s.sdrWhite = IniFloat("SdrWhiteNits", 200.0f);
    if (s.sdrWhite < 80.0f) s.sdrWhite = 80.0f;
    if (s.sdrWhite > 600.0f) s.sdrWhite = 600.0f;
    s.sdrHiNits = IniFloat("SdrHighlightNits", 800.0f);
    if (s.sdrHiNits < 0.0f) s.sdrHiNits = 0.0f;
    if (s.sdrHiNits > 4000.0f) s.sdrHiNits = 4000.0f;
    s.sdrKnee = IniFloat("SdrHighlightKnee", 0.65f);
    if (s.sdrKnee < 0.3f) s.sdrKnee = 0.3f;
    if (s.sdrKnee > 0.95f) s.sdrKnee = 0.95f;
    s.sdrGamma = IniInt("SdrGamma", 0);
    if (s.sdrGamma < 0 || s.sdrGamma > 2) s.sdrGamma = 0;
    s.sdrMethod = std::max(0, std::min(2, IniInt("SdrHighlightMethod", 0)));
    s.sdrBoostStart = std::max(0.001f, std::min(0.99f, IniFloat("SdrBoostStart", 0.15f)));
    s.sdrBoostRamp = std::max(0.25f, std::min(4.0f, IniFloat("SdrBoostRamp", 1.0f)));
    s.sdrBoostSpace = IniInt("SdrBoostSpace", 0) ? 1 : 0;
    s.sdrBoostColor = std::max(0.0f, std::min(100.0f, IniFloat("SdrBoostColor", 50.0f)));
    s.sdrBoostApl = IniInt("SdrBoostAplLimiter", 1) ? 1 : 0;
    s.sdrGamut = IniFloat("SdrGamutExpansion", 0.0f);
    if (s.sdrGamut < 0.0f) s.sdrGamut = 0.0f;
    if (s.sdrGamut > 1.0f) s.sdrGamut = 1.0f;
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
    if (!IniHasKey("DefaultsVersion")) IniWrite("DefaultsVersion", "39");
    g_iniOnlyMissing = false;
    if (fresh)
        LOGI("BFI Presenter: created BFIPresenter.ini with default settings (see BFIPresenter.example.ini for what each does)");
    else if (g_iniAdded[0])
        LOGI("BFI Presenter: added new settings to your BFIPresenter.ini with their defaults: %s", g_iniAdded);
}

// Defaults changed in a release, applied once to an existing BFIPresenter.ini (the file holds every setting, so a
// new default never reaches it otherwise). DefaultsVersion records what was applied; set a value back afterwards
// and it stays. 1.0.39: ReflexAlign on (it was off and written as 0 into every ini). 1.0.45: FrameWarpMaxAhead 0.8 where it
// was the old default, 1.0 (filled flashes snapped back visibly in third-person games above about 0.6 before the ease).
static void MigrateDefaults()
{
    char keys[64];
    if (!GetPrivateProfileStringA("BFI", nullptr, "", keys, sizeof(keys), g_iniPath) || !keys[0]) return;   // new file
    const int v = (int)GetPrivateProfileIntA("BFI", "DefaultsVersion", 0, g_iniPath);
    if (v < 39)
    {
        if (GetPrivateProfileIntA("BFI", "ReflexAlign", 0, g_iniPath) == 0)
        {
            WritePrivateProfileStringA("BFI", "ReflexAlign", "1", g_iniPath);
            LOGI("BFI Presenter: ReflexAlign turned on (the default since 1.0.39; set ReflexAlign=0 to turn it off again)");
        }
        WritePrivateProfileStringA("BFI", "DefaultsVersion", "39", g_iniPath);
    }
    if ((int)GetPrivateProfileIntA("BFI", "DefaultsVersion", 0, g_iniPath) < 45)
    {
        char b[32] = "";
        GetPrivateProfileStringA("BFI", "FrameWarpMaxAhead", "", b, sizeof(b), g_iniPath);
        if (b[0] && fabs(atof(b) - 1.0) < 0.001)
        {
            WritePrivateProfileStringA("BFI", "FrameWarpMaxAhead", "0.800", g_iniPath);
            LOGI("BFI Presenter: FrameWarpMaxAhead set to 0.8 (the default since 1.0.45; set it back to 1.0 if you prefer)");
        }
        WritePrivateProfileStringA("BFI", "DefaultsVersion", "45", g_iniPath);
    }
}

static void LoadShared()
{
    Settings s;
    {
        static std::atomic<bool> migrated{false};
        if (!migrated.exchange(true)) MigrateDefaults();
    }
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
    IniWriteInt("SplitCompare", s.split);
    IniWriteInt("SplitCompareSide", s.splitSide);
    IniWriteInt("SplitComparePosition", s.splitPos);
    IniWriteInt("SplitCompareSeamPx", s.splitSeam);
    IniWriteInt("FrameWarp", s.frameWarp);
    IniWriteFloat("FrameWarpMaxAhead", s.warpMax);
    IniWriteInt("FrameWarpMinRate", s.warpMinRate);
    IniWriteInt("FrameWarpGameMotion", s.warpGame);
    IniWriteInt("FrameWarpGameHud", s.warpGameHud);
    IniWriteInt("FrameWarpGameMotionWithFG", s.warpGameFg);
    IniWriteFloat("FrameWarpGameMotionMaxAheadFG", s.warpMaxGameFg);
    IniWriteInt("FrameWarpEvenPacing", s.warpEven);
    IniWriteInt("FrameWarpGameMotionEaseNoFG", s.warpEaseNoFg);
    IniWriteFloat("FrameWarpGameMotionEaseStrength", s.warpEaseStr);
    IniWriteInt("FrameWarpGameDilate", s.warpDilate);
    IniWriteInt("FrameWarpStillBeforeEffects", s.warpStillPre);
    IniWriteInt("CrtStandaloneHandoff", s.crtHandoff);
    IniWriteInt("FrameWarpPauseCutscenes", s.warpPauseLb);
    IniWriteFloat("FrameWarpSnapX", s.warpSnapX);
    IniWriteFloat("FrameWarpSnapY", s.warpSnapY);
    IniWriteInt("FrameWarpEdge", s.warpEdge);
    IniWriteInt("Osd", s.osd);
    IniWriteInt("OsdCorner", s.osdCorner);
    IniWriteInt("OsdGraph", s.osdGraph);
    IniWriteInt("OsdLoads", s.osdLoads);
    IniWriteFloat("OsdScale", s.osdScale);
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
    IniWriteInt("SdrToHdr", s.sdrToHdr);
    IniWriteFloat("SdrWhiteNits", s.sdrWhite);
    IniWriteFloat("SdrHighlightNits", s.sdrHiNits);
    IniWriteFloat("SdrHighlightKnee", s.sdrKnee);
    IniWriteInt("SdrGamma", s.sdrGamma);
    IniWriteInt("SdrHighlightMethod", s.sdrMethod);
    IniWriteFloat("SdrBoostStart", s.sdrBoostStart);
    IniWriteFloat("SdrBoostRamp", s.sdrBoostRamp);
    IniWriteInt("SdrBoostSpace", s.sdrBoostSpace);
    IniWriteFloat("SdrBoostColor", s.sdrBoostColor);
    IniWriteInt("SdrBoostAplLimiter", s.sdrBoostApl);
    IniWriteFloat("SdrGamutExpansion", s.sdrGamut);
    IniWriteInt("CapGameToRefresh", s.gameCap);
    IniWriteInt("CapDivisor", s.capDivisor);
    IniWriteInt("PresentOnOwnQueue", s.ownQueue);
    IniWriteInt("FramePacing", s.pacingMode);
    IniWriteInt("MaxFramesAhead", s.maxAhead);
    IniWriteInt("AutoFramesAhead", s.autoAhead);
    IniWriteInt("SmoothCatchUp", s.smoothCatchUp);
    IniWriteInt("PresenterBuffers", s.presBuffers);
    IniWriteInt("QueueAhead", s.queueAhead);
    IniWriteInt("QueueDarkRefresh", s.queueDark);
    IniWriteInt("LaterRelease", s.laterRelease);
    IniWriteInt("CapJustInTime", s.capJit);
    IniWriteInt("AutoFrameCap", s.autoCap);
    IniWriteInt("CapWithReflex", s.capReflex);
    IniWriteInt("ReflexAlign", s.reflexAlign);
    IniWriteInt("ReflexAlignCoarse", s.reflexAlignCoarse);
    IniWriteInt("SleepLimitGrid", s.sleepGrid);
    IniWriteInt("FrameGenLimitWithoutReflex", s.fgLimitNoReflex);
    IniWriteInt("AutoCapAtFrameStart", s.autoCapFrameStart);
    IniWriteInt("LearnPerGame", s.learnPerGame);
    IniWriteInt("LearnReflexStart", s.learnReflex);
    IniWriteInt("PresenterThreadBoost", s.threadBoost);
    IniWriteFloat("LaterReleaseMarginMs", s.laterReleaseMs);
    IniWriteInt("WaitForResizeOnRecreatedSwapChain", s.deferRecreated);
    IniWriteInt("GameFrameStatistics", s.virtualStats);
    IniWriteInt("ReuseDrawnBuffers", s.reuseBuffers);
    IniWriteInt("ScaleToWindow", s.fitWindow);
    IniWriteInt("FlashHints", s.flashHints);
    IniWriteInt("AutoCapOwnWithoutFg", s.autoCapOwn);
    IniWriteInt("StartBFIAfterGameResize", s.waitResizeAll);
    IniWriteInt("PresenterResize", s.selfResize);
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
    int    driftOn = 0;                     // the game runs on a clock of its own, a little off the display (common/bfi_clockdrift.h)
    double driftFps = 0.0, driftHz = 0.0, driftSuggest = 0.0, driftSlip = 0.0;
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
    double aplNow  = -1.0;                      // RenoFX APL Limiter: scene average (0..1 of SDR white); -1 = off
    double aplCut  = 0.0;                       // and the share of the boost it removes
    double hotArea = -1.0;                      // ABL guard: percent of the screen whose lit refresh exceeds the cap
    double ceilInUse = 0.0;                     // ABL guard: lit-refresh ceiling in effect (nits)                  // display refreshing slower than it can, with no missed refreshes
    int    warpOn = 0;                          // frame warp: 0 off, 1 waiting (frame rate outside its range), 2 at work, -1 unavailable
    int    warpCycles = 0;                      // per second: strobe cycles shown moved ahead
    double warpRate = 0.0;                      // the game's frames per strobe cycle, as frame warp measures it
    double warpAvg = 0.0;                       // average distance ahead of those cycles, in game frames
    int    warpSrc = 0;                         // what those cycles were moved with: 0 the picture's own motion, 1 the game's
                                                // motion vectors and depth, 2 its motion vectors only
    int    warpGameState = 0;                   // the game's own motion: 0 nothing seen, 1 vectors and depth, 2 vectors only, -1 not usable
    char   warpGameWhy[160] = "";
    int    warpGameVia = 0;                     // through 0 Streamline, 1 DLSS directly (NGX)
    int    warpHud = 0;                         // the game's HUD mask: 0 none, 1 in use, -1 switched off
    int    reused = 0;                          // per second: refreshes that reused an already-drawn buffer   // per second: strobe cycles with no new game frame / frames never shown
    int    deviceLost = 0;
    int    flashHint = 0;                       // missed refreshes keep coming back (3 of the last 10 s; off after 30 s without)
    int    presBufs = 0;                        // real swap chain buffers behind the presenter
    bool   vramOver = false;                    // the game uses more video memory than its budget (D3DKMT)
    double vramUse = 0.0, vramBudget = 0.0;     // MB
    double ownMB = 0.0, crtMB = 0.0;            // MB of it that BFI Presenter holds, and of that the CRT hand-off's
};
static Status g_status;
// The video memory BFI Presenter holds (1.0.50), counted where each part is made and kept, in bytes
static std::atomic<UINT64> g_bytesFrames{0};   // the staging frames (and the frames from before ReShade's effects)
static std::atomic<UINT64> g_bytesCrtGame{0};  // the CRT hand-off's textures on the game's side (dx/bfi_crt.h)
static std::atomic<UINT64> g_bytesCrtPres{0};  // and the presenter's (its passes for moved flashes)
static UINT64 ResBytes(ID3D12Resource *r);
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



// The game's video memory use and budget, in MB (common/bfi_vram.h: the kernel, D3DKMT).
static bool VramMB(double &use, double &budget)
{
    bool unavailable = false;
    const bool ok = bfi_vram::Query(use, budget, &unavailable);
    if (unavailable) LOGI("BFI Presenter: video memory use not available (D3DKMT)");
    return ok;
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
// Seven since frame warp: it keeps the frame shown before the current one (g_presPrevSlot), which would
// otherwise take the slot a finished frame waits in.
static const UINT kStage  = 7;

static CRITICAL_SECTION       g_lock;
static CRITICAL_SECTION       g_submitLock;      // serialises game-side Present calls (FG add-ons present from other threads)
static DWORD                  g_presentThreads[4] = {};
static std::atomic<HWND>      g_watchHwnd{nullptr};    // game window the watchdog checks for "not responding"
static std::atomic<DWORD>     g_uiTid{0};              // DX11: thread that owns the presenter's child window
static IDXGISwapChain3       *g_sc = nullptr;    // managed real swap chain (not AddRef'd)
static reshade::api::swapchain *g_rsSwapchain = nullptr;
// The game swap chain's colour space (reshade::api::color_space), read on the game's threads and kept here for the
// presenter thread: asking the swap chain from the presenter thread blocked on DXGI's lock while the game destroyed it
// at exit, and the game then waited the 5 s for the presenter to stop (Obduction). -1 = not read yet.
static std::atomic<int> g_rsCs{-1};
static void ReadRsColorSpace() { if (g_rsSwapchain) g_rsCs.store((int)g_rsSwapchain->get_color_space()); }
static std::atomic<bool>      g_active{false};   // hooks redirect for g_sc
static ID3D12Device          *g_device = nullptr;
static ID3D12CommandQueue    *g_gameQueue = nullptr;

static DXGI_SWAP_CHAIN_DESC1  g_desc = {};
static UINT g_srcW = 0, g_srcH = 0;   // DX11/DX9/OpenGL: the game's frame size (staging); g_desc can be smaller (ScaleToWindow)
static DXGI_FORMAT            g_stageFmt = DXGI_FORMAT_UNKNOWN;   // the game's frames (staging ring); g_desc.Format if unknown
static std::atomic<bool>      g_hdrUp{false};   // SDR game presented as HDR10 (SdrToHdr): the presenter's own swap chain
                                                 // (DX11 / DX9 / OpenGL), or the DX12 game's real buffers (g_stageFmt: the game's)
static UINT64 ResBytes(ID3D12Resource *r)
{
    if (!r || !g_device) return 0;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    return g_device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
}
static DXGI_FORMAT StageFormat() { return g_stageFmt != DXGI_FORMAT_UNKNOWN ? g_stageFmt : g_desc.Format; }
static ID3D12Resource        *g_virt[kMaxBuf] = {};
static UINT                   g_bufCount = 0;
static UINT                   g_cur = 0;

static ID3D12Resource        *g_stage[kStage] = {};
static ID3D12Resource        *g_pre[kStage] = {};         // FrameWarpStillBeforeEffects: each slot's frame from before ReShade's effects
static bool                   g_preOk[kStage] = {};       // and whether the slot's frame has one
static std::atomic<UINT>      g_preGen{0};                // counts g_pre being made or released (the presenter rewrites its views)
static UINT64                 g_stageFence[kStage] = {};   // game-queue fence value of the copy
static UINT64                 g_stageId[kStage] = {};      // game frame number (0 = empty)
static double                 g_stageTime[kStage] = {};
static UINT64                 g_readFence[kStage] = {};    // presenter fence value of the last read
static int                    g_presCurSlot = -1;          // staging slot the presenter is showing (g_lock)
static int                    g_presCurSlot2 = -1;         // split comparison: the newer frame its side without BFI shows (g_lock)
static int                    g_presPrevSlot = -1;         // frame warp: the frame shown before the current one (g_lock)
static std::atomic<int>       g_slotWaits{0};
// Swap chains the game recreates (God of War Ragnarok on loading) hung when
// the presenter presented on the game's own queue, and also when moved to
// the presenter's queue from inside Present. For those, the game presents
// its own frames (no BFI) until it resizes the swap chain; the move to the
// presenter's queue then happens inside that resize, and BFI starts.
static int                    g_swapChainsManaged = 0;
static std::atomic<bool>      g_deferPresenter{false};              // game copies that had to wait for the presenter
static std::atomic<double>    g_deferSinceT{0.0};                   // when the wait for the game's first resize began
static std::atomic<int>       g_nudgeStage{0};                      // PresenterResize: see SelfResizeTick
static std::atomic<double>    g_lastGamePresentT{0.0};              // the game's last Present on the managed swap chain
static std::atomic<bool>      g_selfResizeWanted{false};            // PresenterResize: resize at the game's next Present
static std::atomic<void **>   g_selfVtbl{nullptr};                  // PresenterResize: the hooked game-facing vtable
static std::atomic<void **>   g_wrapperVtbl{nullptr};               // the game-facing wrapper swap chain's vtable (ReShade's), if any
static void HookSelfPresent();
static void PatchSlot(void **vtbl, int idx, void *hook, void **orig);
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
static ID3D12CommandAllocator    *g_bakeAlloc[kStage] = {};    // the CRT hand-off's picture: its own list after the copy (1.0.50)
static ID3D12GraphicsCommandList *g_bakeList = nullptr;
static UINT64                     g_bakeDone[kStage] = {};     // the game-queue fence value of slot s's last one
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
BFI_TLS(int, t_selfResize);         // inside the presenter's own resize of the game's swap chain (PresenterResize)
BFI_TLS(int, t_inOuter);            // inside the game-facing (outer) swap chain's Present
BFI_TLS(int, t_scCreateDepth);      // nested CreateSwapChain* calls through the factory hook (outermost = 1)
BFI_TLS(void *, t_scLoopF);          // the factory and result pointer of the outermost call (hook ping-pong check)
BFI_TLS(void *, t_scLoopOut);
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
static HRESULT STDMETHODCALLTYPE hk_SetCS1(IDXGISwapChain3 *sc, DXGI_COLOR_SPACE_TYPE cs);
static BOOL g_fsWanted = FALSE;                    // what the game asked for (exclusive fullscreen is held off)
static std::atomic<int> g_cs12{-1};                // colour space the game set on its managed DX12 swap chain
static std::atomic<bool> g_hdr12Ready10{false};    // DX12, 10-bit game buffers, SdrToHdr on, HDR display: SDR to HDR
                                                   // follows the game's colour space (SDR on, HDR10 off) without a resize
static std::atomic<int> g_cs11{-1};                // colour space the game set on its DX11 swap chain



// ---------------------------------------------------------------- helpers
// A 16-bit float (frame warp's check reads its motion textures back).
static float HalfToFloat(uint16_t h)
{
    const uint32_t sgn = (uint32_t)(h & 0x8000) << 16, ex = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t f;
    if (ex == 0) { if (!man) f = sgn; else { float v = std::ldexp((float)man, -24); return sgn ? -v : v; } }
    else if (ex == 31) f = sgn | 0x7F800000u | (man << 13);
    else f = sgn | ((ex + 112) << 23) | (man << 13);
    float out; memcpy(&out, &f, 4); return out;
}

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

// The presenter thread first, as Blur Busters advises: a late strobe flickers, a late game frame is only a stutter.
// 1. Out of Windows' power throttling (EcoQoS): on laptops and in Balanced power plans Windows can run a thread at
//    reduced clock speed; the presenter wakes every refresh and must not be slowed. No administrator rights needed.
// 2. Registered with the Multimedia Class Scheduler (MMCSS, "Games" task, high): the scheduler audio and DWM use,
//    which raises the thread above every normal thread while it keeps to its share (PresenterThreadBoost=0: off).
static void BoostPresenterThread(bool mmcss, const char *who)
{
    struct PowerThrottling { ULONG version, controlMask, stateMask; };
    typedef BOOL (WINAPI *SetThreadInformation_t)(HANDLE, int, LPVOID, DWORD);
    bool ecoOff = false, mm = false;
    if (auto sti = (SetThreadInformation_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation"))
    {
        PowerThrottling ps = { 1, 0x1 /*EXECUTION_SPEED*/, 0 /*off*/ };
        ecoOff = sti(GetCurrentThread(), 3 /*ThreadPowerThrottling*/, &ps, sizeof(ps)) != FALSE;
    }
    if (mmcss)
    {
        typedef HANDLE (WINAPI *AvSet_t)(LPCWSTR, LPDWORD);
        typedef BOOL (WINAPI *AvPrio_t)(HANDLE, int);
        if (HMODULE av = LoadLibraryW(L"avrt.dll"))
        {
            auto set = (AvSet_t)(void *)GetProcAddress(av, "AvSetMmThreadCharacteristicsW");
            auto prio = (AvPrio_t)(void *)GetProcAddress(av, "AvSetMmThreadPriority");
            DWORD idx = 0;
            HANDLE h = set ? set(L"Games", &idx) : nullptr;
            if (h) { mm = true; if (prio) prio(h, 1 /*AVRT_PRIORITY_HIGH*/); }
        }
    }
    LOGI("BFI Presenter: %s thread: power throttling %s, multimedia scheduler %s", who, ecoOff ? "off" : "unchanged",
         !mmcss ? "off (PresenterThreadBoost=0)" : mm ? "on (Games, high)" : "not available");
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
                LOGI("BFI Presenter: just-in-time release active -- frames finish %.1f ms after release (95th percentile, frames queued ahead included), released %.1f ms "
                     "before the refresh that shows them (cap interval %.1f ms)", est * 1000.0, (est + margin) * 1000.0, period * 1000.0);
            else
                LOGI("BFI Presenter: just-in-time release at its limit -- frames finish %.1f ms after release (frames queued ahead included), close to the %.1f ms cap "
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
                LOGI("BFI Presenter: just-in-time release back on -- frames finish %.1f ms after release", est * 1000.0);
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
                const DXGI_FORMAT typed = StageFormat();
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
    LOGI("BFI Presenter: stand-in back buffers report their typed format (format %d)", (int)StageFormat());
}

static bool CreateFrameResources()
{
    { OwnCall own; if (o_GetDesc1) o_GetDesc1(g_sc, &g_desc); else g_sc->GetDesc1(&g_desc); }   // the real buffers
    if (g_virtCount) g_desc.BufferCount = g_virtCount;          // stand-ins match what the game asked for
    // SDR to HDR (DX12): the real buffers are HDR10, the stand-ins and staging frames keep the game's format.
    const DXGI_FORMAT gf = StageFormat();
    g_bufCount = g_desc.BufferCount < kMaxBuf ? g_desc.BufferCount : kMaxBuf;
    D3D12_RESOURCE_FLAGS vf = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (g_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) vf |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (UINT i = 0; i < g_bufCount; i++)
        if (!(g_virt[i] = CreateTex(g_desc, VirtualFormat(gf), vf, L"BFI virtual back buffer", i))) return false;
    if (g_reportTyped.load() && VirtualFormat(gf) != gf) HookResourceGetDesc(g_virt[0]);
    for (UINT s = 0; s < kStage; s++)
    {
        if (!(g_stage[s] = CreateTex(g_desc, gf, D3D12_RESOURCE_FLAG_NONE, L"BFI staging frame", s))) return false;
        g_stageFence[s] = g_stageId[s] = g_readFence[s] = 0;
        g_presCurSlot = -1; g_presCurSlot2 = -1; g_presPrevSlot = -1;
        g_stageTime[s] = 0.0;
    }
    g_cur = 0;
    g_stageNext = 0;
    LOGI("BFI Presenter: %ux%u, format %u (virtual %u), usage 0x%X, flags 0x%X, %u virtual back buffers",
         g_desc.Width, g_desc.Height, (unsigned)g_desc.Format, (unsigned)VirtualFormat(gf),
         (unsigned)g_desc.BufferUsage, (unsigned)g_desc.Flags, g_bufCount);
    EnterCriticalSection(&g_cfgLock);
    g_status.width = g_desc.Width; g_status.height = g_desc.Height;
    g_status.buffers = g_bufCount; g_status.format = (UINT)g_desc.Format;
    LeaveCriticalSection(&g_cfgLock);
    return true;
}

// PresenterResize: after the presenter's own resize of the game's swap chain (same size and format), only the real
// buffers' description is read again; the stand-ins and staging frames stay (the game still renders into them).
static bool RefreshRealDesc()
{
    { OwnCall own; if (o_GetDesc1) o_GetDesc1(g_sc, &g_desc); else g_sc->GetDesc1(&g_desc); }
    if (g_virtCount) g_desc.BufferCount = g_virtCount;
    LOGI("BFI Presenter: %ux%u, format %u, stand-in back buffers kept (the presenter's own resize)",
         g_desc.Width, g_desc.Height, (unsigned)g_desc.Format);
    return g_bufCount > 0;
}

static void ReleaseFrameResources()
{
    for (UINT i = 0; i < kMaxBuf; i++) SafeRelease(g_virt[i]);
    for (UINT s = 0; s < kStage; s++) { SafeRelease(g_stage[s]); SafeRelease(g_pre[s]); g_preOk[s] = false; }
    crt::ReleaseSlots();
    g_preGen++;
    gmo::Release();
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
    uint  flags;      // 1 roll-off, 2 sine, 4 invert, 8 rolling scan, 1024 the game's frames are linear (scRGB; frame warp)
    float seam;       // rolling scan: blended zone between bands, fraction of the screen height
    float decay;      // rolling scan: phosphor decay time in sub-frames (0 = hard bands)
    float soften;     // 0..1: blend toward the steady average picture (LCD phase swap fade)
    float phase;      // LCD gradual saver: start of the lit window within the first sub-frame, 0..1
    float sdrScale;   // outType 4 (SDR game shown as HDR10): SDR white in 80-nit units
    float sdrKnee;    // highlight expansion starts here (linear, 1.0 = SDR white)
    float sdrPeak;    // SDR white highlights expand to this (80-nit units); <= sdrScale = off
    float gamut;      // outType 4: gamut expansion strength (0 = off)
    float2 srcPerOut; // game frame pixels per output pixel: 1 = the same size; > 1 = scaled down to the window
    float splitX;     // split comparison: the seam, in output pixels
    float splitSeam;  // its soft width in output pixels (0 = hard edge)
    uint  splitMode;  // 0 off, 1 the left side without BFI, 2 the right side
    float rfxRamp;    // SDR to HDR, RenoFX HDR Boost (flags & 256): Ramp Intensity
    float rfxColor;   // its colour blend, 0..1 (0 = brightness only)
    float rfxApl;     // its APL Limiter: 1 = on (the scene average is in aplTex, measured from this frame)
    float warp;       // frame warp: the frame is shown this share of its measured motion further on (0 = as rendered);
                      // with the game's own motion (warpSrc != 0): this many game frames further on
    float warpPrevN;  // the game's own motion: game frames between this frame and the one shown before (t4)
    uint  warpSrc;    // 0 motion measured from the picture; 1 the game's motion vectors and depth; 2 its motion vectors only
    float warpSnapX, warpSnapY;  // moves rounded to whole steps of this many game-frame pixels (0 = off): a CRT shader's scanlines
                      // and mask stay where they are instead of breaking up where parts of the picture move differently
    float warpLag;    // the game's motion with frame generation: shown frames this frame is behind the rendered frame its
                      // motion vectors are for (a generated frame sits between two rendered ones; 0 = the rendered frame)
    float warpEase;   // the game's motion: 1 = the part of a move past the rendered frame is scaled by how much the motion
                      // kept up from the frame before (z - 1 of the motion texture): no overshoot when the camera slows down
    float osdX0, osdY0, osdX1, osdY1;   // frame warp: the BFI OSD on the frame (game-frame pixels; x1 <= x0 = none): never moved
};
Texture2D<float4> src : register(t0);
Texture2D<float> aplTex : register(t1);
Texture2D<float4> srcNew : register(t2);  // split comparison: the newest game frame, for the side without BFI   // APL Limiter: the scene average (1x1); for its reduction pass, the 32x18 cells
Texture2D<float4> srcPrev : register(t4); // frame warp: the game frame shown before this one (what stands still is not moved)
Texture2D<float4> preCur  : register(t5); // FrameWarpStillBeforeEffects (flags & 32768): this frame from before ReShade's effects
Texture2D<float4> prePrev : register(t6); // and the frame shown before, likewise
Texture2D<float4> flowTex : register(t3); // frame warp: xy = how far the picture moved since the frame before (uv), z = confidence
SamplerState linClamp : register(s0);

// Frame warp (dx/bfi_warp.h): how far back along its motion the picture shown at uv is taken from, in uv. The vector
// is read where the picture comes from (second step), so a moving area's own motion is used rather than what it
// moves across; at most 8% of the screen.
float4 GameFlow(float2 uv)
{
    uint w, h;
    flowTex.GetDimensions(w, h);
    int2 q = clamp(int2(uv * float2(w, h)), int2(0, 0), int2(w, h) - 1);
    return flowTex.Load(int3(q, 0));
}
// With the game's own motion (one vector per pixel of the game's vectors, per game frame, with how near each point is): the place
// the output pixel comes from is found in up to three steps along the motion, and each step checked -- does the
// point found really move onto this pixel? Of the steps that do, the nearest point wins: what is in front covers
// what is behind it. Where none does, the area is being uncovered, and the background's motion (the farthest of
// the points found) fills it.
// With frame generation, a generated frame shows everything warpLag shown frames short of where the vectors' (rendered)
// frame has it, so the vectors are read where each point is in that frame: warpLag shown frames further along its
// motion (ep8). The picture is still moved by warp.
float2 WarpShiftGame(float2 uv)
{
    uint w, h;
    src.GetDimensions(w, h);
    const float2 osz = float2(w, h);                      // the check below is to a pixel of the game frame
    const float L = warp - warpLag;                       // where along its motion the vectors are read
    float2 m[3];
    m[0] = GameFlow(uv).xy;
    m[1] = GameFlow(uv - m[0] * L).xy;
    m[2] = GameFlow(uv - m[1] * L).xy;
    float2 best = float2(0.0, 0.0), back = float2(0.0, 0.0);
    float bestNear = -1.0, backNear = 2.0, bestK = 1.0, backK = 1.0;
    [unroll] for (int i = 0; i < 3; i++)
    {
        float4 g = GameFlow(uv - m[i] * L);
        if (g.z < 0.5) continue;
        if (length((g.xy - m[i]) * abs(L) * osz) < 2.0 && g.w > bestNear) { bestNear = g.w; best = m[i]; bestK = saturate(g.z - 1.0); }
        if (g.w < backNear) { backNear = g.w; back = m[i]; backK = saturate(g.z - 1.0); }
    }
    if (bestNear < 0.0) { best = backNear <= 1.0 ? back : float2(0.0, 0.0); bestK = backK; }
    // Up to the rendered frame the motion is known (a generated frame); past it, it's a guess that it carries on: that part
    // eases off as the motion slows, so a camera coming to a stop isn't pushed past where it stops and snapped back (ep9).
    const float known = clamp(warpLag, 0.0, warp);
    const float k = warpEase > 0.001 ? pow(max(bestK, 0.0), warpEase) : 1.0;   // warpEase: the strength (1 = before 1.0.49)
    return clamp(best * (known + (warp - known) * k), -0.08, 0.08);
}

float2 WarpShift(float2 uv)
{
    if (warpSrc != 0) return WarpShiftGame(uv);
    float4 f = flowTex.SampleLevel(linClamp, uv, 0);
    float2 d = f.xy * (f.z * warp);
    f = flowTex.SampleLevel(linClamp, uv - d, 0);
    return clamp(f.xy * (f.z * warp), -0.08, 0.08);
}

// The strip at the screen edge a moved flash has no picture for (FrameWarpEdge, 1.0.45). Mirror (flags & 4096): a place
// beyond the edge is read from the picture reflected at the edge. Fade (flags & 8192): on the side the picture comes in
// from, the move eases off over a band three moves wide, so the strip is drawn from the picture squeezed a little
// instead of from one repeated edge pixel.
float2 EdgeMirror(float2 p, float2 size)
{
    p = p < 0.5 ? 1.0 - p : p;
    return p > size - 0.5 ? 2.0 * size - 1.0 - p : p;
}
float2 EdgeFade(float2 cp, float2 d, float2 size)
{
    float2 dist = d > 0.0 ? cp : size - cp;
    float2 band = 3.0 * abs(d);
    float2 k = band > 0.5 ? saturate(dist / max(band, 0.5)) : 1.0;
    return d * k;
}

// Frame warp: a pixel that is the same in this frame and the one shown before it (8-bit: the same value; 10-bit:
// within a step or two).
// What stands still is found in the frames from before ReShade's effects where they are kept (FrameWarpStillBeforeEffects,
// flags & 32768, 1.0.48): a CRT shader's pre-blur and glow spread the moving scene into a HUD's pixels, so in the finished
// frame the HUD changed a little every frame and was moved with the scene (Onimusha with CRT-Standalone).
// 1.0.49: and, while ReShade's window is open (flags & 131072), where it stands still in the finished frames too: it is
// drawn after the effects, so only they have it -- tested on the frames from before the effects alone, ReShade's window
// was moved with the scene (1.0.48).
static bool gStillPre = false;
float3 StillCur(int2 q) { return gStillPre ? preCur.Load(int3(q, 0)).rgb : src.Load(int3(q, 0)).rgb; }
float3 StillPrev(int2 q) { return gStillPre ? prePrev.Load(int3(q, 0)).rgb : srcPrev.Load(int3(q, 0)).rgb; }
float3 StillPrevAt(float2 uv) { return gStillPre ? prePrev.SampleLevel(linClamp, uv, 0).rgb : srcPrev.SampleLevel(linClamp, uv, 0).rgb; }
bool Unchanged(int2 q)
{
    float3 a = StillCur(q), b = StillPrev(q);
    float3 tol = (flags & 1024) != 0 ? 0.002 * (0.1 + max(abs(a), abs(b))) : float3(0.0016, 0.0016, 0.0016);
    return all(abs(a - b) <= tol);
}
// Does the pixel stand still on the screen (a HUD, text, a crosshair) rather than move with the scene? It is
// unchanged, one or more around it are too, and the scene's motion does not explain it: the frame before, one
// motion step back, shows something else there (a crosshair's one-pixel line over a moving scene, the edge or text
// of a HUD). A pixel of a moving picture can be unchanged by chance, along whole lines in a regular pattern (stripes
// that moved by their own spacing), and all over an area with nothing in it to see moving (a clear sky in a pan):
// there the motion explains it, and it moves with the rest. (Before 1.0.46, 5 of 8 unchanged around it was
// still without the motion test: a clear sky stood still, and a tree's outline against it with it, while the rest
// of the tree moved -- the outline flickered on moved flashes. The inside of an even HUD panel can now move too,
// which shows nothing; its edges and text are within a motion step of something else and stay.)
// 1.0.47: a flat or soft area counts as unchanged only where it is exactly the same, bit for bit, around it -- as a HUD
// is, drawn the same every frame. The inside of a soft cloud is only the same within the tolerance in a camera turn, and
// near its edge one motion step back shows another part of the cloud, so patches of it were held and looked a frame
// late on filled flashes (Oblivion Remastered, 1.0.46). (1.0.47-ep1 held no flat area at all: a flat HUD fill near its
// edge then took in the scene from beside it -- the stamina bar, the OSD's and ReShade's windows.)
bool StillIn(int2 q, float2 size)
{
    if (!Unchanged(q)) return false;
    const float3 a0 = StillCur(q);
    const float lc = (flags & 1024) != 0 ? 0.06 * (0.1 + max(max(a0.r, a0.g), a0.b)) : 0.03;
    int n = 0;
    float con = 0.0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
            if (x != 0 || y != 0)
            {
                if (Unchanged(q + int2(x, y))) n++;
                const float3 e = abs(StillCur(q + int2(x, y)) - a0);
                con = max(con, max(max(e.r, e.g), e.b));
            }
    if (n < 1) return false;
    if (con < lc)
    {
        [unroll] for (int y = -1; y <= 1; y++)
            [unroll] for (int x = -1; x <= 1; x++)
                if (any(StillCur(q + int2(x, y)) != StillPrev(q + int2(x, y)))) return false;
    }
    float2 c = float2(q) + 0.5;
    float4 f = flowTex.SampleLevel(linClamp, c / size, 0);
    float2 step = f.xy * size * (warpSrc != 0 ? min(f.z, 1.0) * warpPrevN : f.z);   // this part of the scene's motion since the frame before, pixels
    if (dot(step, step) < 2.25) return true;               // under a pixel and a half: nothing to tell apart
    float3 a = StillCur(q);
    float3 d = abs(a - StillPrevAt((c - step) / size));
    float lim = (flags & 1024) != 0 ? 0.04 * (0.1 + max(max(a.r, a.g), a.b)) : 0.04;
    return max(max(d.r, d.g), d.b) > lim;
}
bool Still(int2 q, float2 size)
{
    gStillPre = (flags & 32768) != 0;
    const bool r = StillIn(q, size);
    gStillPre = false;
    if (r || (flags & 32768) == 0 || (flags & 131072) == 0) return r;
    return StillIn(q, size);
}

bool InOsd(float2 p) { return osdX1 > osdX0 && p.x >= osdX0 && p.x < osdX1 && p.y >= osdY0 && p.y < osdY1; }

// The game's frame moved ahead: each output pixel is taken from where its part of the picture is in the frame.
// False = show the pixel as rendered: what stands still on the screen is never moved, and nothing that stands
// still is pulled over what moves next to it (the edge of a HUD stays clean; the scene coming out from behind it
// is shown as rendered for that flash).
// Same size: Catmull-Rom (9 bilinear taps), so a moved frame is as sharp as one shown as rendered. Scaled down to
// the window: the four-tap area filter, at the moved place.
// Where the pixel shown was taken from in the game's frame (moved, with frame warp): the CRT hand-off reads its
// halation and glow there, so they move with the picture while the mask and scanlines stay on the screen's pixels.
static float2 gSrcPos = float2(0.0, 0.0);
bool FetchWarped(float2 pos, out float3 c)
{
    c = float3(0, 0, 0);
    uint w, h;
    src.GetDimensions(w, h);
    float2 size = float2(w, h);
    float2 inv = 1.0 / size;
    float2 cp = pos * srcPerOut;
    float2 d = WarpShift(cp * inv) * size;
    if ((flags & 8192) != 0) d = EdgeFade(cp, d, size);
    if (warpSnapX > 0.0) d.x = round(d.x / warpSnapX) * warpSnapX;
    if (warpSnapY > 0.0) d.y = round(d.y / warpSnapY) * warpSnapY;
    if (dot(d, d) < 0.0004) return false;                 // under 1/50 pixel
    if (InOsd(cp)) return false;                          // the BFI OSD: never moved
    if (Still(int2(cp), size)) return false;
    float2 p = cp - d;
    if ((flags & 4096) != 0) p = EdgeMirror(p, size);
    p = clamp(p, float2(0.5, 0.5), size - 0.5);
    if (InOsd(p)) return false;                           // and nothing moved is taken from it
    // the game's HUD mask (w = -1): a HUD pixel is never moved, and no moved pixel is taken from the HUD
    if (warpSrc != 0 && (GameFlow(cp * inv).w < -0.5 || GameFlow(p * inv).w < -0.5)) return false;
    if (Still(int2(p), size)) return false;
    gSrcPos = p;
    if (srcPerOut.x > 1.0001 || srcPerOut.y > 1.0001)
    {
        float2 o = 0.25 * srcPerOut;
        c = 0.25 * (src.SampleLevel(linClamp, (p + float2(-o.x, -o.y)) * inv, 0).rgb +
                    src.SampleLevel(linClamp, (p + float2( o.x, -o.y)) * inv, 0).rgb +
                    src.SampleLevel(linClamp, (p + float2(-o.x,  o.y)) * inv, 0).rgb +
                    src.SampleLevel(linClamp, (p + float2( o.x,  o.y)) * inv, 0).rgb);
        return true;
    }
    float2 tc = floor(p - 0.5) + 0.5;
    float2 f = p - tc;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (tc - 1.0) * inv, t3 = (tc + 2.0) * inv, t12 = (tc + w2 / w12) * inv;
    c += src.SampleLevel(linClamp, float2(t0.x,  t0.y),  0).rgb * (w0.x  * w0.y);
    c += src.SampleLevel(linClamp, float2(t12.x, t0.y),  0).rgb * (w12.x * w0.y);
    c += src.SampleLevel(linClamp, float2(t3.x,  t0.y),  0).rgb * (w3.x  * w0.y);
    c += src.SampleLevel(linClamp, float2(t0.x,  t12.y), 0).rgb * (w0.x  * w12.y);
    c += src.SampleLevel(linClamp, float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    c += src.SampleLevel(linClamp, float2(t3.x,  t12.y), 0).rgb * (w3.x  * w12.y);
    c += src.SampleLevel(linClamp, float2(t0.x,  t3.y),  0).rgb * (w0.x  * w3.y);
    c += src.SampleLevel(linClamp, float2(t12.x, t3.y),  0).rgb * (w12.x * w3.y);
    c += src.SampleLevel(linClamp, float2(t3.x,  t3.y),  0).rgb * (w3.x  * w3.y);
    return true;
}

// The game's frame at output pixel pos. Same size: the pixel itself. Larger (a 5K frame in a 4K window): four bilinear
// taps across the output pixel's footprint, an area filter for up to 2x.
float3 Fetch(float2 pos)
{
    gSrcPos = pos * srcPerOut;
    if (warp != 0.0)
    {
        float3 m;
        if (FetchWarped(pos, m)) return m;
        gSrcPos = pos * srcPerOut;
    }
    if (srcPerOut.x <= 1.0001 && srcPerOut.y <= 1.0001) return src.Load(int3(pos, 0)).rgb;
    uint w, h;
    src.GetDimensions(w, h);
    float2 inv = 1.0 / float2(w, h);
    float2 cp = pos * srcPerOut;
    float2 o = 0.25 * srcPerOut;
    return 0.25 * (src.SampleLevel(linClamp, (cp + float2(-o.x, -o.y)) * inv, 0).rgb +
                   src.SampleLevel(linClamp, (cp + float2( o.x, -o.y)) * inv, 0).rgb +
                   src.SampleLevel(linClamp, (cp + float2(-o.x,  o.y)) * inv, 0).rgb +
                   src.SampleLevel(linClamp, (cp + float2( o.x,  o.y)) * inv, 0).rgb);
}

// The same for the newest game frame (split comparison, side without BFI).
float3 FetchNew(float2 pos)
{
    if (srcPerOut.x <= 1.0001 && srcPerOut.y <= 1.0001) return srcNew.Load(int3(pos, 0)).rgb;
    uint w, h;
    srcNew.GetDimensions(w, h);
    float2 inv = 1.0 / float2(w, h);
    float2 cp = pos * srcPerOut;
    float2 o = 0.25 * srcPerOut;
    return 0.25 * (srcNew.SampleLevel(linClamp, (cp + float2(-o.x, -o.y)) * inv, 0).rgb +
                   srcNew.SampleLevel(linClamp, (cp + float2( o.x, -o.y)) * inv, 0).rgb +
                   srcNew.SampleLevel(linClamp, (cp + float2(-o.x,  o.y)) * inv, 0).rgb +
                   srcNew.SampleLevel(linClamp, (cp + float2( o.x,  o.y)) * inv, 0).rgb);
}

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
// SDR shown as HDR10 (outType 4, as the Vulkan layer): SDR white at sdrScale, and above the knee the brightest channel
// (flags & 128: the luminance) is stretched toward sdrPeak (hue kept); below it the image is untouched.
float3 SdrExpand(float3 l)
{
    l = max(l, 0);
    float m = (flags & 128) != 0 ? dot(l, float3(0.2126, 0.7152, 0.0722)) : max(max(l.r, l.g), l.b);
    if (sdrPeak <= sdrScale || m <= sdrKnee) return l * sdrScale;
    float t = (min(m, 1.0) - sdrKnee) / max(1.0 - sdrKnee, 1e-4);
    float o = sdrScale * m + (sdrPeak - sdrScale) * t * t;
    return l * (o / m);
}
// flags & 32: SDR decoded with gamma 2.2, flags & 64: gamma 2.4, else the sRGB curve.
float3 SdrDecode(float3 c) { c = max(c, 0); return (flags & 32) != 0 ? pow(c, 2.2) : (flags & 64) != 0 ? pow(c, 2.4) : SrgbToLin(c); }
// Gamut expansion (Pumbo's AdvancedAutoHDR "Extra HDR saturation", after Unreal Engine): see the Vulkan layer's
// common.glsl. Bright saturated colours move out of BT.709, weighted by their distance from grey (in ACES AP1) and
// their brightness at a 203-nit paper white.
float3 ExpandGamut(float3 v, float amount)
{
    const float pw = 203.0 / 80.0;
    float3 r = v * pw;
    float3 ap1 = float3(dot(r, float3(0.613191478, 0.339512089, 0.047366331)),
                        dot(r, float3(0.070206904, 0.916335817, 0.013450011)),
                        dot(r, float3(0.020618871, 0.109567294, 0.869606747)));
    float luma = dot(ap1, float3(0.2722287168, 0.6740817658, 0.0536895174));
    if (luma <= 0) return v;
    float3 ch = ap1 / luma - 1;
    float k = (1 - exp2(-4 * dot(ch, ch))) * (1 - exp2(-4 * amount * luma * luma));
    float3 e = float3(dot(r, float3(1.375026308, -0.342531728, -0.079080858)),
                      dot(r, float3(-0.077049702, 1.092442754, -0.010129544)),
                      dot(r, float3(-0.020494311, -0.091839135, 1.203626612)));
    return lerp(r, e, k) / pw;
}
// ---- RenoFX HDR Boost, ported from RenoFX by Carlos Lopez (ShortFuse), MIT licence:
// https://github.com/clshortfuse/renofx -- Copyright (c) 2026 Carlos Lopez. Permission is hereby granted, free of charge,
// to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the
// Software without restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to
// do so, subject to the following conditions: The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
// C-infinity inverse tone mapping (HDR Boost, Start, Ramp Intensity) in the LMS working space with its hue restore, or
// by brightness (Yf), and the boost's colour blend. In: SDR linear BT.709 (1 = SDR white); out: the same, boosted.
float RfxTransition(float p)
{
    p = saturate(p);
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1.0;
    return 1.0 / (1.0 + exp2((1.0 - 2.0 * p) / (p * (1.0 - p))));
}
float RfxRampAt(float x, float start, float ri)
{
    if (x <= start) return 0.0;
    float pos = (x - start) / max(1.0 - start, 1e-6);
    float w = pos / (1.0 + pos);
    return pos * pow(2.0 * RfxTransition(w), 1.0 / max(ri, 1e-3));
}
float RfxBoost1(float x, float mult, float peakRamp, float start, float ri)
{
    if (mult <= 1.0 || x <= start) return x;
    return x * (1.0 + (mult - 1.0) * RfxRampAt(x, start, ri) / max(peakRamp, 1e-6));
}
float3 RfxToLms(float3 c)
{
    return float3(dot(float3(0.2896057766, 0.6972466442, 0.0763712786), c),
                  dot(float3(0.0901837576, 0.7073490257, 0.1122031464), c),
                  dot(float3(0.0155287401, 0.0536093073, 0.5097978305), c));
}
float3 RfxFromLms(float3 c)
{
    return float3(dot(float3( 4.9676196501, -4.9223795290,  0.3391991813), c),
                  dot(float3(-0.6196828788,  2.0517506515, -0.3587439845), c),
                  dot(float3(-0.0861520039, -0.0658193831,  1.9889544835), c));
}
static const float3 kRfxLmsW = float3(0.68990272, 0.34832189, 0.0371597069161);
float RfxYf(float3 bt709) { return dot(RfxToLms(bt709), float3(0.68990272, 0.34832189, 0.0)); }
float3 RfxWeightedToMb(float3 w)
{
    float y = max(w.x + w.y, 0.0);
    float iy = y > 0.0 ? 1.0 / y : 0.0;
    return float3(w.x * iy, w.z * iy, y);
}
float3 RfxMbToWeighted(float2 c, float y) { return float3(c.x, 1.0 - c.x, c.y) * y; }
float2 RfxWhiteMb() { return RfxWeightedToMb(RfxToLms(float3(1, 1, 1)) * kRfxLmsW).xy; }
float RfxHalfspaceT(float2 o, float2 d, float2 n, float num)
{
    float den = dot(n, d);
    float an = num + dot(n, RfxWhiteMb() - o);
    return den > 1e-8 ? an / den : 1e20;
}
float RfxRayExitT(float2 o, float2 d)
{
    if (dot(d, d) <= 1e-14) return 1e20;
    float r = 1e20;
    r = min(r, RfxHalfspaceT(o, d, float2(-0.043889, -0.006807), 0.0065035249));
    r = min(r, RfxHalfspaceT(o, d, float2(-0.007821, -0.008564), 0.00104900495));
    r = min(r, RfxHalfspaceT(o, d, float2(-0.000604, -0.007942), 0.000207697044));
    r = min(r, RfxHalfspaceT(o, d, float2(0.0, -0.080835), 0.00165556648));
    r = min(r, RfxHalfspaceT(o, d, float2(0.953597, 0.307020), 0.252472349));
    r = min(r, RfxHalfspaceT(o, d, float2(-0.060969, 0.019752), 0.0241967351));
    r = min(r, RfxHalfspaceT(o, d, float2(-0.106895, 0.004035), 0.0199621232));
    return max(r, 0.0);
}
float3 RfxRestoreLmsHue(float3 src, float3 tgt)
{
    float3 adapt = RfxToLms(float3(1, 1, 1)) * 0.18;
    float3 srcMb = RfxWeightedToMb(src * kRfxLmsW / adapt);
    float3 tgtMb = RfxWeightedToMb(tgt * kRfxLmsW / adapt);
    float2 neutral = RfxWeightedToMb(kRfxLmsW).xy;
    float2 so = srcMb.xy - neutral, to = tgtMb.xy - neutral;
    float sr2 = dot(so, so), tr2 = dot(to, to);
    if (sr2 <= 1e-7 || tr2 <= 1e-7) return tgt;
    float tClipT = RfxRayExitT(neutral, to), tClipS = RfxRayExitT(neutral, so);
    float sp = tClipS > 0.0 ? saturate(1.0 / tClipS) : 0.0;
    float tp = tClipT > 0.0 ? saturate(1.0 / tClipT) : 0.0;
    float pw = sp > 1e-7 ? saturate(tp / sp) : 1.0;
    float sens = lerp(1.0, 0.35, saturate((tClipT - 0.20139844) / (1.02634534 - 0.20139844)));
    float rw = sens * pw;
    if (rw <= 0.0) return tgt;
    float tr = sqrt(tr2);
    float2 dir = lerp(to / tr, so * rsqrt(sr2), saturate(rw));
    float dl = dot(dir, dir);
    dir = dl > 1e-7 ? dir * rsqrt(dl) : to / tr;
    return RfxMbToWeighted(neutral + dir * tr, tgtMb.z) * max(adapt, 1e-6) / kRfxLmsW;
}
float3 RfxHdrBoost(float3 lin, float mult, float start, float ri, float colour, bool byYf)
{
    if (mult <= 1.0001) return lin;
    float peakRamp = RfxRampAt(1.0, start, ri);
    if (peakRamp <= 1e-6) return lin;
    float3 boosted;
    if (byYf)
    {
        float wy = max(RfxYf(float3(1, 1, 1)), 1e-6), sy = max(RfxYf(lin), 0.0);
        float by = RfxBoost1(sy / wy, mult, peakRamp, start, ri) * wy;
        boosted = sy > 1e-6 ? lin * (by / sy) : lin;
    }
    else
    {
        float3 w = RfxToLms(lin), white = RfxToLms(float3(1, 1, 1));
        float3 n = max(0.0, w / white);
        float3 b = float3(RfxBoost1(n.x, mult, peakRamp, start, ri), RfxBoost1(n.y, mult, peakRamp, start, ri),
                          RfxBoost1(n.z, mult, peakRamp, start, ri)) * white;
        boosted = RfxFromLms(RfxRestoreLmsHue(w, b));
    }
    float sY = RfxYf(lin), bY = RfxYf(boosted);
    if (sY <= 1e-6 || bY <= 1e-6) return boosted;
    return lerp(lin * (bY / sY), boosted, saturate(colour));
}
float3 SdrToHdr(float3 lin)
{
    // flags & 256: RenoFX's HDR Boost (peak = SDR highlights / SDR white, Start in sdrKnee); else the knee curve.
    // RenoFX's availability blends each channel between the source and the full boost, which is the same as a smaller
    // boost: 1 + a (boost - 1). The APL Limiter's availability: 1 up to a scene average of 30% of SDR white, smoothly
    // down to 0 at 100% (RenoFX's HDR_BOOST_APL_START 30, minimum 0), from this frame's own average.
    float rfxMult = sdrPeak > sdrScale ? sdrPeak / sdrScale : 1.0;
    if (rfxApl > 0.5) rfxMult -= smoothstep(0.30, 1.0, saturate(aplTex.Load(int3(0, 0, 0)))) * (rfxMult - 1.0);
    float3 o = (flags & 256) != 0 ? RfxHdrBoost(max(lin, 0), rfxMult, sdrKnee, rfxRamp,
                                                rfxColor, (flags & 512) != 0) * sdrScale
                                  : SdrExpand(lin);
    if (gamut > 0) o = ExpandGamut(o / sdrScale, gamut) * sdrScale;
    return o;
}
// BT.709 -> BT.2020 primaries (ITU-R BT.2087).
float3 Bt709To2020(float3 l)
{
    return float3(dot(l, float3(0.627404, 0.329283, 0.043313)),
                  dot(l, float3(0.069097, 0.919540, 0.011362)),
                  dot(l, float3(0.016391, 0.088013, 0.895595)));
}
float3 Decode(float3 c)
{
    if (outType == 0) return SrgbToLin(c);
    if (outType == 2) return PqToLin(c);
    if (outType == 4) return SdrToHdr(SdrDecode(c));
    return c;
}
float3 Encode(float3 l)
{
    if (outType == 0) return saturate(LinToSrgb(l));
    if (outType == 2) return LinToPq(l);
    if (outType == 4) return LinToPq(Bt709To2020(l));
    return l;
}

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
// 4x4 samples per cell. Rows 18-35: RenoFX's APL Limiter -- the average of each
// pixel's SDR brightness (Yf, 1 = SDR white, capped at 1) over the same cells.
// APL Limiter: one 32x18 cell's average SDR brightness (Yf, 1 = SDR white, each pixel capped at 1), 4x4 samples.
float AplCell(float2 cxy)
{
    uint w, h;
    src.GetDimensions(w, h);
    float2 cell = float2(w, h) / float2(32.0, 18.0);
    float apl = 0.0, yw = max(RfxYf(float3(1, 1, 1)), 1e-6);
    [unroll] for (int y = 0; y < 4; y++)
    [unroll] for (int x = 0; x < 4; x++)
    {
        int2 p = int2((cxy + (float2(x, y) + 0.5) / 4.0) * cell);
        apl += saturate(max(RfxYf(max(SdrDecode(src.Load(int3(p, 0)).rgb), 0)), 0.0) / yw);
    }
    return apl / 16.0;
}
// APL Limiter, before the frame's first refresh: its 32x18 cells, then their average into the 1x1 value PSMain reads.
float PSAplCells(float4 pos : SV_Position) : SV_Target { return AplCell(floor(pos.xy)); }
float PSAplReduce(float4 pos : SV_Position) : SV_Target
{
    float a = 0.0;
    for (int y = 0; y < 18; y++)
    {
        float r = 0.0;
        [unroll] for (int x = 0; x < 32; x++) r += aplTex.Load(int3(x, y, 0));
        a += r;
    }
    return a / 576.0;
}
float PSMeasure(float4 pos : SV_Position) : SV_Target
{
    uint w, h;
    src.GetDimensions(w, h);
    float2 cell = float2(w, h) / float2(32.0, 18.0);
    if (pos.y >= 18.0) return AplCell(floor(pos.xy - float2(0.0, 18.0)));   // rows 18-35: the overlay's readout
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

// CRT-Standalone's mask, scanlines and glow (CrtStandaloneHandoff, dx/bfi_crt.h): built in only for the presenter's CRT
// build of this shader, which puts the copied CRT-Standalone code at the marker below.
#ifndef CRT_HANDOFF
float3 CrtColour(float2 pos) { return Fetch(pos); }
#endif
// @CRT_HANDOFF@
float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    float3 c = CrtColour(pos.xy);
    if (method == 2) return float4(outType == 4 ? Encode(Decode(c)) : c, 1);
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
        float phi = (pos.y * srcPerOut.y / float(h)) * n;
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
        float u = (pos.y * srcPerOut.y / float(h)) * float(frames);
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
    if (splitMode != 0 && method < 2)
    {
        // Split comparison (idea: unrefined803, #111): one side shows every refresh at the light the strobe gives over
        // its cycle -- the mean of what each refresh emits, after roll-off and the ceiling -- so both sides match in
        // brightness and differ only in motion clarity and flicker. It shows the newest game frame at every refresh, as a
        // display without BFI would (120 fps at 120 Hz: a new frame each refresh), while the BFI side keeps one frame per
        // strobe cycle.
        float t = splitSeam > 0.0 ? smoothstep(splitX - 0.5 * splitSeam, splitX + 0.5 * splitSeam, pos.x) : step(splitX, pos.x);
        float w = splitMode == 1 ? 1.0 - t : t;
        if (w > 0.0)
        {
            float3 a = float3(0, 0, 0);
            float3 linNew = Decode(FetchNew(pos.xy));
            [unroll] for (uint j = 0; j < 8; j++) if (j < frames) a += Strobe(linNew, j, litGain);
            o = lerp(o, a / float(frames), w);
        }
    }
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

#include "bfi_warp.h"   // frame warp: motion between the last two frames (its shaders and passes)

struct Constants
{
    uint32_t outType, slot, frames, method;
    float    gain, dfloor, knee, ceilLin;
    uint32_t flags;
    float    seam;        // rolling scan: blended zone between bands (fraction of the height)
    float    decay;       // rolling scan: phosphor decay time in sub-frames (0 = hard bands)
    float    soften;      // 0..1: blend toward the steady average picture (LCD phase swap fade)
    float    phase;       // LCD gradual saver: start of the lit window within the first sub-frame, 0..1
    float    sdrScale;    // outType 4: SDR white in 80-nit units
    float    sdrKnee;     // highlight expansion knee (linear, 1 = SDR white)
    float    sdrPeak;     // expanded SDR white (80-nit units); <= sdrScale = off
    float    gamut;       // outType 4: gamut expansion strength (0 = off)
    float    srcPerOutX, srcPerOutY;   // game frame pixels per output pixel (1 = same size)
    float    splitX, splitSeam;        // split comparison: seam position and soft width, output pixels
    uint32_t splitMode;                // 0 off, 1 left side without BFI, 2 right side
    float    rfxRamp, rfxColor;         // SDR to HDR with RenoFX's HDR Boost: Ramp Intensity, colour blend 0..1
    float    rfxApl;                    // its APL Limiter: 1 = on (GPU-measured from the frame being shown)
    float    warp;                      // frame warp: share of the measured motion the frame is moved ahead (0 = off)
    float    warpPrevN;                 // frame warp with the game's own motion: game frames back to the frame shown before
    uint32_t warpSrc;                   // 0 motion from the picture, 1 the game's motion vectors and depth, 2 its vectors only
    float    warpSnapX, warpSnapY;      // frame warp: moves rounded to whole steps of this many game-frame pixels (0 = off)
    float    warpLag;                   // the game's motion with frame generation: shown frames behind its vectors' frame
    float    warpEase;                  // the game's motion: moves past the rendered frame ease off as the motion slows, this hard (0 = off)
    float    osdX0, osdY0, osdX1, osdY1;  // frame warp: the BFI OSD on the frame, game-frame pixels (none: x1 <= x0)
};
static_assert(sizeof(Constants) / 4 + 8 + 3 <= 64, "root signature: the constants, eight tables and a root CBV in at most 64 DWORDs");
namespace pfx { static UINT64 Bytes12(); static ID3D12Resource *Peek12(); static ID3D12Resource *Take12(); static ID3D11Texture2D *Take11(); static void Release(); static void Register(bool on); static bool OverlayOpen(); }

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
    const bool warpSrc = strncmp(entry, "PSWarp", 6) == 0;     // frame warp's passes have a source of their own
    HRESULT hr = compile(warpSrc ? kWarpShader : kShader, (warpSrc ? sizeof(kWarpShader) : sizeof(kShader)) - 1, "bfi_presenter",
                         nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr))
    {
        LOGE("BFI Presenter: shader %s failed: %s", entry, err ? (const char *)err->GetBufferPointer() : "?");
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);
    return true;
}

// ---- Trace file (BFIPresenter_trace.txt next to the ini): each line written and closed at once, for games that end
// without a crash record (Avatar: Frontiers of Pandora closed itself while creating its swap chain; ReShade's log
// may not be flushed by then). Only for the outer swap chain hook's steps.
static void Trace(const char *fmt, ...)
{
    if (!g_iniPath[0]) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s", g_iniPath);
    if (char *s = strrchr(path, '\\')) s[1] = '\0';
    strncat(path, "BFIPresenter_trace.txt", MAX_PATH - strlen(path) - 1);
    char line[512];
    int n = snprintf(line, sizeof(line), "%10.3f [%5lu] ", Now(), GetCurrentThreadId());
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 3, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
    line[n++] = '\r'; line[n++] = '\n';
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, line, (DWORD)n, &wr, nullptr);
    CloseHandle(f);
}

// ---- Shader cache. The presenter's HLSL is compiled at the highest optimisation level, and the source has grown with
// every feature (all of it is compiled whether a feature is on or not): 1-2 s by 1.0.35, more with 1.0.36, during which
// the presenter can't show anything -- a black screen at every start and every presenter restart. Now the shaders are
// compiled once per session on a thread started when the add-on loads (the game takes seconds to its first frame, so
// they are usually ready), and kept on disk in BFIPresenter_shaders.bin next to the ini, keyed to the exact source and
// compile flags: from the second start of a version there is no compile at all.
static const char *const kShEntry[]  = { "VSMain", "PSMain", "PSMeasure", "PSAplCells", "PSAplReduce",
                                         "PSWarpLuma", "PSWarpDown", "PSWarpFlow", "PSWarpSmooth", "PSWarpGame", "PSWarpHud" };   // the last six: kWarpShader (dx/bfi_warp.h)
static const char *const kShTarget[] = { "vs_5_0", "ps_5_0", "ps_5_0", "ps_5_0", "ps_5_0", "ps_5_0", "ps_5_0", "ps_5_0", "ps_5_0", "ps_5_0", "ps_5_0" };
static const int kShN = 11;
static const int kShWarpGame = 9;                // frame warp from the game's own motion vectors and depth
static const int kShWarpHud = 10;                // and its HUD mask from the game's HUD-less picture
static const int kShWarp = 5;                    // first of the frame warp shaders
static ID3DBlob *g_shBlob[kShN] = {};
static HANDLE volatile g_shThread = nullptr;
static volatile LONG g_shStarted = 0, g_shDone = 0;
static char      g_shPath[MAX_PATH] = {};
typedef HRESULT (WINAPI *PFN_D3DCreateBlob)(SIZE_T, ID3DBlob **);

static uint64_t ShaderKey()
{
    uint64_t h = 1469598103934665603ull;                      // FNV-1a over the source, the flags and the entry points
    auto mix = [&](const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } };
    mix(kShader, sizeof(kShader) - 1);
    mix(kWarpShader, sizeof(kWarpShader) - 1);
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    mix(&flags, sizeof(flags));
    for (int i = 0; i < kShN; i++) { mix(kShEntry[i], strlen(kShEntry[i])); mix(kShTarget[i], strlen(kShTarget[i])); }
    return h;
}

// File: "BFISH001", key (8 bytes), then per shader a 4-byte size (0 = failed to compile) and its bytes.
static bool ShaderCacheLoad(HMODULE dc, uint64_t key)
{
    PFN_D3DCreateBlob mk = dc ? (PFN_D3DCreateBlob)(void *)GetProcAddress(dc, "D3DCreateBlob") : nullptr;
    if (!mk) return false;
    HANDLE f = CreateFileA(g_shPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> d;
    LARGE_INTEGER sz = {};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 16 && sz.QuadPart < 16 * 1024 * 1024;
    DWORD got = 0;
    if (ok) { d.resize((size_t)sz.QuadPart); ok = ReadFile(f, d.data(), (DWORD)d.size(), &got, nullptr) && got == d.size(); }
    CloseHandle(f);
    uint64_t k = 0;
    if (ok) memcpy(&k, d.data() + 8, 8);
    ok = ok && memcmp(d.data(), "BFISH001", 8) == 0 && k == key;
    size_t at = 16;
    ID3DBlob *b[kShN] = {};
    for (int i = 0; ok && i < kShN; i++)
    {
        uint32_t n = 0;
        if (at + 4 > d.size()) { ok = false; break; }
        memcpy(&n, d.data() + at, 4); at += 4;
        if (at + n > d.size()) { ok = false; break; }
        if (n && SUCCEEDED(mk(n, &b[i])) && b[i]) memcpy(b[i]->GetBufferPointer(), d.data() + at, n);
        at += n;
    }
    ok = ok && b[0] && b[1];
    for (int i = 0; i < kShN; i++) { if (ok) g_shBlob[i] = b[i]; else SafeRelease(b[i]); }
    return ok;
}

static void ShaderCacheSave(uint64_t key)
{
    std::vector<uint8_t> d(16);
    memcpy(d.data(), "BFISH001", 8);
    memcpy(d.data() + 8, &key, 8);
    for (int i = 0; i < kShN; i++)
    {
        const uint32_t n = g_shBlob[i] ? (uint32_t)g_shBlob[i]->GetBufferSize() : 0;
        const size_t at = d.size();
        d.resize(at + 4 + n);
        memcpy(d.data() + at, &n, 4);
        if (n) memcpy(d.data() + at + 4, g_shBlob[i]->GetBufferPointer(), n);
    }
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_shPath);
    HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;                     // a read-only game folder: compiled each session instead
    DWORD wr = 0;
    const bool ok = WriteFile(f, d.data(), (DWORD)d.size(), &wr, nullptr) && wr == d.size();
    CloseHandle(f);
    if (!ok || !MoveFileExA(tmp, g_shPath, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
}

static bool Compile(PFN_D3DCompile compile, const char *entry, const char *target, ID3DBlob **out);
static DWORD WINAPI ShaderThread(void *self)
{
    const double t0 = Now();
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    const uint64_t key = ShaderKey();
    if (ShaderCacheLoad(dc, key))
        LOGI("BFI Presenter: shaders loaded from BFIPresenter_shaders.bin (%.0f ms)", (Now() - t0) * 1000.0);
    else
    {
        PFN_D3DCompile compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) LOGE("BFI Presenter: d3dcompiler_47.dll not available");
        for (int i = 0; compile && i < kShN; i++)
            if (!Compile(compile, kShEntry[i], kShTarget[i], &g_shBlob[i])) g_shBlob[i] = nullptr;
        LOGI("BFI Presenter: shaders compiled in %.0f ms%s", (Now() - t0) * 1000.0,
             g_shBlob[0] && g_shBlob[1] ? " -- kept in BFIPresenter_shaders.bin for the next start" : "");
        if (g_shBlob[0] && g_shBlob[1]) ShaderCacheSave(key);
    }
    InterlockedExchange(&g_shDone, 1);
    if (self) FreeLibraryAndExitThread((HMODULE)self, 0);    // the reference taken in ShaderPrepare
    return 0;
}

// Starts the compile (or cache load) once per session. Called when the add-on loads; the presenter calls it too, in
// case it wasn't.
static void ShaderPrepare()
{
    if (InterlockedCompareExchange(&g_shStarted, 1, 0) != 0) return;
    snprintf(g_shPath, sizeof(g_shPath), "%s", g_iniPath);
    if (char *s = strrchr(g_shPath, '\\')) s[1] = '\0';
    strncat(g_shPath, "BFIPresenter_shaders.bin", MAX_PATH - strlen(g_shPath) - 1);
    // The thread holds a reference on the add-on, so it can't be unloaded under it.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&ShaderPrepare, &self);
    g_shThread = CreateThread(nullptr, 0, ShaderThread, self, 0, nullptr);
    if (!g_shThread) { if (self) FreeLibrary(self); ShaderThread(nullptr); }
}

// The presenter's copies (AddRef'd, released with its other objects). Waits for the thread if it is still at work.
static bool ShaderGet(ID3DBlob **out)
{
    ShaderPrepare();
    // Another thread may have just started it and not stored the handle yet.
    while (!g_shThread && !g_shDone) Sleep(1);
    if (g_shThread)
    {
        const double t0 = Now();
        WaitForSingleObject(g_shThread, INFINITE);
        const double w = (Now() - t0) * 1000.0;
        if (w > 20.0) LOGI("BFI Presenter: presenter waited %.0f ms for its shaders", w);
    }
    for (int i = 0; i < kShN; i++) { out[i] = g_shBlob[i]; if (out[i]) out[i]->AddRef(); }
    return out[0] && out[1];
}

// The display mode's exact refresh rate: EnumDisplaySettings gives whole numbers, so 119.88 Hz read as 119 (a strobe
// cycle of 16.81 ms against the real 16.68 ms: the frame-start search, drift detection and learned data used the wrong
// cycle). The active display path has it as a fraction (119880/1000). Falls back to the whole number.
static double ExactRefreshHz(const wchar_t *gdiName, double fallback)
{
    UINT32 np = 0, nm = 0;
    if (!gdiName || GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return fallback;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return fallback;
    for (UINT32 i = 0; i < np; i++)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn = {};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = paths[i].sourceInfo.adapterId;
        sn.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, gdiName) != 0) continue;
        const DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
        if (r.Denominator == 0 || r.Numerator == 0) break;
        const double hz = (double)r.Numerator / r.Denominator;
        return (fallback <= 0.0 || fabs(hz - fallback) < 1.0) ? hz : fallback;
    }
    return fallback;
}

// ---------------------------------------------------------------- presenter thread
static int ResolveOutput(const Settings &s)
{
    if (g_hdrUp.load()) return 4;           // SDR game in an HDR10 swap chain of the presenter's own
    if (s.output >= 0) return s.output;
    // A 16-bit float swap chain is always scRGB to Windows, whatever colour
    // space is reported. Mods that upgrade an SDR game's swap chain to HDR
    // (RenoDX) leave the reported space at sRGB some or all of the time;
    // reading that as SDR clipped every highlight above SDR white.
    if (g_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) return 1;
    switch ((reshade::api::color_space)g_rsCs.load())
    {
    case reshade::api::color_space::extended_srgb_linear: return 1;
    case reshade::api::color_space::hdr10_st2084:         return 2;
    default: break;
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
static std::atomic<int>    g_holdPresents{0};          // > 0: the game is creating a swap chain for the window (see PresentHold)
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
    // The Reflex frame limit at the game's sleep (LearnReflexStart): where in the strobe cycle its frames start once the
    // alignment has settled, the margin it settled with, and the frame-generation factor that was for.
    double rfxMs = -1.0;    // start in the strobe cycle (ms), -1 not known
    double rfxMarginMs = 0.0;   // frames ready this long before the pick (the alignment's target then)
    int    rfxFg = 0;       // 1 = no frame generation, 2-4 = its factor
    int    rfxGaveUp = 0;   // 1 = the alignment went round the strobe cycle (the game's frames don't follow it)
    // The CRT hand-off (1.0.50): the game was short of video memory for it (it stayed freed for a session) -- the next
    // session starts that way. Not tied to the refresh rate; written on its own (g_learnCrtShort).
    int    crtShort = 0;
};
static SRWLOCK     g_learnLock = SRWLOCK_INIT;
static std::string g_learnKey;          // section in use ("" = none read yet)
static LearnRec    g_learnRead;         // what the file had for it
static int         g_learnGen = 0;      // bumped each time the section is read
static LearnRec    g_learnOut;          // to be written to it
static bool        g_learnDirty = false;
// "Forget this game" in the overlay: the helper thread deletes the game's section, the presenter starts over.
static std::atomic<bool> g_learnForgetFile{false}, g_learnForgetNow{false};
static std::atomic<int>  g_learnCrtShort{-1};   // to write: 1 = CrtHandoffMemoryShort, 0 = removed, -1 = nothing

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
    // Short runs of a-f left from a build hash once its digits are gone (shadPS4: "WIP main 4d5a..." -> "d a") change with
    // every build of the emulator: dropped, so its games keep their sections across updates.
    {
        std::string out, tok;
        auto flush = [&]()
        {
            bool hex = !tok.empty() && tok.size() <= 3;
            for (char c : tok) if (c < 'a' || c > 'f') { hex = false; break; }
            if (!tok.empty() && !hex) { if (!out.empty()) out += ' '; out += tok; }
            tok.clear();
        };
        for (char c : t) { if (c == ' ') flush(); else tok += c; }
        flush();
        t = out;
    }
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
    const int crtShort = g_learnCrtShort.exchange(-1);
    AcquireSRWLockExclusive(&g_learnLock);
    const bool dirty = g_learnDirty && !forget;
    const LearnRec out = g_learnOut;
    const std::string cur = g_learnKey;
    g_learnDirty = false;
    ReleaseSRWLockExclusive(&g_learnLock);
    char b[64];
    if (crtShort >= 0 && !cur.empty() && !forget)
        WritePrivateProfileStringA(cur.c_str(), "CrtHandoffMemoryShort", crtShort ? "1" : nullptr, path);
    if (dirty && !cur.empty() && out.frames > 0)
    {
        const char *sec = cur.c_str();
        // Whole numbers (us, mHz): a game that sets its own locale reads "120.000" as 120 thousand or 120.
        snprintf(b, sizeof(b), "%d", out.down);                               WritePrivateProfileStringA(sec, "OneFramePerCycle", b, path);
        snprintf(b, sizeof(b), "%ld", (long)std::lround(out.fsMs * 1000.0));   WritePrivateProfileStringA(sec, "FrameStartUs", b, path);
        snprintf(b, sizeof(b), "%ld", (long)std::lround(out.bestMs * 1000.0)); WritePrivateProfileStringA(sec, "ReleaseToFlashUs", b, path);
        snprintf(b, sizeof(b), "%ld", (long)std::lround(out.hz * 1000.0));     WritePrivateProfileStringA(sec, "RefreshMilliHz", b, path);
        snprintf(b, sizeof(b), "%d", out.frames);                             WritePrivateProfileStringA(sec, "Frames", b, path);
        if (out.rfxFg > 0)
        {
            snprintf(b, sizeof(b), "%ld", (long)std::lround(out.rfxMs * 1000.0));       WritePrivateProfileStringA(sec, "ReflexStartUs", b, path);
            snprintf(b, sizeof(b), "%ld", (long)std::lround(out.rfxMarginMs * 1000.0)); WritePrivateProfileStringA(sec, "ReflexMarginUs", b, path);
            snprintf(b, sizeof(b), "%d", out.rfxFg);                                   WritePrivateProfileStringA(sec, "ReflexFrameGen", b, path);
        }
        if (out.rfxGaveUp) WritePrivateProfileStringA(sec, "ReflexAlignGaveUp", "1", path);
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
        r.rfxFg  = (int)get("ReflexFrameGen", 0);
        if (r.rfxFg > 0)
        {
            r.rfxMs       = get("ReflexStartUs", -1000) / 1000.0;
            r.rfxMarginMs = get("ReflexMarginUs", 0) / 1000.0;
        }
        r.rfxGaveUp = (int)get("ReflexAlignGaveUp", 0);
    }
    r.crtShort = (int)get("CrtHandoffMemoryShort", 0);
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
// New frames shown, and how many came more than a third of a cycle late (the frame-start search: uneven frame times)
static std::atomic<int> g_gameIvN{0}, g_gameIvLate{0};

// PresenterResize: BFI (and DX12 SDR to HDR) start inside the game's first resize of its swap chain. Most games do one
// at start-up; some never do on their own (Hades II, Wreckfest 2), and messages to their window don't make them (tried:
// WM_SIZE, and the window made a pixel shorter). RenoDX's swap chain upgrade resizes the game's swap chain itself, at the
// game's Present -- BFI started inside that one. This does the same: once the game has presented steadily for 3 s while
// BFI waits, the presenter resizes the game-facing swap chain (ReShade's, so ReShade resets as for the game's own
// resize) to the size and format it already has, at the start of the game's next Present on the game's thread. The game
// doesn't know about this resize, so its stand-in back buffers are kept (see t_selfResize). Off by default.
static void SelfResizeTick(double t)
{
    static double steadySince = 0.0, stageT = 0.0;
    const int stage = g_nudgeStage.load();
    if (stage == 0 && t - g_deferSinceT.load() < 0.6) steadySince = 0.0;       // a new swap chain
    if (!g_active.load() || g_dx11.load() || !g_deferPresenter.load() || g_ownPresentQueue) { g_selfResizeWanted.store(false); return; }
    EnterCriticalSection(&g_cfgLock);
    const int mode = g_shared.selfResize;
    LeaveCriticalSection(&g_cfgLock);
    const bool on = mode != 0;
    if (!on || stage >= 2) return;
    // Only the game's first swap chain. A swap chain the game recreated (God of War Ragnarok when frame generation is
    // turned on: 6 buffers, Streamline's frame generation presenting from its own thread) went black after the
    // presenter's resize; there BFI waits for the game's resize as before (change resolution or display mode once).
    // PresenterResize=2 (test): also a recreated swap chain -- Onimusha recreates its swap chain at 4K once after
    // starting and never resizes it (no frame generation there).
    if (g_swapChainsManaged > 1 && mode < 2)
    {
        if (stage == 0)
        {
            LOGI("BFI Presenter: PresenterResize: not used on a swap chain the game recreated -- change resolution or display mode once to start BFI");
            g_nudgeStage.store(2);
        }
        return;
    }
    if (!g_selfVtbl.load()) HookSelfPresent();                                  // turned on in the overlay
    if (t - g_lastGamePresentT.load() > 0.5) { steadySince = 0.0; return; }   // loading: wait
    if (steadySince == 0.0) steadySince = t;
    if (stage == 0)
    {
        if (t - steadySince < 3.0) return;
        g_selfResizeWanted.store(true);
        LOGI("BFI Presenter: PresenterResize: the game presents but hasn't resized its swap chain -- resizing it at its next Present");
        g_nudgeStage.store(1); stageT = t;
    }
    else if (t - stageT >= 3.0)
    {
        if (g_selfResizeWanted.exchange(false))
            LOGW("BFI Presenter: PresenterResize: no hook on the game-facing swap chain (its Present didn't come through) -- "
                 "change resolution or display mode once to start BFI");
        else
            LOGW("BFI Presenter: PresenterResize: BFI still waits after the resize -- change resolution or display mode once to start BFI");
        g_nudgeStage.store(2);
    }
}

// The presenter's own resize of the game-facing swap chain, at the start of the game's Present on it.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SelfPresent)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_SelfPresent1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
static PFN_SelfPresent  o_SelfPresent = nullptr;
static PFN_SelfPresent1 o_SelfPresent1 = nullptr;
static HRESULT STDMETHODCALLTYPE hk_SelfPresent(IDXGISwapChain *sc, UINT sync, UINT flags);
static HRESULT STDMETHODCALLTYPE hk_SelfPresent1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *p);
// Hooked when PresenterResize is on: at swap chain creation, or when it's turned on in the overlay later.
static void HookSelfPresent()
{
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    void **vt = g_wrapperVtbl.load();
    if (!vt || g_selfVtbl.load()) return;
    PatchSlot(vt, VT_Present,  (void *)&hk_SelfPresent,  (void **)&o_SelfPresent);
    PatchSlot(vt, VT_Present1, (void *)&hk_SelfPresent1, (void **)&o_SelfPresent1);
    g_selfVtbl.store(vt);
    LOGI("BFI Presenter: PresenterResize: hooked the game-facing swap chain's Present");
}
static void SelfResize(IDXGISwapChain *outer)
{
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(outer->GetDesc(&d))) { LOGW("BFI Presenter: PresenterResize: no swap chain description"); return; }
    if (d.OutputWindow != g_watchHwnd.load())       // another swap chain behind the same wrapper type: not this one
    {
        g_selfResizeWanted.store(true);
        return;
    }
    t_selfResize++;
    const HRESULT hr = outer->ResizeBuffers(d.BufferCount, d.BufferDesc.Width, d.BufferDesc.Height, d.BufferDesc.Format, d.Flags);
    t_selfResize--;
    LOGI("BFI Presenter: PresenterResize: resized the game's swap chain (%u buffers, %ux%u, format %u): 0x%08lX",
         d.BufferCount, d.BufferDesc.Width, d.BufferDesc.Height, (unsigned)d.BufferDesc.Format, (unsigned long)hr);
}
static HRESULT STDMETHODCALLTYPE hk_SelfPresent(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    if (!t_ownCall && !(flags & DXGI_PRESENT_TEST) && g_selfResizeWanted.exchange(false)) SelfResize(sc);
    return o_SelfPresent(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE hk_SelfPresent1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *p)
{
    if (!t_ownCall && !(flags & DXGI_PRESENT_TEST) && g_selfResizeWanted.exchange(false)) SelfResize(sc);
    return o_SelfPresent1(sc, sync, flags, p);
}

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
        SelfResizeTick(t);
        if (g_running.load())
            g_sysCheck.Tick(g_watchHwnd.load() ? GetAncestor(g_watchHwnd.load(), GA_ROOT) : nullptr,
                            [](int lvl, const char *m) { if (lvl) LOGW("BFI Presenter: %s", m); else LOGI("BFI Presenter: %s", m); });
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
                    // The presenter's side of it: its step, its thread and the two fences (a GPU queue waiting on the
                    // other's fence shows as completed below reserved / signalled).
                    LOGW("BFI Presenter: WATCHDOG -- presenter step: %s (%.1f s), %s, %s queue", kStageNames[g_presStage.load()],
                         t - g_presStageT.load(), g_running.load() ? "running" : "stopped", g_ownPresentQueue ? "own" : "game's");
                    if (g_thread) LogThreadStack(GetThreadId(g_thread), "presenter thread");
                    if (g_presFence && g_gameFence)
                    {
                        // a try only: a stuck thread may hold g_lock, and the watchdog must not stop with it
                        const bool locked = TryEnterCriticalSection(&g_lock) != 0;
                        const unsigned long long pres = g_presFenceVal, game = g_gameFenceVal;
                        char slots[256]; int sl = 0;
                        for (UINT i = 0; i < kStage && sl < (int)sizeof(slots) - 48; i++)
                            sl += snprintf(slots + sl, sizeof(slots) - sl, " %u:r%llu/g%llu", i,
                                           (unsigned long long)g_readFence[i], (unsigned long long)g_stageFence[i]);
                        if (locked) LeaveCriticalSection(&g_lock);
                        LOGW("BFI Presenter: WATCHDOG -- fences: presenter %llu done of %llu reserved, game copies %llu done of %llu; "
                             "slots (read / copy):%s; showing %d / %d%s",
                             (unsigned long long)g_presFence->GetCompletedValue(), pres,
                             (unsigned long long)g_gameFence->GetCompletedValue(), game, slots, g_presCurSlot, g_presCurSlot2,
                             locked ? "" : " (presenter lock held by another thread)");
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
// DX11: the second's NvAPI Reflex calls, handed to the game's present thread (ReflexTick / ReflexServe11).
struct Rfx11Req { int limitUs = 0, limitAll = 0; bool atSleep = false; };
static std::mutex g_rfx11Lk;
static Rfx11Req g_rfx11Req;
static ReflexSecond g_rfx11Out;
static std::atomic<bool> g_rfx11Pending{false};
static void ReflexServe11();
static void ReflexTick(int limitUs, ReflexSecond &out);
// How long the last ReflexTick took (its calls into NVIDIA's driver), for the reflex log. (1.0.43 tests also ran it on a
// thread of its own: the calls took at most 0.6 ms either way, in Control, Jusant and Returnal, so nothing to move.)
static std::atomic<double> g_rtMs{-1.0};
static int g_showOverlaysBoot = -1;            // ShowOverlays as read at start (-1 not yet); fixed for the session
static std::atomic<int> g_presHeldMask{0};      // last 8 seconds, newest in bit 0: the presenter's presents held below the refresh rate
static std::atomic<double> g_presHeldRate{0.0}; // presents a second in the last such second
static std::atomic<int> g_darkState{0};   // QueueDarkRefresh: 0 not yet, 1 running, 2 off for this session (the queued present waited)
static std::atomic<int> g_sleepShiftUs{0};    // ReflexAlign: move the next wake-up by this much (+ later), taken once
static double g_coarseTargetS = 0.005;        // the coarse alignment's margin (presenter thread only)
static std::atomic<int> g_sleepPhaseUs{-1};   // where in the strobe cycle the sleep limit starts frames now (-1: not on the grid)
static std::atomic<double> g_sleepPhaseT{0.0};
static std::atomic<int> g_sleepGrid{1};       // SleepLimitGrid (copied from the settings)
static std::atomic<bool> g_alignHunted{false}; // ReflexAlign gave up this session (kept moving earlier: the game's own stutters)
static std::atomic<bool> g_fgXess{false};     // the game presents through XeSS frame generation's swap chain (libxess_fg.dll)
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
static std::atomic<int>  g_fgFactNoRfx{1};       // frame generation factor seen from present counts (the overlay greys its limit at 1)
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
    {
        EnterCriticalSection(&g_cfgLock);
        const bool mm = g_shared.threadBoost != 0;
        LeaveCriticalSection(&g_cfgLock);
        BoostPresenterThread(mm, "presenter");
    }
    const double runStartT = Now();     // automatic frame cap: no step down in the first seconds (warm-up, intros)
    if (g_device)
    {
        // The GPU the game renders on, for the system check (its monitor driven by another GPU).
        const LUID rl = g_device->GetAdapterLuid();
        char gpuName[128] = "";
        IDXGIFactory1 *gf = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&gf)) && gf)
        {
            IDXGIAdapter1 *a = nullptr;
            for (UINT k = 0; gf->EnumAdapters1(k, &a) == S_OK; k++)
            {
                DXGI_ADAPTER_DESC1 dd = {};
                a->GetDesc1(&dd);
                if (dd.AdapterLuid.LowPart == rl.LowPart && dd.AdapterLuid.HighPart == rl.HighPart)
                    WideCharToMultiByte(CP_UTF8, 0, dd.Description, -1, gpuName, sizeof(gpuName), nullptr, nullptr);
                a->Release();
            }
            gf->Release();
        }
        g_sysCheck.SetRenderGpu(rl, gpuName);
    }
    ID3D12DescriptorHeap *rtvHeap = nullptr, *srvHeap = nullptr;
    ID3D12CommandAllocator *alloc[kMaxBuf] = {};
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12RootSignature *rootSig = nullptr;
    ID3D12PipelineState *pso = nullptr, *psoMeas = nullptr;
    ID3DBlob *vs = nullptr, *ps = nullptr, *psMeas = nullptr, *psAplC = nullptr, *psAplR = nullptr, *rsBlob = nullptr, *rsErr = nullptr;
    ID3D12Resource *measRt = nullptr, *measRb = nullptr;
    // Frame warp's GPU time (timing log): four timestamps per real buffer -- around frame warp's own passes and
    // around the per-refresh draw -- read back when that buffer is next recorded (its list is done by then).
    ID3D12QueryHeap *tsHeap = nullptr;
    ID3D12Resource  *tsRb = nullptr;
    double tsToMs = 0.0;
    bool   tsPending[kMaxBuf] = {}, tsPasses[kMaxBuf] = {}, tsWarped[kMaxBuf] = {};
    double secWarpPassMs = 0.0, secDrawWarpMs = 0.0, secDrawPlainMs = 0.0;
    int    secWarpPassN = 0, secDrawWarpN = 0, secDrawPlainN = 0;
    ID3D12DescriptorHeap *measHeap = nullptr;
    // RenoFX APL Limiter on the GPU: the frame's 32x18 cells and their 1x1 average, drawn before its first refresh
    ID3D12PipelineState *psoAplC = nullptr, *psoAplR = nullptr;
    ID3D12Resource *aplCellRt = nullptr, *aplRt = nullptr;
    ID3D12DescriptorHeap *aplRtvHeap = nullptr;
    bool aplOk = false;
    UINT64 aplFrameId = ~0ull;
    // Frame warp: the motion between the last two frames shown (dx/bfi_warp.h) and how far ahead each strobe cycle
    // shows its frame (common/bfi_warpclock.h)
    WarpGpu warp;
    WarpClock warpClock;
    ID3DBlob *psWarp[kWarpShN] = {};
    double warpAhead = 0.0;                  // this strobe cycle: game frames ahead of the frame it shows
    int    warpPrev = -1;                    // the staging slot of the frame shown before the current one
    UINT64 warpPrevId = 0;
    int    secWarpCycles = 0;
    int    secCycles = 0;                 // strobe cycles started this second (the CRT hand-off: share of them moved)
    int    secWarpPaused = 0;   // cycles not filled: a camera cut or a letterboxed cutscene
    // Warp guard (ep10): a repeated cycle shown as it is reuses the picture already drawn; filled, it is drawn anew, and
    // that submission can wait in the driver behind the game's own late frame (South of Midnight: about 6 ms, 214 of 296
    // repeats then missed their refresh and the hitch guard switched BFI off). If filled repeats keep making their
    // refresh late, repeats are shown as they are for a while.
    bool   cycRepeat = false;          // this strobe cycle repeats the frame shown before
    uint8_t warpRepHist = 0;           // the last 8 filled repeats: bit set = drawn too late for its refresh
    int    warpRepHistN = 0;
    double warpRepHoldT = -1.0, warpRepHoldS = 30.0;
    int    secWarpHeld = 0;
    int    winWarpPaused = 0, winCuts = 0, winWarpHeld = 0; double winPauseT = 0.0;
    double secWarpSum = 0.0;
    int    secWarpGame[3] = {};             // cycles moved with: the picture's motion, the game's vectors and depth, its vectors only
    UINT   gameFlowGen = ~0u;               // the game's own motion textures the presenter's views were written for
    // Frame warp check (test, FrameWarpGameMotionCheck): every 2 s, on a cycle moved with the game's motion, the motion
    // measured from the picture for the same pair of frames as well; both read back and compared in the log.
    ID3D12Resource *chkRb[2] = {};          // readback: the game's motion texture, the measured one
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT chkFp[2] = {};
    UINT   chkW[2] = {}, chkH[2] = {};
    UINT64 chkBytes[2] = {};
    UINT64 chkFence = 0, chkFrames = 0;
    double chkT = 0.0;
    bool   chkPending = false;
    int    chkSrc = 0;
    const UINT kGameFlowSrv = kStage + 2 + WarpGpu::kSrvCount;   // their views in srvHeap
    const UINT kPreSrv = kGameFlowSrv + kStage;                  // the frames from before ReShade's effects (g_pre)
    const UINT kCrtSrv = kPreSrv + kStage;                       // CRT hand-off: each slot's halation, glow, wide glow and frame before ReShade's window (4 each)
    UINT   crtGenSeen = ~0u, crtBlobGen = 0;
    ID3D12PipelineState *psoCrt = nullptr;                       // the main shader's CRT build (dx/bfi_crt.h)
    // CRT hand-off with passes after the pattern (sharpen, scanline soften, grain ...; 1.0.49): the pattern and each
    // pass drawn into textures the game frame's size (two in turn, and grain's own), the main shader reads the last
    ID3D12PipelineState *psoCrtPass[1 + kCrtPostN] = {};         // [0] the pattern, [1 + k] kCrtPost[k] (null: off)
    ID3D12Resource *crtRt[3] = {};                               // [0] [1] in turn, [2] grain's
    ID3D12DescriptorHeap *crtRtvHeap = nullptr;
    const UINT kCrtPostSrv = kPreSrv + kStage + 4 * kStage;      // their views: (0, 2) (1, 2), as t11 t12
    // what they were last drawn for: drawn again only for another picture (another frame, another move, other settings),
    // not for every refresh of the strobe (t5 drew them on every refresh: a large GPU cost)
    Constants crtKeyC = {};
    float  crtKeyP[kCrtSlots * 4] = {};
    UINT64 crtKeyId = 0;
    int    crtKeyPrev = -2, crtKeyLast = -1;
    const UINT kBakeSrv = kCrtPostSrv + 4;                       // each slot's CRT picture made with the frame (dx/bfi_crt.h Bake), twice (t11 t12)
    UINT   bakeGenSeen = ~0u;
    // a moved flash with its frame's grain (dx/bfi_crt.h GrainFor): (crtRt[from], grain's g) at kRingSrv + 4 g + 2 from
    const UINT kRingSrv = kBakeSrv + 2 * kStage;
    UINT   ringGenSeen = ~0u, ringRtGen = 0, ringRtSeen = ~0u;
    UINT   crtGrainReused = 0, crtGrainMade = 0;                // moved flashes: with their frame's grain, with their own
    double crtGrainLogT = 0.0;
    bool   crtSnapLogged = false;
    ID3D12Resource *crtCb = nullptr;                             // its settings: an upload ring, one entry per draw
    uint8_t *crtCbMap = nullptr;
    const UINT kCrtCbStride = (UINT)((kCrtSlots * 16 + 255) & ~255), kCrtCbN = 64;
    UINT   crtCbAt = 0;
    UINT   preGenSeen = ~0u;
    bool   kwarp = false;
    UINT64 measFence = 0;
    IDXGIOutput *output = nullptr;
    UINT64 bufFence[kMaxBuf] = {};
    // What each real buffer currently holds. A refresh that would draw the
    // same game frame with the same settings into a buffer that already has
    // it just presents it again -- no GPU work, so a GPU busy with the game
    // (a hitch, shader compilation) can't hold the strobe up.
    UINT64   bufFrame[kMaxBuf] = {}, bufFrame2[kMaxBuf] = {};
    int      pickNew2 = -1;                  // split comparison: the slot its side without BFI shows
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
    hd.NumDescriptors = kStage + 2 + WarpGpu::kSrvCount + kStage + kStage + 4 * kStage + 4 + 2 * kStage + 8;   // the stage frames, the APL Limiter's cells and its 1x1 average,
                                                                       // frame warp's textures, the game's own motion per stage slot
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
        svd.Format                  = StageFormat();
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

    // --- shaders (d3dcompiler_47 ships with Windows 10/11): compiled once per session, or loaded from the cache (above)
    if (ok)
    {
        ID3DBlob *sh[kShN] = {};
        ok = ShaderGet(sh);
        vs = sh[0]; ps = sh[1]; psMeas = sh[2]; psAplC = sh[3]; psAplR = sh[4];   // ABL guard, APL Limiter optional
        for (int i = 0; i < kWarpShN; i++) psWarp[i] = sh[kShWarp + i];             // frame warp optional
        if (!ok) LOGE("BFI Presenter: the presenter's shaders are not available");
    }

    if (ok)
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors     = 1;
        range.BaseShaderRegister = 0;
        D3D12_DESCRIPTOR_RANGE rangeApl = range;
        rangeApl.BaseShaderRegister = 1;                   // t1: the APL Limiter's value (or its cells)
        D3D12_DESCRIPTOR_RANGE rangeNew = range;
        rangeNew.BaseShaderRegister = 2;                   // t2: split comparison's newest game frame
        D3D12_DESCRIPTOR_RANGE rangeFlow = range;
        rangeFlow.BaseShaderRegister = 3;                  // t3: frame warp's motion
        D3D12_DESCRIPTOR_RANGE rangePrev = range;
        rangePrev.BaseShaderRegister = 4;                  // t4: frame warp's frame shown before
        D3D12_DESCRIPTOR_RANGE rangePreC = range;
        rangePreC.BaseShaderRegister = 5;                  // t5: this frame from before ReShade's effects (FrameWarpStillBeforeEffects)
        D3D12_DESCRIPTOR_RANGE rangePreP = range;
        rangePreP.BaseShaderRegister = 6;                  // t6: and the frame shown before, likewise
        D3D12_DESCRIPTOR_RANGE rangeCrt = range;
        rangeCrt.BaseShaderRegister = 7; rangeCrt.NumDescriptors = 4;   // t7-t10: CRT hand-off's halation, glow, wide glow, the frame before ReShade's window
        D3D12_DESCRIPTOR_RANGE rangeCrtPost = range;
        rangeCrtPost.BaseShaderRegister = 11; rangeCrtPost.NumDescriptors = 2;   // t11-t12: CRT hand-off's pass before, grain's texture
        D3D12_ROOT_PARAMETER params[11] = {};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &range;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2] = params[1];
        params[2].DescriptorTable.pDescriptorRanges   = &rangeApl;
        params[3] = params[1];
        params[3].DescriptorTable.pDescriptorRanges   = &rangeNew;
        params[4] = params[1];
        params[4].DescriptorTable.pDescriptorRanges   = &rangeFlow;
        params[5] = params[1];
        params[5].DescriptorTable.pDescriptorRanges   = &rangePrev;
        params[6] = params[1];
        params[6].DescriptorTable.pDescriptorRanges   = &rangePreC;
        params[7] = params[1];
        params[7].DescriptorTable.pDescriptorRanges   = &rangePreP;
        params[8] = params[1];
        params[8].DescriptorTable.pDescriptorRanges   = &rangeCrt;
        params[9].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;   // b1: CRT hand-off's settings
        params[9].Descriptor.ShaderRegister = 1;
        params[9].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
        params[10] = params[1];
        params[10].DescriptorTable.pDescriptorRanges  = &rangeCrtPost;
        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 11;
        rsd.pParameters   = params;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers   = &samp;
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
            rd.Width = 32; rd.Height = 36; rd.DepthOrArraySize = 1; rd.MipLevels = 1;   // rows 18-35: APL Limiter
            rd.Format = DXGI_FORMAT_R32_FLOAT; rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            m = m && SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&measRt)));
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 256 * 36; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
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
        // RenoFX APL Limiter: two tiny passes before the frame's first refresh -- 32x18 cells, then their 1x1 average,
        // which the BFI pass reads (t1) in the same command list.
        if (ok && psAplC && psAplR)
        {
            bool m = true;
            pd.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
            pd.PS = { psAplC->GetBufferPointer(), psAplC->GetBufferSize() };
            m = m && SUCCEEDED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoAplC)));
            pd.PS = { psAplR->GetBufferPointer(), psAplR->GetBufferSize() };
            m = m && SUCCEEDED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoAplR)));
            pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            pd.RTVFormats[0] = g_desc.Format;
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = 32; rd.Height = 18; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R32_FLOAT; rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            m = m && SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&aplCellRt)));
            rd.Width = 1; rd.Height = 1;
            m = m && SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&aplRt)));
            D3D12_DESCRIPTOR_HEAP_DESC ah = {};
            ah.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; ah.NumDescriptors = 2;
            m = m && SUCCEEDED(g_device->CreateDescriptorHeap(&ah, IID_PPV_ARGS(&aplRtvHeap)));
            if (m)
            {
                aplCellRt->SetName(L"BFI APL limiter cells");
                aplRt->SetName(L"BFI APL limiter average");
                D3D12_CPU_DESCRIPTOR_HANDLE h = aplRtvHeap->GetCPUDescriptorHandleForHeapStart();
                g_device->CreateRenderTargetView(aplCellRt, nullptr, h);
                h.ptr += rtvStride;
                g_device->CreateRenderTargetView(aplRt, nullptr, h);
                D3D12_SHADER_RESOURCE_VIEW_DESC av = {};
                av.Format = DXGI_FORMAT_R32_FLOAT;
                av.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                av.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                av.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE sh = srvHeap->GetCPUDescriptorHandleForHeapStart();
                sh.ptr += (SIZE_T)kStage * srvStride;
                g_device->CreateShaderResourceView(aplCellRt, &av, sh);
                sh.ptr += srvStride;
                g_device->CreateShaderResourceView(aplRt, &av, sh);
                aplOk = true;
            }
            else
            {
                LOGW("BFI Presenter: RenoFX APL limiter unavailable (could not create its objects)");
                SafeRelease(psoAplC); SafeRelease(psoAplR); SafeRelease(aplCellRt); SafeRelease(aplRt); SafeRelease(aplRtvHeap);
            }
        }
    }
    if (ok)
    {
        // Frame warp's textures, sized from the game's frames (the staging ring)
        const D3D12_RESOURCE_DESC fd = g_stage[0]->GetDesc();
        if (!warp.Init(g_device, rootSig, vs, psWarp, srvHeap, kStage + 2, (UINT)fd.Width, fd.Height))
            LOGW("BFI Presenter: frame warp unavailable (its shaders or textures could not be created)");
        UINT64 freq = 0;
        if (queue && SUCCEEDED(queue->GetTimestampFrequency(&freq)) && freq)
        {
            D3D12_QUERY_HEAP_DESC qd = {};
            qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            qd.Count = 4 * kMaxBuf;
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 8 * 4 * kMaxBuf; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(g_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&tsHeap))) &&
                SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                            nullptr, IID_PPV_ARGS(&tsRb))))
                tsToMs = 1000.0 / (double)freq;
            else { SafeRelease(tsHeap); SafeRelease(tsRb); }
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
            modeHz = ExactRefreshHz(od.DeviceName, (double)dm.dmDisplayFrequency);
    }

    if (!ok)
    {
        LOGE("BFI Presenter: presenter initialisation failed -- falling back to normal presentation");
        g_presenterFailed.store(true);
        g_running.store(false);
    }
    else
        LOGI("BFI Presenter: presenter running (%s queue, %s pacing, presents on %s queue)",
             g_queuePriority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME ? "global realtime" : "high-priority",
             output ? "vblank" : "present-queue", g_ownPresentQueue ? "its own" : "the game's");
    EnterCriticalSection(&g_cfgLock);
    g_status.running = ok ? 1 : 0;
    LeaveCriticalSection(&g_cfgLock);

    Settings s;
    EnterCriticalSection(&g_cfgLock);
    s = g_shared;
    LeaveCriticalSection(&g_cfgLock);

    uint64_t presents = 0, lastShownId = 0;
    double pickNew0T = 0.0;                    // the staging time of the last new frame shown (the frame-start search)
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
    double aplNow = -1.0;                      // RenoFX APL Limiter: the last measured scene average (0..1 of SDR white)
    bool   aplOn = false;                      // RenoFX APL Limiter in use (SDR to HDR with the RenoFX boost)
    float  lastAplCut = 0.0f;
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
    bool   fgSkipLogged = false;  // the frame-generation try at Refresh rate skipped (logged once)
    int    fgFillSecs = 0;        // seconds in a row the frame-generation estimate said the game could fill the refresh rate
    double fgNeedNow = 0.0, fgTryNeedMs = 0.0, fgFailNeedMs = 0.0;   // GPU/CPU ms per rendered frame: now, at the try, at a failed try
    // The step up by Reflex reports: the work per frame it went up on, and at a step up that came back down within 30 s.
    // The reports are taken under the cap and can promise more than the game holds without it (Onimusha: Way of the
    // Sword, 5 ms per frame at 60 fps, 15 ms and 65-71 of 120 fps without the cap: 3 s of uneven frames every 2 minutes).
    double rfxUpNeedMs = 0.0, rfxFailNeedMs = 0.0;
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
    // QueueDarkRefresh: the refresh after the lit one shows the same game frame, so it is drawn and presented right
    // after the lit refresh instead of one vblank later. A stall while it is drawn (most late presents in Control and
    // Jusant fell there: 45 of 49 and 55 of 60) then has two refreshes of room instead of one, and the lit refresh's
    // pick doesn't move: no added latency. The next lit refresh is picked two vblanks after this one's.
    bool pairNext = false;           // this iteration: the refresh queued behind the lit one (no vblank wait)
    double pairVbT = 0.0;            // > 0: the vblank the lit refresh of the last pair was drawn at
    double pairLitPresS = 0.0;       // how long the lit refresh's Present call took (overlays drawn in it: Cocoon 4.4 ms)
    int pairBlocked = 0, pairSeen = 0, pairLogs = 0;   // queued refreshes whose present blocked, and all of them, in the current window
    int pairBadWins = 0;             // windows in a row where most queued presents waited (two switch it off)
    double pairStartT = 0.0;         // when the dark queue first ran this session (not judged for its first 5 s)
    bool pairOff = false;            // off now (the queued presents kept waiting)
    double pairRetryT = 0.0;         // when it tries again (0: off for the session)
    int pairRetries = 0;             // tries again after 1, 2, 4 and 8 minutes, then stays off
    bool pairWaited = false;         // the last pair's queued present waited (a flip still pending behind it)
    double pairDrainT = -10.0;       // when the presenter last skipped a vblank to empty the flip queue
    int pairDrains = 0;
    double secStatsMs = 0.0, secVramMs = -1.0;   // the once-a-second block's calls into DXGI and the driver, timed for the reflex log
    bool vsyncOverridden = false;    // the driver is not honouring the presenter's VSync
    int  vsyncFastSecs = 0;          // seconds in a row with presents well above the refresh count
    int  queuedNow = -1;
    int lastShown = -1;
    double lastT = Now(), secStart = Now(), ageSum = 0.0;
    int secPresents = 0, secLate = 0;
    static ClockDrift drift;        // the game's own clock against the display's (ICO at 59.94 fps on 120 Hz); kept across rebuilds
    double secPhC = 0.0, secPhS = 0.0; int secPhN = 0;
    int exactProbes = 0;            // automatic frame cap: tries at Refresh in a row that came back at exactly the strobe rate
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
    bool kb = false, km = false, kr = false, krs = false, kk = false, ksp = false, kosd = false;

    while (ok && g_running.load())
    {
        // hotkeys (edge-triggered) act on the shared settings
        bool b = KeyCombo('B'), m = KeyCombo('M'), r = KeyCombo('R'), rs = KeyCombo('S'), sp = KeyCombo('C'), ko = KeyCombo('F'), kz = KeyCombo('Z');
        if ((b && !kb) || (m && !km) || (rs && !krs) || (sp && !ksp) || (ko && !kosd))
        {
            EnterCriticalSection(&g_cfgLock);
            if (ko && !kosd) g_shared.osd = !g_shared.osd;
            if (b && !kb) g_shared.enabled = !g_shared.enabled;
            if (m && !km) g_shared.method = !g_shared.method;
            if (rs && !krs) g_shared.rolling = !g_shared.rolling;
            if (sp && !ksp) g_shared.split = !g_shared.split;
            LeaveCriticalSection(&g_cfgLock);
        }
        krs = rs; ksp = sp; kosd = ko;
        {
            // Ctrl+Shift+Z: the OSD's totals start again
            static bool kzPrev = false;
            if (kz && !kzPrev) { Osd::ResetTotals(Now()); LOGI("BFI Presenter: OSD totals reset"); }
            kzPrev = kz;
        }
        {
            // Ctrl+Shift+W: frame warp on / off
            const bool kw = KeyCombo('W');
            if (kw && !kwarp)
            {
                EnterCriticalSection(&g_cfgLock);
                g_shared.frameWarp = g_shared.frameWarp ? 0 : 1;
                const int fw = g_shared.frameWarp;
                LeaveCriticalSection(&g_cfgLock);
                LOGI("BFI Presenter: frame warp %s (Ctrl+Shift+W)", fw ? "on" : "off");
            }
            kwarp = kw;
        }
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
            const bool llOk  = s.laterRelease && s.pacingMode == 1 && per == 1 && s.gameCap >= 1 && g_fgPaceKind.load() != 2 &&
                                 !(autoCapDown && capByReflex);
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
                              "lr_state,lr_lead_ms,lr_est_ms,lr_margin_ms,smooth_catchup,paired\n");
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
                              "rfx_gpu_active_ms,rfx_sim_ms,rfx_submit_ms,rfx_queue_ms,rendered_fps,align_slack_p10_ms,align_shift_ms,"
                              "frame_repeats,warp_state,warp_filled,warp_ahead,warp_pass_gpu_ms,warp_passes,draw_gpu_ms,draw_warped_gpu_ms,"
                              "sec_block_ms,rfx_tick_ms,vram_ms,stats_max_ms\n");
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
        const bool pairIter = pairNext;      // the refresh queued behind the lit one: drawn now, no vblank wait, no tick
        pairNext = false;
        if (pairIter) {}
        else if ((s.pacing || vsyncOverridden) && output && !refillNow)
        {
            auto tick = [&](uint64_t pres, double tt) {
                g_strobeFrames.store(s.frames);
                g_sleepGrid.store(s.sleepGrid);
                g_tickSlot.store((int)((pres - phaseShift) % (uint64_t)s.frames));
                g_tickT.store(tt);
                g_tickCount.fetch_add(1);
                SetEvent(g_tickEvent);
            };
            output->WaitForVBlank();
            if (pairVbT > 0.0)
            {
                // After a pair two refreshes have been presented: this is the vblank the queued one would have been
                // drawn at, and the next lit refresh is picked one vblank later. If the queued refresh was held up past
                // this vblank, the wait above already returned at the later one: its tick is sent late, so the game's
                // frame cap still gets one tick per refresh.
                const double perP = 1.0 / (modeHz > 1.0 ? modeHz : 120.0);
                const double tw = Now();
                if (tw < pairVbT + 1.5 * perP)
                {
                    tick(presents - 1, tw);
                    output->WaitForVBlank();
                    tick(presents, Now());
                }
                else
                {
                    tick(presents - 1, tw - perP);
                    tick(presents, tw);
                    // The queued present waited for a flip (one missed refresh puts a flip one vblank late). Presenting
                    // the lit refresh now would queue it behind a pending flip, and every pair after it too: the next
                    // queued present waits again, this branch is taken again, and the extra refresh in the queue never
                    // drains (Spider-Man 2, t11-t13: 8 ms queued presents and frame age +1 refresh after one missed
                    // refresh, with Reflex on or off, borderless or exclusive). One more vblank empties it -- the dark
                    // refresh shows one refresh longer, once. At most once in 0.5 s (Returnal, t14: stuck again 0.2 s after a skip, 104 waiting pairs until the 2 s limit ran out): a swap chain that really holds one
                    // pending present waits every pair, and the safeguard below switches the dark queue off instead.
                    if (pairWaited && tw - pairDrainT >= 0.5)
                    {
                        output->WaitForVBlank();
                        pairDrainT = Now();
                        if (++pairDrains <= 20)
                            LOGI("BFI Presenter: QueueDarkRefresh: a flip was still pending after the pair -- skipped one vblank "
                                 "to empty the queue (%d so far)", pairDrains);
                    }
                }
                pairVbT = 0.0;
            }
            else tick(presents, Now());
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
        // Split comparison: its side without BFI shows the newest finished game frame at every refresh, like a display
        // without BFI (120 fps at 120 Hz: every frame), while the BFI side keeps one frame per strobe cycle.
        pickNew2 = pick;
        if (s.split && pick >= 0 && newest >= 0 && newestId > pickId) pickNew2 = newest;
        g_presCurSlot2 = pickNew2 != pick ? pickNew2 : -1;
        // Frame warp keeps the frame shown before this one in the ring: what is the same in both stands still on the
        // screen (a HUD) and is not moved.
        if (!s.frameWarp || !warp.ok) { warpPrev = -1; warpPrevId = 0; }
        else if (pick >= 0 && lastShown >= 0 && lastShownId && pickId != lastShownId)
        {
            warpPrev = (pick != lastShown && g_stageId[lastShown] == lastShownId) ? lastShown : -1;
            warpPrevId = warpPrev >= 0 ? lastShownId : 0;
        }
        g_presPrevSlot = warpPrev;
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
            cycRepeat = lastShownId && pickId == lastShownId;
            if (pickNew0T > 0.0 && pickId != lastShownId && g_stageTime[pick] > pickNew0T)
            {
                // the game's frame times, for the frame-start search: a frame more than a third of a cycle late
                g_gameIvN.fetch_add(1);
                if (g_stageTime[pick] - pickNew0T > 1.35 * (double)s.frames / (modeHz > 1.0 ? modeHz : 120.0) * (double)(pickId - lastShownId))
                    g_gameIvLate.fetch_add(1);
            }
            if (pickId != lastShownId) pickNew0T = g_stageTime[pick];
            if (pickId == lastShownId) secFrameRepeats++;
            else if (lastShownId && pickId > lastShownId + 1) secFrameSkips += (int)(pickId - lastShownId - 1);
        }
        pickTime = g_stageTime[pick];
        // Frame warp: how far ahead of its frame this strobe cycle shows it. Decided once per cycle, so the lit and
        // the dark refresh (and Variable MPRT's spill) show the same picture.
        if (!s.frameWarp || !warp.ok)
        {
            if (warpClock.lastId) { warpClock.Reset(); warp.Forget(); }
            warpAhead = 0.0;
        }
        else if (cycleStart)
            warpAhead = warpClock.Cycle((s.warpEven && s.warpGame && gmo::StateFast() > 0 && (!g_status.fgActive || s.warpGameFg)) ? 2 : 1, pickId, pickTime, tVb, (double)s.frames / (modeHz > 1.0 ? modeHz : 120.0),
                                        (double)s.warpMax, (double)s.warpMinRate / 100.0);
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
        c.srcPerOutX = (g_dx11.load() && g_srcW && g_desc.Width) ? (float)g_srcW / (float)g_desc.Width : 1.0f;
        c.srcPerOutY = (g_dx11.load() && g_srcH && g_desc.Height) ? (float)g_srcH / (float)g_desc.Height : 1.0f;
        c.splitMode = s.split ? (s.splitSide ? 2u : 1u) : 0u;
        c.splitX    = s.split ? (float)g_desc.Width * (float)s.splitPos / 100.0f : 0.0f;
        c.splitSeam = s.split ? (float)s.splitSeam : 0.0f;
        if ((int)c.outType != lastOutType)
        {
            static const char *kOut[] = { "SDR", "HDR scRGB", "HDR10 (PQ)", "?", "SDR shown as HDR10" };
            LOGI("BFI Presenter: output %s (%s, swap chain format %d)", kOut[c.outType % 5],
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
            D3D12_RANGE rr = { 0, 256 * 36 };
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
                {
                    double a = 0.0;
                    for (int y = 18; y < 36; y++)
                        for (int x = 0; x < 32; x++) a += std::max(0.0f, std::min(1.0f, px[y * 64 + x]));
                    aplNow = a / (32.0 * 18.0);
                }
                D3D12_RANGE wr = { 0, 0 };
                measRb->Unmap(0, &wr);
                auto avgAt = [&](double C) { return AblAvgAt(cells, C, false, s.frames, s.method == 1); };
                const double peak = s.peakNits;
                const AblCurveT curve = AblMakeCurve(s.ablCurve, peak, (double)s.ablBudget);
                auto loadAt = [&](double C) { return AblLoadAt(cells, C, curve); };
                if (loadAt(peak) <= 1.0) ablSolve = peak;
                else
                {
                    double lo = 50.0, hi = peak;
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
        c.flags   = (s.rolloff ? 1u : 0u) | (s.sine ? 2u : 0u) | (s.invert ? 4u : 0u) | (s.rolling ? 8u : 0u) |
                    (c.outType == 4 ? (s.sdrGamma == 1 ? 32u : s.sdrGamma == 2 ? 64u : 0u) | (s.sdrMethod == 1 ? 128u : 0u) | (s.sdrMethod == 2 ? 256u : 0u) |
                                             (s.sdrMethod == 2 && s.sdrBoostSpace ? 512u : 0u) : 0u);
        c.sdrScale = s.sdrWhite / 80.0f;
        {
            // knee given as signal level (what the game writes); the shader works in linear light
            const float k = s.sdrKnee;
            c.sdrKnee = s.sdrGamma == 1 ? powf(k, 2.2f) : s.sdrGamma == 2 ? powf(k, 2.4f)
                                         : (k <= 0.04045f ? k / 12.92f : powf((k + 0.055f) / 1.055f, 2.4f));
            if (s.sdrMethod == 2) c.sdrKnee = s.sdrBoostStart;   // RenoFX: Start, already linear
            c.rfxRamp = s.sdrBoostRamp;
            c.rfxColor = s.sdrBoostColor / 100.0f;
            c.gamut = c.outType == 4 ? s.sdrGamut : 0.0f;
            const float hi = (std::min)(s.sdrHiNits, s.peakNits);
            c.sdrPeak = hi > s.sdrWhite ? hi / 80.0f : 0.0f;
            // RenoFX's APL Limiter: from a scene average of 30% of SDR white the boost fades out smoothly, and is gone
            // for an all-white screen (its HDR_BOOST_APL_START 30, minimum 0), from the frame being shown.
            // The shader takes the average from the frame itself (see the APL passes before the BFI pass); aplNow,
            // read back a frame or two later, is only for the overlay.
            aplOn = aplOk && c.outType == 4 && s.sdrMethod == 2 && s.sdrBoostApl && c.sdrPeak > c.sdrScale;
            c.rfxApl = aplOn ? 1.0f : 0.0f;
            lastAplCut = 0.0f;
            if (aplOn && aplNow >= 0.0)
            {
                const double x = std::max(0.0, std::min(1.0, (aplNow - 0.30) / 0.70));
                lastAplCut = (float)(x * x * (3.0 - 2.0 * x));
            }
            if (!aplOn) { aplNow = -1.0; aplFrameId = ~0ull; }
        }
        if (GradualSaverOn(s))
        {
            c.flags |= 16u;
            c.phase = (float)(floor(gradPhase * 256.0) / 256.0);   // steps of 1/256 refresh: buffers can still be reused
        }
        c.seam    = s.rollSeam;
        c.decay   = s.rollDecay;
        // Frame warp: the motion is measured from the frame shown before this one, which may be more than one game
        // frame older (frames never shown): the share of that motion for warpAhead game frames. In steps of 1/256, so
        // the cycle's refreshes compare equal and drawn buffers can still be reused.
        UINT64 warpOld = 0;
        ID3D12Resource *gFlow = nullptr;                // this refresh's motion texture from the game's motion
        if (StageFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT) c.flags |= 1024u;
        if (warpAhead > 0.0 && s.frameWarp && warp.ok && (warpOld = warp.PairFor(pickId, pickTime)) != 0 &&
            warpPrev >= 0 && warpPrev != pick && warpPrevId == warpOld && g_stageId[warpPrev] == warpOld && g_stageFence[warpPrev] <= done)
        {
            // The game's own motion (where it hands its upscaler motion vectors and depth): one game frame's motion,
            // warpAhead frames on. Otherwise the motion measured between this frame and the one shown before.
            const int gsrc = (s.warpGame && (!g_status.fgActive || s.warpGameFg)) ? gmo::SlotFor(pick, pickId) : 0;
            if (gsrc)
            {
                const UINT fg = gmo::FlowGenNow();
                if (fg != gameFlowGen)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE gh = srvHeap->GetCPUDescriptorHandleForHeapStart();
                    gh.ptr += (SIZE_T)kGameFlowSrv * srvStride;
                    gameFlowGen = gmo::WriteFlowSrvs(gh, srvStride);
                }
                gFlow = gmo::FlowFor(pick, gameFlowGen);   // one texture for the whole refresh (both barriers, the check)
            }
            if (gsrc && gFlow)
            {
                // with frame generation, a limit of its own (the game's motion overshoots visibly further out)
                const double ahead = g_status.fgActive ? std::min(warpAhead, (double)s.warpMaxGameFg) : warpAhead;
                c.warp = (float)(floor(ahead * 256.0 + 0.5) / 256.0);
                c.warpPrevN = (float)(pickId - warpOld);
                c.warpSrc = (uint32_t)gsrc;
                c.warpLag = g_status.fgActive ? gmo::SlotLag(pick) : 0.0f;
                c.warpEase = (s.warpEase && (g_status.fgActive || s.warpEaseNoFg)) ? s.warpEaseStr : 0.0f;
            }
            // Even pacing is for the game's motion only: a frame that arrived in time isn't moved by the measured motion.
            else if (s.warpEven && warpClock.repeats == 0) c.warp = 0.0f;
            else c.warp = (float)(floor(warpAhead / (double)(pickId - warpOld) * 256.0 + 0.5) / 256.0);
            c.warpSnapX = s.warpSnapX; c.warpSnapY = s.warpSnapY;
            if (s.warpEdge == 0) c.flags |= 8192u; else if (s.warpEdge == 1) c.flags |= 4096u;   // the screen edges (FrameWarpEdge)
            // what stands still, found in the frames from before ReShade's effects (FrameWarpStillBeforeEffects)
            if (s.warpStillPre && g_pre[pick] && g_pre[warpPrev] && g_preOk[pick] && g_preOk[warpPrev])
                c.flags |= 32768u | (pfx::OverlayOpen() ? 131072u : 0u);   // ReShade's window open: the finished frames too
            if (s.osd && Now() - Osd::drawnT.load() < 0.5)    // the BFI OSD on the frame: never moved (its background is see-through)
            {
                c.osdX0 = Osd::drawnRect[0]; c.osdY0 = Osd::drawnRect[1]; c.osdX1 = Osd::drawnRect[2]; c.osdY1 = Osd::drawnRect[3];
            }
            // Around a camera cut, and in a letterboxed cutscene: shown as rendered (no motion connects two shots).
            const char *pauseWhy = nullptr;
            if (c.warp != 0.0f && gmo::PauseNow(pickId, s.warpPauseLb != 0, &pauseWhy))
            {
                c.warp = 0.0f; c.warpSrc = 0; gFlow = nullptr;
                if (cycleStart) secWarpPaused++;
            }
            // The warp guard: repeats shown as they are while it holds.
            if (c.warp != 0.0f && cycRepeat && s.warpGuard && Now() < warpRepHoldT)
            {
                c.warp = 0.0f; c.warpSrc = 0; gFlow = nullptr;
                if (cycleStart) secWarpHeld++;
            }
        }
        if (cycleStart) secCycles++;
        if (cycleStart && c.warp != 0.0f) { secWarpCycles++; secWarpSum += warpAhead; secWarpGame[c.warpSrc <= 2 ? c.warpSrc : 0]++; }

        if (pickNew2 < 0) pickNew2 = pick;                       // picked later (the wait above): the same frame
        // CRT hand-off (dx/bfi_crt.h): the CRT build arriving, and whether this refresh is drawn with it
        if (s.crtHandoff)
        {
            ID3DBlob *cbs[crt::kBlobN] = {};
            if (crt::TakeBlobs(&crtBlobGen, cbs))
            {
                SafeRelease(psoCrt);
                for (int k = 0; k <= kCrtPostN; k++) SafeRelease(psoCrtPass[k]);
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
                pd.pRootSignature = rootSig;
                pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.RasterizerState.DepthClipEnable = TRUE;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.SampleDesc.Count = 1;
                bool pok = cbs[0] != nullptr;
                for (int e = 0; e < crt::kBlobN && pok; e++)
                {
                    if (!cbs[e]) continue;
                    pd.PS = { cbs[e]->GetBufferPointer(), cbs[e]->GetBufferSize() };
                    pd.RTVFormats[0] = e == 0 ? g_desc.Format : crt::PassFormat();
                    ID3D12PipelineState **dst = e == 0 ? &psoCrt : &psoCrtPass[e - 1];
                    if (FAILED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(dst)))) { *dst = nullptr; pok = false; }
                }
                if (cbs[1] && !psoCrtPass[0]) pok = false;
                if (!pok)
                {
                    SafeRelease(psoCrt);
                    for (int k = 0; k <= kCrtPostN; k++) SafeRelease(psoCrtPass[k]);
                    if (cbs[0]) LOGE("BFI Presenter: CRT hand-off: its pipeline could not be made");
                }
                for (int e = 0; e < crt::kBlobN; e++) SafeRelease(cbs[e]);
            }
            if (!crtCb)
            {
                D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC bd = {};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (UINT64)kCrtCbStride * kCrtCbN; bd.Height = 1; bd.DepthOrArraySize = 1;
                bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&crtCb))))
                {
                    D3D12_RANGE nr = { 0, 0 };
                    if (FAILED(crtCb->Map(0, &nr, (void **)&crtCbMap))) { SafeRelease(crtCb); crtCbMap = nullptr; }
                }
            }
            if (psoCrt && crtCbMap && crt::StateNow() == 2 && crt::SlotReady((UINT)pick))
            {
                c.flags |= 65536u;
                // The mask and scanlines are drawn after the move now, on the screen's pixels: Snap (moves rounded to the
                // scanline spacing, for a CRT shader in ReShade) has nothing to keep in line, and a step that isn't the
                // spacing in use shifts the picture against the scanlines on every moved flash (1.0.49-t8: Snap 3 px with
                // scanlines 2.5 wide, drawn 2 px apart -- a flash on every fill)
                if ((c.warpSnapX > 0.0f || c.warpSnapY > 0.0f) && !crtSnapLogged)
                {
                    crtSnapLogged = true;
                    LOGI("BFI Presenter: CRT hand-off: frame warp's Snap (%.1f x %.1f px) not used -- the scanlines are drawn after the move", (double)c.warpSnapX, (double)c.warpSnapY);
                }
                c.warpSnapX = c.warpSnapY = 0.0f;
            }
        }
        const UINT64 pickId2 = pickNew2 >= 0 ? g_stageId[pickNew2] : 0;
        const bool reuse = s.reuseBuffers && bufValid[bi] && bufFrame[bi] == pickId && bufFrame2[bi] == pickId2 &&
                           memcmp(&bufConst[bi], &c, sizeof(c)) == 0;
        if (!reuse)
        {
            reserve = ++g_presFenceVal;
            g_readFence[pick] = reserve;
            if (pickNew2 != pick) g_readFence[pickNew2] = reserve;
            if (c.warp != 0.0f) g_readFence[warpPrev] = reserve;
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
        if (tsPending[bi] && tsRb)                      // this buffer's last list is done (its fence waited above)
        {
            UINT64 *q = nullptr;
            D3D12_RANGE rr = { (SIZE_T)(8 * 4 * bi), (SIZE_T)(8 * 4 * (bi + 1)) };
            if (SUCCEEDED(tsRb->Map(0, &rr, (void **)&q)) && q)
            {
                const UINT64 *t = q + 4 * bi;
                const double passMs = t[1] >= t[0] ? (double)(t[1] - t[0]) * tsToMs : -1.0;
                const double drawMs = t[3] >= t[2] ? (double)(t[3] - t[2]) * tsToMs : -1.0;
                if (tsPasses[bi] && passMs >= 0.0 && passMs < 50.0) { secWarpPassMs += passMs; secWarpPassN++; }
                if (drawMs >= 0.0 && drawMs < 50.0)
                {
                    if (tsWarped[bi]) { secDrawWarpMs += drawMs; secDrawWarpN++; }
                    else { secDrawPlainMs += drawMs; secDrawPlainN++; }
                }
                D3D12_RANGE wr = { 0, 0 };
                tsRb->Unmap(0, &wr);
            }
            tsPending[bi] = false;
        }
        alloc[bi]->Reset();
        list->Reset(alloc[bi], pso);
        D3D12_RESOURCE_BARRIER br[2] = {
            Transition(real[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Transition(g_stage[pick], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) };
        list->ResourceBarrier(2, br);
        if (pickNew2 != pick)
        {
            D3D12_RESOURCE_BARRIER b2 = Transition(g_stage[pickNew2], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &b2);
        }
        if (c.warp != 0.0f)
        {
            D3D12_RESOURCE_BARRIER b3 = Transition(g_stage[warpPrev], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &b3);
            if ((c.flags & 32768u) != 0)
            {
                D3D12_RESOURCE_BARRIER pb[2] = { Transition(g_pre[pick], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                                                 Transition(g_pre[warpPrev], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) };
                list->ResourceBarrier(pick == warpPrev ? 1 : 2, pb);
            }
            if (c.warpSrc != 0)
            {
                D3D12_RESOURCE_BARRIER b4 = Transition(gFlow, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                list->ResourceBarrier(1, &b4);
            }
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)bi * rtvStride;
        D3D12_GPU_DESCRIPTOR_HANDLE srv = srvHeap->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += (UINT64)pick * srvStride;
        list->SetDescriptorHeaps(1, &srvHeap);
        list->SetGraphicsRootSignature(rootSig);
        list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
        list->SetGraphicsRootDescriptorTable(1, srv);
        D3D12_GPU_DESCRIPTOR_HANDLE aplSrv = srv;                          // unused unless the limiter is on
        if (aplOk)
        {
            aplSrv = srvHeap->GetGPUDescriptorHandleForHeapStart();
            aplSrv.ptr += (UINT64)(kStage + 1) * srvStride;
        }
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // Frame warp: a frame's first refresh measures its brightness pyramid (one pass over the frame, four tiny
        // ones); the motion from the frame before is measured only when the frame is shown moved, once per pair.
        const bool tsOn = tsHeap && logf;                // timed only while the timing log runs
        if (tsOn) list->EndQuery(tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, 4 * bi + 0);
        // Frame warp check: the two motions read back (a cycle or more later), averaged and logged.
        if (chkPending && g_presFence->GetCompletedValue() >= chkFence)
        {
            chkPending = false;
            double sum[2][2] = {}, wsum[2] = {}, mag[2] = {};
            int nAll[2] = {}, nOk[2] = {}, nHud = 0; HRESULT mapHr[2] = { E_FAIL, E_FAIL };
            const D3D12_RESOURCE_DESC fd = g_stage[0]->GetDesc();
            const double W = (double)fd.Width, H = (double)fd.Height;
            static int hudBigN = 0;
            for (int q = 0; q < 2; q++)
            {
                void *mp = nullptr;
                // the whole buffer (gm7 asked for rows x row pitch, which is past its end when the last row isn't padded:
                // E_INVALIDARG for the measured motion's 120-texel rows)
                D3D12_RANGE rr = { 0, (SIZE_T)chkBytes[q] };
                mapHr[q] = chkRb[q]->Map(0, &rr, &mp);
                if (FAILED(mapHr[q]) || !mp) continue;
                const int step = q == 0 ? 4 : 1;                 // the game's texture is large: every 4th texel
                for (UINT y = 0; y < chkH[q]; y += step)
                {
                    const uint16_t *row = (const uint16_t *)((const uint8_t *)mp + chkFp[q].Offset + (size_t)y * chkFp[q].Footprint.RowPitch);
                    for (UINT x = 0; x < chkW[q]; x += step)
                    {
                        const float fx = HalfToFloat(row[4 * x]), fy = HalfToFloat(row[4 * x + 1]);
                        const float fz = HalfToFloat(row[4 * x + 2]), fw = HalfToFloat(row[4 * x + 3]);
                        // the game's: z = 1 where its vector is valid; the measured (evened out): w = 1 where measured
                        const float wgt = q == 0 ? (fz > 0.5f ? 1.0f : 0.0f) : (fw > 0.5f ? fz : 0.0f);
                        nAll[q]++;
                        if (q == 0 && fw < -0.5f) nHud++;
                        if (wgt <= 0.0f || !std::isfinite(fx) || !std::isfinite(fy)) continue;
                        nOk[q]++;
                        sum[q][0] += fx * wgt; sum[q][1] += fy * wgt; wsum[q] += wgt;
                        mag[q] += std::sqrt((double)fx * W * fx * W + (double)fy * H * fy * H) * wgt;   // pixels
                    }
                }
                D3D12_RANGE wr = { 0, 0 };
                chkRb[q]->Unmap(0, &wr);
            }
            // the HUD mask: how much of the picture it covers (much more than any HUD: it doesn't match -- switched off)
            const double hudPct = nAll[0] ? 100.0 * nHud / nAll[0] : 0.0;
            if (gmo::HudNow() > 0)
            {
                static int hudLogs = 0;
                if (s.warpGameCheck && hudLogs++ < 30) LOGI("BFI Presenter: frame warp check: the HUD mask covers %.1f%% of the picture", hudPct);
                // 3 checks in a row over 35% (1.0.48): a single one at a camera cut or a fade switched it off for the session
                // (Directive 8020: 1-13% for 30 s, then one reading of 70% in a cutscene)
                if (hudPct > 35.0) { if (++hudBigN >= 3) { gmo::HudBad(hudPct); hudBigN = 0; } } else hudBigN = 0;
            }
            if (!s.warpGameCheck) { }                     // only the HUD mask's coverage was wanted
            else if (!(wsum[0] > 0.0 && wsum[1] > 0.0 && chkFrames > 0))
            {
                static int nLog = 0;
                if (nLog++ < 10)
                    LOGI("BFI Presenter: frame warp check: nothing to compare (the game's motion: map 0x%08lX, %d of %d points valid, %ux%u; "
                         "measured: map 0x%08lX, %d of %d points measured, %ux%u; %llu frames between)", (unsigned long)mapHr[0], nOk[0], nAll[0],
                         chkW[0], chkH[0], (unsigned long)mapHr[1], nOk[1], nAll[1], chkW[1], chkH[1], (unsigned long long)chkFrames);
            }
            else
            {
                // per shown frame, in pixels of the game frame
                const double gx = sum[0][0] / wsum[0] * W, gy = sum[0][1] / wsum[0] * H;
                const double mx = sum[1][0] / wsum[1] * W / (double)chkFrames, my = sum[1][1] / wsum[1] * H / (double)chkFrames;
                const double gl = std::sqrt(gx * gx + gy * gy), ml = std::sqrt(mx * mx + my * my);
                const double ang = (gl > 0.05 && ml > 0.05) ? std::acos(std::max(-1.0, std::min(1.0, (gx * mx + gy * my) / (gl * ml)))) * 57.29578 : -1.0;
                static int chkLogs = 0;
                if (chkLogs++ < 150)
                    LOGI("BFI Presenter: frame warp check: per shown frame, the game's motion %.1f, %.1f px (%.1f px; points moving %.1f px on average), "
                         "measured from the picture %.1f, %.1f px (%.1f px), game / measured %.2f, %s%.0f deg apart (%llu frame%s between the pair, %s)",
                         gx, gy, gl, mag[0] / wsum[0], mx, my, ml, ml > 0.05 ? gl / ml : 0.0,
                         ang < 0.0 ? "direction n/a " : "", ang < 0.0 ? 0.0 : ang, (unsigned long long)chkFrames, chkFrames == 1 ? "" : "s",
                         chkSrc == 1 ? "vectors and depth" : "vectors only");
            }
        }
        bool warpTouched = false;
        if (s.frameWarp && warp.ok)
        {
            bool touched = false;
            if (pickId != warp.id[warp.cur])
            {
                warp.RecordLuma(list, srv, pickId, pickTime, StageFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT);
                touched = true;
            }
            if (c.warp != 0.0f && c.warpSrc == 0 && (warp.flowNew != pickId || warp.flowOld != warpOld)) { warp.RecordFlow(list); touched = true; }
            // The check: the measured motion for this pair too, and both textures copied for reading back.
            // (1.0.47: also whenever the game's HUD mask is in use, for its coverage -- it switches itself off when it covers too
            // much, which the structure comparison with DLSS's output can do with effects drawn after the game's frame)
            if ((s.warpGameCheck || gmo::HudNow() > 0) && c.warpSrc != 0 && cycleStart && !chkPending && Now() - chkT >= 2.0 && gFlow)
            {
                if (warp.flowNew != pickId || warp.flowOld != warpOld) { warp.RecordFlow(list); touched = true; }
                ID3D12Resource *srcR[2] = { gFlow, warp.flowOut[WarpGpu::Out()] };
                bool okRb = true;
                for (int q = 0; q < 2 && okRb; q++)
                {
                    const D3D12_RESOURCE_DESC dsc = srcR[q]->GetDesc();
                    if (!chkRb[q] || chkW[q] != (UINT)dsc.Width || chkH[q] != dsc.Height)
                    {
                        SafeRelease(chkRb[q]);
                        UINT64 total = 0;
                        g_device->GetCopyableFootprints(&dsc, 0, 1, 0, &chkFp[q], nullptr, nullptr, &total);
                        chkBytes[q] = total;
                        D3D12_HEAP_PROPERTIES rh = {}; rh.Type = D3D12_HEAP_TYPE_READBACK;
                        D3D12_RESOURCE_DESC bd = {};
                        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
                        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                        okRb = SUCCEEDED(g_device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                           nullptr, IID_PPV_ARGS(&chkRb[q])));
                        chkW[q] = (UINT)dsc.Width; chkH[q] = dsc.Height;
                    }
                }
                if (!okRb)
                {
                    static bool once = false;
                    if (!once) { once = true; LOGW("BFI Presenter: frame warp check: its readback buffers could not be made"); }
                    chkT = Now();
                }
                if (okRb)
                {
                    // both in PIXEL_SHADER_RESOURCE here (the game's moved there at the start of this list)
                    D3D12_RESOURCE_BARRIER cb[2] = { Transition(srcR[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                                     Transition(srcR[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE) };
                    list->ResourceBarrier(2, cb);
                    for (int q = 0; q < 2; q++)
                    {
                        D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
                        dl.pResource = chkRb[q]; dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dl.PlacedFootprint = chkFp[q];
                        sl.pResource = srcR[q]; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
                        list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
                    }
                    cb[0] = Transition(srcR[0], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    cb[1] = Transition(srcR[1], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    list->ResourceBarrier(2, cb);
                    chkPending = true; chkFence = reserve; chkFrames = pickId - warpOld; chkT = Now(); chkSrc = (int)c.warpSrc;
                    static int nRec = 0;
                    if (nRec++ < 3) LOGI("BFI Presenter: frame warp check: both motions copied for reading back (fence %llu)", (unsigned long long)reserve);
                }
            }
            warpTouched = touched;
            if (touched)
            {
                list->SetPipelineState(pso);
                list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
                list->SetGraphicsRootDescriptorTable(1, srv);
            }
        }
        if (tsOn) list->EndQuery(tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, 4 * bi + 1);
        if (c.warpSrc != 0)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE gs = srvHeap->GetGPUDescriptorHandleForHeapStart();
            gs.ptr += (UINT64)(kGameFlowSrv + (UINT)pick) * srvStride;
            list->SetGraphicsRootDescriptorTable(4, gs);                           // t3: the game's own motion for this frame
        }
        else list->SetGraphicsRootDescriptorTable(4, warp.ok ? warp.FlowSrv() : srv);   // t3 (read only while a frame is shown moved)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE psrv = srvHeap->GetGPUDescriptorHandleForHeapStart();
            psrv.ptr += (UINT64)(c.warp != 0.0f ? warpPrev : pick) * srvStride;
            list->SetGraphicsRootDescriptorTable(5, psrv);                         // t4: the frame shown before (likewise)
        }
        {
            // t5, t6: the frames from before ReShade's effects (read only with flags & 32768); otherwise the stage frames
            if (preGenSeen != g_preGen.load())
            {
                preGenSeen = g_preGen.load();
                D3D12_SHADER_RESOURCE_VIEW_DESC pv = {};
                pv.Format = StageFormat(); pv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                pv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; pv.Texture2D.MipLevels = 1;
                for (UINT k = 0; k < kStage; k++)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
                    h.ptr += (SIZE_T)(kPreSrv + k) * srvStride;
                    g_device->CreateShaderResourceView(g_pre[k] ? g_pre[k] : g_stage[k], &pv, h);
                }
            }
            const bool pre = (c.flags & 32768u) != 0;
            D3D12_GPU_DESCRIPTOR_HANDLE p5 = srvHeap->GetGPUDescriptorHandleForHeapStart(), p6 = p5;
            p5.ptr += (UINT64)((pre ? kPreSrv : 0) + (UINT)pick) * srvStride;
            p6.ptr += (UINT64)((pre ? kPreSrv : 0) + (UINT)(c.warp != 0.0f ? warpPrev : pick)) * srvStride;
            list->SetGraphicsRootDescriptorTable(6, p5);
            list->SetGraphicsRootDescriptorTable(7, p6);
        }
        // RenoFX APL Limiter: measure the frame before its first refresh, so the boost uses its own average (as in
        // RenoFX). Its later refreshes reuse the value.
        if (aplOn && pickId != aplFrameId)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE cellSrv = srvHeap->GetGPUDescriptorHandleForHeapStart();
            cellSrv.ptr += (UINT64)kStage * srvStride;
            D3D12_CPU_DESCRIPTOR_HANDLE artv = aplRtvHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_RESOURCE_BARRIER ab = Transition(aplCellRt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &ab);
            list->SetPipelineState(psoAplC);
            list->SetGraphicsRootDescriptorTable(2, aplSrv);
            D3D12_VIEWPORT avp = { 0.0f, 0.0f, 32.0f, 18.0f, 0.0f, 1.0f };
            D3D12_RECT asc = { 0, 0, 32, 18 };
            list->RSSetViewports(1, &avp);
            list->RSSetScissorRects(1, &asc);
            list->OMSetRenderTargets(1, &artv, FALSE, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
            D3D12_RESOURCE_BARRIER ab2[2] = {
                Transition(aplCellRt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                Transition(aplRt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET) };
            list->ResourceBarrier(2, ab2);
            list->SetPipelineState(psoAplR);
            list->SetGraphicsRootDescriptorTable(2, cellSrv);
            artv.ptr += rtvStride;
            D3D12_VIEWPORT avp1 = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
            D3D12_RECT asc1 = { 0, 0, 1, 1 };
            list->RSSetViewports(1, &avp1);
            list->RSSetScissorRects(1, &asc1);
            list->OMSetRenderTargets(1, &artv, FALSE, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
            ab = Transition(aplRt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &ab);
            list->SetPipelineState(pso);
            aplFrameId = pickId;
        }
        list->SetGraphicsRootDescriptorTable(2, aplSrv);
        {
            D3D12_GPU_DESCRIPTOR_HANDLE nsrv = srvHeap->GetGPUDescriptorHandleForHeapStart();
            nsrv.ptr += (UINT64)(pickNew2 >= 0 ? pickNew2 : pick) * srvStride;
            list->SetGraphicsRootDescriptorTable(3, nsrv);
        }
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)g_desc.Width, (float)g_desc.Height, 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, (LONG)g_desc.Width, (LONG)g_desc.Height };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (tsOn) list->EndQuery(tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, 4 * bi + 2);
        // CRT hand-off: the main shader's CRT build, with this slot's halation and glow and the shader's settings
        const bool crtDraw = (c.flags & 65536u) != 0 && psoCrt;
        if (crtDraw)
        {
            if (crtGenSeen != crt::SlotGen())
            {
                crtGenSeen = crt::SlotGen();
                for (UINT k = 0; k < kStage; k++)
                    for (int t = 0; t < 4; t++)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
                        h.ptr += (SIZE_T)(kCrtSrv + 4 * k + (UINT)t) * srvStride;
                        ID3D12Resource *r = crt::SlotTex(k, t);
                        D3D12_SHADER_RESOURCE_VIEW_DESC cv = {};
                        cv.Format = r ? r->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
                        cv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        cv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; cv.Texture2D.MipLevels = 1;
                        g_device->CreateShaderResourceView(r, &cv, h);   // null: compiled out of the shader (not read)
                    }
            }
            D3D12_RESOURCE_BARRIER cb3[5]; UINT ncb = 0;
            for (int t = 0; t < 4; t++)
                if (ID3D12Resource *r = crt::SlotTex((UINT)pick, t))
                    cb3[ncb++] = Transition(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (ncb) list->ResourceBarrier(ncb, cb3);
            float cp[kCrtSlots * 4];
            const D3D12_RESOURCE_DESC fd = g_stage[pick]->GetDesc();
            crt::ParamsFor(cp, (float)fd.Width, (float)fd.Height);
            const UINT at = (crtCbAt++ % kCrtCbN) * kCrtCbStride;
            memcpy(crtCbMap + at, cp, sizeof(cp));
            D3D12_GPU_DESCRIPTOR_HANDLE th = srvHeap->GetGPUDescriptorHandleForHeapStart();
            th.ptr += (UINT64)(kCrtSrv + 4 * (UINT)pick) * srvStride;
            list->SetGraphicsRootDescriptorTable(8, th);
            list->SetGraphicsRootConstantBufferView(9, crtCb->GetGPUVirtualAddress() + at);
            // The passes after the pattern: the pattern into crtRt[0], each pass from one texture into the other (grain's
            // first half into crtRt[2]), and the main shader reads the last (t11)
            int crtLast = -1;
            ID3D12Resource *baked = (c.warp == 0.0f && crt::BakeReady((UINT)pick)) ? crt::BakeOut((UINT)pick) : nullptr;
            if (baked)
            {
                // shown as rendered: the frame's own CRT picture, made with it on the game's queue
                if (bakeGenSeen != crt::BakeGen())
                {
                    bakeGenSeen = crt::BakeGen();
                    for (UINT k = 0; k < kStage; k++)
                        for (UINT v = 0; v < 2; v++)
                        {
                            ID3D12Resource *r = crt::BakeOut(k);
                            D3D12_SHADER_RESOURCE_VIEW_DESC bv = {};
                            bv.Format = r ? r->GetDesc().Format : crt::PassFormat(); bv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                            bv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; bv.Texture2D.MipLevels = 1;
                            D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
                            h.ptr += (SIZE_T)(kBakeSrv + 2 * k + v) * srvStride;
                            g_device->CreateShaderResourceView(r, &bv, h);
                        }
                }
                cb3[ncb++] = Transition(baked, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                list->ResourceBarrier(1, &cb3[ncb - 1]);
                D3D12_GPU_DESCRIPTOR_HANDLE ph = srvHeap->GetGPUDescriptorHandleForHeapStart();
                ph.ptr += (UINT64)(kBakeSrv + 2 * (UINT)pick) * srvStride;
                list->SetGraphicsRootDescriptorTable(10, ph);
                crtLast = 0;
            }
            else if (psoCrtPass[0])
            {
                const UINT fw = (UINT)fd.Width, fh = fd.Height;
                if (crtRt[0] && (crtRt[0]->GetDesc().Width != fw || crtRt[0]->GetDesc().Height != fh || crtRt[0]->GetDesc().Format != crt::PassFormat()))
                {
                    // resized: the textures remade once the presenter's work so far is done
                    EnterCriticalSection(&g_lock);
                    const UINT64 v = ++g_presFenceVal;
                    LeaveCriticalSection(&g_lock);
                    queue->Signal(g_presFence, v);
                    WaitFence(g_presFence, v, ev);
                    for (int k = 0; k < 3; k++) SafeRelease(crtRt[k]);
                    g_bytesCrtPres = 0;
                }
                if (!crtRtvHeap)
                {
                    D3D12_DESCRIPTOR_HEAP_DESC rh = {}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 3;
                    if (FAILED(g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&crtRtvHeap)))) crtRtvHeap = nullptr;
                }
                bool rok = crtRtvHeap != nullptr;
                bool made = false;
                for (int k = 0; k < 3 && rok; k++)
                {
                    if (crtRt[k]) continue;
                    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                    D3D12_RESOURCE_DESC rd = {};
                    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = fw; rd.Height = fh; rd.DepthOrArraySize = 1;
                    rd.MipLevels = 1; rd.Format = crt::PassFormat(); rd.SampleDesc.Count = 1;
                    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&crtRt[k]))))
                    { crtRt[k] = nullptr; rok = false; break; }
                    crtRt[k]->SetName(L"BFI CRT-Standalone hand-off (passes after the pattern)");
                    D3D12_CPU_DESCRIPTOR_HANDLE rh = crtRtvHeap->GetCPUDescriptorHandleForHeapStart();
                    rh.ptr += (SIZE_T)k * rtvStride;
                    g_device->CreateRenderTargetView(crtRt[k], nullptr, rh);
                    made = true;
                }
                if (rok && made)
                {
                    g_bytesCrtPres = ResBytes(crtRt[0]) + ResBytes(crtRt[1]) + ResBytes(crtRt[2]);
                    // views (0, 2) and (1, 2): each texture with grain's after it, as t11 t12
                    const int order[4] = { 0, 2, 1, 2 };
                    for (int v = 0; v < 4; v++)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
                        h.ptr += (SIZE_T)(kCrtPostSrv + (UINT)v) * srvStride;
                        g_device->CreateShaderResourceView(crtRt[order[v]], nullptr, h);
                    }
                }
                if (made) { crtKeyLast = -1; ringRtGen++; }
                Constants kc = c;                                    // what the pattern and the passes depend on
                kc.slot = 0; kc.method = 0; kc.soften = 0.0f; kc.phase = 0.0f; kc.gain = 0.0f; kc.dfloor = 0.0f;
                const int kPrev = c.warp != 0.0f ? warpPrev : -1;
                if (rok && crtKeyLast >= 0 && crtKeyId == pickId && crtKeyPrev == kPrev &&
                    memcmp(&crtKeyC, &kc, sizeof(kc)) == 0 && memcmp(crtKeyP, cp, sizeof(cp)) == 0)
                {
                    crtLast = crtKeyLast;                            // the same picture: drawn already
                    D3D12_GPU_DESCRIPTOR_HANDLE ph = srvHeap->GetGPUDescriptorHandleForHeapStart();
                    ph.ptr += (UINT64)(kCrtPostSrv + 2 * (UINT)crtLast) * srvStride;
                    list->SetGraphicsRootDescriptorTable(10, ph);
                }
                else if (rok)
                {
                    crtKeyC = kc; memcpy(crtKeyP, cp, sizeof(cp)); crtKeyId = pickId; crtKeyPrev = kPrev;
                    // A moved flash takes its frame's grain (made with the frame, dx/bfi_crt.h) instead of making it again
                    // (and so does any flash whose picture wasn't made with the frame: memory short, or not done yet)
                    const int ring = reserve ? crt::GrainFor((UINT)pick, reserve) : -1;
                    ID3D12Resource *ringTex = crt::GrainTex(ring);
                    if (ring >= 0 && (ringGenSeen != crt::RingGen() || ringRtSeen != ringRtGen))
                    {
                        ringGenSeen = crt::RingGen(); ringRtSeen = ringRtGen;
                        for (int g = 0; g < 2; g++)
                            for (int from = 0; from < 2; from++)
                                for (int v = 0; v < 2; v++)
                                {
                                    ID3D12Resource *r = v ? crt::GrainTex(g) : crtRt[from];
                                    D3D12_SHADER_RESOURCE_VIEW_DESC rv = {};
                                    rv.Format = r ? r->GetDesc().Format : crt::PassFormat(); rv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                                    rv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; rv.Texture2D.MipLevels = 1;
                                    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
                                    h.ptr += (SIZE_T)(kRingSrv + 4 * g + 2 * from + v) * srvStride;
                                    g_device->CreateShaderResourceView(r, &rv, h);
                                }
                    }
                    if (ring >= 0 && ringTex)
                    {
                        D3D12_RESOURCE_BARRIER gb = Transition(ringTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        list->ResourceBarrier(1, &gb);
                    }
                    D3D12_VIEWPORT pvp = { 0.0f, 0.0f, (float)fw, (float)fh, 0.0f, 1.0f };
                    D3D12_RECT psc = { 0, 0, (LONG)fw, (LONG)fh };
                    list->RSSetViewports(1, &pvp);
                    list->RSSetScissorRects(1, &psc);
                    auto into = [&](int t, ID3D12PipelineState *pp, int from)
                    {
                        D3D12_RESOURCE_BARRIER rb = Transition(crtRt[t], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        list->ResourceBarrier(1, &rb);
                        D3D12_GPU_DESCRIPTOR_HANDLE ph = srvHeap->GetGPUDescriptorHandleForHeapStart();
                        const UINT fv = (UINT)(from >= 0 ? from : 1);                                        // the pattern pass: any (not read)
                        ph.ptr += (UINT64)(ring >= 0 && ringTex ? kRingSrv + 4 * (UINT)ring + 2 * fv : kCrtPostSrv + 2 * fv) * srvStride;
                        list->SetGraphicsRootDescriptorTable(10, ph);
                        D3D12_CPU_DESCRIPTOR_HANDLE rh = crtRtvHeap->GetCPUDescriptorHandleForHeapStart();
                        rh.ptr += (SIZE_T)t * rtvStride;
                        list->OMSetRenderTargets(1, &rh, FALSE, nullptr);
                        list->SetPipelineState(pp);
                        list->DrawInstanced(3, 1, 0, 0);
                        rb = Transition(crtRt[t], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        list->ResourceBarrier(1, &rb);
                    };
                    into(0, psoCrtPass[0], -1);
                    crtLast = 0;
                    for (int k = 0; k < kCrtPostN; k++)
                    {
                        if (!psoCrtPass[1 + k] || !crt::PostOn(k, cp)) continue;   // compiled out, or at its off value
                        if (crt::PostIsGrainRaw(k))
                        {
                            if (!(ring >= 0 && ringTex)) into(2, psoCrtPass[1 + k], crtLast);   // else: the frame's own
                            continue;
                        }
                        into(1 - crtLast, psoCrtPass[1 + k], crtLast);
                        crtLast = 1 - crtLast;
                    }
                    if (ring >= 0 && ringTex)
                    {
                        D3D12_RESOURCE_BARRIER gb = Transition(ringTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                        list->ResourceBarrier(1, &gb);
                        crtGrainReused++;
                    }
                    else crtGrainMade++;
                    list->RSSetViewports(1, &vp);
                    list->RSSetScissorRects(1, &sc);
                    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                    D3D12_GPU_DESCRIPTOR_HANDLE ph = srvHeap->GetGPUDescriptorHandleForHeapStart();
                    ph.ptr += (UINT64)(kCrtPostSrv + 2 * (UINT)crtLast) * srvStride;
                    list->SetGraphicsRootDescriptorTable(10, ph);
                    crtKeyLast = crtLast;
                }
            }
            if (crtGrainReused + crtGrainMade && Now() - crtGrainLogT > 10.0)
            {
                if (crtGrainLogT > 0.0)
                    LOGI("BFI Presenter: CRT hand-off: last 10 s, CRT pictures drawn by the presenter (moved flashes, ...) with their frame's grain %u, that made their own %u",
                         crtGrainReused, crtGrainMade);
                crtGrainLogT = Now(); crtGrainReused = crtGrainMade = 0;
            }
            if (!psoCrtPass[0] || crtLast >= 0) list->SetPipelineState(psoCrt);   // else the textures could not be made: as without the hand-off
            list->DrawInstanced(3, 1, 0, 0);
            list->SetPipelineState(pso);
            for (UINT i = 0; i < ncb; i++) std::swap(cb3[i].Transition.StateBefore, cb3[i].Transition.StateAfter);
            if (ncb) list->ResourceBarrier(ncb, cb3);
        }
        else list->DrawInstanced(3, 1, 0, 0);
        if (tsOn)
        {
            list->EndQuery(tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, 4 * bi + 3);
            list->ResolveQueryData(tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, 4 * bi, 4, tsRb, (UINT64)(8 * 4 * bi));
            tsPending[bi] = true; tsPasses[bi] = warpTouched; tsWarped[bi] = c.warp != 0.0f;
        }
        if (((ablOn && c.method < 2) || aplOn) && !measFence && pickId != lastShownId)
        {
            Constants mc = c;
            mc.gain = s.gain;                                  // brightness at the configured boost
            mc.ceilLin = s.peakNits / 80.0f;                   // and the full ceiling
            D3D12_RESOURCE_BARRIER mb = Transition(measRt, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &mb);
            list->SetPipelineState(psoMeas);
            list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &mc, 0);
            D3D12_CPU_DESCRIPTOR_HANDLE mrtv = measHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_VIEWPORT mvp = { 0.0f, 0.0f, 32.0f, 36.0f, 0.0f, 1.0f };
            D3D12_RECT msc = { 0, 0, 32, 36 };
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
            dst.PlacedFootprint.Footprint.Height = 36;
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
        if (pickNew2 != pick)
        {
            D3D12_RESOURCE_BARRIER b2 = Transition(g_stage[pickNew2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &b2);
        }
        if (c.warp != 0.0f)
        {
            D3D12_RESOURCE_BARRIER b3 = Transition(g_stage[warpPrev], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &b3);
            if ((c.flags & 32768u) != 0)
            {
                D3D12_RESOURCE_BARRIER pb[2] = { Transition(g_pre[pick], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                                 Transition(g_pre[warpPrev], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON) };
                list->ResourceBarrier(pick == warpPrev ? 1 : 2, pb);
            }
            if (c.warpSrc != 0)
            {
                D3D12_RESOURCE_BARRIER b4 = Transition(gFlow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                list->ResourceBarrier(1, &b4);
            }
        }
        if (FAILED(list->Close())) LOGE("BFI Presenter: presenter draw list failed to record (Close) -- see BFIPresenter_debug.txt with Diagnostics=2");

        ID3D12CommandList *lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(g_presFence, reserve);
        bufFence[bi] = reserve;
        bufFrame[bi] = pickId;
        bufFrame2[bi] = pickId2;
        bufConst[bi] = c;
        bufValid[bi] = true;
        }

        if (g_holdPresents.load() > 0)
        {
            // the game is creating a new swap chain for this window: no Present on the old one meanwhile (at most 2 s)
            const double th = Now();
            while (g_holdPresents.load() > 0 && g_running.load() && Now() - th < 2.0) Sleep(1);
        }
        const double tSub = Now();
        if (cycleStart && cycRepeat && c.warp != 0.0f && s.warpGuard)
        {
            const bool late = tSub - tFence > 0.4 / (modeHz > 0.0 ? modeHz : 120.0);
            warpRepHist = (uint8_t)((warpRepHist << 1) | (late ? 1 : 0));
            if (warpRepHistN < 8) warpRepHistN++;
            int n = 0; for (int b = 0; b < 8; b++) n += (warpRepHist >> b) & 1;
            if (warpRepHistN >= 5 && n >= 3)
            {
                warpRepHoldT = tSub + warpRepHoldS;
                LOGI("BFI Presenter: frame warp guard: %d of the last %d filled repeats were drawn too late for their refresh (drawing one took "
                     "%.1f ms; the game's own late frame was being sent to the GPU at the same time) -- repeats are shown as they are for %.0f s",
                     n, warpRepHistN, (tSub - tFence) * 1000.0, warpRepHoldS);
                warpRepHoldS = std::min(300.0, warpRepHoldS * 2.0);
                warpRepHist = 0; warpRepHistN = 0;
            }
        }
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
        if (!tickedAtVb && !pairIter)
        {
            g_tickT.store(Now());
            g_tickCount.fetch_add(1);
            SetEvent(g_tickEvent);
        }
        // QueueDarkRefresh: after the lit refresh of a 2+ refresh cycle, the next one (the same game frame) follows at
        // once. Not with Queue one refresh ahead (its own queue), the LCD savers (they shift the cycle), the hitch
        // guard's steady picture, or fewer than 3 presenter buffers.
        if (pairIter)
        {
            // The second present should return at once. If it waits for the first one's vblank, the presenter's queue
            // takes only one present and pairing would shift the strobe: off for this session.
            // Judged over 120 steady pairs (2 s): a swap chain that holds one pending present blocks nearly every one,
            // while a stall at start or in loading blocks a few (Control, t1: off after 3 waits in its first second,
            // while the game was still loading).
            // Not judged while the game starts up (Soul Reaver, OpenGL, t7: off after 115 waits of 120 in its first
            // 3 s, around a hitch pause, then 0.09 ms presents for the rest of the session), and a hitch pause starts
            // the window again.
            {
                const double wHz = modeHz > 1.0 ? modeHz : 120.0, wDur = tPres - tSub;
                pairWaited = wDur > 0.5 / wHz && wDur - pairLitPresS > 0.25 / wHz;
            }
            if (hitchPaused) pairSeen = pairBlocked = pairBadWins = 0;
            else if (!pairOff && tPres - pairStartT >= 5.0)
            {
                pairSeen++;
                // Waited: the call took half a refresh, and a quarter of a refresh longer than the lit one's. Overlay
                // hooks (ShowOverlays=1) draw inside every Present, the lit one too (Cocoon, t3: 4.4 ms each, judged
                // as 120 waits of 120 and switched off), while a swap chain that holds one present makes only the
                // queued one wait until the lit refresh's vblank.
                const double prHz = modeHz > 1.0 ? modeHz : 120.0, prDur = tPres - tSub;
                if (prDur > 0.5 / prHz && prDur - pairLitPresS > 0.25 / prHz) pairBlocked++;
                if (pairSeen >= 120)
                {
                    pairBadWins = pairBlocked * 2 > pairSeen ? pairBadWins + 1 : 0;
                    if (pairBadWins >= 2)
                    {
                        pairOff = true;
                        // Waits can come and go (Spider-Man 2, t10: 0.1 ms for 5 minutes, then every queued present 8 ms, one
                        // refresh, with no frame limiter and no late presents): it tries again later, four times.
                        const int retryS = pairRetries < 4 ? 60 << pairRetries : 0;
                        pairRetryT = retryS > 0 ? tPres + retryS : 0.0;
                        char again[64];
                        if (retryS > 0) snprintf(again, sizeof(again), "off for %d min (tries again then)", retryS / 60);
                        else snprintf(again, sizeof(again), "off for this session");
                        // Overlay hooks (ShowOverlays=1) make the queued present wait for the lit one's overlay drawing
                        // (Cocoon, t6: off with ShowOverlays=1, 1 wait in 120 without).
                        const bool viaOverlays = g_showOverlaysBoot == 1;
                        // A frame limiter on the presenter's presents holds the queued one back (Spider-Man 2, t9: off at
                        // 18:40:33, then "held to 116.0 presents/s" -- NVIDIA's own cap under the refresh rate).
                        const int heldMask = g_presHeldMask.load();
                        int heldN = 0;
                        for (int b = 0; b < 4; b++) heldN += (heldMask >> b) & 1;
                        const bool viaLimiter = !viaOverlays && heldN >= 2;
                        g_darkState.store(viaOverlays ? 3 : viaLimiter ? 4 : 2);
                        if (viaLimiter)
                            LOGW("BFI Presenter: QueueDarkRefresh %s: the queued refresh's present waited in %d of %d "
                                 "pairs -- a frame limiter holds the presenter's presents (%.1f a second): RTSS's frame limit, Max Frame Rate "
                                 "in the NVIDIA control panel, or NVIDIA's cap under the refresh rate with Low Latency Mode Ultra or Reflex "
                                 "plus G-SYNC and VSync; with those off it can run", again, pairBlocked, pairSeen, g_presHeldRate.load());
                        else if (viaOverlays)
                            LOGW("BFI Presenter: QueueDarkRefresh %s: the queued refresh's present waited in %d of %d "
                                 "pairs -- the overlays drawn in each present (ShowOverlays=1) hold it up; ShowOverlays=0 lets it run",
                                 again, pairBlocked, pairSeen);
                        else
                            LOGW("BFI Presenter: QueueDarkRefresh %s: the queued refresh's present waited in %d of %d "
                                 "pairs (the swap chain took only one pending present)", again, pairBlocked, pairSeen);
                    }
                    else if (pairBlocked > 0 && ++pairLogs <= 20)
                        LOGI("BFI Presenter: QueueDarkRefresh: the queued refresh's present waited in %d of the last %d pairs", pairBlocked, pairSeen);
                    pairSeen = pairBlocked = 0;
                }
            }
        }
        else if (s.queueDark && pairOff && pairRetryT > 0.0 && tPres >= pairRetryT)
        {
            pairOff = false;
            pairRetries++;
            pairSeen = pairBlocked = pairBadWins = 0;
            pairStartT = 0.0;                // judged again from 5 s after it starts
            g_darkState.store(0);
            LOGI("BFI Presenter: QueueDarkRefresh: trying again (try %d of 4)", pairRetries);
        }
        else if (s.queueDark && !pairOff && tickedAtVb && s.pacing && !s.queueAhead && !vsyncOverridden && n >= 3 && s.enabled &&
                 s.frames >= 2 && slotNow == 0 && !hitchPaused && swapState == 0 && soften <= 0.0f && !GradualSaverOn(s) && SUCCEEDED(hr))
        {
            pairNext = true;
            pairVbT = tVb;
            pairLitPresS = tPres - tSub;
            if (pairStartT <= 0.0) pairStartT = tPres;
            g_darkState.store(1);
            static bool pairLogged = false;
            if (!pairLogged)
            {
                pairLogged = true;
                LOGI("BFI Presenter: QueueDarkRefresh on: the refresh after the lit one is queued with it");
            }
        }
        // Why the dark queue isn't used, once per session (Cocoon, t3: PresenterBuffers=2 left in its ini, nothing in the log).
        if (s.queueDark && s.enabled && s.frames >= 2 && !pairOff && !pairIter)
        {
            static bool pairWhyLogged = false;
            const char *why = !s.pacing ? "Wait for vblank is off" : s.queueAhead ? "Queue one refresh ahead is on" :
                              n < 3 ? "fewer than 3 presenter buffers (PresenterBuffers)" : GradualSaverOn(s) ? "an LCD saver is on" :
                              vsyncOverridden ? "the driver is not honouring VSync" : nullptr;
            if (why && !pairWhyLogged && presents > 600)
            {
                pairWhyLogged = true;
                LOGI("BFI Presenter: QueueDarkRefresh not used: %s", why);
            }
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
        if (pickNew && modeHz > 1.0)   // clock drift check: the new frames' arrival phase over one refresh
        {
            const double ang = 6.283185307179586 * (t - pickTime) * modeHz;
            secPhC += cos(ang); secPhS += sin(ang); secPhN++;
        }
        if (t - lastT > 1.5 / (modeHz > 0.0 ? modeHz : 120.0) && presents > 1)   // a missed refresh at the display's own rate
        {
            secLate++;
            lateTimes[lateIdx++ & 3] = t;
        }
        // A queued refresh (QueueDarkRefresh) is late only when it wasn't presented before the vblank it was meant for,
        // two after the one its lit refresh was drawn at.
        else if (pairIter && pairVbT > 0.0 && t > pairVbT + 1.9 / (modeHz > 0.0 ? modeHz : 120.0))
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
                g_fgFactNoRfx.store(fgFact);
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
#ifdef _WIN64
            slp::Tick(t);
            ngx::Tick(t);
#endif
            DXGI_FRAME_STATISTICS st = {};
            const double tSt0 = Now();
            const bool sok = SUCCEEDED(ScStats(&st));
            secStatsMs = std::max(secStatsMs, (Now() - tSt0) * 1000.0);
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
                         "VRR (G-SYNC) or Windows' dynamic refresh rate is active. Turn G-SYNC off for this game (NVIDIA profile) and Dynamic "
                         "refresh rate off in Windows' display settings, or turn on Queue one refresh ahead.",
                         hz, peakHz);
                }
                displayHz = hz;
                // A frame limiter hooked into the presenter's own swap chain
                // (RTSS at 118 in God of War): the display keeps its fixed
                // refresh, but the presenter only gets a little under that many
                // presents through, so the strobe slips a refresh every half
                // second or so. VRR would pull the display rate down with it.
                const double prate = secPresents / (t - secStart);
                const bool heldSec = prate < 0.985 * hz && prate > 0.5 * hz && secLate < 3 && hz > 0.97 * peakHz;
                limitSecs = heldSec ? limitSecs + 1 : 0;
                g_presHeldMask.store(((g_presHeldMask.load() << 1) | (heldSec ? 1 : 0)) & 0xFF);
                if (heldSec) g_presHeldRate.store(prate);
                if (limitSecs == 3 && !limitLogged)
                {
                    limitLogged = true;
                    LOGW("BFI Presenter: the presenter is held to %.1f presents/s on a %.1f Hz display -- a frame limiter "
                         "is acting on it: RTSS's frame limit, Max Frame Rate in the NVIDIA control panel, or NVIDIA's own cap a little under "
                         "the refresh rate (e.g. 116 at 120 Hz) that Low Latency Mode Ultra or Reflex sets with G-SYNC and VSync on -- set "
                         "those to off / 0 for this game. The strobe slips a refresh about %.0f times a second, which shows as flicker.",
                         prate, hz, hz - prate);
                }
            }
            if (sok && haveStats) rep = (int)(st.SyncRefreshCount - lastSync) - (int)(st.PresentCount - lastPC);
            if (sok) { lastSync = st.SyncRefreshCount; lastPC = st.PresentCount; haveStats = true; }
            EnterCriticalSection(&g_cfgLock);
            g_status.refreshHz  = secPresents / (t - secStart);
            const int stagedSec = g_stagedThisSecond.exchange(0);
            g_status.gameFps    = stagedSec / (t - secStart);
            g_status.frameAgeMs = secPresents ? ageSum / secPresents : 0.0;
            {
                const double phase = secPhN ? atan2(secPhS, secPhC) : NAN;
                secPhC = secPhS = 0.0; secPhN = 0;
                if (drift.Second(stagedSec, secPresents, phase, modeHz, g_status.fgActive != 0))
                {
                    if (drift.on)
                        LOGW("BFI Presenter: the game runs at %.2f fps on a clock of its own, the display at %.2f Hz -- its frames drift "
                             "against the strobe cycle (a game frame slips about every %.0f s: stutter, and flashes when it lines up "
                             "with the flip). Set the display to %.2f Hz for this game.", drift.gameFps, modeHz, drift.slipS, drift.suggestHz);
                    else LOGI("BFI Presenter: the game's frames match the display rate again");
                }
                g_status.driftOn = drift.on ? 1 : 0;
                g_status.driftFps = drift.gameFps; g_status.driftHz = modeHz;
                g_status.driftSuggest = drift.suggestHz; g_status.driftSlip = drift.slipS;
            }
            g_status.late       = secLate;
            g_status.repeats    = rep > 0 ? rep : 0;
            g_status.frameRepeats = secFrameRepeats;
            g_status.warpOn       = !s.frameWarp ? 0 : !warp.ok ? -1 : warpClock.engaged ? 2 : 1;
            g_status.warpCycles   = secWarpCycles;
            // CRT hand-off: when most cycles are shown moved (e.g. with frame generation), the picture made with each frame
            // is rarely shown -- not made then (it cost the game's GPU time for nothing); moved flashes draw their own
            crt::SetBakeSkip(secCycles > 0 && secWarpCycles * 10 > secCycles * 6);
            secCycles = 0;
            g_status.warpAvg      = secWarpCycles ? secWarpSum / secWarpCycles : 0.0;
            g_status.warpRate     = warpClock.rate;
            g_status.warpSrc      = secWarpGame[1] ? 1 : secWarpGame[2] ? 2 : 0;
            g_status.warpGameState = gmo::StateNow(g_status.warpGameWhy, sizeof(g_status.warpGameWhy));
            g_status.warpGameVia = gmo::ViaNow();
            g_status.warpHud = gmo::HudNow();
            {
                // the log says which motion frame warp uses, when it changes (at most every 5 s)
                static int srcWas = -1;
                static double srcLogT = -10.0;
                const int src = secWarpCycles ? g_status.warpSrc : -1;
                if (src >= 0 && src != srcWas && t - srcLogT > 5.0)
                {
                    LOGI("BFI Presenter: frame warp moves the picture with %s (%d of %d cycles in the last second)",
                         src == 1 ? "the game's own motion vectors and depth" : src == 2 ? "the game's own motion vectors (no depth)"
                                  : "the motion measured from the picture", src ? secWarpGame[src] : secWarpGame[0], secWarpCycles);
                    srcWas = src; srcLogT = t;
                }
            }
            secWarpGame[0] = secWarpGame[1] = secWarpGame[2] = 0;
            {
                // the log says when frame warp starts and stops working (at most every 5 s)
                static int warpWas = 0;
                static double warpLogT = 0.0;
                if (g_status.warpOn != warpWas && (g_status.warpOn == 2 || warpWas == 2) && t - warpLogT > 5.0)
                {
                    if (g_status.warpOn == 2)
                        LOGI("BFI Presenter: frame warp at work (fill missing frames): the game makes %.0f%% of the strobe rate",
                             warpClock.rate * 100.0);
                    else LOGI("BFI Presenter: frame warp waiting (%s)", !s.frameWarp ? "switched off"
                              : warpClock.rate > 0.0 ? "the game's frame rate is outside its range" : "no steady frame rate");
                    warpLogT = t;
                    warpWas = g_status.warpOn;
                }
                else if (g_status.warpOn != 2 && warpWas != 2) warpWas = g_status.warpOn;
            }
            winWarpPaused += secWarpPaused; winCuts += gmo::CutsTake(); winWarpHeld += secWarpHeld;
            if (t - winPauseT >= 10.0)
            {
                static int pauseLogs = 0;
                if ((winWarpPaused || winCuts || winWarpHeld) && pauseLogs++ < 60)
                    LOGI("BFI Presenter: frame warp, last %.0f s: %d camera cuts flagged by the game, %d strobe cycles not filled (camera cuts, "
                         "letterboxed cutscenes), %d repeats shown as they are (the warp guard)", t - winPauseT, winCuts, winWarpPaused, winWarpHeld);
                winWarpPaused = winCuts = winWarpHeld = 0; winPauseT = t;
            }
            secWarpPaused = 0; secWarpHeld = 0;
            secWarpCycles = 0; secWarpSum = 0.0;
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
            g_status.aplNow       = aplOn ? aplNow : -1.0;
            g_status.aplCut       = aplOn ? lastAplCut : 0.0;
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
                const double tVr0 = Now();
                const bool have = VramMB(vu, vbud) && vbud > 0.0;
                secVramMs = (Now() - tVr0) * 1000.0;
                const bool over = have && vu > vbud;
                overWin = ((overWin << 1) | (over ? 1 : 0)) & 31;
                int nOver = 0; for (int b = 0; b < 5; b++) nOver += (overWin >> b) & 1;
                underSecs = over ? 0 : underSecs + 1;
                bool on = g_status.vramOver;
                if (!on && nOver >= 3)
                {
                    on = true;
                    const double crtNow = (double)(g_bytesCrtGame.load() + g_bytesCrtPres.load()) / 1048576.0;
                    LOGW("BFI Presenter: the game uses more video memory than Windows gives it (%.0f of %.0f MB; BFI Presenter's "
                         "share %.0f MB, of it the CRT hand-off %.0f MB). Part of it is moved to system memory and the GPU stalls "
                         "now and then: missed refreshes, frame generation drops frames, BFI pauses. Lower the game's texture or "
                         "memory settings%s.", vu, vbud, (double)g_bytesFrames.load() / 1048576.0 + crtNow, crtNow,
                         crtNow >= 64.0 ? ", or turn the CRT hand-off off for this game" : "");
                }
                else if (on && underSecs >= 10)
                {
                    on = false;
                    LOGI("BFI Presenter: video memory back under the budget (%.0f of %.0f MB)", vu, vbud);
                }
                g_status.vramOver = on; g_status.vramUse = vu; g_status.vramBudget = vbud;
                g_status.crtMB = (double)(g_bytesCrtGame.load() + g_bytesCrtPres.load()) / 1048576.0;
                {
                    // the CRT hand-off's per-frame picture only with video memory to spare (1 GB under the budget; again
                    // once 1.5 GB is free, counting what it would take back)
                    // Each switch stalls the game's Present (freeing waits for the GPU; remaking them took 8 ms in Oblivion), and
                    // a game whose memory follows what is free flips it over and over (Oblivion: every 10 s, 1.0.50-t2). So: back
                    // only after 60 s with room to spare, and after it was needed a second time it stays freed for the session.
                    static bool tight = false;
                    static int backs = 0;
                    static ULONGLONG tightAt = 0;
                    const ULONGLONG nowMs = GetTickCount64();
                    // nothing in the first 30 s after the presenter starts: loading makes the game's memory jump (1.0.50-t3)
                    static ULONGLONG startAt = 0, seenAt = 0;
                    static bool learnedTight = false, learnedChecked = false;
                    if (!seenAt || nowMs - seenAt > 3000) { startAt = nowMs; learnedChecked = false; }   // not run for a while: the presenter (re)started
                    seenAt = nowMs;
                    const bool inUse = crt::StateNow() == 2;
                    // Short of it in an earlier session (LearnPerGame): starts that way, without the 30 s wait and the
                    // switches; back once 1.5 GB is free for 60 s, and then forgotten for the game.
                    if (!learnedChecked && s.learnPerGame && learnGen != 0 && inUse)
                    {
                        learnedChecked = true;
                        learnedTight = learnRead.crtShort != 0;
                        if (learnedTight && !tight)
                        {
                            tight = true; tightAt = nowMs; backs = 0;
                            LOGI("BFI Presenter: CRT hand-off: the game was short of video memory for it in an earlier session -- the CRT "
                                 "picture is drawn by each refresh from the start (the grain still made with the frame)");
                        }
                    }
                    if (vbud > 0.0 && (nowMs - startAt >= 30000 || (learnedTight && tight)))
                    {
                        if (!tight && vu > vbud - 1024.0 && nowMs - tightAt >= 10000)
                        {
                            tight = true; tightAt = nowMs;
                            if (backs >= 1)
                            {
                                LOGI("BFI Presenter: CRT hand-off: video memory was short again -- the CRT picture stays drawn by each refresh for this session");
                                if (s.learnPerGame && inUse && !learnedTight) { g_learnCrtShort = 1; learnedTight = true; }
                            }
                        }
                        else if (tight && backs < 1 && vu < vbud - 1536.0 && nowMs - tightAt >= 60000)
                        {
                            tight = false; tightAt = nowMs; backs++;
                            if (learnedTight && s.learnPerGame)
                            {
                                g_learnCrtShort = 0; learnedTight = false;
                                LOGI("BFI Presenter: CRT hand-off: video memory to spare for it now -- no longer remembered as short for this game");
                            }
                        }
                    }
                    crt::SetMemoryTight(tight);
                }
                g_status.ownMB = (double)g_bytesFrames.load() / 1048576.0 + g_status.crtMB;
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
            Osd::AddSecond(g_status.frameSkips, g_status.frameRepeats, g_status.late, Now());
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
                    // Reflex reports still coming (within 3 s) but no rendered count for now: a stall, so the state is
                    // kept (Alan Wake 2 with 2x read as "not active" right after each hitch, and the cap went to Refresh).
                    // Frame generation already on: kept for up to 8 s without reports, so a stall of a few seconds doesn't
                    // switch the limit to 1x and back (Alan Wake 2, 3x: "not active (60 presented, 20 rendered)" right after
                    // a 2 s hitch, back 5 s later).
                    if (rsEverT > 0.0 && (!rfxStopped || (fgWas && t - rsEverT < 8.0))) fg = fgWas;
                    else fg = dlssOnPath && (!g_fgCallerDlss.load() || (rfxStopped && (fgWas || fgRate)));
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
                // Also a second without the game's Reflex reports: a game collapsed under the limit keeps reporting (Returnal
                // at 7 fps), a load screen doesn't (Control, 3x frame generation: 20 presented, 0 reports, 2 s after a hitch
                // -- read as a collapse, and with frame generation the cap then stayed at Refresh rate for the session).
                const bool stallSec = fpsA < 3.0 || hitchPaused || t - rsLastT >= 1.5 || rsLast.frames <= 0;
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
                // Overshoot (as the Vulkan layer, 1.0.47): the limit has to hold the game too. A game whose Reflex sleep
                // doesn't pace it ran well above it (Vulkan: 54-74 fps under a 60 fps limit), and with the limit on the
                // presenter takes every frame. 4 of the last 6 s more than 8% above one frame per strobe cycle switch
                // it off for the session; the presenter's own cap holds the game at Present.
                {
                    static unsigned fastBits = 0;
                    const double capFps = hzA > 0.0 ? hzA / s.frames : 0.0;
                    // Never with frame generation: the presenter's own cap doesn't hold frame generation, so switching the limit
                    // off would leave the game without a cap (Townfall with frame generation, ep7).
                    const bool on = autoCapDown && capByReflex && !rfxLimitBad && !fgWas && !fgChanging;
                    const bool fast = on && !stallSec && t - rfxChkT >= 2.0 && capFps > 0.0 && fpsA > 1.08 * capFps;
                    fastBits = on ? (((fastBits << 1) | (fast ? 1u : 0u)) & 63u) : 0u;
                    if (__builtin_popcount(fastBits) >= 4)
                    {
                        fastBits = 0;
                        g_rfxLimitBad.store(true); capByReflex = false;
                        LOGW("BFI Presenter: Reflex frame limit: the game ran %.0f fps under a %.0f fps limit (its Reflex sleep doesn't "
                             "pace it) -- no Reflex frame limit for the rest of this session; the automatic frame cap uses the "
                             "presenter's own cap", fpsA, capFps);
                    }
                }
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
                // Reflex in use: reports within the last 3 s, not only in the last second -- a one-second stall with no
                // reports took the cap to Refresh rate with frame generation for ~7 s (Alan Wake 2, 2x: "frame generation
                // without Reflex").
                // (8 s with frame generation on, as its state is kept that long: Alan Wake 2 paused its reports for 3-4 s
                // after some hitches, and with frame generation still on the cap went to Refresh rate for 5 s.)
                const bool rfxUse = rsEverT > 0.0 && t - rsEverT < (fgWas ? 8.0 : 3.0) && !g_rfxLimitBad.load();
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
                // Frame generation under Reflex's limit: skip the 5-minute try at Refresh rate when the game's rendered
                // frames can't reach it anyway -- the frame-generation factor times what the GPU / CPU time per rendered
                // frame allows, against the refresh rate. Each try was 4 s of uneven frames (Alan Wake 2, 3x: GPU 33 ms
                // per rendered frame, at most ~90 of 120).
                // Reflex's GPU time leaves out frame generation's own GPU work, so the estimate is optimistic (Alan Wake 2,
                // 3x: 27 ms read as ~111 fps possible, the try held 67): it has to say "can fill" for 5 s in a row, and
                // after a failed try, the next one waits until the GPU time per rendered frame is 20% below that try's.
                auto FgCantFill = [&]() -> bool {
                    if (t - rsLastT >= 3.0 || rsLast.frames <= 0 || rsLast.gpuActiveMs <= 0.0) { fgFillSecs = 0; return false; }
                    const double rend = g_rfxRenderedFps.load();
                    if (rend <= 1.0 || fpsA <= 0.0) return false;
                    const double factor = std::max(1.0, std::round(fpsA / rend));
                    double needMs = rsLast.gpuActiveMs;
                    if (rsLast.simMs > needMs) needMs = rsLast.simMs;
                    if (rsLast.submitMs > needMs) needMs = rsLast.submitMs;
                    const double canFps = factor * 1000.0 / needMs;
                    fgNeedNow = needMs;
                    const bool failedHere = fgFailNeedMs > 0.0 && needMs > 0.8 * fgFailNeedMs;
                    fgFillSecs = (canFps >= 0.9 * hzA && !failedHere) ? fgFillSecs + 1 : 0;
                    if (fgFillSecs >= 5) return false;
                    if (!fgSkipLogged)
                    {
                        fgSkipLogged = true;
                        if (failedHere)
                            LOGI("BFI Presenter: automatic frame cap: no try at Refresh rate with frame generation -- %.1f ms per rendered "
                                 "frame, not well below the %.1f ms of the last try that failed", needMs, fgFailNeedMs);
                        else
                            LOGI("BFI Presenter: automatic frame cap: no try at Refresh rate with frame generation -- %.0fx at %.1f ms "
                                 "per rendered frame allows about %.0f of %.0f fps", factor, needMs, canFps, hzA);
                    }
                    return true;
                };
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
                                // Its own limit (see above) -- but a game locked to the strobe rate all along (ICO at 60) pays for
                                // each try with 4 s of frames off the strobe cycle: after two such tries in a row the next waits
                                // 5 minutes. Not saved: a new session tries every 30 s again.
                                const bool exact = fabs(fpsA - hzA / s.frames) < 0.02 * hzA / s.frames;
                                exactProbes = exact ? exactProbes + 1 : 0;
                                if (exact) probeWait = exactProbes >= 2 ? 300.0 : 30.0;
                                else if (fpsA < 0.8 * hzA) probeWait = 300.0;
                                if (exactProbes == 2)
                                    LOGI("BFI Presenter: automatic frame cap: the game stays at exactly %.0f fps (a limit of its own) -- "
                                         "tries at Refresh rate every 5 minutes from now; set the cap to One game frame per strobe "
                                         "cycle to stop them", hzA / s.frames);
                                if (fg && fgTryNeedMs > 0.0) { fgFailNeedMs = fgTryNeedMs; fgSkipLogged = false; }
                                LOGI("BFI Presenter: automatic frame cap: still one game frame per strobe cycle (%.0f fps of %.0f "
                                     "at Refresh); next try in %.0f s", fpsA, hzA, probeWait);
                            }
                        }
                    }
                    else if (!autoCapDown)
                    {
                        // Not while frame generation may be starting, nor in the first 10 s after the presenter starts: the
                        // game is still warming up (Castlevania: 60 fps for 3 s while its GPU caught up, then a minute at 60).
                        capDownSecs = (!hitchPaused && fgVotes == 0 && t - runStartT > 10.0 && fpsA > 0.3 * hzA / s.frames && fpsA < 0.95 * hzA)
                                      ? capDownSecs + 1 : 0;
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
                            // Frame generation: a try that held the refresh rate only for a few seconds (a load screen or a
                            // light corner) counts as failed too, so the next one waits for clearly lighter frames (Control,
                            // 3x: up at 02:10:59, down 9 s later at 65 fps).
                            if (fg && t - capUpT < 30.0 && fgTryNeedMs > 0.0) { fgFailNeedMs = fgTryNeedMs; fgSkipLogged = false; }
                            // A step up by Reflex reports that didn't hold: the next one needs clearly lighter frames than
                            // these. One that held 30 s or more clears it.
                            if (rfxUpNeedMs > 0.0)
                            {
                                if (t - capUpT < 30.0)
                                {
                                    rfxFailNeedMs = rfxFailNeedMs > 0.0 && rfxFailNeedMs < rfxUpNeedMs ? rfxFailNeedMs : rfxUpNeedMs;
                                    LOGI("BFI Presenter: automatic frame cap: the step up on Reflex's reports (%.1f ms per frame) didn't "
                                         "hold -- the next one needs frames under %.1f ms", rfxUpNeedMs, 0.8 * rfxFailNeedMs);
                                }
                                else rfxFailNeedMs = 0.0;
                                rfxUpNeedMs = 0.0;
                            }
                            // Well under the refresh rate: first try after a minute -- unless the game holds exactly the strobe rate,
                            // which is a limit of its own (an intro, a menu), not a struggle: tries every 30 s cost nothing there.
                            const bool atStrobe = fabs(fpsA - hzA / s.frames) < 0.02 * hzA / s.frames;
                            if (atStrobe) probeWait = exactProbes >= 2 ? 300.0 : 30.0;
                            else if (fpsA < 0.8 * hzA && probeWait < 60.0) probeWait = 60.0;
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
                        capUpSecs = (!hitchPaused && needMs < 0.85 * refMs &&
                                     (rfxFailNeedMs <= 0.0 || needMs < 0.8 * rfxFailNeedMs)) ? capUpSecs + 1 : 0;
                        if (capUpSecs >= 10 && t - capSwitchT >= stayMin)
                        {
                            autoCapDown = false; capDownSecs = capUpSecs = 0; capSwitchT = t; capUpT = t;
                            rfxUpNeedMs = needMs;
                            LOGI("BFI Presenter: automatic frame cap: Refresh rate -- game frames take %.1f ms (GPU %.1f, "
                                 "simulation %.1f, submit %.1f), within a refresh", needMs, rsLast.gpuActiveMs, rsLast.simMs, rsLast.submitMs);
                        }
                    }
                    // With frame generation, a try means 3 s without the limit, and DLSS-G re-pacing twice:
                    // a visible stutter each time (God of War Ragnarok, every 30-120 s). Frame generation that
                    // could fill the refresh rate is rare, so try only every 5 minutes then.
                    else if (t - capSwitchT >= (fg && capByReflex ? 300.0 : probeWait) && !hitchPaused &&
                             !(fg && capByReflex && FgCantFill()))
                    {
                        // No Reflex reports (or frame generation, whose reports are for rendered frames):
                        // try Refresh for a few seconds.
                        autoCapDown = false; capProbe = true; probeSecs = probeGood = 0; capSwitchT = t; capUpT = t;
                        rfxUpNeedMs = 0.0;
                        fgTryNeedMs = fg ? fgNeedNow : 0.0; fgFillSecs = 0;
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
                // ReflexAlign steers only without frame generation (and with Lowest latency): with frame generation the nudge is
                // still needed, or frames that land on the pick stay there now that the limit keeps its place (Control, 3x:
                // 42-57 of 60 shown for a minute, frames ready 0.3 ms before the pick).
                const bool alignSteers = s.reflexAlign && !g_alignHunted.load() && g_sleepFgFactor.load() == 1 && !stNow.fgActive && s.pacingMode == 1;
                if (!alignSteers && limitUs > 0 && rsLast.limitUs > 0 && !hitchPaused && stNow.frameRepeats >= 5 && stNow.frameSkips >= 5 && t >= phaseNextT)
                {
                    phaseUntil = t + 1.0; phaseNextT = t + 4.0;
                    if (++phaseNudges <= 5)
                        LOGI("BFI Presenter: Reflex frame limit: game frames arrive at the lit refresh's pick (%d repeated, %d skipped "
                             "last second) -- moving them earlier", stNow.frameRepeats, stNow.frameSkips);
                }
                if (limitUs > 0 && t < phaseUntil) limitUs -= 150;
                // At the game's Reflex sleep the driver gets no limit, so the 0.15 ms above did nothing there: move the
                // sleep-point limit's start 2 ms earlier instead, once per nudge (not while its start times are searched).
                {
                    static double nudgedT = -1.0;
                    if (t < phaseUntil && phaseUntil != nudgedT && g_sleepLimitUs.load() > 0 && !swSearching &&
                        !(g_fgXess.load() && g_sleepFgFactor.load() > 1))   // XeSS frame generation: as 1.0.38
                    { nudgedT = phaseUntil; g_sleepShiftUs.fetch_sub(2000); }
                }
                // ReflexAlign: the limiter paces the game on its own clock, so a game frame is ready
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
                    const bool fgPace = g_fgPaceUs.load() > 0;
                    // (1.0.39 tests also searched the start time of frame generation with Reflex at the game's sleep; it
                    // hunted and never beat the fixed place in the strobe cycle -- Silent Hill: Townfall, Dead Space.)
                    const bool fgOn = fgPace;
                    std::atomic<int> &shiftUs = fgPace ? g_fgShiftUs : g_sleepShiftUs;
                    const int sleepUs = fgPace ? g_fgPaceUs.load() : g_sleepLimitUs.load();
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
                    static int swIvN = 0, swIvLate = 0;     // the search: new frames shown, and those over a third of a cycle late
                    static unsigned swTightHist = 0;     // the last 5 seconds: frames repeated with little to spare (one bit each)
                    static double swBackMs = 0.0;        // moved earlier since the start time was found (more to spare)
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
                    const char *swName = g_fgPaceKind.load() == 2 ? "automatic frame cap at the frame start" : "frame limit for frame generation";
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
                        const int ivNSec = g_gameIvN.exchange(0), ivLateSec = g_gameIvLate.exchange(0);
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
                            swTightHist = 0; swBackMs = 0.0;
                            swIvN = swIvLate = 0;
                            swStart = base;
                            moveTo(swStart);
                            LOGI("BFI Presenter: %s: trying %d start times across the strobe cycle (%.0f s)",
                                 swName,
                                 swN, swN * (settle + 1.0));
                        }
                        else if (swIdx >= 0)
                        {
                            if (!ok) { swSecs = 0; swShown = swRelN = swRep = 0; swRel = 0.0; swSlack = 1.0; }
                            else if (++swSecs >= settle)
                            {
                                swIvN += ivNSec; swIvLate += ivLateSec;
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
                                        // Uneven frame times (ep10): over 2% of the game's frames came more than a third of a cycle
                                        // late during the search -- the quickest start with 3-5 ms to spare missed one cycle in 10
                                        // (South of Midnight: +14.6 ms with 4.6 ms to spare, 296 repeats in 52 s). There, 6 ms to
                                        // spare first, then 3 ms, then any start time that showed every frame.
                                        const int ivN = swIvN, ivLate = swIvLate;
                                        const bool uneven = !fsr && ivN >= 200 && ivLate * 50 > ivN;
                                        if (uneven)
                                            for (int k = 0; k < swN; k++)
                                                if (swScore[k] < 1e3)
                                                    swScore[k] = swSlackK[k] >= 0.006 ? swRelMs[k] : swSlackK[k] >= 0.003 ? 50.0 + swRelMs[k] : 100.0 + swRelMs[k];
                                        int best = 0;
                                        for (int k = 1; k < swN; k++) if (swScore[k] < swScore[best]) best = k;
                                        if (uneven)
                                            LOGI("BFI Presenter: %s: the game's frame times are uneven (%d of %d frames over a third of a cycle late) -- "
                                                 "a start time with more to spare is preferred", swName, ivLate, ivN);
                                        moveTo(start + best * cyc / swN);
                                        swIdx = -1; swDoneT = t; swBestMs = swRelMs[best];
                                        learnSw();
                                        char list[400]; int len = 0;
                                        for (int k = 0; k < swN && len < (int)sizeof(list) - 32; k++)
                                            len += snprintf(list + len, sizeof(list) - len, " %.1f:%d/%.0f/%.1f", k * cyc / swN * 1000.0,
                                                            swShownK[k], swRelMs[k], swSlackK[k] < 1.0 ? swSlackK[k] * 1000.0 : -1.0);
                                        LOGI("BFI Presenter: %s%s lined up with the flash: start %+.1f ms "
                                             "in the cycle, %.1f ms from release to flash, %d of %.0f frames shown (start ms:shown/ms/ready ms before the flash --%s)",
                                             swName,
                                             fsr ? " (FSR)" : "", best * cyc / swN * 1000.0, swRelMs[best], swShownK[best], 2.0 * capFps, list);
                                    }
                                }
                            }
                        }
                        // Held: tried again after 10 s (FSR 5 s) in a row with under 90% of the frames shown, or 6 ms or more
                        // slower than the start time found.
                        else
                        {
                            swBad = (ok && (relN < (int)(0.9 * capFps) || (relN > 0 && relSum / relN > swBestMs + swBackMs + 6.0))) ? swBad + 1 : 0;   // the steps back below
                            // don't count as slower
                            // Held, frames repeating with little to spare before their flash (the GPU work grew): 2 ms
                            // earlier when 3 of the last 5 seconds had 2 or more repeats with under 6 ms to spare (the 10th
                            // percentile: the few frames that miss are below it), at most 8 ms earlier than the start time
                            // found. Stray: the search took the quickest start time (4.4 ms to spare), then 2-7 frames a
                            // second repeated at 0.5-3.5 ms to spare -- never under the 1.5 ms (3 s in a row) this needed
                            // before, so it stayed there; 2 ms earlier had 8.5 ms. Kena (gm14, under 3 ms): 1-5 repeats a
                            // second at 2.5-5 ms to spare, never moved.
                            // Only where the game holds the cap, so a scene too heavy for it moves nothing.
                            const bool tight = !fsr && ok && stNow.frameRepeats >= 2 && p10 >= 0.0 && p10 < 0.006;
                            swTightHist = ((swTightHist << 1) | (tight ? 1u : 0u)) & 0x1Fu;
                            int nTight = 0; for (int k = 0; k < 5; k++) nTight += (swTightHist >> k) & 1u;
                            if (nTight >= 3 && swBackMs < 7.9)
                            {
                                swTightHist = 0;
                                swBackMs += 2.0;
                                moveTo(swPhase - 0.002);
                                learnSw();
                                LOGI("BFI Presenter: %s: frames repeated with %.1f ms to spare before their flash -- 2 ms earlier "
                                     "(%.0f ms earlier than the start time found)", swName, p10 * 1000.0, swBackMs);
                            }
                        }
                    }
                    // Frame generation at the game's Reflex sleep (no ReflexAlign there, no start-time search): frames ready
                    // under 1 ms before the pick for 3 s in a row while frames go missing -> 3 ms earlier. The limit keeps its
                    // place in the strobe cycle, so a start time on the pick no longer drifts away by itself.
                    {
                        static int fgTight = 0;
                        const bool sleepFgNoSearch = !fgPace && g_sleepLimitUs.load() > 0 && g_sleepFgFactor.load() > 1 && locked;
                        const bool missing = stNow.frameRepeats + stNow.frameSkips >= 2;
                        // Presented frames against one per strobe cycle (capFps above counts rendered frames here).
                        const double presCap = hzL > 0.0 ? hzL / s.frames : 0.0;
                        const bool holdsPres = presCap > 0.0 && stNow.gameFps > 0.95 * presCap && stNow.gameFps < 1.05 * presCap;
                        fgTight = (sleepFgNoSearch && !g_fgXess.load() && holdsPres && !hitchPaused && p10 >= 0.0 && p10 < 0.001 && missing) ? fgTight + 1 : 0;
                        if (fgTight >= 3)
                        {
                            fgTight = 0;
                            g_sleepShiftUs.fetch_sub(3000); alignShiftTot -= 0.003;
                            LOGI("BFI Presenter: frame limit at the game's Reflex sleep (frame generation): frames ready %.1f ms before the "
                                 "lit refresh's pick, %d repeated -- 3 ms earlier", p10 * 1000.0, stNow.frameRepeats);
                        }
                    }
                    // The alignment going round the strobe cycle: its total moved by 1.5 cycles or more within 90 s, either
                    // way. Then where the game's frames finish doesn't follow the frame start (Returnal: frames ready 10 ms
                    // before the pick however far it moved them later, +188 ms in 3 minutes; on another run 72 ms earlier
                    // in 3.5 minutes). Both alignments stop for this session; the limit keeps its place in the strobe cycle.
                    {
                        static double histT[96] = {}, histS[96] = {};
                        static int histN = 0;
                        histT[histN % 96] = t; histS[histN % 96] = alignShiftTot; histN++;
                        double span = 0.0;
                        for (int k = 0; k < 96 && k < histN; k++)
                            if (t - histT[k] <= 90.0) span = std::max(span, fabs(alignShiftTot - histS[k]));
                        if (cyc > 0.0 && span >= 1.5 * cyc && !g_alignHunted.exchange(true))
                            LOGI("BFI Presenter: Reflex frame limit alignment went round the strobe cycle (%.0f ms within 90 s): "
                                 "the game's frames don't follow its frame start -- alignment off for this session", span * 1000.0);
                    }
                    if (s.reflexAlign && !g_alignHunted.load() && atSleepNoFg && holds && !hitchPaused && t >= alignCoolT)
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
                        static double coarseT = -1e9, coarseHoldT = -1e9;
                        double &coarseTarget = g_coarseTargetS;
                        // The nudge moves the frames ~9 ms earlier itself: no step of this one on top of it (Dead Space,
                        // both in the same second: 40 of 60 frames shown for 3 s, until the next nudge).
                        const bool nudging = t < phaseUntil;
                        if (nudging) coarseHoldT = t + 20.0;
                        // Frame generation at the game's Reflex sleep too (1.0.40), whether ReflexAlign is on or not (it doesn't
                        // steer there): the limit keeps its place in the strobe cycle, wherever it began (Indiana Jones, Vulkan,
                        // 2x: frames ready 12-13 ms before the pick, 33 ms from release to flash; 26 ms after this move). Not with
                        // XeSS frame generation (no fixed place there). Presented frames against one per strobe cycle.
                        const double presCapC = hzL > 0.0 ? hzL / s.frames : 0.0;
                        const bool atSleepFgC = !fgPace && g_sleepLimitUs.load() > 0 && g_sleepFgFactor.load() > 1 && locked &&
                                                g_sleepGrid.load() && !g_fgXess.load();
                        const bool holdsC = atSleepFgC ? (presCapC > 0.0 && stNow.gameFps > 0.95 * presCapC && stNow.gameFps < 1.05 * presCapC) : holds;
                        const bool quietC = atSleepFgC ? stNow.frameRepeats + stNow.frameSkips < 5 : stNow.frameRepeats == 0;
                        const double nC = atSleepFgC ? presCapC : capFps;
                        if (!nudging && !g_alignHunted.load() && s.reflexAlignCoarse && ((atSleepNoFg && !s.reflexAlign) || atSleepFgC) &&
                            holdsC && !hitchPaused && p10 >= 0.0 && alignN >= (int)(0.8 * nC))
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
                                coarseSecs = (p10 > coarseTarget + 0.003 && quietC) ? coarseSecs + 1 : 0;
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
                    // Learned per game (LearnReflexStart, test): the alignments above find where frames should start each
                    // time the limit comes on, and that takes a while (Jusant without frame generation: 1.5 minutes with
                    // 3.5-5.6 frames missing a second while ReflexAlign moved 20 ms and widened its margin to 5 ms; Control
                    // with frame generation: later and earlier steps undoing each other). The settled start (its place in
                    // the strobe cycle) and margin are kept per game, refresh rate, frames per cycle and frame-generation
                    // factor, and the limit starts there the next time. The alignments go on as usual from that start.
                    {
                        static bool rlWas = false, rlApplied = false, rlGaveUpSaved = false;
                        static double rlOnT = 0.0, rlMoveT = 0.0, rlLastPh = -1.0;
                        static int rlQuiet = 0;
                        const int fgF = std::max(1, std::min(4, g_sleepFgFactor.load()));
                        const bool rlFg = !fgPace && g_sleepLimitUs.load() > 0 && fgF > 1 && locked && g_sleepGrid.load() && !g_fgXess.load();
                        const bool rlNoFg = atSleepNoFg && g_sleepGrid.load() != 0;
                        const bool rlOn = s.learnPerGame && s.learnReflex && learnMine.frames > 0 && (rlFg || rlNoFg) && cyc > 0.0;
                        const int phUs = g_sleepPhaseUs.load();
                        const bool phOk = phUs >= 0 && t - g_sleepPhaseT.load() < 1.0;
                        if (rlOn && !rlWas) { rlOnT = t; rlApplied = false; rlMoveT = t; rlLastPh = -1.0; rlQuiet = 0; }
                        rlWas = rlOn;
                        if (rlOn && phOk)
                        {
                            const double ph = phUs * 1e-6;
                            if (!rlApplied && t - rlOnT >= 2.0)
                            {
                                rlApplied = true;
                                if (learnMine.rfxGaveUp && s.reflexAlign && !g_alignHunted.exchange(true))
                                {
                                    rlGaveUpSaved = true;
                                    LOGI("BFI Presenter: ReflexAlign off for this session: in an earlier session this game's frames didn't "
                                         "follow it -- the limit keeps its place");
                                }
                                if (learnMine.rfxMs >= 0.0 && learnMine.rfxFg == fgF && learnMine.rfxMs < cyc * 1000.0)
                                {
                                    double d = learnMine.rfxMs * 0.001 - ph;
                                    d = std::fmod(d + 1.5 * cyc, cyc) - 0.5 * cyc;        // the shorter way round the cycle
                                    const int du = (int)(d * 1e6);
                                    if (du) { shiftUs.fetch_add(du); alignShiftTot += d; }
                                    if (rlNoFg && learnMine.rfxMarginMs > 0.0) alignTarget = std::max(alignTarget, std::min(0.008, learnMine.rfxMarginMs * 0.001));
                                    if (rlFg && learnMine.rfxMarginMs > 0.0) g_coarseTargetS = std::max(g_coarseTargetS, std::min(0.008, learnMine.rfxMarginMs * 0.001));
                                    rlMoveT = t; rlLastPh = learnMine.rfxMs * 0.001;
                                    LOGI("BFI Presenter: frame limit at the game's Reflex sleep: start time learned for this game in an earlier "
                                         "session (start +%.1f ms in the cycle, %.1f ms to spare; moved %+.1f ms)",
                                         learnMine.rfxMs, learnMine.rfxMarginMs, d * 1000.0);
                                }
                            }
                            // Settled: 60 s since the limit came on, 30 s since the start last moved, and 5 s in a row
                            // holding the cap with at most 1 frame repeated or skipped.
                            if (rlLastPh < 0.0 || fabs(std::fmod(ph - rlLastPh + 1.5 * cyc, cyc) - 0.5 * cyc) > 0.0003) { rlLastPh = ph; rlMoveT = t; }
                            const double presCapL = hzL > 0.0 ? hzL / s.frames : 0.0;
                            const bool holdsL = rlFg ? (presCapL > 0.0 && stNow.gameFps > 0.95 * presCapL && stNow.gameFps < 1.05 * presCapL) : holds;
                            rlQuiet = (holdsL && !hitchPaused && p10 >= 0.0 && stNow.frameRepeats + stNow.frameSkips <= 1) ? rlQuiet + 1 : 0;
                            if (rlApplied && t - rlOnT >= 60.0 && t - rlMoveT >= 30.0 && rlQuiet >= 5)
                            {
                                const double margin = rlNoFg && s.reflexAlign && !g_alignHunted.load() ? alignTarget : g_coarseTargetS;
                                const bool changed = learnMine.rfxFg != fgF || learnMine.rfxMs < 0.0 ||
                                                     fabs(std::fmod(ph * 1000.0 - learnMine.rfxMs + 1.5 * cyc * 1000.0, cyc * 1000.0) - 0.5 * cyc * 1000.0) > 0.75 ||
                                                     fabs(margin * 1000.0 - learnMine.rfxMarginMs) > 0.75;
                                if (changed)
                                {
                                    learnMine.rfxMs = ph * 1000.0; learnMine.rfxMarginMs = margin * 1000.0; learnMine.rfxFg = fgF;
                                    LearnPost(learnMine);
                                    LOGI("BFI Presenter: frame limit at the game's Reflex sleep: start time kept for this game (start +%.1f ms "
                                         "in the cycle, %.1f ms to spare, frame generation %dx)", learnMine.rfxMs, learnMine.rfxMarginMs, fgF);
                                }
                            }
                        }
                        // The alignment gave up this session (it went round the strobe cycle): remembered, so the next
                        // session doesn't try again.
                        if (s.learnPerGame && s.learnReflex && learnMine.frames > 0 && g_alignHunted.load() && !rlGaveUpSaved && !learnMine.rfxGaveUp)
                        {
                            rlGaveUpSaved = true;
                            learnMine.rfxGaveUp = 1;
                            LearnPost(learnMine);
                        }
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
                    // Frame generation on but no rendered count for now (the game's Reflex reports pause for 3-4 s after
                    // some hitches): the factor is kept -- it went to 1x at once and the limit came off (Alan Wake 2, 3x).
                    else if (rf <= 1.0 && g_sleepFgFactor.load() > 1) cand = g_sleepFgFactor.load();
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
                {
                    const double tRt0 = Now();
                    ReflexTick(limitUs, rs);
                    g_rtMs.store((Now() - tRt0) * 1000.0);
                }
                rsLast = rs; rsLastT = t;
                if (rs.frames > 0) rsEverT = t;
                EnterCriticalSection(&g_cfgLock);
                g_status.rfxGameUses = rs.frames > 0 ? 1 : 0;
                g_status.rfxLimitUs = rs.limitUs;
                g_status.rfxSimGpuMs = rs.simGpuMs;
                LeaveCriticalSection(&g_cfgLock);
                if (rfxf)
                {
                    fprintf(rfxf, "%.3f,%.1f,%d,%d,%d,%d,%d,%d,%d,%d,%.2f,%d,%d,%.2f,%.2f,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f,%d,%d,%d,%.2f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                            t, stNow.gameFps, userCap, perNow, stNow.autoCapDown, stNow.autoCapProbe, stNow.fgActive, s.pacingMode,
                            s.capJit, stNow.lrState, stNow.lrEst, stNow.aheadInUse, rs.limitUs,
                            relN ? relSum / relN : -1.0, relN ? relMax : -1.0, relN, rs.frames, rs.simGpuMs, rs.simPresentMs,
                            rs.inputGpuMs, rs.gpuFrameMs, rs.gpuActiveMs, rs.simMs, rs.submitMs, rs.queueMs,
                            Now() - g_rfxRenderedT.load() < 3.0 ? g_rfxRenderedFps.load() : -1.0,
                            alignLogSlack >= 0.0 ? alignLogSlack * 1000.0 : -1.0, alignShiftTot * 1000.0,
                            stNow.frameRepeats, stNow.warpOn, stNow.warpCycles, stNow.warpAvg,
                            secWarpPassN ? secWarpPassMs / secWarpPassN : -1.0, secWarpPassN,
                            secDrawPlainN ? secDrawPlainMs / secDrawPlainN : -1.0,
                            secDrawWarpN ? secDrawWarpMs / secDrawWarpN : -1.0,
                            (Now() - t) * 1000.0, g_rtMs.load(), secVramMs, secStatsMs);
                    secWarpPassMs = secDrawWarpMs = secDrawPlainMs = 0.0;
                    secWarpPassN = secDrawWarpN = secDrawPlainN = 0;
                    fflush(rfxf);
                }
                relSum = relMax = 0.0; relN = 0;
            }
            secStatsMs = 0.0;
            secStart = t; secPresents = 0; secLate = 0; ageSum = 0.0; secSlowPres = 0;
            if (g_device->GetDeviceRemovedReason() != S_OK) { DumpDeviceRemoved(g_device, "presenter check"); break; }
            if (g_dx11.load()) Sync11Window();
        }
        if (logf)
        {
            DXGI_FRAME_STATISTICS st = {};
            const double tSt1 = Now();
            const bool sok = SUCCEEDED(ScStats(&st));
            secStatsMs = std::max(secStatsMs, (Now() - tSt1) * 1000.0);
            Status lrLog;
            EnterCriticalSection(&g_cfgLock);
            lrLog.lrState = g_status.lrState; lrLog.lrEst = g_status.lrEst; lrLog.lrMargin = g_status.lrMargin;
            LeaveCriticalSection(&g_cfgLock);
            fprintf(logf, "%.3f,%llu,%u,%llu,%.3f,%.3f,%d,%d,%d,%d,%d,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%d,%.0f,%.1f,%.1f,%d,%.3f,%.3f,%.3f,%d,%d\n", t * 1000.0,
                    (unsigned long long)presents, c.slot,
                    (unsigned long long)pickId, (t - pickTime) * 1000.0, (t - lastT) * 1000.0,
                    sok ? (int)st.PresentCount : -1, sok ? (int)st.SyncRefreshCount : -1,
                    s.enabled, s.method, s.frames, s.gain, s.dfloor, s.rolloff, s.pacingMode, s.gameCap,
                    g_maxAhead.load(), s.pacing, s.queueAhead, queuedNow,
                    (tVb - tLoop0) * 1000.0, (tFence - tVb) * 1000.0, (tSub - tFence) * 1000.0, (tPres - tSub) * 1000.0, hitchPaused ? 1 : 0, c.ceilLin * 80.0f, hotArea, rawAvg,
                    lrLog.lrState, g_lrLead.load() * 1000.0, lrLog.lrEst, lrLog.lrMargin, s.smoothCatchUp, pairIter ? 1 : 0);
        }
        // A queued refresh is shown one refresh after the lit one it follows: the next interval is counted from there
        // (or the lit refresh after it, two vblanks on, would read as a missed refresh).
        lastT = pairIter ? lastT + 1.0 / (modeHz > 1.0 ? modeHz : 120.0) : t;
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
    SafeRelease(psoCrt);
    for (int k = 0; k <= kCrtPostN; k++) SafeRelease(psoCrtPass[k]);
    for (int k = 0; k < 3; k++) SafeRelease(crtRt[k]);
    g_bytesCrtPres = 0;
    SafeRelease(crtRtvHeap);
    if (crtCb && crtCbMap) crtCb->Unmap(0, nullptr);
    SafeRelease(crtCb);
    SafeRelease(psoMeas);
    SafeRelease(measRt);
    SafeRelease(measRb);
    SafeRelease(measHeap);
    SafeRelease(tsHeap);
    SafeRelease(tsRb);
    SafeRelease(psMeas);
    SafeRelease(psoAplC);
    SafeRelease(psoAplR);
    SafeRelease(aplCellRt);
    SafeRelease(aplRt);
    SafeRelease(aplRtvHeap);
    SafeRelease(psAplC);
    SafeRelease(psAplR);
    warp.Release();
    SafeRelease(chkRb[0]); SafeRelease(chkRb[1]);
    for (int i = 0; i < kWarpShN; i++) SafeRelease(psWarp[i]);
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

// ---- The presenter's own GPU time on the game's queue (1.0.49 test; while the timing log is on) ----
// Timestamps around each part of what BFI Presenter adds to the game's frame: ReShade's command list at the start and
// end of its effects (the frame from before the effects, CRT hand-off copies) and the frame copy list at the Present
// (the frame's copy, the game's motion, the frame from before the effects into the slot, the CRT hand-off). Read back a
// few frames later and logged every 10 s, so their cost can be told apart from the game's.
namespace gt
{
// Q_B0..: the CRT hand-off's picture made with the frame -- before it, after the pattern, after each kCrtPost pass
enum { Q_PFX0, Q_PFX1, Q_FIN0, Q_FIN1, Q_P0, Q_P1, Q_P2, Q_P3, Q_P4, Q_B0, kQ = Q_B0 + 2 + kCrtPostN };
static const int kR = 16;
static ID3D12QueryHeap *heap = nullptr;
static ID3D12Resource *rb = nullptr;
static UINT64 *rbMap = nullptr;
static ID3D12Device *dev = nullptr;
static double freq = 0.0;
static int cur = 0;
static bool used[kR][kQ] = {};
static UINT64 fenceOf[kR] = {};
static bool pending[kR] = {};
static double sum[6] = {}; static int cnt[6] = {}; static int frames = 0;
static double bSum[1 + kCrtPostN] = {}; static int bCnt[1 + kCrtPostN] = {};   // the hand-off's passes: [0] the pattern
static double logT = 0.0;
static bool On() { return g_shared.log != 0 && g_device && g_gameQueue && !g_dx11.load(); }
static bool Ensure()
{
    if (dev != g_device)
    {
        if (rb && rbMap) rb->Unmap(0, nullptr);
        SafeRelease(rb); SafeRelease(heap); rbMap = nullptr;
        for (int r = 0; r < kR; r++) { pending[r] = false; for (int q = 0; q < kQ; q++) used[r][q] = false; }
        dev = g_device;
        if (!dev) return false;
        D3D12_QUERY_HEAP_DESC qd = {}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = kR * kQ;
        if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap)))) { heap = nullptr; return false; }
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (UINT64)kR * kQ * 8; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
        { rb = nullptr; SafeRelease(heap); return false; }
        if (FAILED(rb->Map(0, nullptr, (void **)&rbMap))) { rbMap = nullptr; SafeRelease(rb); SafeRelease(heap); return false; }
        UINT64 f = 0;
        freq = SUCCEEDED(g_gameQueue->GetTimestampFrequency(&f)) && f ? (double)f : 0.0;
    }
    return heap && rbMap && freq > 0.0;
}
static void Mark(ID3D12GraphicsCommandList *l, int q)
{
    if (!l || !On() || !Ensure() || pending[cur]) return;
    l->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(cur * kQ + q));
    used[cur][q] = true;
}
static void Collect()
{
    if (!g_gameFence) return;
    const UINT64 done = g_gameFence->GetCompletedValue();
    for (int r = 0; r < kR; r++)
    {
        if (!pending[r] || fenceOf[r] > done) continue;
        const UINT64 *t = rbMap + (size_t)r * kQ;
        auto d = [&](int a, int b) { return (used[r][a] && used[r][b] && t[b] >= t[a]) ? (double)(t[b] - t[a]) * 1000.0 / freq : -1.0; };
        const double v[6] = { d(Q_PFX0, Q_PFX1), d(Q_FIN0, Q_FIN1), d(Q_P0, Q_P1), d(Q_P1, Q_P2), d(Q_P2, Q_P3), d(Q_P3, Q_P4) };
        for (int i = 0; i < 6; i++) if (v[i] >= 0.0 && v[i] < 100.0) { sum[i] += v[i]; cnt[i]++; }
        // each hand-off pass: from the mark before it (the last one used) to its own
        for (int k = 0, prev = used[r][Q_B0] ? Q_B0 : -1; k <= kCrtPostN && prev >= 0; k++)
        {
            const int q = Q_B0 + 1 + k;
            if (!used[r][q]) continue;
            const double ms = d(prev, q);
            if (ms >= 0.0 && ms < 100.0) { bSum[k] += ms; bCnt[k]++; }
            prev = q;
        }
        frames++;
        pending[r] = false;
        for (int q = 0; q < kQ; q++) used[r][q] = false;
    }
    const double tn = Now();
    if (logT == 0.0) logT = tn;
    if (tn - logT >= 10.0 && frames > 0)
    {
        auto a = [&](int i) { return cnt[i] ? sum[i] / cnt[i] : 0.0; };
        auto n = [&](int i) { return frames ? 100.0 * cnt[i] / frames : 0.0; };
        LOGI("BFI Presenter: GPU time on the game's queue per presented frame, last %.0f s (%d frames; ms, %% of frames): frame copy %.3f; "
             "the game's motion %.3f (%.0f%%); frame before the effects: at the effects %.3f (%.0f%%), into the slot %.3f (%.0f%%); "
             "CRT hand-off: after the effects %.3f (%.0f%%), at the Present %.3f (%.0f%%)",
             tn - logT, frames, a(2), a(3), n(3), a(0), n(0), a(4), n(4), a(1), n(1), a(5), n(5));
        if (bCnt[0])
        {
            std::string pl;
            char one[96];
            for (int k = 0; k <= kCrtPostN; k++)
            {
                if (!bCnt[k]) continue;
                const char *nm = k == 0 ? "PSCrtPattern" : kCrtPost[k - 1].entry;
                snprintf(one, sizeof(one), "%s%s %.3f (%.0f%%)", pl.empty() ? "" : ", ", nm + 5, bSum[k] / bCnt[k], 100.0 * bCnt[k] / frames);
                pl += one;
            }
            LOGI("BFI Presenter: CRT hand-off, its passes at the Present (ms, %% of frames): %s", pl.c_str());
        }
        for (int k = 0; k <= kCrtPostN; k++) { bSum[k] = 0.0; bCnt[k] = 0; }
        for (int i = 0; i < 6; i++) { sum[i] = 0.0; cnt[i] = 0; }
        frames = 0; logT = tn;
    }
}
// At the end of the frame copy list, before it is closed: its timestamps resolved; fence: the value signalled after it.
static void EndFrame(ID3D12GraphicsCommandList *l, UINT64 fence)
{
    if (!On() || !Ensure()) return;
    Collect();
    bool any = false;
    for (int q = 0; q < kQ; q++)
        if (used[cur][q]) { l->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(cur * kQ + q), 1, rb, (UINT64)(cur * kQ + q) * 8); any = true; }
    if (any) { fenceOf[cur] = fence; pending[cur] = true; cur = (cur + 1) % kR; }
}
}   // namespace gt

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
        // Pass-through diagnostics (Avatar: the picture stayed black on a recreated swap chain): once a second for
        // the first 30 s of a swap chain, what the pass-through did.
        static int s_ptPres = 0, s_ptCopy = 0;
        static double s_ptT = 0.0;
        static HRESULT s_ptHr = S_OK;
        const bool ptLog = Now() - g_deferSinceT.load() < 30.0;
        s_ptPres++;
        GameStage(ST_G_OWN_GETBUF);
        // SDR to HDR: the real buffers are HDR10, the game's frames 8-bit -- a copy between them isn't allowed (it
        // removes the device). Only the presenter can draw them; until it runs, the screen keeps its last picture.
        static bool s_noCopyLogged = false;
        const bool fmtDiffers = g_hdrUp.load() && g_stageFmt != DXGI_FORMAT_UNKNOWN && g_stageFmt != g_desc.Format;
        if (fmtDiffers && !s_noCopyLogged)
        {
            s_noCopyLogged = true;
            LOGW("BFI Presenter: SDR to HDR: the presenter isn't running, and the game's frames can't be copied to the HDR10 "
                 "buffers -- turn SdrToHdr off if the picture stays frozen");
        }
        if (!fmtDiffers && bi < kMaxBuf && SUCCEEDED(o_GetBuffer(g_sc, bi, __uuidof(ID3D12Resource), (void **)&rb)))
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
            s_ptCopy++;
        }
        const UINT stand = g_cur;
        g_cur = (g_cur + 1) % g_bufCount;
        ReleaseGameSlot();
        GameStage(ST_G_OWN_PRESENT);
        const HRESULT phr = OwnPresent();
        if (FAILED(phr)) s_ptHr = phr;
        const double tn = Now();
        if (ptLog && tn - s_ptT >= 1.0)
        {
            LOGI("BFI Presenter: pass-through: %d presents, %d copied to the real buffer (real index %u, stand-in %u of %u, "
                 "%s queue, formats %d -> %d, last failed present 0x%08lX)", s_ptPres, s_ptCopy, bi, stand, g_bufCount,
                 q == g_gameQueue ? "game" : "presenter", (int)g_stageFmt, (int)g_desc.Format, (unsigned long)s_ptHr);
            s_ptT = tn; s_ptPres = s_ptCopy = 0; s_ptHr = S_OK;
        }
        return phr;
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
            if ((int)i == g_presCurSlot || (int)i == g_presCurSlot2 || (int)i == g_presPrevSlot) continue;
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
    gt::Mark(g_copyList, gt::Q_P0);
    D3D12_RESOURCE_BARRIER br[2] = {
        Transition(g_virt[g_cur], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(g_stage[s], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
    g_copyList->ResourceBarrier(2, br);
    g_copyList->CopyResource(g_stage[s], g_virt[g_cur]);
    br[0] = Transition(g_virt[g_cur], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    br[1] = Transition(g_stage[s], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    g_copyList->ResourceBarrier(2, br);
    gt::Mark(g_copyList, gt::Q_P1);
    gmo::AtPresent(g_copyList, s);            // frame warp: this frame's motion from the game's own vectors and depth
    gt::Mark(g_copyList, gt::Q_P2);
    {
        // FrameWarpStillBeforeEffects: the frame from before ReShade's effects, kept with the slot (dx/bfi_prefx.h)
        bool preOk = false;
        ID3D12Resource *pre = pfx::Take12();
        if (pre && !g_pre[s])
        {
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = g_stage[s]->GetDesc();
            if (SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_pre[s]))))
            { g_pre[s]->SetName(L"BFI frame before ReShade's effects (slot)"); g_preGen++; }
            else g_pre[s] = nullptr;
        }
        if (pre && g_pre[s])
        {
            D3D12_RESOURCE_BARRIER pb[2] = { Transition(pre, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                             Transition(g_pre[s], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
            g_copyList->ResourceBarrier(2, pb);
            g_copyList->CopyResource(g_pre[s], pre);
            pb[0] = Transition(pre, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            pb[1] = Transition(g_pre[s], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            g_copyList->ResourceBarrier(2, pb);
            preOk = true;
        }
        g_preOk[s] = preOk;
    }
    gt::Mark(g_copyList, gt::Q_P3);
    {
        static double countT = 0.0;
        const double tn = Now();
        if (tn - countT > 1.0)
        {
            countT = tn;
            UINT64 b = pfx::Bytes12();
            for (UINT k = 0; k < kStage; k++) b += ResBytes(g_stage[k]) + ResBytes(g_pre[k]);
            g_bytesFrames = b;
        }
    }
    crt::CopyAtPresent(g_copyList, s);        // CRT-Standalone's halation and glow, for the CRT hand-off
    const bool bakeList = g_bakeList && g_bakeAlloc[s];
    if (!bakeList)
    {
        UINT64 pw = 0;
        crt::Bake(g_copyList, s, g_stage[s], waitRead, &pw);   // and the frame's CRT picture with them (shown as is; a moved flash draws its own)
        if (pw > waitRead) waitRead = pw;
        gt::Mark(g_copyList, gt::Q_P4);
        gt::EndFrame(g_copyList, g_gameFenceVal + 1);
    }
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
    gmo::Published(s, g_frameId);
    g_recentFence[g_frameId % 8] = g_gameFenceVal;
    g_stageTime[s]  = Now();
    LeaveCriticalSection(&g_lock);
    LrStaged(g_frameId, g_gameFenceVal);
    if (bakeList)
    {
        // The frame's CRT picture (the CRT hand-off), after the frame is published: the frame counts as arrived without
        // its passes; until this list is done a flash of it draws its CRT picture itself (crt::BakeReady)
        const UINT64 published = g_gameFenceVal;
        WaitFence(g_gameFence, g_bakeDone[s], g_gameFenceEvent);   // its allocator idle (long done: one frame per slot)
        g_bakeAlloc[s]->Reset();
        g_bakeList->Reset(g_bakeAlloc[s], nullptr);
        UINT64 presWait = 0;                                        // grain's texture: the presenter's last read of it
        const bool baked = crt::Bake(g_bakeList, s, g_stage[s], waitRead, &presWait);
        gt::Mark(g_bakeList, gt::Q_P4);
        gt::EndFrame(g_bakeList, published + 1);
        const bool timed = gt::On();
        if (SUCCEEDED(g_bakeList->Close()) && (baked || timed))
        {
            ID3D12CommandList *bl[] = { g_bakeList };
            if (presWait) g_gameQueue->Wait(g_presFence, presWait);
            g_gameQueue->ExecuteCommandLists(1, bl);
            g_gameQueue->Signal(g_gameFence, ++g_gameFenceVal);
            g_bakeDone[s] = g_gameFenceVal;
            crt::BakeSubmitted(s, g_gameFenceVal);
        }
    }
    g_gtCopy = Now();

    g_stageNext = (s + 1) % kStage;
    g_cur = (g_cur + 1) % g_bufCount;
    g_stagedThisSecond.fetch_add(1);
    Osd::FramePush(Now());
    if (!g_firstStaged.exchange(true)) LOGI("BFI Presenter: first game frame received");
    if ((g_frameId & 31) == 1) ReadRsColorSpace();     // for the presenter thread (see g_rsCs)
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
    g_lastGamePresentT.store(Now());
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
    if (SUCCEEDED(hr) && d && IsManaged(sc) && !t_presenterThread && !t_ownCall)
    {
        if (g_virtCount) d->BufferCount = g_virtCount;
        if (g_hdrUp.load() && g_stageFmt != DXGI_FORMAT_UNKNOWN) d->BufferDesc.Format = g_stageFmt;   // SDR to HDR: its own format
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_GetDesc1(IDXGISwapChain1 *sc, DXGI_SWAP_CHAIN_DESC1 *d)
{
    const HRESULT hr = o_GetDesc1(sc, d);
    if (SUCCEEDED(hr) && d && IsManaged(sc) && !t_presenterThread && !t_ownCall)
    {
        if (g_virtCount) d->BufferCount = g_virtCount;
        if (g_hdrUp.load() && g_stageFmt != DXGI_FORMAT_UNKNOWN) d->Format = g_stageFmt;
    }
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
    ReflexServe11();
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
        static double phase = 0.0;   // where in the strobe cycle frames start (moved by the alignment and the search)
        const int sh = g_sleepShiftUs.exchange(0);
        if (sh != 0) { nextT += sh * 1e-6; phase += sh * 1e-6; }
        // On the strobe-cycle grid, as the frame limit for frame generation without Reflex: the limit runs on the PC's
        // clock and the display on its own, and a whole number of cycles in microseconds is never exact, so the frames
        // drifted past the lit refresh's pick every so often (Silent Hill: Townfall, XeSS frame generation: 6-10
        // repeated and skipped a second each time). Only when the limit is a whole number of strobe cycles.
        // Not with XeSS frame generation: it spaces its frames unevenly by itself, and a start time on the pick then
        // stayed there (Silent Hill: Townfall: 1.9-2.3 frames a second not shown, against 0.5-1.0 with the PC's clock,
        // whose drift moves it on). DLSS frame generation keeps the grid (Control: 0.05 against 1.7).
        if (g_sleepGrid.load() && !(g_fgXess.load() && g_sleepFgFactor.load() > 1))
        {
            UINT64 c = 0; int sl = 0; double tick = 0.0;
            for (int i = 0; i < 4; i++)
            {
                c = g_tickCount.load(); sl = g_tickSlot.load(); tick = g_tickT.load();
                if (g_tickCount.load() == c) break;
            }
            const double per = g_lrPeriod.load();
            const double cyc = per * std::max(1, g_strobeFrames.load());
            const double k = cyc > 0.0 ? iv / cyc : 0.0;
            const bool whole = k >= 0.9 && fabs(k - std::floor(k + 0.5)) * cyc < 0.0003;
            if (whole && tick > 0.0 && per > 0.0 && now - tick < 0.1)
            {
                const double cycStart = tick - sl * per;
                double ph = std::fmod(phase, cyc);
                if (ph < 0.0) ph += cyc;
                nextT = cycStart + ph + std::floor((nextT - cycStart - ph) / cyc + 0.5) * cyc;
                g_sleepPhaseUs.store((int)(ph * 1e6));
                g_sleepPhaseT.store(now);
            }
        }
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

// ---------------------------------------------------------------------------------------------------------
// Streamline probe (test build sl1): what a game hands DLSS through Streamline each frame -- the tagged
// buffers (depth, motion vectors) and the camera constants -- logged every 10 s. Only sl.interposer.dll's
// slSetTag, slSetTagForFrame, slSetConstants and slEvaluateFeature are hooked: a few calls a frame, no
// draw events (the depth probe through ReShade's draw events crashed South of Midnight). Nothing is changed.
// Layouts follow the Streamline SDK 2.x headers (sl_struct.h, sl.h, sl_consts.h); values are checked for
// sanity before they are logged.
namespace slp
{
struct Base { void *next; uint8_t type[16]; size_t version; };                   // sl::BaseStructure, 32 bytes
struct Res  { Base b; uint32_t type; uint32_t pad; void *native; void *mem; void *view; uint32_t state;
              uint32_t width, height, nativeFormat, mips, layers; };              // sl::Resource (start)
struct Ext  { uint32_t top, left, width, height; };                               // sl::Extent
struct Tag  { Base b; Res *res; uint32_t bufType; uint32_t lifecycle; Ext ext; };   // sl::ResourceTag (2.4: the extent by value)
static_assert(sizeof(Tag) == 64, "sl::ResourceTag layout");
struct Consts                                                                     // sl::Constants (start)
{
    Base b;
    float viewToClip[16], clipToView[16], clipToLensClip[16], clipToPrevClip[16], prevClipToClip[16];
    float jitter[2], mvecScale[2], pinhole[2];
    float pos[3], up[3], right[3], fwd[3];
    float zNear, zFar, fov, aspect, mvInvalid;
    uint8_t depthInverted, cameraMotionIncluded, mv3D, reset, ortho, mvDilated, mvJittered;
};
static_assert(offsetof(Consts, zNear) == 424 && offsetof(Consts, depthInverted) == 444, "sl::Constants layout");

typedef int (*PFN_SetTag)(const void *vp, const Tag *tags, uint32_t n, void *cmd);
typedef int (*PFN_SetTagForFrame)(const void *frame, const void *vp, const Tag *tags, uint32_t n, void *cmd);
typedef int (*PFN_SetConstants)(const Consts &c, const void *frame, const void *vp);
typedef int (*PFN_Evaluate)(uint32_t feature, const void *frame, const Base **inputs, uint32_t n, void *cmd);
static PFN_SetTag o_SetTag; static PFN_SetTagForFrame o_SetTagForFrame; static PFN_SetConstants o_SetConstants; static PFN_Evaluate o_Evaluate;

struct TagStat { uint32_t calls = 0, w = 0, h = 0, fmt = 0, extW = 0, extH = 0, resW = 0, resH = 0;
                 uint32_t noRes = 0, badRes = 0, noNative = 0, badNative = 0, noQI = 0, ok = 0; bool d3d12 = false, d3d11 = false; };
static std::mutex g_lk;
static bool g_tried = false, g_hooked = false;
static double g_winT = 0.0;
static int g_reports = 0;
static std::map<uint32_t, TagStat> g_tags, g_tagsEval;   // from slSetTag / from slEvaluateFeature's inputs
static std::map<uint32_t, uint32_t> g_evalIn;               // other structures in slEvaluateFeature's inputs, by GUID's first word
static int g_rawLeft = 8;                                   // raw tag dumps left (sl4)
static const uint8_t kTagGuid[16] = { 0xad,0x5a,0x6a,0x4c, 0x45,0xb4, 0x6c,0x49, 0x87,0xff,0x1a,0xf3,0x84,0x5b,0xe6,0x53 };
static std::map<uint32_t, uint32_t> g_evals;
static uint32_t g_consts = 0, g_constsBad = 0, g_resets = 0;
static Consts g_last = {}; static bool g_haveLast = false;
static double g_moved = 0.0, g_turned = 0.0;      // camera travel and turning over the window (from pos and fwd)
static float g_prevPos[3], g_prevFwd[3]; static bool g_havePrev = false;

// Every pointer from the game is checked before it is read: Rift Apart (sl1) crashed reading a tag's extent
// at 0x100000008 -- a Streamline older than the 2.x layout assumed here, or a different one.
static bool Readable(const void *p, size_t n)
{
    if (!p || (uintptr_t)p < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mi = {};
    if (!VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const BYTE *)p + n <= (const BYTE *)mi.BaseAddress + mi.RegionSize;
}
// Cheap check without a system call: Rift Apart slowed to 13 fps with VirtualQuery on every tag (sl4) --
// it takes the process's address-space lock, which the game's streaming threads hold for long stretches.
// So VirtualQuery runs only on one sampled call a second; every other call just checks the pointer's range.
static inline bool Plaus(const void *p) { const uintptr_t u = (uintptr_t)p; return u >= 0x10000 && u < 0x7FFFFFFF0000ull && !(u & 7); }
static std::atomic<bool> g_deepDue{true};
static bool g_deep = false;                          // this call is the sampled one (set under g_lk)
static inline bool Chk(const void *p, size_t n) { return g_deep ? Readable(p, n) : Plaus(p); }
static double g_probeS = 0.0, g_probeMaxS = 0.0; static uint32_t g_probeCalls = 0;   // time in the probe's own code
static uint32_t g_badTags = 0;
static char g_slVer[48] = "unknown";
static thread_local int g_slDepth = 0;               // inside slEvaluateFeature on this thread (its own DLSS call is left to this hook)
static int g_slMajor = 0;

static const char *TagName(uint32_t t)
{
    switch (t)
    {
    case 0: return "depth"; case 1: return "motion vectors"; case 2: return "HUD-less colour"; case 3: return "upscaler input colour";
    case 4: return "upscaler output colour"; case 5: return "normals"; case 6: return "roughness"; case 7: return "albedo";
    default: return nullptr;
    }
}

static void NoteOne(const Tag &tg, std::map<uint32_t, TagStat> &into, const char *from)
{
    if (tg.b.version > 64 || tg.bufType > 255) { g_badTags++; return; }
    TagStat &s = into[tg.bufType];
    s.calls++;
    if (tg.ext.width) { s.extW = tg.ext.width; s.extH = tg.ext.height; }
    Res *r = tg.res;
    const bool rOk = r && Chk(r, sizeof(Res));
    if (g_rawLeft > 0 && g_deep)
    {
        g_rawLeft--;
        LOGI("BFI Presenter: Streamline probe: raw tag (%s): type %u, lifecycle %u, extent %u,%u %ux%u, resource %p%s native %p, res size %ux%u fmt %u, state 0x%x",
             from, tg.bufType, tg.lifecycle, tg.ext.left, tg.ext.top, tg.ext.width, tg.ext.height, (void *)r, rOk ? "" : " (unreadable)",
             rOk ? r->native : nullptr, rOk ? r->width : 0, rOk ? r->height : 0, rOk ? r->nativeFormat : 0, rOk ? r->state : 0);
    }
    if (!r) { s.noRes++; return; }
    if (!rOk) { s.badRes++; return; }
    if (r->width) { s.resW = r->width; s.resH = r->height; }
    if (!r->native) { s.noNative++; return; }
    if (!g_deep) { if (Plaus(r->native)) s.ok++; else s.badNative++; return; }   // the texture is looked up only on the sampled call
    if (!Readable(r->native, 8) || !Readable(*(void **)r->native, 24)) { s.badNative++; return; }
    if (s.w) { s.ok++; return; }                         // size looked up once per buffer type and window
    IUnknown *u = (IUnknown *)r->native;
    ID3D12Resource *r12 = nullptr; ID3D11Texture2D *t11 = nullptr;
    if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D12Resource), (void **)&r12)) && r12)
    {
        const D3D12_RESOURCE_DESC d = r12->GetDesc();
        s.w = (uint32_t)d.Width; s.h = d.Height; s.fmt = (uint32_t)d.Format; s.d3d12 = true; s.ok++;
        r12->Release();
    }
    else if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t11)) && t11)
    {
        D3D11_TEXTURE2D_DESC d = {}; t11->GetDesc(&d);
        s.w = d.Width; s.h = d.Height; s.fmt = (uint32_t)d.Format; s.d3d11 = true; s.ok++;
        t11->Release();
    }
    else s.noQI++;
}

static void NoteTags(const Tag *tags, uint32_t n)
{
    const double t0 = Now();
    std::lock_guard<std::mutex> l(g_lk);
    g_deep = g_deepDue.exchange(false);
    if (n != 0 && n <= 64 && Chk(tags, n * sizeof(Tag))) { for (uint32_t i = 0; i < n; i++) NoteOne(tags[i], g_tags, "slSetTag"); }
    else g_badTags++;
    const double d = Now() - t0; g_probeS += d; g_probeCalls++; if (d > g_probeMaxS) g_probeMaxS = d;
}

// slEvaluateFeature can carry the tags itself, as inputs (each a chain of structures through 'next').
static void NoteInputs(const Base **in, uint32_t n)
{
    const double t0 = Now();
    std::lock_guard<std::mutex> l(g_lk);
    g_deep = g_deepDue.exchange(false);
    if (in && n != 0 && n <= 64 && Chk(in, n * sizeof(void *)))
    {
        for (uint32_t i = 0; i < n; i++)
        {
            const Base *b = in[i];
            for (int depth = 0; b && depth < 8; depth++)
            {
                if (!Chk(b, sizeof(Base))) { g_badTags++; break; }
                if (memcmp(b->type, kTagGuid, 16) == 0) { if (Chk(b, sizeof(Tag))) NoteOne(*(const Tag *)b, g_tagsEval, "slEvaluateFeature"); }
                else { uint32_t g; memcpy(&g, b->type, 4); g_evalIn[g]++; }
                b = (const Base *)b->next;
            }
        }
    }
    const double d = Now() - t0; g_probeS += d; g_probeCalls++; if (d > g_probeMaxS) g_probeMaxS = d;
}

} // namespace slp
namespace gmo
{
static void NoteSetTags(const slp::Tag *tags, uint32_t n, void *cmd);
static void NoteConsts(const slp::Consts &c);
static void OnEvaluate(const slp::Base **in, uint32_t n, void *cmd);
}
namespace slp
{
static int hk_SetTag(const void *vp, const Tag *tags, uint32_t n, void *cmd) { NoteTags(tags, n); gmo::NoteSetTags(tags, n, cmd); return o_SetTag(vp, tags, n, cmd); }
static int hk_SetTagForFrame(const void *frame, const void *vp, const Tag *tags, uint32_t n, void *cmd) { NoteTags(tags, n); gmo::NoteSetTags(tags, n, cmd); return o_SetTagForFrame(frame, vp, tags, n, cmd); }
static int hk_SetConstants(const Consts &c, const void *frame, const void *vp)
{
    {
        std::lock_guard<std::mutex> l(g_lk);
        g_consts++;
        if (!Plaus(&c)) { g_constsBad++; return o_SetConstants(c, frame, vp); }
        const bool sane = c.fov > 0.05f && c.fov < 3.2f && c.zNear > 0.0f && c.zFar != c.zNear;
        if (!sane) g_constsBad++;
        else
        {
            if (c.reset == 1) g_resets++;
            if (g_havePrev)
            {
                const double dx = c.pos[0] - g_prevPos[0], dy = c.pos[1] - g_prevPos[1], dz = c.pos[2] - g_prevPos[2];
                g_moved += std::sqrt(dx * dx + dy * dy + dz * dz);
                const double dot = c.fwd[0] * g_prevFwd[0] + c.fwd[1] * g_prevFwd[1] + c.fwd[2] * g_prevFwd[2];
                g_turned += std::acos(std::max(-1.0, std::min(1.0, dot))) * 57.29578;
            }
            memcpy(g_prevPos, c.pos, sizeof(g_prevPos)); memcpy(g_prevFwd, c.fwd, sizeof(g_prevFwd)); g_havePrev = true;
            g_last = c; g_haveLast = true;
        }
    }
    if (Plaus(&c)) gmo::NoteConsts(c);
    return o_SetConstants(c, frame, vp);
}
static int hk_Evaluate(uint32_t feature, const void *frame, const Base **inputs, uint32_t n, void *cmd)
{
    { std::lock_guard<std::mutex> l(g_lk); g_evals[feature]++; }
    NoteInputs(inputs, n);
    if (feature == 0 || feature == 1001) gmo::OnEvaluate(inputs, n, cmd);   // DLSS / DLSS ray reconstruction: the game's motion
                                                                              // vectors and depth for frame warp
    g_slDepth++;
    const int r = o_Evaluate(feature, frame, inputs, n, cmd);
    g_slDepth--;
    return r;
}

static const char *FeatureName(uint32_t f)
{
    switch (f)
    {
    case 0: return "DLSS"; case 1: return "NRD"; case 2: return "NIS"; case 3: return "Reflex"; case 4: return "PCL";
    case 1000: return "DLSS frame generation"; case 1001: return "DLSS ray reconstruction"; default: return nullptr;
    }
}

// Once a second from the presenter: hooks Streamline when it is loaded, and reports every 10 s.
static void Tick(double t)
{
    if (!g_tried)
    {
        HMODULE sl = GetModuleHandleA("sl.interposer.dll");
        if (!sl) return;
        g_tried = true;
        if (!g_shared.slHooks)
        {
            LOGI("BFI Presenter: Streamline found, not hooked (StreamlineHooks=0): frame warp uses the picture's own motion");
            return;
        }
        {
            char path[MAX_PATH] = "";
            GetModuleFileNameA(sl, path, MAX_PATH);
            DWORD h = 0, n = GetFileVersionInfoSizeA(path, &h);
            std::vector<BYTE> vb(n ? n : 1);
            VS_FIXEDFILEINFO *fi = nullptr; UINT fl = 0;
            if (n && GetFileVersionInfoA(path, 0, n, vb.data()) && VerQueryValueA(vb.data(), "\\", (void **)&fi, &fl) && fi)
            {
                g_slMajor = HIWORD(fi->dwFileVersionMS);
                snprintf(g_slVer, sizeof(g_slVer), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                         HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            }
        }
        if (g_slMajor < 2)
        {
            LOGI("BFI Presenter: Streamline probe (test build): sl.interposer.dll version %s -- not 2.x, left alone", g_slVer);
            return;
        }
        struct { const char *name; void *hook; void **orig; } fns[] = {
            { "slSetTag", (void *)&hk_SetTag, (void **)&o_SetTag },
            { "slSetTagForFrame", (void *)&hk_SetTagForFrame, (void **)&o_SetTagForFrame },
            { "slSetConstants", (void *)&hk_SetConstants, (void **)&o_SetConstants },
            { "slEvaluateFeature", (void *)&hk_Evaluate, (void **)&o_Evaluate },
        };
        char list[400] = "";
        for (auto &f : fns)
        {
            void *p = (void *)GetProcAddress(sl, f.name);
            char why[96] = "not exported";
            void *orig = p ? InlineHook64(p, f.hook, why, sizeof(why)) : nullptr;
            if (orig) { *f.orig = orig; g_hooked = true; }
            char one[160];
            snprintf(one, sizeof(one), "%s%s %s%s%s", list[0] ? "; " : "", f.name, orig ? "hooked" : "not hooked", orig ? "" : " -- ", orig ? "" : why);
            strncat(list, one, sizeof(list) - strlen(list) - 1);
        }
        LOGI("BFI Presenter: Streamline probe (test build): sl.interposer.dll %s: %s", g_slVer, list);
        g_winT = t;
        return;
    }
    if (!g_hooked) return;
    g_deepDue = true;                                  // one sampled (fully checked) call a second
    if (g_reports >= 60 || t - g_winT < 10.0) return;
    // Take the window's numbers under the lock, then log without it (the game's render thread waits on the lock).
    std::map<uint32_t, TagStat> tagsSet, tagsEv; std::map<uint32_t, uint32_t> evalIn, evals;
    uint32_t badTags, consts, constsBad, resets, probeCalls; double moved, turned, probeS, probeMaxS; Consts last; bool haveLast;
    {
        std::lock_guard<std::mutex> l(g_lk);
        tagsSet.swap(g_tags); tagsEv.swap(g_tagsEval); evalIn.swap(g_evalIn); evals.swap(g_evals);
        badTags = g_badTags; consts = g_consts; constsBad = g_constsBad; resets = g_resets; moved = g_moved; turned = g_turned;
        last = g_last; haveLast = g_haveLast; probeCalls = g_probeCalls; probeS = g_probeS; probeMaxS = g_probeMaxS;
        g_badTags = 0; g_consts = g_constsBad = g_resets = 0; g_moved = g_turned = 0.0; g_haveLast = false;
        g_probeCalls = 0; g_probeS = g_probeMaxS = 0.0;
    }
    const double secs = t - g_winT;
    g_winT = t;
    g_reports++;
    // One line per buffer type (the log line is capped at 512 characters).
    auto list = [&](std::map<uint32_t, TagStat> &m, const char *from) {
        for (auto &kv : m)
        {
            const TagStat &s = kv.second;
            const char *nm = TagName(kv.first);
            char nb[24]; snprintf(nb, sizeof(nb), "type %u", kv.first);
            char extra[80] = "";
            if (s.resW) snprintf(extra, sizeof(extra), ", stated %ux%u", s.resW, s.resH);
            if (s.extW) { char e[40]; snprintf(e, sizeof(e), ", used %ux%u", s.extW, s.extH); strncat(extra, e, sizeof(extra) - strlen(extra) - 1); }
            LOGI("BFI Presenter: Streamline probe: %s tag %s %.0f/s: %ux%u fmt %u%s%s [ok %u, no resource %u, resource unreadable %u, no native %u, "
                 "native unreadable %u, not a texture %u]", from, nm ? nm : nb, s.calls / secs, s.w, s.h, s.fmt,
                 s.d3d12 ? " D3D12" : s.d3d11 ? " D3D11" : "", extra, s.ok, s.noRes, s.badRes, s.noNative, s.badNative, s.noQI);
        }
    };
    list(tagsSet, "slSetTag");
    list(tagsEv, "slEvaluateFeature");
    char other[300] = "";
    for (auto &kv : evalIn)
    {
        char one[48]; snprintf(one, sizeof(one), "%s%08x %.0f/s", other[0] ? ", " : "", kv.first, kv.second / secs);
        strncat(other, one, sizeof(other) - strlen(other) - 1);
    }
    char ev[300] = "";
    for (auto &kv : evals)
    {
        char one[80]; const char *nm = FeatureName(kv.first);
        snprintf(one, sizeof(one), "%s%s%s %.0f/s", ev[0] ? ", " : "", nm ? nm : "feature ", nm ? "" : std::to_string(kv.first).c_str(), kv.second / secs);
        strncat(ev, one, sizeof(ev) - strlen(ev) - 1);
    }
    LOGI("BFI Presenter: Streamline probe: last %.0f s: %u slSetTag buffer types, %u inside slEvaluateFeature, unreadable tag calls %u | evaluated: %s | "
         "other evaluate inputs: %s", secs, (unsigned)tagsSet.size(), (unsigned)tagsEv.size(), badTags, ev[0] ? ev : "none", other[0] ? other : "none");
    LOGI("BFI Presenter: Streamline probe: its own time in the game's calls: %u calls, %.1f us on average, %.0f us at most", probeCalls,
         probeCalls ? probeS * 1e6 / probeCalls : 0.0, probeMaxS * 1e6);
    if (haveLast)
    {
        const Consts &c = last;
        bool mat = false;
        for (int i = 0; i < 16; i++) if (c.viewToClip[i] != 0.0f) mat = true;
        LOGI("BFI Presenter: Streamline probe: camera constants %.0f/s (%u implausible), FOV %.1f deg, aspect %.2f, near %.3g, far %.3g, depth %s, "
             "motion vectors scale %.3g x %.3g%s%s%s, view-to-clip matrix %s, camera moved %.1f units and turned %.0f deg in %.0f s, %u resets",
             consts / secs, constsBad, c.fov * 57.29578, c.aspect, c.zNear, c.zFar,
             c.depthInverted == 1 ? "reversed" : c.depthInverted == 0 ? "standard" : "not stated", c.mvecScale[0], c.mvecScale[1],
             c.cameraMotionIncluded == 1 ? ", include camera motion" : "", c.mvJittered == 1 ? ", jittered" : "", c.mv3D == 1 ? ", 3D" : "",
             mat ? "set" : "all zero", moved, turned, secs, resets);
    }
    else LOGI("BFI Presenter: Streamline probe: no camera constants in the last %.0f s (%u calls, %u implausible)", secs, consts, constsBad);
}
} // namespace slp

// ---------------------------------------------------------------------------------------------------------
// Frame warp from the game's own motion (test): where a D3D12 game hands its upscaler its motion vectors and depth
// through Streamline (DLSS), frame warp moves each part of the picture by the game's own vector for it instead of
// the motion it measures from the finished picture, and where things overlap the nearer one wins.
//
//   1. At the upscaler call (slEvaluateFeature, the game's render thread): the game's motion vectors and depth are
//      copied on the game's own command list -- copies only, so nothing the game has set on its list is changed --
//      into one of three pairs of textures, with the camera constants of that frame (motion-vector scale, reversed
//      depth, a camera cut).
//   2. At the game's Present (its Present thread): the oldest copy not yet taken becomes that frame's motion
//      texture (PSWarpGame, dx/bfi_warp.h), 1/4 of the game frame, made on the presenter's copy list on the game's
//      queue right after the frame is copied into its staging slot. One per staging slot.
//   3. The presenter (per refresh) reads that slot's motion texture when it shows the frame moved ahead.
//
// Nothing of this runs while frame warp is off. Games without Streamline, DX11 games and games whose motion vectors
// leave out the camera's motion keep the motion measured from the picture; the overlay says which is used.
namespace gmo
{
using slp::Tag; using slp::Res; using slp::Consts; using slp::Base; using slp::Plaus;

static const int kRing = 3;
struct Entry
{
    ID3D12Resource *mv = nullptr, *depth = nullptr;       // the copies (ours)
    DXGI_FORMAT mvFmt = DXGI_FORMAT_UNKNOWN, dFmt = DXGI_FORMAT_UNKNOWN;   // the game's formats
    UINT mvW = 0, mvH = 0, dW = 0, dH = 0;
    float mvMap[4] = {}, dMap[4] = {};                    // screen uv -> texel: xy scale, zw offset
    float mvScale[2] = {}, mvInvalid = 0.0f;
    bool  depthRev = false, hasDepth = false, reset = false;
    UINT  gen = 0;                                        // bumped when the copies are made anew
};
struct GConsts { float outTexel[2], mvScale[2], mvMap[4], dMap[4], mvInvalid; uint32_t flags; float pad[2]; float hMap[4]; float hudThr; uint32_t hudMode; float pad2[2]; };
static_assert(sizeof(GConsts) == 96, "PSWarpGame / PSWarpHud constants");

static std::mutex lk;
static Entry ring[kRing];
static int  pend[kRing]; static int pendN = 0;            // filled entries not yet taken by a Present, oldest first
static int  latest = -1;                                 // the newest filled entry (frame generation: every Present uses it)
static int  evalsSec = 0, presSec = 0; static double secT = 0.0;
static double perEval = 1.0;                             // Presents per upscaler call over the last seconds (2 or 3 with frame generation)
static int  fgK = 0;                                     // frame generation: shown frames per rendered one, a whole number (0 = not settled)
static int  fgKNext = 0, fgKSecs = 0;
static uint64_t seq = 0;
static UINT genNext = 1;
static std::vector<std::pair<ID3D12Resource *, double>> retired;   // replaced copies, released 5 s on (a game list that
                                                         // copied into one has run by then; they used to stay until the
                                                         // device changed: ~100 MB more each time Townfall letterboxed)
static Tag setTag[2]; static Res setRes[2]; static bool haveSet[2] = {};   // the last depth / vectors given to slSetTag

// HUD mask: the game's HUD-less picture (Streamline buffer type 2), copied when the game tags it with a command list
// (at its slSetTag or upscaler call), else used where it is at the Present when it stays valid until then.
static ID3D12Resource *hudCopy = nullptr;                // the copy (ours), in COPY_DEST between uses
static DXGI_FORMAT hudFmt = DXGI_FORMAT_UNKNOWN;
static UINT  hudW = 0, hudH = 0;
static float hudMap[4] = {};
static double hudCopyT = -10.0;                          // when it was last copied
static Tag   hudLiveTag; static Res hudLiveRes; static bool hudLive = false;   // valid until the Present (no copy)
static ID3D12Resource *hudRaw[2] = {}, *hudMask = nullptr;   // the motion texture's size, R8; in PIXEL_SHADER_RESOURCE between uses
static int  hudCur = 0;
static bool hudMaskOk = false;                           // hudMask holds a mask from the last Presents
static ID3D12PipelineState *psoHud = nullptr;
static std::atomic<int> hudState{0};                     // 0 no HUD-less picture seen, 1 in use, -1 switched off (didn't match)
// How the frame and the HUD-less picture are compared (PSWarpHud's gHudMode). 007 First Light's HUD-less picture is
// linear (RGBA16F) against an HDR10 frame: compared as they are, the mask covered 99.9% (gm9). The modes are tried
// in turn, from the likeliest for the two formats; the check (every 2 s) moves on when the mask covers too much.
// 1.0.47: by structure (3) last -- tone mapping and post-processing between the HUD-less picture and the frame shown
// (Directive 8020: 74-75% in every way of comparing by value) keep the scene's structure.
static int hudModes[4] = { 0, 1, 2, 3 }; static int hudModeN = 4; static std::atomic<int> hudModeI{0};
static bool hudModesSet = false;
// Without a HUD-less picture (FrameWarpHudFromDlss, 1.0.47): DLSS's own output, copied on the game's list right after its
// call -- the scene upscaled, before post-processing and the HUD (Unreal Engine). It differs from the frame by tone
// mapping, grading, bloom and vignette as well as the HUD, so it is compared by structure (gHudMode 3), not value.
static bool hudFromOut = false;                          // hudCopy holds DLSS's output, not a HUD-less picture
static double hudRealT = -10.0;                          // when the game last tagged a real HUD-less picture
static char hudWhy[160] = "";
static Consts consts = {}; static bool haveConsts = false;
static bool depthFailed = false;                         // a copy texture of the game's depth format couldn't be made
static ID3D12Device *ringDev = nullptr;                  // the device the copies belong to (not owned)

// GPU objects for step 2 (Present thread)
static ID3D12RootSignature *rs = nullptr;
static ID3D12PipelineState *pso = nullptr;
static ID3D12DescriptorHeap *srvHeap = nullptr, *rtvHeap = nullptr;
static UINT srvStride = 0, rtvStride = 0;
static ID3D12Resource *flow[kStage] = {};
// The motion textures are made anew when the game's vector size changes (Townfall: its cutscenes switch between 2258x1271
// and a letterboxed 2258x961, twice within 2 s before the GPU hung in 1.0.44-ep1). The old ones were released at once,
// while the presenter's queue could still be reading one and its command list had a barrier on one and the matching
// barrier back on the new one. Now they are swapped under flowLk, kept for 3 s before release, and the presenter takes
// one texture per refresh, with the generation its views were written for.
static std::mutex flowLk;
struct RetiredRes { ID3D12Resource *r; double t; };
static std::vector<RetiredRes> flowRetired;
static UINT flowW = 0, flowH = 0;
static std::atomic<UINT> flowGen{0};                     // bumped when the motion textures are made anew
static bool slotPend[kStage] = {}, slotOk[kStage] = {}, slotDepth[kStage] = {};
static float slotLag[kStage] = {};                       // frame generation: shown frames this one is behind its vectors' frame
static uint64_t fgSeqSeen = 0; static int fgIdx = 0;     // which shown frame since the last upscaler call (0 = the first)
static std::atomic<UINT64> slotId[kStage];
// Camera cuts and letterboxed cutscenes (ep3): the frame after a cut (the game's reset flag, from Streamline or direct
// DLSS calls) and the next ones aren't filled -- there is no motion connecting two shots, and the motion measured from
// the picture across a cut is between two unrelated pictures. A cutscene letterboxed through DLSS (its output smaller
// than the screen) pauses frame warp until it ends.
static bool slotCut[kStage] = {};
static std::atomic<UINT64> cutId{0};
static std::atomic<int> cutsSec{0};
static std::atomic<bool> lbOn{false};
static std::atomic<double> lbT{0.0};
// Only a letterbox that follows full-screen output in this session counts as a cutscene, and one that lasts over 10
// minutes in a row is play (a game letterboxed throughout, by design): frame warp works there as usual.
static std::atomic<bool> lbFullSeen{false};
static std::atomic<double> lbStartT{0.0};
static void NoteLetterbox(bool lb, unsigned w, unsigned h, unsigned fw, unsigned fh, const char *from)
{
    const bool was = lbOn.exchange(lb);
    if (lb) lbT = Now();
    if (!lb) lbFullSeen = true;
    if (lb && !was) lbStartT = Now();
    if (lb != was)
    {
        static int logged = 0;
        if (logged++ < 40)
        {
            if (lb) LOGI("BFI Presenter: frame warp: letterboxed cutscene (%s output %ux%u of %ux%u)", from, w, h, fw, fh);
            else    LOGI("BFI Presenter: frame warp: the letterboxed cutscene ended (%s output %ux%u)", from, w, h);
        }
    }
}

// What the game hands over, for the overlay and the log.
static std::atomic<int> state{0};                        // 0 nothing seen yet, 1 vectors and depth, 2 vectors only, -1 not usable
static char why[160] = "";
static int  whyLogged = 0;

static void SetState(int st, const char *fmt, ...)
{
    char b[160];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    state = st;
    if (strcmp(b, why) != 0)
    {
        snprintf(why, sizeof(why), "%s", b);
        if (whyLogged++ < 20) LOGI("BFI Presenter: frame warp, the game's own motion: %s", b);
    }
}

static DXGI_FORMAT CopyFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    default: return f;
    }
}
static DXGI_FORMAT DepthView(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
static DXGI_FORMAT MvView(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static bool On() { return g_shared.frameWarp != 0 && g_shared.warpGame != 0 && !g_dx11.load(); }
// With frame generation the game's motion isn't used (FrameWarpGameMotionWithFG=0, the default): its vectors, depth and
// HUD-less picture aren't copied either (1.0.49: they were, on every upscaler call, for nothing)
static bool FgUnused() { return (g_status.fgActive && !g_shared.warpGameFg) || g_status.warpOn == 1; }
// (and while frame warp waits -- the game's frame rate outside its range, no steady rate: nothing is filled, so the
// game's motion isn't prepared either. Returnal's loading stalls: this step took up to 75 ms of a 93 ms frame there.)

// slSetTag: the last depth and motion vectors a game tagged with a resource (games that pass them here rather than to
// slEvaluateFeature). Only tags that stay valid up to the upscaler call are kept.
static void CopyHud(const Tag &t, void *cmd);
static void NoteSetTags(const Tag *tags, uint32_t n, void *cmd)
{
    if (!On() || !tags || n == 0 || n > 64 || !Plaus(tags)) return;
    std::lock_guard<std::mutex> l(lk);
    for (uint32_t i = 0; i < n; i++)
    {
        const Tag &t = tags[i];
        if (t.bufType == 2) { if (g_shared.warpGameHud && t.res && Plaus(t.res) && t.res->native) CopyHud(t, cmd); continue; }
        if (t.bufType > 1) continue;
        const int k = (int)t.bufType;
        // Any life-cycle: 007 First Light tags its depth here and hands only the vectors to the upscaler call that
        // follows on the same frame. One valid only when tagged is forgotten at that upscaler call (OnEvaluate).
        if (!t.res || !Plaus(t.res) || !t.res->native) { haveSet[k] = false; continue; }
        setTag[k] = t; setRes[k] = *t.res; setTag[k].res = &setRes[k]; haveSet[k] = true;
    }
}

static void NoteConsts(const Consts &c) { std::lock_guard<std::mutex> l(lk); consts = c; haveConsts = true; }

static ID3D12Resource *Native(const Res *r, D3D12_RESOURCE_DESC *d)
{
    if (!r || !Plaus(r) || !r->native || !Plaus(r->native)) return nullptr;
    ID3D12Resource *n = nullptr;
    if (FAILED(((IUnknown *)r->native)->QueryInterface(__uuidof(ID3D12Resource), (void **)&n)) || !n) return nullptr;
    // The same GPU as the presenter's device? Compared by adapter, not by pointer: ReShade and Streamline each put a
    // device of their own in front of the driver's (gm1: the pointers differed for the same device). D3D12 makes one
    // device per adapter, so the same adapter is the same device.
    ID3D12Device *dev = nullptr;
    bool same = false;
    if (SUCCEEDED(n->GetDevice(IID_PPV_ARGS(&dev))) && dev)
    {
        const LUID a = dev->GetAdapterLuid(), b = g_device->GetAdapterLuid();
        same = a.LowPart == b.LowPart && a.HighPart == b.HighPart;
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LOGI("BFI Presenter: frame warp, the game's own motion: the game's buffers are on device %p, the presenter's is %p (%s adapter)",
                 (void *)dev, (void *)g_device, same ? "the same" : "another");
        }
        dev->Release();
    }
    if (!same) { n->Release(); return nullptr; }
    *d = n->GetDesc();
    if (d->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d->SampleDesc.Count != 1) { n->Release(); return nullptr; }
    return n;                                             // AddRef'd: the caller releases
}

static ID3D12Resource *MakeCopy(const D3D12_RESOURCE_DESC &src, const wchar_t *name)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = src.Width; rd.Height = src.Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = CopyFormat(src.Format);
    rd.SampleDesc.Count = 1;
    ID3D12Resource *r = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    r->SetName(name);
    return r;
}

static void Map(const Tag &t, UINT w, UINT h, float m[4])
{
    const bool e = t.ext.width && t.ext.height;
    m[0] = e ? (float)t.ext.width : (float)w;  m[1] = e ? (float)t.ext.height : (float)h;
    m[2] = e ? (float)t.ext.left : 0.0f;       m[3] = e ? (float)t.ext.top : 0.0f;
}

static const char *HudModeName(uint32_t m)
{
    return m == 1 ? "HDR10 frame against a linear scRGB picture" : m == 2 ? "sRGB frame against a linear picture"
         : m == 3 ? "by structure (tone mapping and post-processing between them allowed)" : "the same encoding";
}

static DXGI_FORMAT ColorView(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return f;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The HUD-less picture: copied now on the game's list when there is one (lk held). Without a list, kept to be read at
// the Present if it stays valid until then.
static void CopyHud(const Tag &t, void *cmd)
{
    if (FgUnused()) return;
    if (hudState.load() < 0 || !g_device) return;
    if (!cmd)
    {
        if (t.lifecycle == 1) { hudLiveTag = t; hudLiveRes = *t.res; hudLiveTag.res = &hudLiveRes; hudLive = true; }
        return;
    }
    D3D12_RESOURCE_DESC hd = {};
    ID3D12Resource *src = Native(t.res, &hd);
    if (!src) return;
    ID3D12GraphicsCommandList *cl = nullptr;
    const uint32_t st = t.res->state;
    if (st == UINT_MAX || ColorView(hd.Format) == DXGI_FORMAT_UNKNOWN ||
        FAILED(((IUnknown *)cmd)->QueryInterface(__uuidof(ID3D12GraphicsCommandList), (void **)&cl)) || !cl)
    { src->Release(); return; }
    if (!hudCopy || hudW != (UINT)hd.Width || hudH != hd.Height || hudFmt != hd.Format)
    {
        if (hudCopy) retired.push_back({ hudCopy, Now() });
        hudCopy = MakeCopy(hd, L"BFI frame warp: the game's HUD-less picture");
        hudW = (UINT)hd.Width; hudH = hd.Height; hudFmt = hd.Format;
    }
    if (hudCopy)
    {
        D3D12_RESOURCE_BARRIER b0, b1;
        const bool tr = !(st & D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (tr) { b0 = Transition(src, (D3D12_RESOURCE_STATES)st, D3D12_RESOURCE_STATE_COPY_SOURCE); cl->ResourceBarrier(1, &b0); }
        D3D12_TEXTURE_COPY_LOCATION dst = {}, sl = {};
        dst.Type = sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = hudCopy; sl.pResource = src;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &sl, nullptr);
        if (tr) { b1 = Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, (D3D12_RESOURCE_STATES)st); cl->ResourceBarrier(1, &b1); }
        Map(t, hudW, hudH, hudMap);
        hudCopyT = Now();
        hudLive = false;
        hudFromOut = false; hudRealT = hudCopyT;
    }
    cl->Release();
    src->Release();
}

// DLSS's output as the stand-in for a HUD-less picture (FrameWarpHudFromDlss): copied on the game's list right after the
// DLSS call recorded on it (ngx::hk_Eval). Not while the game tags a real HUD-less picture.
static void CopyOut(void *cmd, ID3D12Resource *src)
{
    if (!g_shared.hudFromDlss || !g_shared.warpGameHud || hudState.load() < 0 || !g_device || !cmd || !src) return;
    std::lock_guard<std::mutex> l(lk);
    if (Now() - hudRealT < 1.0) return;
    const D3D12_RESOURCE_DESC hd = src->GetDesc();
    if (hd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || ColorView(hd.Format) == DXGI_FORMAT_UNKNOWN) return;
    ID3D12GraphicsCommandList *cl = nullptr;
    if (FAILED(((IUnknown *)cmd)->QueryInterface(__uuidof(ID3D12GraphicsCommandList), (void **)&cl)) || !cl) return;
    if (!hudCopy || hudW != (UINT)hd.Width || hudH != hd.Height || hudFmt != hd.Format)
    {
        if (hudCopy) retired.push_back({ hudCopy, Now() });
        hudCopy = MakeCopy(hd, L"BFI frame warp: DLSS's output (for the HUD mask)");
        hudW = (UINT)hd.Width; hudH = hd.Height; hudFmt = hd.Format;
    }
    if (hudCopy)
    {
        const D3D12_RESOURCE_STATES st = (D3D12_RESOURCE_STATES)g_shared.dlssOutState;
        D3D12_RESOURCE_BARRIER b = Transition(src, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->ResourceBarrier(1, &b);
        cl->CopyResource(hudCopy, src);
        b = Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
        cl->ResourceBarrier(1, &b);
        hudMap[0] = (float)hudW; hudMap[1] = (float)hudH; hudMap[2] = hudMap[3] = 0.0f;
        hudCopyT = Now();
        hudLive = false;
        if (!hudFromOut)
        {
            static int logged = 0;
            if (logged++ < 4)
                LOGI("BFI Presenter: frame warp, the game's own motion: no HUD-less picture from the game -- DLSS's output (%ux%u, format %d) "
                     "is compared with the frame by structure for the HUD mask", hudW, hudH, (int)hudFmt);
        }
        hudFromOut = true;
    }
    cl->Release();
}

static void Feed(const Tag *const tg[2], const Consts &consts, void *cmd, int from);
static std::atomic<int> via{0};                          // where the game's motion comes from: 0 Streamline, 1 DLSS (NGX)

// Step 1, from the slEvaluateFeature hook (DLSS), before the call goes on.
static void OnEvaluate(const Base **in, uint32_t n, void *cmd)
{
    if (!On() || !g_device || !cmd) return;
    const Tag *tg[2] = {}, *hl = nullptr, *outTag = nullptr;
    if (in && n && n <= 64 && Plaus(in))
        for (uint32_t i = 0; i < n; i++)
            for (const Base *b = in[i]; b && Plaus(b); b = (const Base *)b->next)
                if (memcmp(b->type, slp::kTagGuid, 16) == 0)
                {
                    const Tag *t = (const Tag *)b;
                    if (t->bufType <= 2 && t->res) { if (t->bufType == 2) hl = t; else tg[t->bufType] = t; }
                    else if (t->bufType == 4 && t->res) outTag = t;
                }
    // The upscaler's output used only in part, wider than tall against the texture: a letterboxed cutscene.
    if (outTag && Plaus(outTag->res) && outTag->ext.width && outTag->ext.height && outTag->res->width && outTag->res->height)
    {
        const Res *r = outTag->res;
        NoteLetterbox(outTag->ext.height < 0.9 * r->height && outTag->ext.width >= 0.9 * r->width,
                      outTag->ext.width, outTag->ext.height, r->width, r->height, "Streamline");
    }
    std::lock_guard<std::mutex> l(lk);
    if (hl && g_shared.warpGameHud && Plaus(hl->res) && hl->res->native) CopyHud(*hl, cmd);
    for (int k = 0; k < 2; k++) if (!tg[k] && haveSet[k]) tg[k] = &setTag[k];
    // slSetTag's tags valid only when tagged (life-cycle 0): for this upscaler call only
    struct Forget { ~Forget() { for (int k = 0; k < 2; k++) if (haveSet[k] && setTag[k].lifecycle == 0) haveSet[k] = false; } } forget;
    if (!tg[1]) { SetState(-1, "the game hands its upscaler no motion vectors that last to the upscaler call -- the picture's own motion is used"); return; }
    if (!haveConsts) { SetState(-1, "no camera constants from the game -- the picture's own motion is used"); return; }
    if (consts.cameraMotionIncluded != 1)
    { SetState(-1, "the game's motion vectors leave out the camera's motion (not supported in this test) -- the picture's own motion is used"); return; }
    Feed(tg, consts, cmd, 0);
}

// The copies and the ring entry, from either source (lk held): Streamline's tags, or the parameters of a direct DLSS
// call (NGX) put into the same shape. via: 0 Streamline, 1 DLSS (NGX).
static void Feed(const Tag *const tg[2], const Consts &consts, void *cmd, int from)
{
    if (FgUnused()) return;
    if (!retired.empty())
    {
        const double tn = Now();
        for (size_t i = 0; i < retired.size();)
            if (tn - retired[i].second > 5.0) { retired[i].first->Release(); retired[i] = retired.back(); retired.pop_back(); }
            else i++;
    }
    ID3D12GraphicsCommandList *cl = nullptr;
    if (FAILED(((IUnknown *)cmd)->QueryInterface(__uuidof(ID3D12GraphicsCommandList), (void **)&cl)) || !cl)
    { SetState(-1, "the upscaler call has no D3D12 command list -- the picture's own motion is used"); return; }
    D3D12_RESOURCE_DESC md = {}, dd = {};
    ID3D12Resource *mv = Native(tg[1]->res, &md);
    ID3D12Resource *dp = (tg[0] && !depthFailed) ? Native(tg[0]->res, &dd) : nullptr;
    if (dp && DepthView(dd.Format) == DXGI_FORMAT_UNKNOWN) { dp->Release(); dp = nullptr; }
    if (!mv || MvView(md.Format) == DXGI_FORMAT_UNKNOWN)
    {
        // A second, occasional upscaler call with 8-bit "vectors" (Rift Apart, Spider-Man 2): skipped quietly while
        // the main one works.
        if (mv && state.load() > 0) { mv->Release(); if (dp) dp->Release(); cl->Release(); return; }
        SetState(-1, mv ? "the game's motion vectors are in a format not supported (%d) -- the picture's own motion is used"
                        : "the game's motion vectors are not a D3D12 texture on the presenter's GPU -- the picture's own motion is used", (int)md.Format);
        if (mv) mv->Release();
        if (dp) dp->Release();
        cl->Release();
        return;
    }
    const uint32_t mvState = tg[1]->res->state, dState = dp ? tg[0]->res->state : 0;
    if (mvState == UINT_MAX || (dp && dState == UINT_MAX))
    {
        SetState(-1, "the game didn't give its buffers' states -- the picture's own motion is used");
        mv->Release(); if (dp) dp->Release(); cl->Release();
        return;
    }
    if (ringDev != g_device)
    {
        // A new device: the copies of the old one go (kept across swap chain resizes otherwise -- a copy the game has
        // recorded on a list it hasn't submitted yet must still exist when that list runs).
        for (int i = 0; i < kRing; i++) { SafeRelease(ring[i].mv); SafeRelease(ring[i].depth); ring[i] = Entry(); }
        SafeRelease(hudCopy); hudW = hudH = 0; hudFmt = DXGI_FORMAT_UNKNOWN; hudLive = false; hudModesSet = false;
        for (auto &r : retired) r.first->Release();
        retired.clear();
        pendN = 0; latest = -1; depthFailed = false;
        ringDev = g_device;
    }
    {
        // The main view: some games upscale more than one view a frame (Indiana Jones on Vulkan: a small 640x360 one
        // next to the main view). Only the largest is used, so the warp never mixes two views' motion and the copies
        // don't change size from call to call (remade on almost every call, the replaced ones filled video memory).
        static uint64_t mainArea = 0; static double mainSeen = -1.0; static UINT mainW = 0, mainH = 0; static int sideLogged = 0;
        const double tn = Now();
        const uint64_t area = md.Width * (uint64_t)md.Height;
        if (area < mainArea && tn - mainSeen < 0.5) { mv->Release(); if (dp) dp->Release(); cl->Release(); return; }
        if (mainArea && area > mainArea && sideLogged++ < 2)
            LOGI("BFI Presenter: frame warp, the game's own motion: the game upscales more than one view (%ux%u and %ux%u) -- only the "
                 "largest is used", mainW, mainH, (UINT)md.Width, md.Height);
        mainArea = area; mainSeen = tn; mainW = (UINT)md.Width; mainH = md.Height;
        // replaced copies wait 5 s; if they pile up anyway (sizes changing all the time) the call is skipped
        if (retired.size() > 16) { mv->Release(); if (dp) dp->Release(); cl->Release(); return; }
    }
    const int k = (int)(seq % kRing);
    Entry &e = ring[k];
    if (!e.mv || e.mvW != (UINT)md.Width || e.mvH != md.Height || e.mvFmt != md.Format)
    {
        if (e.mv) retired.push_back({ e.mv, Now() });
        e.mv = MakeCopy(md, L"BFI frame warp: game motion vectors");
        e.mvW = (UINT)md.Width; e.mvH = md.Height; e.mvFmt = md.Format; e.gen = genNext++;
    }
    if (dp && (!e.depth || e.dW != (UINT)dd.Width || e.dH != dd.Height || e.dFmt != dd.Format))
    {
        if (e.depth) retired.push_back({ e.depth, Now() });
        e.depth = MakeCopy(dd, L"BFI frame warp: game depth");
        e.dW = (UINT)dd.Width; e.dH = dd.Height; e.dFmt = dd.Format; e.gen = genNext++;
        if (!e.depth)
        {
            depthFailed = true;
            LOGW("BFI Presenter: frame warp, the game's own motion: no copy of the game's depth (format %d) -- motion vectors only", (int)dd.Format);
        }
    }
    if (!e.mv) { SetState(-1, "no copy of the game's motion vectors could be made"); mv->Release(); if (dp) dp->Release(); cl->Release(); return; }
    if (dp && !e.depth) { dp->Release(); dp = nullptr; }

    // The copies, on the game's list. A buffer not in a state it can be copied from is moved there and back.
    D3D12_RESOURCE_BARRIER pre[2], post[2]; UINT nb = 0;
    if (!(mvState & D3D12_RESOURCE_STATE_COPY_SOURCE))
    { pre[nb] = Transition(mv, (D3D12_RESOURCE_STATES)mvState, D3D12_RESOURCE_STATE_COPY_SOURCE);
      post[nb] = Transition(mv, D3D12_RESOURCE_STATE_COPY_SOURCE, (D3D12_RESOURCE_STATES)mvState); nb++; }
    if (dp && !(dState & D3D12_RESOURCE_STATE_COPY_SOURCE))
    { pre[nb] = Transition(dp, (D3D12_RESOURCE_STATES)dState, D3D12_RESOURCE_STATE_COPY_SOURCE);
      post[nb] = Transition(dp, D3D12_RESOURCE_STATE_COPY_SOURCE, (D3D12_RESOURCE_STATES)dState); nb++; }
    if (nb) cl->ResourceBarrier(nb, pre);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.Type = src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = e.mv; src.pResource = mv;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (dp) { dst.pResource = e.depth; src.pResource = dp; cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); }   // subresource 0: the depth plane
    if (nb) cl->ResourceBarrier(nb, post);
    cl->Release();

    Map(*tg[1], e.mvW, e.mvH, e.mvMap);
    if (dp) Map(*tg[0], e.dW, e.dH, e.dMap);
    e.mvScale[0] = consts.mvecScale[0]; e.mvScale[1] = consts.mvecScale[1];
    e.mvInvalid = consts.mvInvalid;
    e.depthRev = consts.depthInverted == 1;
    e.hasDepth = dp != nullptr;
    e.reset = consts.reset == 1;
    mv->Release(); if (dp) dp->Release();
    if (pendN >= 2) { pend[0] = pend[1]; pendN = 1; }      // a Present that never came: the older copy is dropped
    pend[pendN++] = k;
    latest = k;
    evalsSec++;
    seq++;
    via = from;
    const char *srcName = from == 1 ? "DLSS (NGX)" : "Streamline";
    if (g_status.fgActive)
        SetState(e.hasDepth ? 1 : 2, "the game's motion vectors (%ux%u)%s from %s are used, with frame generation: each shown frame moves by 1/%.0f of a rendered frame's motion",
                 e.mvW, e.mvH, e.hasDepth ? " and depth" : "", srcName, (double)(fgK >= 2 ? fgK : 2));
    else if (e.hasDepth)
        SetState(1, "the game's motion vectors (%ux%u) and depth (%ux%u) from %s are used", e.mvW, e.mvH, e.dW, e.dH, srcName);
    else
        SetState(2, "the game's motion vectors (%ux%u) from %s are used, without depth", e.mvW, e.mvH, srcName);
}

static bool EnsureGpu()
{
    if (pso) return true;
    if (!g_shDone || !g_shBlob[0] || !g_shBlob[kShWarpGame] || !g_device) return false;
    static bool failed = false;
    if (failed) return false;
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 7;
    range.BaseShaderRegister = 5;                          // t5 the vectors, t6 the depth, t7 the HUD mask, t8-t10 its pass's inputs,
                                                           // t11 the vectors of the rendered frame before
    D3D12_ROOT_PARAMETER p[2] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.ShaderRegister = 1;                     // b1
    p[0].Constants.Num32BitValues = sizeof(GConsts) / 4;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &range;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2; rsd.pParameters = p;
    ID3DBlob *blob = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) &&
              SUCCEEDED(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    SafeRelease(blob); SafeRelease(err);
    if (ok)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rs;
        pd.VS = { g_shBlob[0]->GetBufferPointer(), g_shBlob[0]->GetBufferSize() };
        pd.PS = { g_shBlob[kShWarpGame]->GetBufferPointer(), g_shBlob[kShWarpGame]->GetBufferSize() };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count = 1;
        ok = SUCCEEDED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
        if (ok && g_shBlob[kShWarpHud])
        {
            pd.PS = { g_shBlob[kShWarpHud]->GetBufferPointer(), g_shBlob[kShWarpHud]->GetBufferSize() };
            pd.NumRenderTargets = 2;
            pd.RTVFormats[0] = pd.RTVFormats[1] = DXGI_FORMAT_R8_UNORM;
            pd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoHud)))) psoHud = nullptr;   // optional
        }
    }
    if (ok)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        // one block of 6 views per staging slot, written at that slot's Present (its previous use is finished then:
        // the game's side waits for the slot's last copy list before reusing it)
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 7 * kStage; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ok = SUCCEEDED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kStage + 3; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ok = ok && SUCCEEDED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
        srvStride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtvStride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    if (!ok)
    {
        failed = true;
        SafeRelease(psoHud); SafeRelease(pso); SafeRelease(rs); SafeRelease(srvHeap); SafeRelease(rtvHeap);
        LOGW("BFI Presenter: frame warp, the game's own motion: its pass could not be created -- the picture's own motion is used");
    }
    return ok;
}

// The motion textures: one vector per pixel of the game's own vectors, at most half the game frame's size (gm1-gm4
// had one per 4x4 pixels of the frame, coarser than the game's vectors: thin things -- window frames, the edges of
// columns -- broke into steps and bent).
static bool EnsureFlow(UINT mvW, UINT mvH)
{
    if (!g_stage[0]) return false;
    const D3D12_RESOURCE_DESC sd = g_stage[0]->GetDesc();
    const UINT w = std::max(64u, std::min(mvW, ((UINT)sd.Width + 1) / 2)), h = std::max(64u, std::min(mvH, (sd.Height + 1) / 2));
    {
        // Retired textures released 3 s on: no queue reads them by then.
        std::lock_guard<std::mutex> l(flowLk);
        const double tn = Now();
        for (size_t i = 0; i < flowRetired.size();)
            if (tn - flowRetired[i].t > 3.0) { flowRetired[i].r->Release(); flowRetired[i] = flowRetired.back(); flowRetired.pop_back(); }
            else i++;
    }
    if (flow[0] && flowW == w && flowH == h) return true;
    {
        std::lock_guard<std::mutex> l(flowLk);
        const double tn = Now();
        for (UINT i = 0; i < kStage; i++)
        {
            if (flow[i]) flowRetired.push_back({ flow[i], tn });
            flow[i] = nullptr; slotPend[i] = slotOk[i] = false; slotId[i] = 0;
        }
        ID3D12Resource **hr0[3] = { &hudRaw[0], &hudRaw[1], &hudMask };
        for (auto r : hr0) { if (*r) flowRetired.push_back({ *r, tn }); *r = nullptr; }
        hudMaskOk = false;
        flowGen++;                                        // the presenter's views of the old ones are out of date
    }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *nf[kStage] = {};
    for (UINT i = 0; i < kStage; i++)
    {
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&nf[i]))))
        {
            for (UINT j = 0; j < kStage; j++) SafeRelease(nf[j]);
            return false;
        }
        nf[i]->SetName(L"BFI frame warp: the game's motion");
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rh.ptr += (SIZE_T)i * rtvStride;
        g_device->CreateRenderTargetView(nf[i], nullptr, rh);
    }
    {
        std::lock_guard<std::mutex> l(flowLk);
        for (UINT i = 0; i < kStage; i++) flow[i] = nf[i];
    }
    // the HUD mask's textures (optional)
    rd.Format = DXGI_FORMAT_R8_UNORM;
    ID3D12Resource **hr[3] = { &hudRaw[0], &hudRaw[1], &hudMask };
    for (int i = 0; i < 3; i++)
    {
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(hr[i]))))
        { SafeRelease(hudRaw[0]); SafeRelease(hudRaw[1]); SafeRelease(hudMask); break; }
        (*hr[i])->SetName(L"BFI frame warp: HUD mask");
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rh.ptr += (SIZE_T)(kStage + i) * rtvStride;
        g_device->CreateRenderTargetView(*hr[i], nullptr, rh);
    }
    {
        std::lock_guard<std::mutex> l(flowLk);
        flowW = w; flowH = h;
        flowGen++;
    }
    return true;
}

// Step 2, on the copy list that has just copied the game's frame into staging slot s (before it is closed).
static void AtPresent(ID3D12GraphicsCommandList *list, UINT s)
{
    if (s >= kStage) return;
    slotId[s] = 0; slotPend[s] = slotOk[s] = false;
    if (!On() || g_status.warpOn == 1) { std::lock_guard<std::mutex> l(lk); pendN = 0; latest = -1; return; }
    Entry e, ep;
    int k;
    float scale = 1.0f, lag = 0.0f;
    {
        std::lock_guard<std::mutex> l(lk);
        presSec++;
        const double tn = Now();
        if (tn - secT >= 1.0)
        {
            if (evalsSec > 0 && presSec > 0)
            {
                perEval = std::max(1.0, std::min(4.0, (double)presSec / evalsSec));
                // A whole number, taken once two seconds in a row agree (gm4 used a running average, which started at
                // 1 and wandered: with 1 against a real 2 a filled flash moved twice as far and snapped back).
                const int kk = (int)std::floor(perEval + 0.5);
                if (kk == fgKNext) { if (++fgKSecs >= 2) fgK = kk; }
                else { fgKNext = kk; fgKSecs = 1; }
            }
            evalsSec = presSec = 0; secT = tn;
        }
        if (g_status.fgActive)
        {
            // Frame generation: two or three frames are shown per rendered one, and the game's vectors are a rendered
            // frame's motion. Every shown frame uses the newest vectors, for the share of that motion one shown frame
            // covers. (A generated frame sits between two rendered ones: its picture is a little off from the vectors'.)
            pendN = 0;
            if (!g_shared.warpGameFg) return;             // the game's motion isn't used with frame generation (the default)
            if (latest < 0 || fgK < 2) return;
            // DLSS frame generation shows the generated frames first, then the rendered one the latest vectors are for:
            // the i-th Present since the upscaler call is fgK-1-i shown frames behind it.
            if (seq != fgSeqSeen) { fgSeqSeen = seq; fgIdx = 0; } else if (fgIdx < 8) fgIdx++;
            const int lf = g_shared.warpFgLag;
            lag = lf > 0 ? (float)std::max(0, fgK - 1 - fgIdx) : lf < 0 ? -(float)std::min(fgIdx, fgK - 1) : 0.0f;            // not settled yet (or not really generating): the picture's motion
            k = latest;
            scale = 1.0f / (float)fgK;
        }
        else
        {
            if (pendN == 0) return;                       // no upscaler call since the last Present (a menu, DLSS off)
            k = pend[0];
            for (int i = 1; i < pendN; i++) pend[i - 1] = pend[i];
            pendN--;
        }
        e = ring[k];
        ep = ring[(k + kRing - 1) % kRing];
    }
    if (!e.mv || !EnsureGpu() || !EnsureFlow(e.mvW, e.mvH)) return;
    // the rendered frame before's vectors (how much the motion kept up), where they are the same kind
    const bool prevOk = ep.mv && ep.mv != e.mv && ep.mvW == e.mvW && ep.mvH == e.mvH && ep.mvFmt == e.mvFmt && !e.reset;
    // The HUD-less picture for the mask: a copy made this frame (or a few frames ago), or the game's own buffer when it
    // stays valid until the Present.
    ID3D12Resource *hl = nullptr; uint32_t hlState = 0; DXGI_FORMAT hlFmt = DXGI_FORMAT_UNKNOWN; float hlMap[4] = {};
    bool hlLive = false;
    {
        std::lock_guard<std::mutex> l(lk);
        if (g_shared.warpGameHud && hudState.load() >= 0 && psoHud && hudMask)
        {
            if (hudCopy && Now() - hudCopyT < 0.25) { hl = hudCopy; hl->AddRef(); hlState = D3D12_RESOURCE_STATE_COPY_DEST; hlFmt = hudFmt; memcpy(hlMap, hudMap, sizeof(hlMap)); }
            else if (hudLive)
            {
                D3D12_RESOURCE_DESC d = {};
                if ((hl = Native(hudLiveTag.res, &d)) != nullptr)
                {
                    hlState = hudLiveRes.state; hlFmt = d.Format; hlLive = true; Map(hudLiveTag, (UINT)d.Width, d.Height, hlMap);
                    if (hlState == UINT_MAX || ColorView(hlFmt) == DXGI_FORMAT_UNKNOWN) { hl->Release(); hl = nullptr; }
                }
            }
        }
    }
    // The frame the HUD-less picture is compared with: from before ReShade's effects where it is kept (FrameWarpStillBeforeEffects,
    // 1.0.48) -- a CRT shader's mask, grain and glow differ from the HUD-less picture everywhere -- else the finished frame.
    ID3D12Resource *hFrame = pfx::Peek12();
    if (!hFrame) hFrame = g_stage[s];
    // the views for this slot: t5 vectors, t6 depth, t7 HUD mask, t8 the frame, t9 the HUD-less picture, t10 the raw mask before
    D3D12_CPU_DESCRIPTOR_HANDLE sh = srvHeap->GetCPUDescriptorHandleForHeapStart();
    sh.ptr += (SIZE_T)(7 * s) * srvStride;
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        auto at = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = sh; h.ptr += (SIZE_T)i * srvStride; return h; };
        sv.Format = MvView(e.mvFmt);
        g_device->CreateShaderResourceView(e.mv, &sv, at(0));
        g_device->CreateShaderResourceView(prevOk ? ep.mv : nullptr, &sv, at(6));
        sv.Format = e.hasDepth ? DepthView(e.dFmt) : DXGI_FORMAT_R32_FLOAT;
        g_device->CreateShaderResourceView(e.hasDepth ? e.depth : nullptr, &sv, at(1));
        sv.Format = DXGI_FORMAT_R8_UNORM;
        g_device->CreateShaderResourceView(hudMask, &sv, at(2));
        sv.Format = StageFormat();
        g_device->CreateShaderResourceView(hFrame, &sv, at(3));
        sv.Format = hl ? ColorView(hlFmt) : DXGI_FORMAT_R8G8B8A8_UNORM;
        g_device->CreateShaderResourceView(hl, &sv, at(4));
        sv.Format = DXGI_FORMAT_R8_UNORM;
        g_device->CreateShaderResourceView(hudRaw[1 - hudCur], &sv, at(5));
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gh = srvHeap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += (UINT64)(7 * s) * srvStride;
    list->SetDescriptorHeaps(1, &srvHeap);
    list->SetGraphicsRootSignature(rs);
    list->SetGraphicsRootDescriptorTable(1, gh);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)flowW, (float)flowH, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)flowW, (LONG)flowH };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    GConsts gc = {};
    gc.outTexel[0] = 1.0f / (float)flowW; gc.outTexel[1] = 1.0f / (float)flowH;
    gc.mvScale[0] = e.mvScale[0] * (float)g_shared.warpGameSign * scale; gc.mvScale[1] = e.mvScale[1] * (float)g_shared.warpGameSign * scale;
    memcpy(gc.mvMap, e.mvMap, sizeof(gc.mvMap)); memcpy(gc.dMap, e.dMap, sizeof(gc.dMap));
    gc.mvInvalid = e.mvInvalid;
    gc.pad[0] = scale > 0.0f ? 1.0f / scale : 1.0f;        // shown frames per rendered one: back to a whole rendered frame's motion
    gc.pad[1] = prevOk ? 1.0f : 0.0f;
    const bool linear = StageFormat() == DXGI_FORMAT_R16G16B16A16_FLOAT;
    gc.flags = (e.depthRev ? 1u : 0u) | (e.hasDepth ? 2u : 0u) | (linear ? 8u : 0u) | ((uint32_t)g_shared.warpDilate << 16);
    memcpy(gc.hMap, hlMap, sizeof(gc.hMap));
    if (hl && !hudModesSet)
    {
        // the order to try: a float picture against a 10- or 8-bit frame is most likely HDR10 / sRGB against linear
        const DXGI_FORMAT v = ColorView(hlFmt), f = StageFormat();
        const bool hlFloat = v == DXGI_FORMAT_R16G16B16A16_FLOAT || v == DXGI_FORMAT_R32G32B32A32_FLOAT || v == DXGI_FORMAT_R11G11B10_FLOAT;
        const bool fFloat = f == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (hlFloat && !fFloat) { hudModes[0] = f == DXGI_FORMAT_R10G10B10A2_UNORM ? 1 : 2; hudModes[1] = hudModes[0] == 1 ? 2 : 1; hudModes[2] = 0; }
        else { hudModes[0] = 0; hudModes[1] = 1; hudModes[2] = 2; }
        hudModes[3] = 3;
        hudModeI = 0; hudModesSet = true;
    }
    gc.hudMode = (uint32_t)hudModes[std::min(hudModeI.load(), hudModeN - 1)];
    if (hl && hudFromOut && !hlLive) gc.hudMode = 3;        // DLSS's output: by structure
    gc.hudThr = (linear || gc.hudMode != 0) ? 0.08f : 0.04f;

    // 1. The HUD mask (when the game gives a HUD-less picture)
    if (hl)
    {
        D3D12_RESOURCE_BARRIER hb[4]; UINT nh = 0;
        hb[nh++] = Transition(hFrame, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const bool hlTr = !(hlState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (hlTr) hb[nh++] = Transition(hl, (D3D12_RESOURCE_STATES)hlState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hb[nh++] = Transition(hudRaw[hudCur], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        hb[nh++] = Transition(hudMask, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(nh, hb);
        list->SetPipelineState(psoHud);
        list->SetGraphicsRoot32BitConstants(0, sizeof(GConsts) / 4, &gc, 0);
        D3D12_CPU_DESCRIPTOR_HANDLE rt[2] = { rtvHeap->GetCPUDescriptorHandleForHeapStart(), rtvHeap->GetCPUDescriptorHandleForHeapStart() };
        rt[0].ptr += (SIZE_T)(kStage + hudCur) * rtvStride;
        rt[1].ptr += (SIZE_T)(kStage + 2) * rtvStride;
        list->OMSetRenderTargets(2, rt, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        nh = 0;
        hb[nh++] = Transition(hFrame, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (hlTr) hb[nh++] = Transition(hl, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, (D3D12_RESOURCE_STATES)hlState);
        hb[nh++] = Transition(hudRaw[hudCur], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hb[nh++] = Transition(hudMask, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(nh, hb);
        hudCur = 1 - hudCur;
        hudMaskOk = true;
        if (hudState.load() == 0)
        {
            hudState = 1;
            LOGI("BFI Presenter: frame warp, the game's own motion: the HUD is kept still with %s (%ux%u, format %d, "
                 "the frame's %d, %s); compared as: %s", gc.hudMode == 3 ? "DLSS's output" : "the game's HUD-less picture",
                 (unsigned)hlMap[0], (unsigned)hlMap[1], (int)hlFmt, (int)StageFormat(),
                 hlLive ? "read at the Present" : "copied when tagged", HudModeName(gc.hudMode));
        }
        hl->Release();
    }
    else hudMaskOk = false;
    if (hudMaskOk) gc.flags |= 4u;

    // 2. The motion texture
    D3D12_RESOURCE_BARRIER b[4] = {
        Transition(flow[s], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(e.mv, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) };
    UINT nb = 2;
    if (e.hasDepth) b[nb++] = Transition(e.depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (prevOk) b[nb++] = Transition(ep.mv, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(nb, b);
    list->SetPipelineState(pso);
    list->SetGraphicsRoot32BitConstants(0, sizeof(GConsts) / 4, &gc, 0);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rh.ptr += (SIZE_T)s * rtvStride;
    list->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    nb = 0;
    b[nb++] = Transition(flow[s], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    b[nb++] = Transition(e.mv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (e.hasDepth) b[nb++] = Transition(e.depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (prevOk) b[nb++] = Transition(ep.mv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(nb, b);
    slotPend[s] = true; slotOk[s] = !e.reset; slotDepth[s] = e.hasDepth; slotCut[s] = e.reset; slotLag[s] = lag;
}

// After the frame in slot s is published as game frame id.
static void Published(UINT s, UINT64 id)
{
    if (s >= kStage || !slotPend[s]) return;
    slotId[s] = id;
    if (slotCut[s] && cutId.load() + 1 < id) { cutId = id; cutsSec++; }   // a run of reset frames counts once
    else if (slotCut[s]) cutId = id;
}
static int CutsTake() { return cutsSec.exchange(0); }
// No frame warp for this frame: within 3 rendered frames after a camera cut, or in a letterboxed cutscene.
static bool PauseNow(UINT64 id, bool letterbox, const char **why)
{
    const UINT64 c = cutId.load();
    const UINT64 k = (UINT64)std::max(1, g_status.fgActive ? fgK : 1);
    if (c && id >= c && id < c + 3 * k) { *why = "camera cut"; return true; }
    if (letterbox && lbOn.load() && Now() - lbT.load() < 1.0 && lbFullSeen.load())
    {
        if (Now() - lbStartT.load() < 600.0) { *why = "letterboxed cutscene"; return true; }
        static double loggedFor = -1.0;
        if (loggedFor != lbStartT.load())
        {
            loggedFor = lbStartT.load();
            LOGI("BFI Presenter: frame warp: letterboxed for over 10 minutes -- taken as play, not a cutscene; frame warp works again");
        }
    }
    return false;
}

// The presenter: is frame `id` in slot s warped with the game's motion? 0 no, 1 vectors and depth, 2 vectors only.
static float SlotLag(int s) { return (s >= 0 && s < (int)kStage) ? slotLag[s] : 0.0f; }
static int SlotFor(int s, UINT64 id)
{
    if (s < 0 || s >= (int)kStage || !id || slotId[s].load() != id || !slotOk[s]) return 0;
    return slotDepth[s] ? 1 : 2;
}

// The presenter: shader-resource views of the motion textures, into its own heap (when they were made anew).
static UINT WriteFlowSrvs(D3D12_CPU_DESCRIPTOR_HANDLE base, UINT stride)
{
    std::lock_guard<std::mutex> l(flowLk);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    for (UINT i = 0; i < kStage; i++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = base; h.ptr += (SIZE_T)i * stride;
        g_device->CreateShaderResourceView(flow[i], &sv, h);   // a null view while there is none
    }
    return flowGen.load();
}
// The slot's motion texture if the views the presenter wrote (generation gen) are still for it; else none this refresh.
static ID3D12Resource *FlowFor(int s, UINT gen)
{
    std::lock_guard<std::mutex> l(flowLk);
    return s >= 0 && s < (int)kStage && flowGen.load() == gen ? flow[s] : nullptr;
}
static UINT FlowGenNow() { return flowGen.load(); }
static ID3D12Resource *FlowRes(int s) { return s >= 0 && s < (int)kStage ? flow[s] : nullptr; }
static int StateNow(char *w, size_t n) { std::lock_guard<std::mutex> l(lk); snprintf(w, n, "%s", why); return state.load(); }
static int HudNow() { return hudState.load(); }
static int ViaNow() { return via.load(); }
static int StateFast() { return state.load(); }
// The check found the mask covering much of the picture: the HUD-less picture doesn't match the frame shown as it is
// compared now. The next way of comparing is tried; after the last, the mask is switched off for the session (effects
// drawn after the HUD-less picture -- ReShade's included -- or an encoding none of the modes undo).
static void HudBad(double pct)
{
    if (hudState.load() < 0) return;
    if (hudFromOut)
    {
        hudState = -1;
        LOGW("BFI Presenter: frame warp, the game's own motion: HUD protection off -- compared by structure with DLSS's output, the mask "
             "covered %.0f%% of the frame (the game's post-processing, or ReShade's effects?)", pct);
        return;
    }
    const int i = hudModeI.load();
    if (i + 1 < hudModeN)
    {
        hudModeI = i + 1;
        LOGI("BFI Presenter: frame warp, the game's own motion: the HUD mask covered %.0f%% compared as %s -- now trying: %s",
             pct, HudModeName((uint32_t)hudModes[i]), HudModeName((uint32_t)hudModes[i + 1]));
        return;
    }
    hudState = -1;
    LOGW("BFI Presenter: frame warp, the game's own motion: HUD protection off -- the game's HUD-less picture differs from the frame "
         "shown over %.0f%% of it in every way of comparing tried (effects drawn after it, ReShade's included?)", pct);
}

// With the frame resources (the swap chain resized or gone): the motion textures and the pass. The copies of the
// game's buffers stay (see OnEvaluate) until the device changes.
static void Release()
{
    std::lock_guard<std::mutex> l(lk);
    {
        std::lock_guard<std::mutex> l2(flowLk);
        for (UINT i = 0; i < kStage; i++) { SafeRelease(flow[i]); slotPend[i] = slotOk[i] = false; slotId[i] = 0; }
        for (auto &r : flowRetired) r.r->Release();
        flowRetired.clear();
    }
    flowW = flowH = 0;
    flowGen++;
    SafeRelease(hudRaw[0]); SafeRelease(hudRaw[1]); SafeRelease(hudMask); hudMaskOk = false;
    pendN = 0; latest = -1; haveSet[0] = haveSet[1] = false; hudLive = false;
    SafeRelease(psoHud); SafeRelease(pso); SafeRelease(rs); SafeRelease(srvHeap); SafeRelease(rtvHeap);
}
} // namespace gmo

// ---------------------------------------------------------------------------------------------------------
// The game's own motion from DLSS called directly (NGX, test): games that call DLSS without Streamline (most Unreal
// Engine games, Townfall) hand it their motion vectors and depth in an NGX parameter object at
// NVSDK_NGX_D3D12_EvaluateFeature. Those exports of the driver's _nvngx.dll (or a nvngx.dll in the game's folder) are
// hooked; at each DLSS call (super resolution or ray reconstruction) the parameters are read and put into the shape
// Streamline's tags have, and gmo::Feed copies the buffers as for Streamline. A DLSS call made by Streamline itself is
// left to the Streamline hook. Layouts and names follow the DLSS SDK headers (nvsdk_ngx.h, nvsdk_ngx_defs.h,
// nvsdk_ngx_params.h).
namespace ngx
{
using slp::Plaus; using slp::Readable;
struct Handle { unsigned int id; };
typedef int (*PFN_Create)(void *cl, int feature, void *params, Handle **out);
typedef int (*PFN_Eval)(void *cl, const Handle *h, const void *params, void *cb);
typedef int (*PFN_Get)(const void *self, const char *name, void *out);
// hooked in up to two modules: a nvngx.dll in the game's folder (OptiScaler) and the driver's _nvngx.dll
static PFN_Create o_Creates[2] = {};
static PFN_Eval o_Eval[2] = {}, o_EvalC[2] = {};
static const int kSuccess = 1;                         // NVSDK_NGX_Result_Success
enum { kSR = 1, kRR = 13 };                            // NVSDK_NGX_Feature_SuperSampling, _RayReconstruction
enum { kMVLowRes = 1 << 1, kDepthInverted = 1 << 3 };  // NVSDK_NGX_DLSS_Feature_Flags
struct Feat { int feature; int flags; unsigned w, h, ow = 0, oh = 0; };
static std::mutex lk;
static std::map<uintptr_t, Feat> feats;                // by handle, from CreateFeature: the handle's address, not its id -- DLSS
                                                       // frame generation numbers its handles on its own (Directive 8020: super
                                                       // resolution and frame generation both handle 1)
static bool tried[2] = {};                             // "nvngx.dll", "_nvngx.dll"
static bool hooked = false;
static thread_local int inside = 0;                    // nested NGX call (OptiScaler calling the driver's DLSS)
static thread_local bool usedNow = false;              // this call was taken as an upscaler call (OnEval -> AfterEval)
// The parameter object is a C++ interface: 8 Set overloads, then 8 Get overloads, then Reset. MSVC puts overloads
// of one name in reverse order, so Get(ID3D12Resource**) is slot 9 there and slot 14 in declaration order. Which one
// is found once, from the first DLSS call, by asking for "MotionVectors" and checking that what comes back is a D3D12
// texture. Int and unsigned int getters are slots 11 and 12 either way round (both read a 4-byte value).
static int slotRes = 0, slotF = 0;                     // 0 = not found yet, -1 = not recognised (NGX left alone)
static uint32_t nEval = 0, nUsed = 0, nOther = 0, nUnknown = 0, nNoMv = 0, nSl = 0;
static std::map<int, uint32_t> nFeat;
static double winT = 0.0; static int reports = 0;

static int Get(const void *p, int slot, const char *name, void *out) { return ((PFN_Get)(*(void ***)p)[slot])(p, name, out); }
static bool GetU(const void *p, const char *name, unsigned &v)
{
    uint64_t x = 0;                                        // int or unsigned int: slot 11 or 12 in either order
    if (Get(p, 11, name, &x) != kSuccess && Get(p, 12, name, &x) != kSuccess) return false;
    v = (unsigned)x; return true;
}

// A D3D12 texture? Only for the one-time layout check: the pointer, its vtable and the vtable's first function are
// checked (the function must be in a loaded module) before anything is called.
static bool LooksLikeD3D12(void *p)
{
    if (!Plaus(p) || !Readable(p, 8)) return false;
    void **vt = *(void ***)p;
    if (!Plaus(vt) || !Readable(vt, 24)) return false;
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[0], &m) || !m) return false;
    ID3D12Resource *r = nullptr;
    if (FAILED(((IUnknown *)p)->QueryInterface(__uuidof(ID3D12Resource), (void **)&r)) || !r) return false;
    r->Release();
    return true;
}

static void *GetRes(const void *p, const char *name, const char *enc)
{
    uint64_t v = 0;
    if (Get(p, slotRes, name, &v) == kSuccess && v) return (void *)(uintptr_t)v;
    v = 0;
    if (enc && Get(p, slotRes, enc, &v) == kSuccess && v) return (void *)(uintptr_t)v;
    return nullptr;
}

static bool Layout(const void *p)
{
    if (slotRes) return slotRes > 0;
    const int tryRes[2] = { 9, 14 };
    for (int i = 0; i < 2 && !slotRes; i++)
    {
        for (const char *nm : { "MotionVectors", "#\x27" })
        {
            uint64_t v = 0;                                // 8 bytes: in the other order the slot is Get(float*)
            if (Get(p, tryRes[i], nm, &v) == kSuccess && v && LooksLikeD3D12((void *)(uintptr_t)v))
            { slotRes = tryRes[i]; slotF = tryRes[i] == 9 ? 14 : 9; break; }
        }
    }
    if (!slotRes)
    {
        slotRes = -1;
        LOGW("BFI Presenter: frame warp, the game's own motion: DLSS's parameter object isn't laid out as expected -- direct DLSS calls are left alone");
        return false;
    }
    LOGI("BFI Presenter: frame warp, the game's own motion: DLSS parameters read in %s order", slotRes == 9 ? "MSVC's" : "declaration");
    return true;
}

static void OnCreate(int feature, const void *params, Handle *h)
{
    if (!h || !Plaus(h) || !params || !Plaus(params)) return;
    Feat f = { feature, -1, 0, 0 };
    unsigned fl = 0;
    if (GetU(params, "DLSS.Feature.Create.Flags", fl)) f.flags = (int)fl;
    GetU(params, "Width", f.w); GetU(params, "Height", f.h); GetU(params, "OutWidth", f.ow); GetU(params, "OutHeight", f.oh);
    std::lock_guard<std::mutex> l(lk);
    feats[(uintptr_t)h] = f;
    static int logged = 0;
    if (logged++ < 8)
        LOGI("BFI Presenter: DLSS feature %d created (handle %u): render size %ux%u, flags 0x%x%s", feature, h->id, f.w, f.h, (unsigned)fl,
             f.flags < 0 ? " (not stated)" : "");
}

static void OnEval(void *cmd, const Handle *h, const void *params)
{
    if (!gmo::On() || !g_device || !cmd || !params || !Plaus(params) || !h || !Plaus(h)) return;
    Feat f = { 0, -1, 0, 0 };
    bool known;
    {
        std::lock_guard<std::mutex> l(lk);
        nEval++;
        if (slp::g_slDepth > 0) { nSl++; return; }          // Streamline's own DLSS call: the Streamline hook has it
        auto it = feats.find((uintptr_t)h);
        known = it != feats.end();
        if (known) f = it->second;
        nFeat[known ? f.feature : -1]++;
        if (known && f.feature != kSR && f.feature != kRR) { nOther++; return; }
        if (!known) nUnknown++;
        if (!Layout(params)) return;
    }
    void *mv = GetRes(params, "MotionVectors", "#\x27");
    if (!mv) { std::lock_guard<std::mutex> l(lk); nNoMv++; return; }   // not an upscaler call (frame generation has names of its own)
    void *dp = GetRes(params, "Depth", "#\x3d");
    float sx = 1.0f, sy = 1.0f;
    if (Get(params, slotF, "MV.Scale.X", &sx) != kSuccess) Get(params, slotF, "#\x2c", &sx);
    if (Get(params, slotF, "MV.Scale.Y", &sy) != kSuccess) Get(params, slotF, "#\x2d", &sy);
    unsigned reset = 0, rw = 0, rh = 0, flags = 0, mvX = 0, mvY = 0, dX = 0, dY = 0, cw = 0, ch = 0;
    GetU(params, "Reset", reset);
    GetU(params, "DLSS.Render.Subrect.Dimensions.Width", rw); GetU(params, "DLSS.Render.Subrect.Dimensions.Height", rh);
    GetU(params, "DLSS.Input.MV.Subrect.Base.X", mvX); GetU(params, "DLSS.Input.MV.Subrect.Base.Y", mvY);
    GetU(params, "DLSS.Input.Depth.Subrect.Base.X", dX); GetU(params, "DLSS.Input.Depth.Subrect.Base.Y", dY);
    if (!rw || !rh) { rw = f.w; rh = f.h; }
    if ((!rw || !rh) && GetU(params, "Width", cw) && GetU(params, "Height", ch)) { rw = cw; rh = ch; }
    bool flagsGiven = f.flags >= 0;
    if (flagsGiven) flags = (unsigned)f.flags;
    else if (GetU(params, "DLSS.Feature.Create.Flags", flags)) flagsGiven = true;
    else flags = kMVLowRes | kDepthInverted;               // not stated: as most current engines (Unreal's included)

    // The vectors' texture size, for vectors at the output size (no MVLowRes) and for a missing render size.
    UINT mvW = 0, mvH = 0;
    {
        ID3D12Resource *r = nullptr;
        if (Plaus(mv) && SUCCEEDED(((IUnknown *)mv)->QueryInterface(__uuidof(ID3D12Resource), (void **)&r)) && r)
        { const D3D12_RESOURCE_DESC d = r->GetDesc(); mvW = (UINT)d.Width; mvH = d.Height; r->Release(); }
    }
    if (!mvW) return;
    const bool low = (flags & kMVLowRes) != 0;
    if (!rw || !rh) { rw = mvW; rh = mvH; }

    // Into Streamline's shape: the extents in use, the vector scale to screen uv (DLSS's scale goes to pixels of the
    // vectors' own size), reversed depth, a camera cut. DLSS's vectors always include the camera's motion.
    slp::Res res[2] = {}; slp::Tag tag[2] = {};
    res[1].native = mv; res[1].state = (uint32_t)g_shared.dlssInState;
    tag[1].res = &res[1]; tag[1].bufType = 1;
    if (low) tag[1].ext = { mvY, mvX, rw, rh };            // top, left, width, height
    if (dp) { res[0].native = dp; res[0].state = (uint32_t)g_shared.dlssInState; tag[0].res = &res[0]; tag[0].bufType = 0; tag[0].ext = { dY, dX, rw, rh }; }
    slp::Consts c = {};
    c.mvecScale[0] = sx / (float)(low ? rw : mvW);
    c.mvecScale[1] = sy / (float)(low ? rh : mvH);
    c.depthInverted = (flags & kDepthInverted) ? 1 : 0;
    c.cameraMotionIncluded = 1;
    c.reset = reset ? 1 : 0;
    // DLSS's output smaller than the screen, wider than tall against it: a letterboxed cutscene (Townfall: 3840x1634).
    if (known && f.ow && f.oh && g_stage[0])
    {
        const D3D12_RESOURCE_DESC sd = g_stage[0]->GetDesc();
        gmo::NoteLetterbox(f.oh < 0.9 * sd.Height && f.ow >= 0.9 * sd.Width, f.ow, f.oh, (unsigned)sd.Width, sd.Height, "DLSS");
    }
    c.mvInvalid = 3.402823466e38f;                         // DLSS has no marker for a missing vector
    static int logged = 0;
    if (logged < 4)
    {
        logged++;
        LOGI("BFI Presenter: DLSS call (feature %d%s): vectors %ux%u%s, scale %.3g x %.3g, render %ux%u, depth %s, flags 0x%x%s",
             known ? f.feature : -1, known ? "" : ", created before the hook", mvW, mvH, low ? " (render size)" : " (output size)", sx, sy, rw, rh,
             dp ? ((flags & kDepthInverted) ? "reversed" : "standard") : "none", flags, flagsGiven ? "" : " (assumed: not stated)");
    }
    const slp::Tag *tg[2] = { dp ? &tag[0] : nullptr, &tag[1] };
    {
        std::lock_guard<std::mutex> l(gmo::lk);
        gmo::Feed(tg, c, cmd, 1);
    }
    std::lock_guard<std::mutex> l(lk);
    nUsed++;
    usedNow = true;
}

template <int I> static int hk_Create(void *cl, int feature, void *params, Handle **out)
{
    inside++;
    const int r = o_Creates[I](cl, feature, params, out);
    inside--;
    if (r == kSuccess && out && Plaus(out) && inside == 0) OnCreate(feature, params, *out);
    return r;
}
// DLSS's output, for the HUD mask without a HUD-less picture: after the call, so the copy is of what DLSS wrote.
static void AfterEval(void *cmd, const void *params)
{
    if (!usedNow || !g_shared.hudFromDlss) return;
    usedNow = false;
    void *out = GetRes(params, "Output", nullptr);
    ID3D12Resource *r = nullptr;
    if (!out || !Plaus(out) || FAILED(((IUnknown *)out)->QueryInterface(__uuidof(ID3D12Resource), (void **)&r)) || !r) return;
    gmo::CopyOut(cmd, r);
    r->Release();
}
template <int I> static int hk_Eval(void *cl, const Handle *h, const void *params, void *cb)
{
    if (inside == 0) OnEval(cl, h, params);
    inside++;
    const int r = o_Eval[I](cl, h, params, cb);
    inside--;
    if (inside == 0 && r == kSuccess) AfterEval(cl, params);
    return r;
}
template <int I> static int hk_EvalC(void *cl, const Handle *h, const void *params, void *cb)
{
    if (inside == 0) OnEval(cl, h, params);
    inside++;
    const int r = o_EvalC[I](cl, h, params, cb);
    inside--;
    if (inside == 0 && r == kSuccess) AfterEval(cl, params);
    return r;
}

// Once a second from the presenter: hooks DLSS's exports once a module with them is loaded; reports every 10 s.
static void Tick(double t)
{
    static const char *mods[2] = { "nvngx.dll", "_nvngx.dll" };
    for (int i = 0; i < 2; i++)
    {
        if (tried[i]) continue;
        HMODULE m = GetModuleHandleA(mods[i]);
        if (!m) continue;
        tried[i] = true;
        if (!g_shared.dlssHooks)
        {
            LOGI("BFI Presenter: %s found, not hooked (DlssHooks=0)", mods[i]);
            continue;
        }
        void *fC = (void *)GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature");
        void *fE = (void *)GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature");
        void *fEC = (void *)GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature_C");
        if (!fE && !fEC) { LOGI("BFI Presenter: %s has no D3D12 DLSS calls -- not hooked", mods[i]); continue; }
        struct { const char *name; void *fn; void *hook; void **orig; } fns[3] = {
            { "CreateFeature", fC, i ? (void *)&hk_Create<1> : (void *)&hk_Create<0>, (void **)&o_Creates[i] },
            { "EvaluateFeature", fE, i ? (void *)&hk_Eval<1> : (void *)&hk_Eval<0>, (void **)&o_Eval[i] },
            { "EvaluateFeature_C", fEC, i ? (void *)&hk_EvalC<1> : (void *)&hk_EvalC<0>, (void **)&o_EvalC[i] },
        };
        char list[400] = "";
        for (auto &f : fns)
        {
            char why[96] = "not exported";
            void *orig = f.fn ? InlineHook64(f.fn, f.hook, why, sizeof(why)) : nullptr;
            if (orig) { *f.orig = orig; hooked = true; }
            char one[160];
            snprintf(one, sizeof(one), "%s%s %s%s%s", list[0] ? "; " : "", f.name, orig ? "hooked" : "not hooked", orig ? "" : " -- ", orig ? "" : why);
            strncat(list, one, sizeof(list) - strlen(list) - 1);
        }
        char path[MAX_PATH] = "";
        GetModuleFileNameA(m, path, MAX_PATH);
        LOGI("BFI Presenter: DLSS (NGX, test build): %s: %s", path, list);
        if (!winT) winT = t;
    }
    if (!hooked || reports >= 30 || t - winT < 10.0) return;
    uint32_t e, u, o, un, nm, sl; std::map<int, uint32_t> fs;
    {
        std::lock_guard<std::mutex> l(lk);
        e = nEval; u = nUsed; o = nOther; un = nUnknown; nm = nNoMv; sl = nSl; fs.swap(nFeat);
        nEval = nUsed = nOther = nUnknown = nNoMv = nSl = 0;
    }
    const double secs = t - winT;
    winT = t;
    if (!e) return;
    reports++;
    char fl[200] = "";
    for (auto &kv : fs)
    {
        char one[64];
        if (kv.first < 0) snprintf(one, sizeof(one), "%sa feature created before the hook %.0f/s", fl[0] ? ", " : "", kv.second / secs);
        else snprintf(one, sizeof(one), "%sfeature %d %.0f/s", fl[0] ? ", " : "", kv.first, kv.second / secs);
        strncat(fl, one, sizeof(fl) - strlen(fl) - 1);
    }
    LOGI("BFI Presenter: DLSS (NGX): last %.0f s: %.0f calls/s (%s), used %.0f/s, from Streamline %.0f/s, other features %.0f/s, created before the hook %.0f/s, "
         "without vectors %.0f/s", secs, e / secs, fl[0] ? fl : "none", u / secs, sl / secs, o / secs, un / secs, nm / secs);
}
} // namespace ngx

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

static void ReflexNv(int limitUs, int limitAll, bool atSleep, ReflexSecond &out);
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
    // DX11: NvAPI's Reflex calls on the game's D3D11 device are made on the game's own present thread (ReflexServe11),
    // not here. From the presenter thread GetLatency deadlocked with the game's Present (Days Gone: the game's present
    // thread waited in NvAPI inside dxgi's Present, the presenter in NvAPI inside d3d11 -- the game never started).
    // The second's results are then a second old.
    if (g_dx11.load())
    {
        std::lock_guard<std::mutex> l(g_rfx11Lk);
        g_rfx11Req = { limitUs, limitAll, atSleep };
        g_rfx11Pending = true;
        out = g_rfx11Out;
        g_rfx11Out = ReflexSecond();
        return;
    }
    ReflexNv(limitUs, limitAll, atSleep, out);
}

// NvAPI's part of the second: the game's latency reports and the frame limit sent to the driver.
static void ReflexNv(int limitUs, int limitAll, bool atSleep, ReflexSecond &out)
{
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
// DX11, on the game's present thread: the presenter's request for this second's NvAPI calls (see ReflexTick).
static void ReflexServe11()
{
    if (!g_rfx11Pending.load(std::memory_order_relaxed)) return;
    Rfx11Req q;
    {
        std::lock_guard<std::mutex> l(g_rfx11Lk);
        if (!g_rfx11Pending) return;
        g_rfx11Pending = false;
        q = g_rfx11Req;
    }
    ReflexSecond r;
    ReflexNv(q.limitUs, q.limitAll, q.atSleep, r);
    std::lock_guard<std::mutex> l(g_rfx11Lk);
    g_rfx11Out = r;
}
#else
static int ReflexGameLimitUs() { return 0; }
static void ReflexTick(int, ReflexSecond &out) { out = ReflexSecond(); }
static void ReflexServe11() {}
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

// SDR to HDR for DX12: the game keeps its 8-bit swap chain format as far as it can tell (GetDesc, the stand-ins,
// ReShade); the real buffers, which only the presenter draws into, are HDR10 (R10G10B10A2, ST.2084), so the lit
// refresh has headroom above SDR white. Decided at each of the game's resizes: on for an 8-bit format on a display
// with Windows HDR on; off when the game resizes to a 10-bit or float format (its own HDR,
// RenoDX, the AutoHDR add-on) or the setting is off. Returns the format for the real ResizeBuffers.
static bool HdrDisplayFor(IDXGIFactory4 *fac, HWND hwnd);
static DXGI_FORMAT Hdr12ResizeFormat(IDXGISwapChain *sc, DXGI_FORMAT f)
{
    DXGI_FORMAT game = f;
    if (game == DXGI_FORMAT_UNKNOWN)                              // "keep the format": the game's current one
    {
        if (g_hdrUp.load() && g_stageFmt != DXGI_FORMAT_UNKNOWN) game = g_stageFmt;
        else { DXGI_SWAP_CHAIN_DESC1 d = {}; OwnCall own; if (o_GetDesc1) o_GetDesc1((IDXGISwapChain1 *)sc, &d); game = d.Format; }
    }
    Settings cur;
    EnterCriticalSection(&g_cfgLock);
    cur = g_shared;
    LeaveCriticalSection(&g_cfgLock);
    const bool was = g_hdrUp.load();
    // 8-bit buffers are always SDR (HDR10 needs 10 bits, scRGB float): the colour space the game set doesn't matter
    // here (a game that turns its HDR off resizes to 8-bit before it sets the SDR colour space again).
    const bool sdr8 = game == DXGI_FORMAT_R8G8B8A8_UNORM || game == DXGI_FORMAT_B8G8R8A8_UNORM;
    // 10-bit buffers can be SDR (Unreal Engine 4.22+ and 5 on DX12 use them in SDR too: Jusant) or HDR10: the colour space
    // the game sets says which, and it can change without a resize (hk_SetCS1). The real buffers are already 10-bit, so
    // only their colour space changes; nothing is resized.
    const bool ten = game == DXGI_FORMAT_R10G10B10A2_UNORM;
    const int cs = g_cs12.load();
    const bool sdr10 = ten && (cs < 0 || cs == (int)DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    bool on = false, display = false;
    g_hdr12Ready10.store(false);
    if (cur.sdrToHdr && cur.output < 0 && (sdr8 || ten))
    {
        IDXGIFactory4 *fac = nullptr;
        HWND hwnd = nullptr;
        IDXGISwapChain1 *s1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&s1))) && s1) { s1->GetHwnd(&hwnd); s1->Release(); }
        if (SUCCEEDED(sc->GetParent(IID_PPV_ARGS(&fac))) && fac)
        {
            display = hwnd && HdrDisplayFor(fac, hwnd);
            fac->Release();
        }
        on = display && (sdr8 || sdr10);
        g_hdr12Ready10.store(display && ten);
        if (!display && !was) LOGI("BFI Presenter: SDR game; the display has no HDR output (Windows HDR off?) -- staying SDR, use Gain 0.5");
        else if (ten && !sdr10 && !was)
            LOGI("BFI Presenter: SDR to HDR not used -- the game outputs HDR itself (10-bit, colour space %d)", cs);
    }
    else if (cur.sdrToHdr && cur.output < 0)
    {
        if (was) LOGI("BFI Presenter: the game resized to format %u (not SDR) -- SDR to HDR off", (unsigned)game);
        else
        {
            static DXGI_FORMAT s_logged = DXGI_FORMAT_UNKNOWN;
            if (s_logged != game) { s_logged = game; LOGI("BFI Presenter: SDR to HDR not used -- the game's swap chain is format %u (not SDR)", (unsigned)game); }
        }
    }
    g_hdrUp.store(on);
    g_stageFmt = on ? game : DXGI_FORMAT_UNKNOWN;
    if (on && !was)
        LOGI(ten ? "BFI Presenter: SDR to HDR: 10-bit SDR game (format %u) -- the real buffers switch to the HDR10 colour space"
                 : "BFI Presenter: SDR to HDR: the game's format %u stays its own; the real buffers are HDR10", (unsigned)game);
    else if (!on && was) LOGI("BFI Presenter: SDR to HDR off -- the real buffers follow the game's format again");
    return (on && sdr8) ? DXGI_FORMAT_R10G10B10A2_UNORM : f;
}

// After a real resize: ST.2084 on the HDR10 buffers, or the game's own colour space again when the conversion ended.
static void Hdr12ApplyColorSpace(IDXGISwapChain *sc, bool wasOn)
{
    if (!o_SetCS1) return;
    IDXGISwapChain3 *s3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&s3))) || !s3) return;
    s3->Release();
    OwnCall own;
    if (g_hdrUp.load())
    {
        const HRESULT hr = o_SetCS1(s3, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        LOGI("BFI Presenter: SDR to HDR: real buffers set to HDR10 (0x%08lX)", (unsigned long)hr);
    }
    else if (wasOn)
    {
        const int cs = g_cs12.load();
        o_SetCS1(s3, cs >= 0 ? (DXGI_COLOR_SPACE_TYPE)cs : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }
}

static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain *sc, UINT cnt, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    if (!t_ownCall && IsGame11(sc)) return Resize11(sc, cnt, w, h, f, fl, nullptr, nullptr, false);
    if (t_ownCall || !IsManaged(sc)) return o_ResizeBuffers(sc, cnt, w, h, f, fl);
    if (cnt) g_virtCount = cnt;
    GameStage(ST_R_STOP);
    if (!StopPresenter())
    {
        GameStage(ST_G_OUTSIDE);
        g_hdrUp.store(false); g_stageFmt = DXGI_FORMAT_UNKNOWN;    // unmanaged from here: the game's own format
        return StuckResize(sc, [&] { OwnCall own; return o_ResizeBuffers(sc, cnt, w, h, f, fl); });
    }
    GameStage(ST_R_RELEASE);
    if (!t_selfResize) ReleaseFrameResources();     // PresenterResize: the game keeps rendering into its stand-ins
    GameStage(ST_R_REBIND);
    const bool hdrWas = g_hdrUp.load();
    const DXGI_FORMAT rf = Hdr12ResizeFormat(sc, f);
    // A pending move to the presenter's queue happens here: nothing holds the
    // real buffers and ReShade's lock is not held.
    const bool wantOwn = g_ownPresentQueue || g_needRebind.exchange(false);
    HRESULT hr = wantOwn ? ResizeOnPresenterQueue((IDXGISwapChain3 *)sc, cnt, w, h, rf, fl) : E_FAIL;
    if (wantOwn && FAILED(hr) && !g_ownPresentQueue) g_needRebind.store(true);   // try again at the next resize
    g_ownPresentQueue = SUCCEEDED(hr);
    if (wantOwn) LOGI("BFI Presenter: presenting on %s queue after resize", g_ownPresentQueue ? "its own" : "the game's");
    GameStage(ST_R_ORIG);
    if (FAILED(hr)) { OwnCall own; hr = o_ResizeBuffers(sc, cnt, w, h, rf, fl); }
    if (FAILED(hr) && rf != f)
    {
        LOGW("BFI Presenter: SDR to HDR: HDR10 buffers refused (0x%08lX) -- the game's own format", (unsigned long)hr);
        g_hdrUp.store(false); g_stageFmt = DXGI_FORMAT_UNKNOWN;
        OwnCall own; hr = o_ResizeBuffers(sc, cnt, w, h, f, fl);
    }
    if (SUCCEEDED(hr)) Hdr12ApplyColorSpace(sc, hdrWas);
    GameStage(ST_R_CREATE);
    if (g_ownPresentQueue && g_deferPresenter.exchange(false))
        LOGI("BFI Presenter: %s -- BFI starts", t_selfResize ? "the presenter resized the game's swap chain (PresenterResize)"
                                                              : "the game resized its swap chain");
    if ((t_selfResize ? RefreshRealDesc() : CreateFrameResources()) && !g_presenterFailed.load() &&
        !(g_deferPresenter.load() && !g_ownPresentQueue)) StartPresenter();
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
    if (!StopPresenter())
    {
        GameStage(ST_G_OUTSIDE);
        g_hdrUp.store(false); g_stageFmt = DXGI_FORMAT_UNKNOWN;
        return StuckResize(sc, [&] { OwnCall own; return o_ResizeBuffers1(sc, cnt, w, h, f, fl, mask, queues); });
    }
    GameStage(ST_R_RELEASE);
    if (!t_selfResize) ReleaseFrameResources();     // PresenterResize: the game keeps rendering into its stand-ins
    GameStage(ST_R_REBIND);
    const bool hdrWas = g_hdrUp.load();
    const DXGI_FORMAT rf = Hdr12ResizeFormat(sc, f);
    const bool wantOwn = g_ownPresentQueue || g_needRebind.exchange(false);
    HRESULT hr = wantOwn ? ResizeOnPresenterQueue(sc, cnt, w, h, rf, fl) : E_FAIL;
    if (wantOwn && FAILED(hr) && !g_ownPresentQueue) g_needRebind.store(true);   // try again at the next resize
    g_ownPresentQueue = SUCCEEDED(hr);
    if (wantOwn) LOGI("BFI Presenter: presenting on %s queue after resize", g_ownPresentQueue ? "its own" : "the game's");
    GameStage(ST_R_ORIG);
    if (FAILED(hr)) { OwnCall own; hr = o_ResizeBuffers1(sc, cnt, w, h, rf, fl, mask, queues); }
    if (FAILED(hr) && rf != f)
    {
        LOGW("BFI Presenter: SDR to HDR: HDR10 buffers refused (0x%08lX) -- the game's own format", (unsigned long)hr);
        g_hdrUp.store(false); g_stageFmt = DXGI_FORMAT_UNKNOWN;
        OwnCall own; hr = o_ResizeBuffers1(sc, cnt, w, h, f, fl, mask, queues);
    }
    if (SUCCEEDED(hr)) Hdr12ApplyColorSpace(sc, hdrWas);
    GameStage(ST_R_CREATE);
    if (g_ownPresentQueue && g_deferPresenter.exchange(false))
        LOGI("BFI Presenter: %s -- BFI starts", t_selfResize ? "the presenter resized the game's swap chain (PresenterResize)"
                                                              : "the game resized its swap chain");
    if ((t_selfResize ? RefreshRealDesc() : CreateFrameResources()) && !g_presenterFailed.load() &&
        !(g_deferPresenter.load() && !g_ownPresentQueue)) StartPresenter();
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
    PatchSlot(vtbl, VT_SetColorSpace1,            (void *)&hk_SetCS1,         (void **)&o_SetCS1);   // SDR to HDR (DX12)
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
    if (vt == g_hookedVtbl || vt == g_outerVtbl || vt == g_selfVtbl.load()) return;   // already known (the last: PresenterResize's hook)
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
            g_wrapperVtbl.store(vt);
            if (strstr(l, "xess")) g_fgXess.store(true);
            EnterCriticalSection(&g_cfgLock);
            const bool self = g_shared.selfResize != 0;
            LeaveCriticalSection(&g_cfgLock);
            if (self) HookSelfPresent();
            return;
        }
    }
    PatchSlot(vt, VT_Present,  (void *)&hk_OuterPresent,  (void **)&o_OuterPresent);
    PatchSlot(vt, VT_Present1, (void *)&hk_OuterPresent1, (void **)&o_OuterPresent1);
    g_outerVtbl = vt;
    LOGI("BFI Presenter: the game presents to FSR frame generation's swap chain (%s) -- its presents are counted there "
         "(FSR presents every frame from a thread of its own)", mod);
}
// A vtable slot's original function, read from the module file the vtable lives in (DXGI's own, in dxgi.dll): the value
// in the file, relocated to where the module is loaded. Another add-on can sit on the same slot and re-hook it after us,
// saving our hook as its "original" -- then its hook and ours call each other until the stack runs out (Elden Ring with
// ERSS-FG, #119: 3993 nested calls; most likely Avatar too, which closed without a crash record). The real function
// from the file breaks that loop. nullptr when the vtable isn't in a file on disk or the value doesn't check out.
static std::atomic<bool> g_scLoopLogged{false};
static void *PristineVtableSlot(void **vtbl, int idx)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vtbl, &m) || !m)
        return nullptr;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return nullptr;
    BYTE *base = (BYTE *)m;
    auto *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    const ULONGLONG prefBase = nt->OptionalHeader.ImageBase;
    const DWORD rva = (DWORD)((BYTE *)&vtbl[idx] - base);
    // the file offset of that RVA
    auto *sec = IMAGE_FIRST_SECTION(nt);
    DWORD off = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (rva >= sec->VirtualAddress && rva + sizeof(void *) <= sec->VirtualAddress + sec->SizeOfRawData)
        { off = rva - sec->VirtualAddress + sec->PointerToRawData; break; }
    if (!off) return nullptr;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    ULONGLONG raw = 0;
    DWORD got = 0;
    LARGE_INTEGER pos; pos.QuadPart = off;
    const bool ok = SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && ReadFile(f, &raw, sizeof(void *), &got, nullptr) && got == sizeof(void *);
    CloseHandle(f);
    if (!ok || !raw) return nullptr;
    BYTE *fn = (BYTE *)(uintptr_t)(raw - prefBase + (ULONGLONG)(uintptr_t)base);
    // must point into that module's executable code
    sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) && fn >= base + sec->VirtualAddress && fn < base + sec->VirtualAddress + sec->Misc.VirtualSize)
            return fn;
    return nullptr;
}
static void *g_pristineSC = nullptr, *g_pristineSCHwnd = nullptr, *g_pristineSCCore = nullptr;
static void **g_scFactoryVt = nullptr;   // the factory vtable the hooks sit on
// Another add-on re-hooked the slot over ours with ours as its original, while ours still has its hook as the original:
// each would call the other. Then ours goes straight to DXGI's own function (it is called by that add-on's hook, which
// has done its part already).
static bool ScPingPong(int idx, void *hook, void *orig)
{
    if (!g_scFactoryVt) return false;
    void *cur = g_scFactoryVt[idx];
    if (cur == hook || cur != orig) return false;
    if (!g_scLoopLogged.exchange(true))
        LOGW("BFI Presenter: another add-on hooked swap chain creation on top of BFI Presenter with BFI Presenter's hook as "
             "its original -- going to DXGI's own function from there, so the two don't call each other in a loop");
    return true;
}

// The hook was entered again for the same factory and result pointer inside its own call: another add-on's hook on the
// same slot has ours as its "original". Go to DXGI's real function instead of round again.
static bool ScLoop(void *f, void *out)
{
    if (t_scCreateDepth < 1 || f != t_scLoopF || out != t_scLoopOut) return false;
    if (!g_scLoopLogged.exchange(true))
        LOGW("BFI Presenter: another add-on's hook on swap chain creation calls ours back (each saved the other as the "
             "original) -- calling DXGI's own function to break the loop");
    Trace("hook loop at depth %d: calling DXGI's own function", (int)t_scCreateDepth);
    return true;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_FCreateSC)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_FCreateSCHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_FCreateSCCore)(IDXGIFactory2 *, IUnknown *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *,
                                                        IDXGIOutput *, IDXGISwapChain1 **);
static PFN_FCreateSC     o_FCreateSC = nullptr;
static PFN_FCreateSCHwnd o_FCreateSCHwnd = nullptr;
static PFN_FCreateSCCore o_FCreateSCCore = nullptr;
// The game creates a new swap chain for the window the presenter is showing on (Onimusha switching DLSS frame
// generation off: Streamline makes a new one while the old is still alive). With OptiScaler and Streamline that call
// waits for the old swap chain to go quiet, while the presenter's next Present on it waits in the driver for DLSS-G's
// present thread, which waits on the game's call: all three stopped. The presenter holds its presents while such a
// call runs (from before it reaches the wrappers below), so the old swap chain is idle; the game presents again as soon
// as it returns, and the old swap chain's release stops the presenter as usual.
struct PresentHold
{
    bool on = false;
    PresentHold(HWND w)
    {
        if (!w || !g_active.load() || !g_running.load() || w != g_watchHwnd.load()) return;
        on = true;
        g_holdPresents.fetch_add(1);
        const double t0 = Now();
        while (g_presStage.load() == ST_PRESENT && Now() - t0 < 0.1) Sleep(1);   // a Present under way finishes first
        LOGI("BFI Presenter: the game creates a new swap chain for its window -- presents held until it returns");
    }
    ~PresentHold() { if (on) g_holdPresents.fetch_sub(1); }
};

static HRESULT STDMETHODCALLTYPE hk_FCreateSC(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out)
{
    // Only the outermost call: wrappers create the real swap chain (and ReShade its own) inside it, through the
    // same factory methods, and the game gets the object the outermost call returns.
    if (g_pristineSC && (ScPingPong(10, (void *)&hk_FCreateSC, (void *)o_FCreateSC) || ScLoop((void *)f, (void *)out)))
        return ((decltype(o_FCreateSC))g_pristineSC)(f, dev, d, out);
    if (t_scCreateDepth == 0) { t_scLoopF = (void *)f; t_scLoopOut = (void *)out; }
    t_scCreateDepth++;
    Trace("FCreateSC: enter (depth %d, own call %d) -> %p", t_scCreateDepth, t_ownCall ? 1 : 0, (void *)o_FCreateSC);
    PresentHold hold(t_scCreateDepth == 1 && !t_ownCall && d ? d->OutputWindow : nullptr);
    const HRESULT hr = o_FCreateSC(f, dev, d, out);
    const int depth = t_scCreateDepth--;
    Trace("FCreateSC: returned 0x%08lX (depth %d)", (unsigned long)hr, depth);
    if (SUCCEEDED(hr) && out && !t_ownCall && depth == 1) { OuterCheck(*out); Trace("FCreateSC: outer check done"); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_FCreateSCHwnd(IDXGIFactory2 *f, IUnknown *dev, HWND w, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *o, IDXGISwapChain1 **out)
{
    // Only the outermost call: wrappers create the real swap chain (and ReShade its own) inside it, through the
    // same factory methods, and the game gets the object the outermost call returns.
    if (g_pristineSCHwnd && (ScPingPong(15, (void *)&hk_FCreateSCHwnd, (void *)o_FCreateSCHwnd) || ScLoop((void *)f, (void *)out)))
        return ((decltype(o_FCreateSCHwnd))g_pristineSCHwnd)(f, dev, w, d, fs, o, out);
    if (t_scCreateDepth == 0) { t_scLoopF = (void *)f; t_scLoopOut = (void *)out; }
    t_scCreateDepth++;
    Trace("FCreateSCHwnd: enter (depth %d, own call %d) -> %p", t_scCreateDepth, t_ownCall ? 1 : 0, (void *)o_FCreateSCHwnd);
    PresentHold hold(t_scCreateDepth == 1 && !t_ownCall ? w : nullptr);
    const HRESULT hr = o_FCreateSCHwnd(f, dev, w, d, fs, o, out);
    const int depth = t_scCreateDepth--;
    Trace("FCreateSCHwnd: returned 0x%08lX (depth %d)", (unsigned long)hr, depth);
    if (SUCCEEDED(hr) && out && !t_ownCall && depth == 1) { OuterCheck(*out); Trace("FCreateSCHwnd: outer check done"); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_FCreateSCCore(IDXGIFactory2 *f, IUnknown *dev, IUnknown *win, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                  IDXGIOutput *o, IDXGISwapChain1 **out)
{
    // Only the outermost call: wrappers create the real swap chain (and ReShade its own) inside it, through the
    // same factory methods, and the game gets the object the outermost call returns.
    if (g_pristineSCCore && (ScPingPong(16, (void *)&hk_FCreateSCCore, (void *)o_FCreateSCCore) || ScLoop((void *)f, (void *)out)))
        return ((decltype(o_FCreateSCCore))g_pristineSCCore)(f, dev, win, d, o, out);
    if (t_scCreateDepth == 0) { t_scLoopF = (void *)f; t_scLoopOut = (void *)out; }
    t_scCreateDepth++;
    Trace("FCreateSCCore: enter (depth %d, own call %d) -> %p", t_scCreateDepth, t_ownCall ? 1 : 0, (void *)o_FCreateSCCore);
    const HRESULT hr = o_FCreateSCCore(f, dev, win, d, o, out);
    const int depth = t_scCreateDepth--;
    Trace("FCreateSCCore: returned 0x%08lX (depth %d)", (unsigned long)hr, depth);
    if (SUCCEEDED(hr) && out && !t_ownCall && depth == 1) { OuterCheck(*out); Trace("FCreateSCCore: outer check done"); }
    return hr;
}
// The function the game itself imports from dxgi.dll (its own import table): with OptiScaler that is
// OptiScaler's dxgi.dll, so the factory made here is the kind the game gets.
// Every offset is checked against the image's size and the memory it points at: some games' executables have their
// import table rewritten or partly removed once loaded (RE4: a crash here in 1.0.37), and then nothing is found and the
// caller falls back to dxgi.dll.
static bool ImageSpanOk(const BYTE *base, size_t size, ULONGLONG rva, size_t len)
{
    if (rva >= size || len > size - rva) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    const BYTE *a = base + rva, *e = a + len;
    while (a < e)
    {
        if (VirtualQuery(a, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mbi.Protect)
            return false;
        a = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}
// The string at rva, if it lies in the image and ends there within max characters.
static const char *ImageStr(const BYTE *base, size_t size, ULONGLONG rva, size_t max)
{
    if (rva >= size) return nullptr;
    const size_t n = size - rva < max ? (size_t)(size - rva) : max;
    if (!ImageSpanOk(base, size, rva, n)) return nullptr;
    const char *s = (const char *)(base + rva);
    return memchr(s, 0, n) ? s : nullptr;
}
static void *GameDxgiImport(const char *fn)
{
    BYTE *base = (BYTE *)GetModuleHandleW(nullptr);
    if (!base) return nullptr;
    auto *dos = (IMAGE_DOS_HEADER *)base;
    if (!ImageSpanOk(base, sizeof(IMAGE_DOS_HEADER), 0, sizeof(IMAGE_DOS_HEADER)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000 || !ImageSpanOk(base, 0x20000, (ULONGLONG)dos->e_lfanew, sizeof(IMAGE_NT_HEADERS)))
        return nullptr;
    auto *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) return nullptr;
    const size_t size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (ULONGLONG d = dir.VirtualAddress; ImageSpanOk(base, size, d, sizeof(IMAGE_IMPORT_DESCRIPTOR));
         d += sizeof(IMAGE_IMPORT_DESCRIPTOR))
    {
        auto *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + d);
        if (!imp->Name) break;
        const char *dll = ImageStr(base, size, imp->Name, 260);
        if (!dll || _stricmp(dll, "dxgi.dll") != 0) continue;
        // Without the name table (OriginalFirstThunk) the FirstThunk entries are already the resolved addresses, not
        // names: nothing to match.
        if (!imp->OriginalFirstThunk || !imp->FirstThunk) continue;
        for (ULONGLONG i = 0; i < 4096; i++)
        {
            const ULONGLONG nr = imp->OriginalFirstThunk + i * sizeof(IMAGE_THUNK_DATA);
            const ULONGLONG ar = imp->FirstThunk + i * sizeof(IMAGE_THUNK_DATA);
            if (!ImageSpanOk(base, size, nr, sizeof(IMAGE_THUNK_DATA)) || !ImageSpanOk(base, size, ar, sizeof(IMAGE_THUNK_DATA)))
                break;
            const IMAGE_THUNK_DATA *nm = (const IMAGE_THUNK_DATA *)(base + nr);
            if (!nm->u1.AddressOfData) break;
            if (IMAGE_SNAP_BY_ORDINAL(nm->u1.Ordinal)) continue;
            const char *name = ImageStr(base, size, nm->u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name), 256);
            if (name && strcmp(name, fn) == 0) return (void *)((const IMAGE_THUNK_DATA *)(base + ar))->u1.Function;
        }
    }
    return nullptr;
}
// OptiScaler in this game: its ini next to the game's exe, a module named OptiScaler, or a module whose version
// information says OptiScaler (it usually loads as dxgi.dll, winmm.dll, version.dll ... next to the exe). Returns what
// was found, or nullptr.
static const char *OptiScalerFound(char *buf, size_t n)
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return nullptr;
    wchar_t dir[MAX_PATH];
    wcscpy(dir, exe);
    if (wchar_t *s = wcsrchr(dir, L'\\')) s[1] = 0;
    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%sOptiScaler.ini", dir);
    if (GetFileAttributesW(ini) != INVALID_FILE_ATTRIBUTES) { snprintf(buf, n, "OptiScaler.ini next to the game"); return buf; }
    typedef DWORD (WINAPI *PFN_GFVIS)(LPCWSTR, LPDWORD);
    typedef BOOL (WINAPI *PFN_GFVI)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL (WINAPI *PFN_VQV)(LPCVOID, LPCWSTR, LPVOID *, PUINT);
    HMODULE ver = LoadLibraryA("version.dll");
    PFN_GFVIS gs = ver ? (PFN_GFVIS)(void *)GetProcAddress(ver, "GetFileVersionInfoSizeW") : nullptr;
    PFN_GFVI  gi = ver ? (PFN_GFVI)(void *)GetProcAddress(ver, "GetFileVersionInfoW") : nullptr;
    PFN_VQV   vq = ver ? (PFN_VQV)(void *)GetProcAddress(ver, "VerQueryValueW") : nullptr;
    const char *found = nullptr;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W me = { sizeof(me) };
        for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
        {
            if (_wcsnicmp(me.szModule, L"optiscaler", 10) == 0) { snprintf(buf, n, "%ls loaded", me.szModule); found = buf; break; }
            if (_wcsnicmp(me.szExePath, dir, wcslen(dir)) != 0 || !gs || !gi || !vq) continue;   // only modules next to the exe
            DWORD h = 0, sz = gs(me.szExePath, &h);
            if (!sz || sz > 1 << 20) continue;
            std::vector<BYTE> vi(sz);
            if (!gi(me.szExePath, 0, sz, vi.data())) continue;
            struct LangCp { WORD lang, cp; } *tr = nullptr;
            UINT tl = 0;
            if (!vq(vi.data(), L"\\VarFileInfo\\Translation", (LPVOID *)&tr, &tl) || tl < sizeof(LangCp)) continue;
            static const wchar_t *const keys[] = { L"ProductName", L"FileDescription", L"InternalName", L"OriginalFilename" };
            for (const wchar_t *k : keys)
            {
                wchar_t q[96];
                _snwprintf(q, 96, L"\\StringFileInfo\\%04x%04x\\%ls", tr->lang, tr->cp, k);
                wchar_t *v = nullptr; UINT vl = 0;
                if (vq(vi.data(), q, (LPVOID *)&v, &vl) && v && vl)
                {
                    std::wstring s(v);
                    for (auto &c : s) c = (wchar_t)towlower(c);
                    if (s.find(L"optiscaler") != std::wstring::npos) { snprintf(buf, n, "%ls (OptiScaler)", me.szModule); found = buf; break; }
                }
            }
        }
        CloseHandle(snap);
    }
    return found;
}

static void InstallOuterFactoryHook()
{
    static bool done = false;
    if (done) return;
    done = true;
    // HookOuterSwapChain: 0 off; 1 (default) only with OptiScaler -- the hook is there for FSR frame generation through
    // OptiScaler (1.0.26) and does nothing anywhere else, while hooking every DX game's swap chain creation could meet
    // another add-on's hook on the same function (Elden Ring with ERSS-FG, #119; Avatar); 2 every game, as 1.0.26-1.0.37.
    const int mode = (int)GetPrivateProfileIntA("BFI", "HookOuterSwapChain", 1, g_iniPath);
    if (mode == 0) { LOGI("BFI Presenter: outer swap chain hook off (ini)"); return; }
    if (mode == 1)
    {
        char why[160];
        const char *opti = OptiScalerFound(why, sizeof(why));
        // PresenterResize finds ReShade's game-facing swap chain through this hook too (OuterCheck): without it nothing
        // resizes (Ratchet & Clank, Spider-Man 2 since 1.0.38).
        const bool selfResize = GetPrivateProfileIntA("BFI", "PresenterResize", 0, g_iniPath) != 0;
        if (!opti && !selfResize) { LOGI("BFI Presenter: outer swap chain hook not needed (no OptiScaler in this game; HookOuterSwapChain=2 forces it)"); return; }
        if (opti) LOGI("BFI Presenter: OptiScaler found (%s) -- outer swap chain hook for FSR frame generation", opti);
        else LOGI("BFI Presenter: outer swap chain hook for PresenterResize (to find the game-facing swap chain)");
    }
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
    // DXGI's own functions for these slots, from dxgi.dll's file (see PristineVtableSlot), before anything is patched.
    g_pristineSC = PristineVtableSlot(vt, 10);
    g_pristineSCHwnd = PristineVtableSlot(vt, 15);
    g_pristineSCCore = PristineVtableSlot(vt, 16);
    Trace("outer hook: DXGI's own functions %p %p %p (slots now %p %p %p)", g_pristineSC, g_pristineSCHwnd, g_pristineSCCore,
          vt[10], vt[15], vt[16]);
    g_scFactoryVt = vt;
    PatchSlot(vt, 10, (void *)&hk_FCreateSC,     (void **)&o_FCreateSC);
    Trace("outer hook: factory %p vtable %p, CreateSwapChainForHwnd %p", (void *)f, (void *)vt, vt[15]);
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
    // The frame limits are set by the presenter thread once a second; with it stopped they must not stay on, or a
    // recreated swap chain's pass-through (no BFI until the game resizes it) ran at the old cap (Onimusha: 30 fps,
    // the 2x frame generation limit, after the game recreated its swap chain).
    if (g_sleepLimitUs.exchange(0) > 0) LOGI("BFI Presenter: frame limit at the game's Reflex sleep off (presenter stopped)");
#ifdef _WIN64
    g_rfxWantLimitUs.store(0);
#endif
    ReleaseFrameResources();
    for (UINT s = 0; s < kStage; s++) { SafeRelease(g_copyAlloc[s]); SafeRelease(g_bakeAlloc[s]); g_bakeDone[s] = 0; }
    SafeRelease(g_copyList);
    SafeRelease(g_bakeList);
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
    g_rsCs.store(-1);
    g_hdrUp.store(false);
    g_stageFmt = DXGI_FORMAT_UNKNOWN;
    g_cs12.store(-1);
    g_hdr12Ready10.store(false);
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
static ID3D11Texture2D      *g_pre11[kStage] = {};         // FrameWarpStillBeforeEffects: g_pre[s] opened on the game's DX11 device
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

// The presenter's swap chain size for game frames of w x h. A swap chain larger than the screen it shows on can't be
// flipped to the display: Windows' compositor scales it, and a present that reaches it just after the vblank is shown a
// refresh late, which breaks the lit/dark order (Journey at 5120x2880 on a 3840x2160 display flickered). With
// ScaleToWindow on, frames larger than the game window's monitor are scaled down by the presenter's own pass instead
// (aspect kept). Measured against the monitor, not the window: a game can resize its swap chain while its window is
// still small (Assassin's Creed Unity: 4K frames while the window was 1147x645 at start), and the presenter's swap chain
// then stayed at that size when the window grew. Frames at or below the monitor's size stay as they are.
static void OutSize11(const Settings &cur, HWND hwnd, UINT w, UINT h, UINT &ow, UINT &oh)
{
    ow = w; oh = h;
    if (!cur.fitWindow || !hwnd || !w || !h) return;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const LONG mwL = mi.rcMonitor.right - mi.rcMonitor.left, mhL = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (mwL <= 0 || mhL <= 0) return;
    const UINT cw = (UINT)mwL, ch = (UINT)mhL;
    if (w <= cw && h <= ch) return;
    const double sc = (std::min)((double)cw / w, (double)ch / h);
    ow = (std::max)(1u, (UINT)(w * sc + 0.5));
    oh = (std::max)(1u, (UINT)(h * sc + 0.5));
    if (ow + 1 >= cw && ow <= cw + 1) ow = cw;        // same aspect: exactly the monitor
    if (oh + 1 >= ch && oh <= ch + 1) oh = ch;
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
    if (g_hdrUp.load()) cs = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;   // SDR game presented as HDR10
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
    for (UINT s = 0; s < kStage; s++) { SafeRelease(g_stage11[s]); SafeRelease(g_stage[s]); SafeRelease(g_pre11[s]); SafeRelease(g_pre[s]); g_preOk[s] = false; }
    g_preGen++;
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
        rd.Width            = g_srcW ? g_srcW : g_desc.Width;
        rd.Height           = g_srcH ? g_srcH : g_desc.Height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = StageFormat();
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
                 (unsigned long)hr, (unsigned)rd.Width, rd.Height, (unsigned)StageFormat());
            ReleaseStage11();
            return false;
        }
        g_stage[s]->SetName(L"BFI staging frame (shared with DX11)");
        g_stageFence[s] = g_stageId[s] = g_readFence[s] = 0;
        g_stageTime[s] = 0.0;
    }
    g_presCurSlot = -1; g_presCurSlot2 = -1; g_presPrevSlot = -1;
    g_stageNext = 0;
    LOGI("BFI Presenter: DX11: presenting %ux%u, format %u, %u buffers (game: %ux%u, format %u, %u buffers)%s",
         g_desc.Width, g_desc.Height, (unsigned)g_desc.Format, g_bufCount, g_srcW ? g_srcW : g_desc.Width,
         g_srcH ? g_srcH : g_desc.Height, (unsigned)g_desc11.Format, g_desc11.BufferCount,
         (g_srcW && (g_srcW != g_desc.Width || g_srcH != g_desc.Height)) ? " -- the game's frames are scaled down to the monitor by the presenter" : "");
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
    if (!t_ownCall && IsManaged(sc))
    {
        // DX12: remembered for the next resize. With SDR to HDR on, the real buffers stay HDR10 (the game's SDR colour
        // space would show its frames wrongly); the game is told it worked.
        if (g_cs12.exchange((int)cs) != (int)cs) LOGI("BFI Presenter: the game set colour space %d", (int)cs);
        const bool sdrCs = cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if (g_hdr12Ready10.load())
        {
            // 10-bit game buffers: the colour space decides, at once. SDR: converted (the real buffers stay ST.2084, the
            // game is told it worked); HDR10 or anything else: shown as it is, the game's colour space applies.
            const bool was = g_hdrUp.exchange(sdrCs);
            if (sdrCs)
            {
                g_stageFmt = DXGI_FORMAT_R10G10B10A2_UNORM;
                if (!was)
                {
                    OwnCall own;
                    o_SetCS1(sc, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                    LOGI("BFI Presenter: SDR to HDR on -- the game set the SDR colour space on its 10-bit buffers");
                }
                return S_OK;
            }
            g_stageFmt = DXGI_FORMAT_UNKNOWN;
            if (was) LOGI("BFI Presenter: SDR to HDR off -- the game outputs HDR itself now (colour space %d)", (int)cs);
        }
        else if (g_hdrUp.load())
        {
            if (!sdrCs)
                LOGI("BFI Presenter: SDR to HDR: the game asked for an HDR colour space on its 8-bit buffers -- off at its next resize");
            return S_OK;
        }
    }
    const HRESULT hr = o_SetCS1(sc, cs);
    if (!t_ownCall && IsGame11(sc) && SUCCEEDED(hr) && g_cs11.exchange((int)cs) != (int)cs)
    {
        LOGI("BFI Presenter: DX11: the game set colour space %d", (int)cs);
        if (g_hdrUp.load() && cs != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
        {
            // The game now outputs HDR itself (a 10-bit swap chain switched to HDR10): shown as it is, not converted again.
            g_hdrUp.store(false);
            LOGI("BFI Presenter: DX11: the game switched to an HDR colour space -- SDR to HDR off");
        }
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
    g_rsCs.store(-1);
    g_ownPresentQueue = false;
    g_cs11.store(-1);
    g_fsWanted = FALSE;
    g_hdrUp.store(false);
    g_stageFmt = DXGI_FORMAT_UNKNOWN;
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
// Windows HDR on for the monitor the window is on (its output reports the HDR10 colour space).
static bool HdrDisplayFor(IDXGIFactory4 *fac, HWND hwnd)
{
    if (!fac) return false;
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    bool hdr = false;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++)
    {
        IDXGIOutput *o = nullptr;
        for (UINT k = 0; a->EnumOutputs(k, &o) == S_OK; k++)
        {
            IDXGIOutput6 *o6 = nullptr;
            DXGI_OUTPUT_DESC1 od = {};
            if (SUCCEEDED(o->QueryInterface(IID_PPV_ARGS(&o6))) && o6 && SUCCEEDED(o6->GetDesc1(&od)) && od.Monitor == mon)
                hdr = od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            SafeRelease(o6);
            o->Release();
        }
        a->Release();
    }
    return hdr;
}

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
    // SDR to HDR: an 8-bit SDR game on a display with Windows HDR on gets an HDR10 swap chain of the presenter's own,
    // so the lit refresh has headroom above SDR white; the game keeps its own 8-bit swap chain and frames.
    g_stageFmt = ff;
    g_hdrUp.store(false);
    if (ok)
    {
        // 8-bit, or 10-bit with the SDR colour space (many UE4 games: Obduction). A 10-bit game that switches to HDR10
        // later turns the conversion off again (hk_SetCS1).
        const bool sdrGame = (ff == DXGI_FORMAT_R8G8B8A8_UNORM || ff == DXGI_FORMAT_B8G8R8A8_UNORM || ff == DXGI_FORMAT_R10G10B10A2_UNORM) &&
                             (!swapchain || swapchain->get_color_space() == reshade::api::color_space::srgb_nonlinear ||
                              swapchain->get_color_space() == reshade::api::color_space::unknown) &&
                             (g_cs11.load() < 0 || g_cs11.load() == (int)DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        if (cur.sdrToHdr && cur.output < 0 && sdrGame)
        {
            if (HdrDisplayFor(fac, hwnd)) g_hdrUp.store(true);
            else LOGI("BFI Presenter: %s: SDR game; the display has no HDR output (Windows HDR off?) -- staying SDR, use Gain 0.5", what);
        }
        else if (cur.sdrToHdr && cur.output < 0)
            LOGI("BFI Presenter: %s: SDR to HDR not used -- the game's swap chain is format %u, colour space %d (not SDR)",
                 what, (unsigned)ff, swapchain ? (int)swapchain->get_color_space() : -1);
        else if (cur.sdrToHdr)
            LOGI("BFI Presenter: %s: SDR to HDR not used -- Output is set by hand (Auto needed)", what);
    }
    if (ok)
    {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        g_srcW = d.Width; g_srcH = d.Height;
        OutSize11(cur, hwnd, d.Width, d.Height, sd.Width, sd.Height);
        sd.Format      = g_hdrUp.load() ? DXGI_FORMAT_R10G10B10A2_UNORM : ff;
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
        if (FAILED(hr) && g_hdrUp.load())
        {
            LOGW("BFI Presenter: %s: HDR10 swap chain refused (0x%08lX) -- staying SDR", what, (unsigned long)hr);
            g_hdrUp.store(false);
            sd.Format = ff;
            OwnCall own;
            g_creatingOwn.store(true);
            hr = RawCreateSwapChain(fac, g_presQueue, g_childHwnd.load(), &sd, &own1);
            g_creatingOwn.store(false);
        }
        ok = SUCCEEDED(hr) && own1 && SUCCEEDED(own1->QueryInterface(IID_PPV_ARGS(&g_sc)));
        SafeRelease(own1);
        if (ok && g_hdrUp.load())
        {
            UINT sup = 0;
            if (FAILED(g_sc->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &sup)) ||
                !(sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
            {
                LOGW("BFI Presenter: %s: HDR10 not supported for presentation on this swap chain -- staying SDR", what);
                g_hdrUp.store(false);
                OwnCall own;
                SC_DIRECT(PFN_ResizeBuffers, 13)(g_sc, 0, sd.Width, sd.Height, ff, 0);
            }
            else LOGI("BFI Presenter: %s: SDR game presented as HDR10 (SDR white %.0f nits%s) for BFI headroom", what, cur.sdrWhite,
                      cur.sdrGamma ? ", gamma 2.2" : "");
        }
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
    ReadRsColorSpace();
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
    const UINT stW = g_srcW ? g_srcW : g_desc.Width, stH = g_srcH ? g_srcH : g_desc.Height;
    if (!bb || bd.Width != stW || bd.Height != stH)
    {
        if (!g_sizeLogged) { g_sizeLogged = true; LOGW("BFI Presenter: DX11: back buffer %ux%u does not match the presenter (%ux%u)", bd.Width, bd.Height, stW, stH); }
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
            if ((int)i == g_presCurSlot || (int)i == g_presCurSlot2 || (int)i == g_presPrevSlot) continue;
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
    if (bd.SampleDesc.Count > 1) g_ctx11->ResolveSubresource(g_stage11[s], 0, bb, 0, StageFormat());
    else                         g_ctx11->CopyResource(g_stage11[s], bb);
    {
        // FrameWarpStillBeforeEffects: the frame from before ReShade's effects, kept with the slot (dx/bfi_prefx.h). The
        // slot's own texture is made on the add-on's D3D12 device and shared with the game's, as the stage frames are.
        bool preOk = false;
        ID3D11Texture2D *pre = pfx::Take11();
        if (pre && !g_pre11[s])
        {
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            const D3D12_RESOURCE_DESC rd = g_stage[s]->GetDesc();
            HANDLE h = nullptr;
            HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_pre[s]));
            if (SUCCEEDED(hr)) hr = g_device->CreateSharedHandle(g_pre[s], nullptr, GENERIC_ALL, nullptr, &h);
            if (SUCCEEDED(hr)) hr = g_dev11->OpenSharedResource1(h, IID_PPV_ARGS(&g_pre11[s]));
            if (h) CloseHandle(h);
            if (FAILED(hr)) { SafeRelease(g_pre11[s]); SafeRelease(g_pre[s]); }
            else { g_pre[s]->SetName(L"BFI frame before ReShade's effects (slot, shared with DX11)"); g_preGen++; }
        }
        if (pre && g_pre11[s])
        {
            D3D11_TEXTURE2D_DESC a = {}, b = {};
            pre->GetDesc(&a); g_pre11[s]->GetDesc(&b);
            if (a.Width == b.Width && a.Height == b.Height) { g_ctx11->CopyResource(g_pre11[s], pre); preOk = true; }
        }
        g_preOk[s] = preOk;
    }
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
    Osd::FramePush(Now());
    if (!g_firstStaged.exchange(true)) LOGI("BFI Presenter: DX11: first game frame received");
    if ((g_frameId & 31) == 1) ReadRsColorSpace();     // for the presenter thread (see g_rsCs)

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
    if (ff == DXGI_FORMAT_UNKNOWN) ff = StageFormat();
    g_stageFmt = ff;
    // SDR to HDR only while the game's swap chain stays 8-bit SDR. Another add-on can upgrade it after the presenter
    // took over (AutoHDR.addon64 resizes it to scRGB or HDR10 in its own swap chain event): the game's frames are HDR
    // from then on and are shown as they are -- decoding them as SDR again would be a second conversion.
    if (g_hdrUp.load() && ff != DXGI_FORMAT_R8G8B8A8_UNORM && ff != DXGI_FORMAT_B8G8R8A8_UNORM && ff != DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        g_hdrUp.store(false);
        LOGI("BFI Presenter: DX11: the game's swap chain is now format %u (upgraded to HDR by the game or an add-on?) -- "
             "SDR to HDR off, its frames are presented as they are", (unsigned)ff);
    }
    HRESULT hr2;
    {
        Settings cur;
        EnterCriticalSection(&g_cfgLock);
        cur = g_shared;
        LeaveCriticalSection(&g_cfgLock);
        UINT ow = d.Width, oh = d.Height;
        g_srcW = d.Width; g_srcH = d.Height;
        OutSize11(cur, g_gameHwnd, d.Width, d.Height, ow, oh);
        OwnCall own;
        hr2 = SC_DIRECT(PFN_ResizeBuffers, 13)(g_sc, 0, ow, oh, g_hdrUp.load() ? DXGI_FORMAT_R10G10B10A2_UNORM : ff, 0);
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
    ReadRsColorSpace();
    g_lastPresentT.store(0.0);
    g_stallLogged.store(false);
    g_firstStaged.store(false);
    g_firstPresented.store(false);
    for (DWORD &t : g_presentThreads) t = 0;
    LoadShared();

    bool ok = SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_gameFence))) &&
              SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_presFence)));
    for (UINT s = 0; s < kStage && ok; s++)
        ok = SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_copyAlloc[s]))) &&
             SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_bakeAlloc[s])));
    if (ok)
        ok = SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_copyAlloc[0], nullptr,
                                                   IID_PPV_ARGS(&g_copyList))) &&
             SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bakeAlloc[0], nullptr,
                                                   IID_PPV_ARGS(&g_bakeList)));
    if (ok)
    {
        g_copyList->SetName(L"BFI frame copy list");
        g_copyList->Close();
        g_bakeList->SetName(L"BFI CRT hand-off picture list");
        g_bakeList->Close();
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
    g_deferSinceT.store(Now());
    g_nudgeStage.store(0);
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
    if (!g_active.load() || VirtualFormat(StageFormat()) == StageFormat()) return false;
    bool ours = false;
    for (UINT i = 0; i < g_bufCount; i++)
        if (resource.handle == (uint64_t)(uintptr_t)g_virt[i]) { ours = true; break; }
    if (!ours) return false;
    const reshade::api::format typeless = (reshade::api::format)VirtualFormat(StageFormat());
    if (desc.format != reshade::api::format::unknown && desc.format != typeless) return false;
    desc.format = (reshade::api::format)StageFormat();
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

// The in-game OSD (common/bfi_osd.h), every frame through ReShade's overlay event.
static void on_osd(reshade::api::effect_runtime *rt)
{
    // One runtime only. ReShade draws its overlay for every swap chain it wraps, and with FSR frame generation (Callisto
    // Protocol through OptiScaler) that is two -- the game's, presented on the game's thread, and the real one, presented
    // on FSR's thread -- at the same time, while ImGui's context is shared: drawing on both crashed the game. The swap
    // chain the presenter manages when there is one, otherwise the first runtime that draws (another takes over after
    // it has been quiet for a second).
    {
        static std::atomic<void *> s_rt{nullptr};
        static std::atomic<double> s_rtT{0.0};
        void *nat = reinterpret_cast<void *>(rt->get_native());
        // The game's own swap chain the presenter manages: DX12's (g_sc), or DX11's (g_sc11). In DX9 and OpenGL games
        // g_sc is the presenter's own DX11 swap chain, which has no ReShade runtime -- there ReShade's runtime is the
        // game's DX9 / OpenGL one, so the first runtime draws.
        void *gameSc = g_dx11.load() ? (void *)g_sc11 : (void *)g_sc;
        const bool managed = gameSc && nat == gameSc;
        const bool anyManaged = gameSc != nullptr;
        const double now = Now();
        if (anyManaged ? !managed : (s_rt.load() && s_rt.load() != (void *)rt && now - s_rtT.load() < 1.0)) return;
        s_rt.store((void *)rt); s_rtT.store(now);
    }
    EnterCriticalSection(&g_cfgLock);
    const bool on = g_shared.osd != 0;
    const int corner = g_shared.osdCorner, graph = g_shared.osdGraph, loads = g_shared.osdLoads;
    const float scale = g_shared.osdScale;
    OsdData d;
    d.gameFps = g_status.gameFps; d.refreshHz = g_status.refreshHz; d.frames = g_shared.frames;
    d.lost = g_status.frameSkips; d.repeated = g_status.frameRepeats; d.missed = g_status.late; d.now = Now();
    d.paused = g_status.hitchPaused; d.bfiOn = g_shared.enabled != 0 && g_status.running; d.relFlashMs = g_status.relFlashMs;
    d.warpOn = g_status.warpOn; d.warped = g_status.warpCycles; d.warpAhead = g_status.warpAvg;
    // which motion: what the last second's moved cycles used, or (none moved) what the game hands over -- unless frame
    // generation is on with the game's motion off for it, as in the settings window (Townfall, 2x: the OSD switched
    // between the game's vectors and the measured motion with each second that had no cycle to fill)
    const bool fgOff = g_status.fgActive && !g_shared.warpGameFg;
    d.warpSrc = g_status.warpCycles ? g_status.warpSrc
              : (g_shared.warpGame && !fgOff && g_status.warpGameState > 0 ? g_status.warpGameState : 0);
    d.warpVia = g_status.warpGameVia;
    d.vramUse = g_status.vramUse; d.vramBudget = g_status.vramBudget; d.ownMB = g_status.ownMB; d.crtMB = g_status.crtMB;
    LeaveCriticalSection(&g_cfgLock);
    if (on) Osd::Draw(d, corner, graph != 0, loads != 0, scale);
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
            ImGui::TextColored(kAmber, "BFI starts when the game resizes its swap chain.\nIf it doesn't, change resolution or display mode once (and back),\n"
                               "or turn on Advanced > Resize the game's swap chain to start BFI.");
        if (st.vrrSuspect)
            ImGui::TextColored(kRed, "The display is refreshing slower than it can: VRR (G-SYNC) or Windows' dynamic refresh rate is active.\n"
                               "Turn G-SYNC off for this game in the NVIDIA profile, and Dynamic refresh rate off in Windows' display\n"
                               "settings (Advanced display), or turn on Queue one refresh ahead.");
        if (st.limitSuspect)
            ImGui::TextColored(kRed, "A frame limiter is holding the presenter below the display's refresh rate.\nSet the RTSS frame limit to 0 for this game (and check NVIDIA Profile Inspector).");
        if (st.vramOver)
            ImGui::TextColored(kRed, "The game uses more video memory than Windows gives it (%.0f of %.0f MB): the GPU stalls,\n"
                               "refreshes are missed, frame generation drops frames. Lower its texture or memory settings.",
                               st.vramUse, st.vramBudget);
        if (st.vramOver && st.crtMB >= 64.0)
            ImGui::TextColored(kRed, "Of that, %.0f MB are the CRT hand-off's: turning it off for this game frees them.", st.crtMB);
        {
            // System check: power plan, GPUs, displays (the displays note only while refreshes are missed).
            std::string always, withFlashes;
            SysCheck::Lines(g_sysCheck.Get(), always, withFlashes);
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            if (!always.empty()) ImGui::TextWrapped("%s", always.c_str());
            if (!withFlashes.empty() && st.flashHint && s.enabled) ImGui::TextWrapped("%s", withFlashes.c_str());
            ImGui::PopStyleColor();
        }
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
        // The game's own clock a little off the display's (ICO: 59.94 fps on 120.00 Hz): the cause of its flashes,
        // so it replaces the flash hints.
        if (st.driftOn)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextWrapped("The game runs at %.2f fps on a clock of its own, the display at %.2f Hz: its frames drift against the "
                               "strobe cycle (a slip about every %.0f s, with stutter and flashes). Set the display to %.2f Hz for "
                               "this game.", st.driftFps, st.driftHz, st.driftSlip, st.driftSuggest);
            ImGui::PopStyleColor();
        }
        if (st.flashHint && s.flashHints && s.enabled && !redWarn && !st.driftOn)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            if (s.queuePriority >= 2 && g_queuePriority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH)
                ImGui::TextWrapped("Flashes? Give the presenter's GPU queue realtime priority, so the game's GPU work can't hold up the "
                                   "flashes: BFI Presenter Manager > GPU priority > Allow for my account (then sign out of Windows and "
                                   "back in), or start Steam (or the game's launcher) as administrator.");
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
            // The cap in effect: one game frame per strobe cycle while the automatic frame cap is down (it warned
            // "below 120" at 60 fps, the rate the automatic cap had chosen).
            const bool autoDown = s.gameCap == 1 && s.autoCap && st.autoCapDown && !st.autoCapProbe;
            const double capRate = autoDown ? st.refreshHz / std::max(1, s.frames)
                                            : st.refreshHz / std::max(1, CapPeriod(s.gameCap, s.frames, s.capDivisor));
            if (s.gameCap >= 1 && st.gameFps < 0.97 * capRate)
                ImGui::TextColored(kAmber, "The game is below the cap's frame rate (%.0f of %.0f fps): motion stutters.\n"
                                   "Lower its settings or resolution, or use a lower cap (Half refresh).", st.gameFps, capRate);
            else
                ImGui::TextColored(kAmber, "%s", st.fgActive && s.pacingMode == 1
                    ? "Game frames reach the strobe unevenly: frame generation is handing frames over in bunches,\n"
                      "and Lowest latency shows only the newest of each. Use Smooth pacing (every frame once, in order)."
                    : "Game frames reach the strobe unevenly: motion may stutter. If it persists, the GPU\n"
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
        bool spc = s.split != 0;
        if (ImGui::Checkbox("Split comparison (Ctrl+Shift+C)", &spc)) s.split = spc ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("One side of the screen without BFI, the other with it, at the same brightness:\n"
                                                      "shows what BFI does to motion in one picture (for videos and comparisons).\n"
                                                      "Idea: unrefined803.");
        if (s.split)
        {
            ImGui::Indent(16.0f);
            int side = s.splitSide;
            ImGui::Combo("Without BFI", &side, "Left side\0Right side\0");
            s.splitSide = side ? 1 : 0;
            ImGui::SliderInt("Seam position (% of width)", &s.splitPos, 5, 95);
            ImGui::SliderInt("Seam softness (pixels, 0 = hard)", &s.splitSeam, 0, 400);
            ImGui::Unindent(16.0f);
        }
        bool osd = s.osd != 0;
        if (ImGui::Checkbox("On-screen display (Ctrl+Shift+F)", &osd)) s.osd = osd ? 1 : 0;
        if (s.osd) { ImGui::SameLine(); if (ImGui::SmallButton("Reset totals (Ctrl+Shift+Z)")) Osd::ResetTotals(Now()); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A small panel over the game: its frame rate and frame time, the presenter's lost and repeated\n"
                                                      "frames and missed refreshes, a frame-time graph and CPU / GPU load (GPU: NVIDIA).\n"
                                                      "For changing the game's settings without RTSS or opening this menu.");
        if (s.osd)
        {
            ImGui::Indent(16.0f);
            ImGui::Combo("Corner", &s.osdCorner, "Top left\0Top right\0Bottom left\0Bottom right\0");
            bool og = s.osdGraph != 0, ol = s.osdLoads != 0;
            if (ImGui::Checkbox("Frame-time graph", &og)) s.osdGraph = og ? 1 : 0;
            ImGui::SameLine();
            if (ImGui::Checkbox("CPU / GPU load", &ol)) s.osdLoads = ol ? 1 : 0;
            ImGui::SliderFloat("Size", &s.osdScale, 0.75f, 2.5f, "%.2f");
            ImGui::Unindent(16.0f);
        }
    }

    // ================================================================ 2. brightness and output
    if (ImGui::CollapsingHeader("Brightness and output", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int out = s.output + 1;
        if (ImGui::Combo("Output", &out, kOutputNames)) s.output = out - 1;
        ImGui::SliderFloat("Display peak (nits)", &s.peakNits, 50.0f, 4000.0f, "%.0f");
        if (s.peakNits < 100.0f)
        {
            // below 100 only on purpose: say what it does, in red
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
            ImGui::TextWrapped("Below 100 nits: the lit refresh can't reach SDR white (80 nits or more), so the picture is darker "
                               "than with BFI off and highlights are flattened to this ceiling. Only for a dim display or a "
                               "deliberately dim picture.");
            ImGui::PopStyleColor();
        }
        bool ag = s.ablGuard != 0;
        if (ImGui::Checkbox("ABL guard (keep bright scenes within the panel's budget)", &ag)) s.ablGuard = ag ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("OLED panels dim the picture when a refresh puts out too much light overall (area x brightness).\nWith BFI the lit refresh carries the boost, so bright skies and white screens can trigger it.\nThe lit-refresh ceiling is lowered just enough to stay within the budget (the rest spills into\nthe dark refresh), and returns to full when the scene allows. HDR only.");
        if (s.ablGuard)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderInt("Panel budget (average nits of a lit refresh)", &s.ablBudget, 50, 1500);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your panel's full-screen (100%% window) white brightness. Bright areas are held to what the\npanel sustains at their size: from Peak brightness at 10%% of the screen down to this budget at\n100%% (AblCurve in the ini takes your panel's measured curve). The ceiling is lowered just enough;\nsmall highlights keep the full one. Lower it if bright scenes still darken.");
            if (s.ablBudget < 100)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
                ImGui::TextWrapped("Below 100 nits: bright scenes are cut hard, so the brightness swings between dark and "
                                   "bright scenes get larger. Only if your panel really holds this little on a full white screen.");
                ImGui::PopStyleColor();
            }
            if (st.hotArea >= 0.0)
                ImGui::Text("Scene asks for: %.0f nits average   After ceiling: %.0f   Ceiling in use: %.0f nits", st.rawAvg, st.hotArea, st.ceilInUse);
            ImGui::Unindent(16.0f);
        }
        // SDR to HDR: the presenter's own swap chain (DX11, DX9, OpenGL), or a DX12 game's real buffers.
        const bool dx12Game = st.managed && !g_dx11.load();
        ImGui::BeginDisabled(s.output >= 0);
        bool sh = s.sdrToHdr != 0;
        if (ImGui::Checkbox(dx12Game ? "SDR games: present as HDR10 (applies at the game's next resize)"
                                     : "SDR games: present as HDR10 (applies when the swap chain is next created)", &sh)) s.sdrToHdr = sh ? 1 : 0;
        ImGui::EndDisabled();
        if (dx12Game && ImGui::IsItemHovered())
            ImGui::SetTooltip("DX12: takes effect when the game resizes its swap chain (change resolution or display mode once,\n"
                              "or restart the game). 8-bit SDR games only.");
        if (s.output >= 0) ImGui::TextDisabled("Only with Output: Auto.");
        else if (g_hdrUp.load()) ImGui::TextDisabled("In use: this SDR game is shown as HDR10.");
        if (GetModuleHandleA("AutoHDR.addon64") || GetModuleHandleA("AutoHDR.addon32") || GetModuleHandleA("AutoHDR.addon"))
            ImGui::TextWrapped("AutoHDR add-on loaded: it upgrades the game's swap chain to HDR and its effect (e.g. "
                               "AdvancedAutoHDR.fx) does the conversion; BFI Presenter's SDR to HDR then stays off. Use one or the other.");
        if (s.sdrToHdr)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderFloat("SDR white (nits)", &s.sdrWhite, 80.0f, 600.0f, "%.0f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Windows: 80 + 4 x the 'SDR content brightness' slider (40 -> 240).");
            ImGui::SliderFloat("SDR highlights expand to (nits, 0 = off)", &s.sdrHiNits, 0.0f, 2000.0f, "%.0f");
            if (s.sdrMethod != 2) ImGui::SliderFloat("Highlight expansion starts at (SDR signal)", &s.sdrKnee, 0.3f, 0.95f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Below this level the image is unchanged; above it the brightest parts\nstretch toward the highlight value (an HDR look for SDR games).");
            ImGui::Combo("Highlights stretched by", &s.sdrMethod, "Brightest channel\0Luminance (mostly near-white highlights)\0RenoFX HDR Boost (ShortFuse)\0");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brightest channel also brightens bright saturated colours (reds, blues).\nLuminance stretches mostly near-white highlights; saturated colours stay closer to SDR.");
            if (s.sdrMethod == 2)
            {
                ImGui::TextDisabled("RenoFX HDR Boost (ShortFuse, MIT): boost = highlights / SDR white (e.g. 2.5x).");
                ImGui::SliderFloat("Boost start (linear, 1 = SDR white)", &s.sdrBoostStart, 0.001f, 0.99f, "%.3f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the boost begins: lower affects more of the image, higher only the brightest parts.");
                ImGui::SliderFloat("Ramp intensity", &s.sdrBoostRamp, 0.25f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shape of the smooth rise from the start to the peak.");
                ImGui::Combo("Boost working space", &s.sdrBoostSpace, "LMS (RenoFX default, hue restored)\0Brightness (Yf, colour balance kept)\0");
                ImGui::SliderFloat("Boost colour (0-100)", &s.sdrBoostColor, 0.0f, 100.0f, "%.0f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much colour expansion the boost adds while keeping its brightness:\n0 = the source colour balance, 100 = the full result of the working space.");
                bool apl = s.sdrBoostApl != 0;
                if (ImGui::Checkbox("APL limiter", &apl)) s.sdrBoostApl = apl ? 1 : 0;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("RenoFX's APL Limiter: lowers the boost in bright scenes. Above an average picture level of 30%%\nof SDR white it fades out smoothly, and an all-white screen gets no boost.");
                if (s.sdrBoostApl && st.aplNow >= 0.0)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("scene average %.0f%%, boost %.0f%%", st.aplNow * 100.0, (1.0 - st.aplCut) * 100.0);
                }
            }
            ImGui::Combo("SDR decoding", &s.sdrGamma, "sRGB curve (as Windows)\0Gamma 2.2 (deeper shadows)\0Gamma 2.4 (dark-room grading)\0");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the SDR picture is turned into light. Most SDR games were made on gamma 2.2 screens:\nthe sRGB curve (Windows' own SDR-in-HDR and Auto HDR) shows their shadows a little raised.");
            ImGui::SliderFloat("Gamut expansion", &s.sdrGamut, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves bright saturated colours past SDR's colour range, toward the wider colours of\nHDR games (after Pumbo's AdvancedAutoHDR / Unreal Engine). 0 = SDR's colours exactly.");
            ImGui::Unindent(16.0f);
        }
    }

    // ================================================================ 3. frame pacing and latency
    // Grouped by the cap: the cap first, then what applies at the Refresh rate cap, then what applies below it,
    // then what applies to every cap. Options that don't apply to the cap in use are greyed out with the reason.
    if (ImGui::CollapsingHeader("Frame pacing and latency", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const int per = CapPeriod(s.gameCap, s.frames, s.capDivisor);
        // What is in effect right now, in one line.
        {
            static const char *kCapNames[] = { "no cap", "Refresh rate", "Half refresh", "Quarter refresh", "one game frame per strobe cycle", "Custom cap" };
            char use[320];
            int n = 0;
            if (s.gameCap == 1 && st.fgLimitUs > 0)   // works with the automatic frame cap on or off
                n = snprintf(use, sizeof(use), "frame limit for frame generation (%.1f ms per rendered frame)", st.fgLimitUs / 1000.0);
            else if (s.gameCap == 1 && s.autoCap)
            {
                if (st.autoCapProbe)    n = snprintf(use, sizeof(use), "automatic cap: trying Refresh rate");
                else if (st.autoCapDown && st.rfxLimitUs > 0) n = snprintf(use, sizeof(use), "automatic cap: one game frame per strobe cycle (Reflex frame limit)");
                else if (st.autoCapDown && st.feLimitUs > 0)  n = snprintf(use, sizeof(use), "automatic cap: one game frame per strobe cycle, held at the frame start");
                else if (st.autoCapDown)     n = snprintf(use, sizeof(use), "automatic cap: one game frame per strobe cycle");
                else                         n = snprintf(use, sizeof(use), "Refresh rate (automatic cap ready)");
            }
            else n = snprintf(use, sizeof(use), "%s", kCapNames[(s.gameCap >= 0 && s.gameCap <= 5) ? s.gameCap : 0]);
            if (n > 0 && n < (int)sizeof(use))
                n += snprintf(use + n, sizeof(use) - n, ", %s pacing, %d frame%s ahead", s.pacingMode == 1 ? "lowest latency" : "smooth",
                              st.aheadInUse, st.aheadInUse == 1 ? "" : "s");
            if (st.relFlashMs >= 0.0 && n > 0 && n < (int)sizeof(use)) snprintf(use + n, sizeof(use) - n, ", release -> flash %.1f ms", st.relFlashMs);
            // The game stays below the cap in use: something outside the presenter limits it (its graphics settings,
            // its own fps limit, video memory). Shown after 5 s, so a short dip or a cap switch doesn't trigger it.
            double capFps = 0.0;
            if (st.refreshHz > 1.0 && !st.fgActive && !st.hitchPaused && !st.autoCapProbe)
            {
                if (s.gameCap == 1 && s.autoCap && st.autoCapDown) capFps = st.refreshHz / std::max(1, s.frames);
                else if (per >= 2) capFps = st.refreshHz / per;
            }
            static double belowT = -1.0;
            const double tNow = Now();
            const bool below = capFps > 0.0 && st.gameFps > 1.0 && st.gameFps < 0.9 * capFps;
            if (!below) belowT = -1.0; else if (belowT < 0.0) belowT = tNow;
            if (below && tNow - belowT >= 5.0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
                ImGui::TextWrapped("In use: %s", use);
                ImGui::TextWrapped("The game runs at %.0f fps, below the %.0f fps cap: something outside BFI Presenter limits it "
                                   "(its graphics settings, its own fps limit, video memory). Lower the game's settings, or choose "
                                   "a lower cap (e.g. Quarter refresh for a steady 30 fps at 120 Hz).", st.gameFps, capFps);
                ImGui::PopStyleColor(1);
            }
            else ImGui::TextWrapped("In use: %s", use);
        }
        ImGui::Combo("Game frame cap", &s.gameCap, "Off\0Refresh rate\0Half refresh (one game frame per lit + dark pair)\0Quarter refresh (one game frame per two lit + dark pairs)\0One game frame per strobe cycle (refresh / Frames)\0Custom (refresh / divisor)\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh rate: lowest latency when the game holds it; with the automatic frame cap below it also\nsteps down to one game frame per strobe cycle when the game can't (the usual choice).\nHalf refresh = 60 fps at 120 Hz: every game frame is shown once lit, the smoothest motion\nwith 2-frame BFI. With frame generation use Refresh rate: Half throws away half the generated frames.\nOne game frame per strobe cycle: refresh / Frames per cycle -- 60 fps at 360 Hz with 6 frames,\n90 fps at 360 Hz with 4, 80 fps at 240 Hz with 3.\nCustom: the game gets a new frame every N refreshes.");
        if (s.gameCap == 5)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderInt("Refreshes per game frame", &s.capDivisor, 1, 8);
            ImGui::Unindent(16.0f);
        }
        if (per >= 1)
        {
            const double hz = st.refreshHz > 1.0 ? st.refreshHz : 0.0;
            if (hz > 0.0) ImGui::TextDisabled("Cap: %.0f fps (a new game frame every %d refresh%s)", hz / per, per, per == 1 ? "" : "es");
            if (per >= 2 && st.fgActive && st.rfxLimitUs <= 0)
                ImGui::TextColored(kAmber, "Frame generation with a cap below the refresh rate: half its frames never reach the screen\n"
                                   "and latency doubles. Use Refresh rate.");
            if (per >= 2 && per % s.frames != 0)
                ImGui::TextColored(kAmber, "%d refreshes per game frame doesn't fit %d frames per cycle: some game frames start\n"
                                   "on a dark refresh and motion judders. Use a multiple of %d.", per, s.frames, s.frames);
        }

        // ---------------------------------------------------------- at the Refresh rate cap
        ImGui::Spacing();
        ImGui::SeparatorText("At the Refresh rate cap");
        ImGui::BeginDisabled(s.gameCap != 1);
        {
            bool ac = s.autoCap != 0;
            if (ImGui::Checkbox("Automatic frame cap", &ac)) s.autoCap = ac ? 1 : 0;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("While the game can't hold the refresh rate, the cap drops to one game frame per strobe cycle\n"
                                                          "(e.g. 60 fps at 120 Hz with 2 frames), so every flash shows the next frame instead of frames\n"
                                                          "dropped unevenly (judder). Back to Refresh rate when the game's frames fit a refresh again (its\n"
                                                          "Reflex reports, or a short try every 30 to 120 s).\n"
                                                          "In games with NVIDIA Reflex (every DLSS frame-generation game) it uses Reflex's own frame limit\n"
                                                          "for this, which also paces frame generation. Frame generation without Reflex: see the frame limit below.");
            if (!s.autoCap && s.gameCap == 1 && st.fgLimitUs > 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(not needed now: the frame limit for frame generation below holds the game -- used when frame generation stops)");
            }
            if (s.autoCap && s.gameCap == 1)
            {
                ImGui::SameLine();
                if (st.rfxLimitUs > 0) ImGui::TextDisabled("(in use: Reflex frame limit %.1f ms)", st.rfxLimitUs / 1000.0);
                else if (st.fgLimitUs > 0) ImGui::TextDisabled("(at Refresh rate: the frame limit for frame generation holds the game)");
                else if (st.feLimitUs > 0) ImGui::TextDisabled("(in use: one game frame per strobe cycle, held at the frame start)");
                else if (st.fgActive && !st.rfxGameUses) ImGui::TextDisabled("(frame generation without Reflex: Refresh rate)");
                else ImGui::TextDisabled("%s", st.autoCapProbe ? "(trying Refresh rate)" :
                                               st.autoCapDown ? "(in use: one game frame per strobe cycle)" : "(at Refresh rate)");
            }
            // In a Reflex game the cap steps down with Reflex's frame limit (unless the presenter's own cap is chosen
            // and frame generation is off): the presenter's own limiter is then idle, so what only tunes it is greyed out.
            const bool rfxPaces = st.rfxGameUses && (!s.autoCapOwn || st.fgActive);
            ImGui::Indent(16.0f);
            ImGui::BeginDisabled(!s.autoCap);
            bool aco = s.autoCapOwn != 0;
            ImGui::BeginDisabled(!st.rfxGameUses);   // only a choice in Reflex games: without Reflex the presenter's own cap is the only one
            if (ImGui::Checkbox("Presenter's own cap without frame generation", &aco)) s.autoCapOwn = aco ? 1 : 0;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Games with Reflex. Off (default): the cap steps down with Reflex's frame limit, with frame\n"
                                                          "generation or without.\n"
                                                          "On: the presenter's own cap (held at the frame start, below) while frame generation is off;\n"
                                                          "Reflex's frame limit while it is on. Follows frame generation being switched on and off.");
            ImGui::EndDisabled();
            if (!st.rfxGameUses) { ImGui::SameLine(); ImGui::TextDisabled("(no Reflex in this game: the presenter's own cap is used anyway)"); }
            bool fe = s.autoCapFrameStart != 0;
            ImGui::BeginDisabled(rfxPaces);
            if (ImGui::Checkbox("Hold at the frame start", &fe)) s.autoCapFrameStart = fe ? 1 : 0;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Games without Reflex or frame generation (or with the presenter's own cap above): when the\n"
                                                          "cap steps down to one game frame per strobe cycle, the game is held before it starts its\n"
                                                          "next frame, like Reflex's limit, instead of its finished frame waiting at the present.\n"
                                                          "It reads your input later, so less delay to the flash. For about 25 s it tries start\n"
                                                          "times across the strobe cycle and keeps the quickest that shows every frame.\n"
                                                          "Needs Lowest latency pacing.");
            ImGui::EndDisabled();
            if (rfxPaces) { ImGui::SameLine(); ImGui::TextDisabled(st.fgActive ? "(not used: Reflex's frame limit paces frame generation)" :
                                                                   "(not used: Reflex's frame limit paces this game -- tick the presenter's own cap above to use it)"); }
            else if (s.autoCap && s.autoCapFrameStart && s.pacingMode != 1) { ImGui::SameLine(); ImGui::TextColored(kAmber, "needs Lowest latency pacing"); }
            bool lp = s.learnPerGame != 0;
            if (ImGui::Checkbox("Remember per game", &lp)) s.learnPerGame = lp ? 1 : 0;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("What the automatic frame cap finds for a game (one game frame per strobe cycle, the frame\n"
                                                          "start time) is kept in BFIPresenter_learned.ini, and the next session starts from it instead\n"
                                                          "of finding it again. Still checked as usual: tries at Refresh rate, a new search after lost\n"
                                                          "frames. Once you've settled on a game's settings, press Forget this game once.");
            if (s.learnPerGame)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Forget this game")) { g_learnForgetFile.store(true); g_learnForgetNow.store(true); }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Deletes what was remembered for this game and starts over now: the cap goes back to\n"
                                                              "Refresh rate and steps down again if needed, and the frame start is searched for again.\n"
                                                              "E.g. after changing the game's graphics settings or the display mode.");
            }
            ImGui::EndDisabled();
            ImGui::Unindent(16.0f);
            bool fl = s.fgLimitNoReflex != 0;
            // Acts only on frame generation it detects in a game without Reflex: greyed out otherwise. The setting is kept,
            // and the box comes back by itself once frame generation starts.
            const bool flIdle = st.rfxGameUses || (!st.fgActive && g_fgFactNoRfx.load() <= 1 && st.fgLimitUs <= 0);
            ImGui::BeginDisabled(flIdle);
            if (ImGui::Checkbox("Frame limit for frame generation without Reflex", &fl)) s.fgLimitNoReflex = fl ? 1 : 0;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Frame generation in games without NVIDIA Reflex (e.g. XeSS frame generation through\n"
                                                          "OptiScaler): the presenter holds the game's own frames to one per (strobe cycle x\n"
                                                          "frame-generation factor) -- e.g. 30 rendered fps at 120 Hz with 2 frames and 2x -- so every\n"
                                                          "flash shows the next frame; for about 25 s after it comes on it tries start times to find the\n"
                                                          "quickest one that shows every frame.\n"
                                                          "Best with Lowest latency pacing. Turn off any frame limit in the game or in OptiScaler\n"
                                                          "(FramerateLimit = 0). On by default.");
            if (s.fgLimitNoReflex && st.fgLimitUs > 0 && s.gameCap == 1)
            {
                ImGui::SameLine();
                if (st.alignSlackMs >= 0.0) ImGui::TextDisabled("(in use: %.1f ms per rendered frame, ready %.1f ms before the pick)", st.fgLimitUs / 1000.0, st.alignSlackMs);
                else ImGui::TextDisabled("(in use: %.1f ms per rendered frame)", st.fgLimitUs / 1000.0);
            }
            ImGui::EndDisabled();
            if (st.rfxGameUses) { ImGui::SameLine(); ImGui::TextDisabled("(not used: the game has Reflex)"); }
            else if (flIdle) { ImGui::SameLine(); ImGui::TextDisabled("(not used: no frame generation detected -- comes on by itself when it starts)"); }
        }
        ImGui::EndDisabled();
        if (s.gameCap != 1) ImGui::TextDisabled("Only with the Refresh rate cap.");

        // ---------------------------------------------------------- below the refresh rate
        ImGui::Spacing();
        ImGui::SeparatorText("At caps below the refresh rate");
        ImGui::BeginDisabled(per < 2);
        {
            bool cj = s.capJit != 0;
            if (ImGui::Checkbox("Just-in-time release", &cj)) s.capJit = cj ? 1 : 0;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Releases the game for each frame at the lit refresh minus its measured frame time and a\n"
                                                          "margin, so every frame lands on the same lit refresh: no repeat-then-skip at Half, no\n"
                                                          "1-then-3 cycle frames at Quarter. Light games also get fresher frames. On by default.");
            if (s.capJit && per >= 2)
            {
                static const char *kLrNames[] = { "off (needs Wait for vblank)", "measuring", "active", "at its limit (frames finish too close to the refresh)" };
                ImGui::Indent(16.0f);
                ImGui::TextDisabled("%s", kLrNames[st.lrState & 3]);
                if (st.lrState >= 2)
                    ImGui::TextDisabled("Frames take %.1f ms, margin %.1f ms: released %.1f ms before the refresh (misses last second: %d)",
                                        st.lrEst, st.lrMargin, st.lrEst + st.lrMargin, st.lrMisses);
                ImGui::Unindent(16.0f);
            }
            bool cr = s.capReflex != 0;
            if (ImGui::Checkbox("Apply the cap with Reflex's frame limit (irregular frame delivery only)", &cr)) s.capReflex = cr ? 1 : 0;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Half / Quarter cap in games with NVIDIA Reflex: the cap is applied by Reflex's own frame limit,\n"
                                                          "like a game's own Reflex fps limit, instead of the presenter releasing the game just in time.\n"
                                                          "Only for games whose frames still arrive irregularly at Half or Quarter -- just-in-time release\n"
                                                          "keeps switching itself off and motion stutters (The Midnight Walk: 1.7%% of flashes off-cadence\n"
                                                          "with it, 15.6%% without).\n"
                                                          "Adds latency compared with just-in-time release (about 10-20 ms: Silent Hill 2 34 ms against\n"
                                                          "25 ms) and is no smoother where just-in-time release works. Off by default.");
            if (s.capReflex && per >= 2)
            {
                ImGui::SameLine();
                if (st.rfxLimitUs > 0) ImGui::TextDisabled("(in use: %.1f ms)", st.rfxLimitUs / 1000.0);
                else ImGui::TextDisabled("(not in use: no Reflex in this game)");
            }
        }
        ImGui::EndDisabled();
        if (per < 2) ImGui::TextDisabled("Only with Half, Quarter, one game frame per strobe cycle or Custom.");

        // ---------------------------------------------------------- every cap
        ImGui::Spacing();
        ImGui::SeparatorText("Every cap");
        ImGui::Combo("Frame pacing", &s.pacingMode, "Smooth (every game frame once, in order)\0Lowest latency (newest frame)\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lowest latency shows the newest frame -- best at the Refresh cap, and needed by the hold at\nthe frame start. Smooth holds up to one finished frame in reserve (about one refresh more latency)\nso every frame is lit once and in order -- it only helps at the Half or Quarter cap.");
        if (s.pacingMode == 0)
        {
            ImGui::Indent(16.0f);
            bool cu = s.smoothCatchUp != 0;
            if (ImGui::Checkbox("Catch up after a hitch", &cu)) s.smoothCatchUp = cu ? 1 : 0;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off (default): after a game hitch the presenter stays one frame behind (about one cycle\n"
                                                          "more latency) and shows every frame once, in order.\n"
                                                          "On: when two finished frames keep waiting, one is skipped to get back to the lowest\n"
                                                          "latency -- a second stutter shortly after the first.");
            ImGui::Unindent(16.0f);
        }
        bool aa = s.autoAhead != 0;
        if (ImGui::Checkbox("Automatic frames ahead (1, or 2 while the game misses cycles)", &aa)) s.autoAhead = aa ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many frames the game may queue on the GPU ahead of the screen.\n1 = lowest input lag (like a low-latency mode); 2 if the frame rate drops.");
        ImGui::Indent(16.0f);
        if (!s.autoAhead) ImGui::SliderInt("Max frames ahead (0 = off)", &s.maxAhead, 0, 3);
        else ImGui::TextDisabled("Frames ahead in use: %d", st.aheadInUse);
        ImGui::Unindent(16.0f);
        ImGui::TextDisabled("Fine-tuning (later release at Refresh rate, the just-in-time margin, lining Reflex's limit up): Advanced.");
        ImGui::Spacing();
        bool fw = s.frameWarp != 0;
        if (ImGui::Checkbox("Frame warp: fill missing frames (Ctrl+Shift+W)", &fw)) s.frameWarp = fw ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("For games that run just under the strobe rate (50-59 fps against a 60 Hz strobe). A strobe\n"
                                                      "cycle without a new game frame normally lights the last one again: the picture stands\n"
                                                      "still for a flash, then jumps. Frame warp shows that frame moved part of the way along the\n"
                                                      "camera's motion instead. Frames that arrive in time are shown exactly as rendered, and no\n"
                                                      "frame is held back, so it adds no latency (unlike frame generation).\n"
                                                      "\n"
                                                      "Worth trying: a game that holds about 75-99%% of the strobe rate and misses a frame now and\n"
                                                      "then, in camera pans and turns.\n"
                                                      "Not useful: the game holds the strobe rate (nothing to fill); far below it (use the Half\n"
                                                      "cap or frame generation); for hitches and loading stalls; for things that move on their own\n"
                                                      "(a character, a car), which keep their stutter. With frame generation it works, with less to fill.\n"
                                                      "A filled flash is a guess: edges of things moving against the background can smear for that\n"
                                                      "flash, and a CRT shader's mask moves with the picture (see Snap below). Details: the settings guide.");
        {
            bool ch = s.crtHandoff != 0;
            if (ImGui::Checkbox("Apply CRT-Standalone after frame warp", &ch)) s.crtHandoff = ch ? 1 : 0;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("CRT-Standalone's mask, scanlines, halation and glow -- and its passes after them: sharpen,\n"
                                                          "scanline soften, grain, noise floor, gamut expand, corner round -- are drawn by the presenter\n"
                                                          "after frame warp, so a moved frame doesn't move them with the picture (moire, mostly in the\n"
                                                          "sky). Needs BFI_PRESENTER_HANDOFF=1 in CRT-Standalone's preprocessor definitions too: its\n"
                                                          "settings stay in its own window. DirectX 12; not with its PIPELINE 1/2 or ENABLE_SCANLINE_SS=1 yet.\n"
                                                          "Costs GPU time (the CRT picture once per frame, and again on every flash frame warp moves) and\n"
                                                          "video memory: only turn it on in a game where the moire is visible, and leave it off elsewhere.");
            if (s.crtHandoff)
            {
                const int cs = crt::StateNow();
                ImGui::SameLine();
                ImGui::TextDisabled(cs == 2 ? "(in use)" : cs == 1 ? "(CRT-Standalone doesn't hand off)" : cs < 0 ? "(not with this CRT-Standalone setup)" : "(waiting)");
            }
        }
        if (s.frameWarp)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderFloat("Furthest ahead (game frames)", &s.warpMax, 0.25f, 1.5f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("A frame is never shown further ahead than this. Lower = fewer errors, but a\n"
                                                          "missing frame is only partly filled.");
            ImGui::SliderInt("Only from (% of the strobe rate)", &s.warpMinRate, 40, 95);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frame warp works while the game makes at least this share of the strobe rate\n"
                                                          "(75%% of a 60 Hz strobe = 45 fps). Below it frames repeat as without it: the\n"
                                                          "further a frame has to be moved, the more errors show.");
            if (st.warpOn < 0) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Frame warp is not available (see ReShade.log).");
            else if (st.warpOn == 2) ImGui::Text("At work: game at %.0f%% of the strobe rate, %d cycles a second moved (%.2f frames ahead on average).",
                                                 st.warpRate * 100.0, st.warpCycles, st.warpAvg);
            else if (st.warpRate > 0.0) ImGui::Text("Waiting: game at %.0f%% of the strobe rate.", st.warpRate * 100.0);
            else ImGui::Text("Waiting for the game's frame rate.");
            bool gm = s.warpGame != 0;
            if (ImGui::Checkbox("Use the game's own motion vectors and depth", &gm)) s.warpGame = gm ? 1 : 0;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the game hands DLSS (through Streamline or directly, DX12) its motion vectors and\n"
                                                          "depth, frame warp moves each part of the picture by the game's own motion for it, and\n"
                                                          "where things overlap the nearer one stays in front -- instead of the motion it measures\n"
                                                          "from the finished picture, which can only follow the camera. Other games keep the\n"
                                                          "measured motion. The line below says which one is in use.");
            if (s.warpGame)
            {
                ImGui::SameLine();
                bool ev = s.warpEven != 0;
                if (ImGui::Checkbox("Even pacing", &ev)) s.warpEven = ev ? 1 : 0;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("With the game's own motion only: every strobe cycle shows the picture where the game's motion\n"
                                                              "is at that moment, not just the cycles without a new frame, so every step on screen is the same.\n"
                                                              "Frames are moved a little all the time: any error shows on every flash, and the further the\n"
                                                              "game is from the strobe rate, the further frames are moved. Best just under it (e.g. 55-59 fps on\n"
                                                              "a 60 Hz strobe). Games without the game's motion fill missing frames as usual. Off by default.\n"
                                                              "Every flash is drawn moved: more GPU time than filling missing frames (see the settings guide).");
                bool en = s.warpEaseNoFg != 0;
                if (ImGui::Checkbox("Ease off as the motion slows", &en)) s.warpEaseNoFg = en ? 1 : 0;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("With the game's own motion, without frame generation too: a filled flash is moved less as the\n"
                                                              "game's motion slows (each point's motion against the frame before), so a camera coming to a\n"
                                                              "stop isn't carried past it and snapped back. With frame generation it is always on.");
                if (s.warpEaseNoFg || g_status.fgActive)
                {
                    ImGui::Indent(16.0f);
                    ImGui::SliderFloat("Ease off strength", &s.warpEaseStr, 0.0f, 3.0f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How much less a filled flash is moved as the motion slows. 0 = not at all, 1 = as before\n"
                                                                  "1.0.49, 2-3 = much less as soon as the camera slows: less overshoot when it stops (worth it in\n"
                                                                  "first-person games, where the whole picture moves with the camera, and with a CRT shader, whose\n"
                                                                  "scanlines make an overshoot easy to see), but a slowing pan is filled less too.");
                    ImGui::Unindent(16.0f);
                }
            }
            bool pl = s.warpPauseLb != 0;
            if (ImGui::Checkbox("Pause in letterboxed cutscenes", &pl)) s.warpPauseLb = pl ? 1 : 0;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("No frame warp while a cutscene is letterboxed through DLSS (its output smaller than the\n"
                                                          "screen, e.g. Townfall): its quick cuts between camera angles have no motion to follow.\n"
                                                          "Only after full-screen play in the same session, and not once a letterbox has lasted\n"
                                                          "10 minutes (play, in a game letterboxed by design). Turn it off for a game whose play\n"
                                                          "switches in and out of letterbox. Right after a camera cut the game flags, frames are\n"
                                                          "never filled either way.");
            {
                const bool fgOff = st.fgActive && !s.warpGameFg;   // the game's motion isn't used with frame generation
                const int src = st.warpCycles ? st.warpSrc : (s.warpGame && !fgOff && st.warpGameState > 0 ? st.warpGameState : 0);
                ImGui::Text("Motion used: %s%s", src == 1 ? "the game's motion vectors and depth" : src == 2 ? "the game's motion vectors (no depth)"
                                                       : "measured from the picture",
                            src <= 0 ? "" : st.warpGameVia == 1 ? ", from DLSS" : ", from Streamline");
                if (s.warpGame && fgOff && st.warpGameState > 0) { ImGui::SameLine(); ImGui::TextDisabled("(the game's is off with frame generation)"); }
                else if (s.warpGame && st.warpGameWhy[0]) { ImGui::SameLine(); ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.warpGameWhy); }
                else if (s.warpGame && st.warpGameState == 0 && ImGui::IsItemHovered())
                    ImGui::SetTooltip("This game hasn't handed DLSS any motion data (no Streamline or DLSS call seen).");
            }
            {
                bool sp = s.warpStillPre != 0;
                if (ImGui::Checkbox("Find the HUD before ReShade's effects", &sp)) s.warpStillPre = sp ? 1 : 0;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("What stands still on the screen (a HUD, text) is found in the game's frame from before\n"
                                                              "ReShade's effects, so a CRT shader's blur and glow over the HUD don't make it look as if\n"
                                                              "it moved (it broke up on filled flashes). Only while an effect is on: one more copy of\n"
                                                              "each frame. DirectX 12 and 11.");
            }
            ImGui::SliderInt("Edge dilation (depth texels)", &s.warpDilate, 0, 3, s.warpDilate > 0 ? "%d" : "off");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("With the game's motion: each point takes the motion of the nearest thing within this many of\n"
                                                          "the game's depth texels, so the edges of what is in front move with it instead of staying\n"
                                                          "behind with the background (outlines around characters). Higher reaches further, for\n"
                                                          "things close to the camera; too high and the background next to an edge moves with it\n"
                                                          "(a halo). 1 by default, 0 = off.");
            ImGui::SliderFloat("Snap moves across (px)", &s.warpSnapX, 0.0f, 16.0f, s.warpSnapX > 0.0f ? "%.2f" : "off");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves are rounded to whole steps of this many pixels sideways. With a CRT shader (e.g.\n"
                                                          "CRT-Standalone), set it to the mask's width so the mask stays in place. 0 = off.");
            ImGui::SliderFloat("Snap moves up and down (px)", &s.warpSnapY, 0.0f, 16.0f, s.warpSnapY > 0.0f ? "%.2f" : "off");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves are rounded to whole steps of this many pixels up and down. With a CRT shader,\n"
                                                          "set it to the scanline width (e.g. CRT-Standalone's Scanline Width) so the scanlines\n"
                                                          "don't break up where parts of the picture move by different amounts. 0 = off.");
            if ((s.warpSnapX > 0.0f || s.warpSnapY > 0.0f) && s.crtHandoff && crt::StateNow() == 2)
                ImGui::TextDisabled("Snap isn't used while CRT-Standalone is applied after frame warp: its scanlines no longer move.");
            {
                int em = s.warpEdge;
                if (ImGui::Combo("Screen edges", &em, "Fade the move\0Mirror the picture\0Repeat the edge\0")) s.warpEdge = em;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("A moved flash needs picture from beyond the screen's edge on the side new picture comes in\n"
                                                              "from. Fade (default): the move eases off toward that edge, so nothing has to be made up (the\n"
                                                              "strip moves a little less than the rest). Mirror: the picture next to the edge is reflected\n"
                                                              "into it. Repeat: the edge's last pixels are stretched over the strip, a smear as wide as the\n"
                                                              "move (before 1.0.45).");
            }
            if (s.warpGame)
            {
                bool gf = s.warpGameFg != 0;
                if (ImGui::Checkbox("Also with frame generation", &gf)) s.warpGameFg = gf ? 1 : 0;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The game's motion vectors are for the frames it renders, not the ones frame generation\n"
                                                              "adds in between, so on those the edges of moving things double up (outlines around\n"
                                                              "characters, e.g. 007 First Light, Directive 8020). Off by default: with frame generation\n"
                                                              "on, frame warp uses the motion measured from the picture.");
                if (s.warpGameFg)
                {
                    ImGui::SliderFloat("Furthest ahead with frame generation", &s.warpMaxGameFg, 0.1f, 1.5f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("With the game's own motion and frame generation on, a frame is never shown\n"
                                                                  "further ahead than this (shown frames). Further out it overshoots and snaps back.");
                }
                bool hud = s.warpGameHud != 0;
                if (ImGui::Checkbox("Keep the HUD still (the game's HUD-less picture)", &hud)) s.warpGameHud = hud ? 1 : 0;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the game hands its upscaler a picture without the HUD (e.g. 007 First Light),\n"
                                                              "whatever differs from it is HUD and is never moved. Switches itself off if the\n"
                                                              "two don't match (effects drawn after it, ReShade's included).");
                if (s.warpGameHud)
                    ImGui::TextDisabled(st.warpHud > 0 ? "HUD mask: in use" : st.warpHud < 0 ? "HUD mask: off (the HUD-less picture doesn't match the frame)"
                                                       : "HUD mask: the game gives no HUD-less picture");
            }
            ImGui::Unindent(16.0f);
        }
    }

    // ================================================================ 4. status
    if (st.managed && ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
    {
        static const char *outs[] = { "SDR", "scRGB", "HDR10", "-", "SDR shown as HDR10" };
        ImGui::Text("Game: %.1f fps   Refreshes: %.1f/s (display %.1f Hz)   Frame age: %.1f ms",
                    st.gameFps, st.refreshHz, st.displayHz, st.frameAgeMs);
        ImGui::Text("Uneven strobe cycles: %d/s (a frame repeated %d, frames skipped %d)",
                    st.unevenRep + st.unevenSkip, st.unevenRep, st.unevenSkip);
        ImGui::Text("Late refreshes: %d/s   Display repeats: %d/s", st.late, st.repeats);
        ImGui::Text("Presenter: %s   Output: %s   Format: %u   %ux%u, %u buffers",
                    st.running ? "running" : "stopped",
                    (st.outType >= 0 && st.outType <= 4) ? outs[st.outType] : "-", st.format,
                    st.width, st.height, st.buffers);
        ImGui::Text("Presenter queue: %s", g_queuePriority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME ? "global realtime" :
                                            g_queuePriority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH ? "high (realtime: Manager > GPU priority, or run the launcher as administrator)" : "normal");
        if (g_scOwner[0]) ImGui::TextDisabled("Swap chain implemented by %s", g_scOwner);
    }

    // ================================================================ 5. advanced
    if (ImGui::CollapsingHeader("Advanced"))
    {
        // ---------------------------------------------------------- latency fine-tuning (moved from Frame pacing)
        ImGui::SeparatorText("Latency fine-tuning");
        bool lr = s.laterRelease != 0;
        const bool lrRfx = s.gameCap == 1 && st.rfxLimitUs > 0;   // Reflex's frame limit holds the game: later release idles
        ImGui::BeginDisabled(lrRfx);
        if (ImGui::Checkbox("Later release at the Refresh rate cap (Lowest latency, no frame generation)", &lr)) s.laterRelease = lr ? 1 : 0;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Lets the game start each frame just in time for the refresh that shows it, instead of\na full refresh earlier, so its input is read later -- up to ~5 ms less latency in games\nthat render well within a refresh. Does nothing while frames take nearly a whole refresh. Off by default.\nNot used while Reflex's frame limit holds the game.");
        ImGui::EndDisabled();
        if (lrRfx) { ImGui::SameLine(); ImGui::TextDisabled("(not used: Reflex's frame limit paces this game)"); }
        else if (s.laterRelease && s.gameCap == 1)
        {
            static const char *kLrNames2[] = { "off (needs Wait for vblank and Lowest latency)", "measuring", "active", "at its limit (frames finish too close to the refresh)" };
            ImGui::Indent(16.0f);
            ImGui::TextDisabled("%s", kLrNames2[st.lrState & 3]);
            ImGui::Unindent(16.0f);
        }
        ImGui::SliderFloat("Just-in-time safety margin (ms)", &s.laterReleaseMs, 0.5f, 5.0f, "%.1f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How long before its refresh a game frame is due when the game is released just in time\n(just-in-time release below the refresh rate, later release at it). Widened by itself when frames miss.");
        bool ra = s.reflexAlign != 0;
        ImGui::BeginDisabled(!st.rfxGameUses);
        if (ImGui::Checkbox("Line Reflex's frame limit up with the flash", &ra)) s.reflexAlign = ra ? 1 : 0;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Games with Reflex, capped with Reflex's frame limit (e.g. the automatic frame cap) and without frame\n"
                                                      "generation: the presenter moves the moment the game starts each frame so the frame is ready just\n"
                                                      "before the lit refresh picks it, instead of waiting there for up to a whole strobe cycle.\n"
                                                      "Needs Lowest latency pacing. On by default since 1.0.39 (God of War Ragnarok, Dead Space,\n"
                                                      "Spider-Man 2, The Midnight Walk: as smooth or smoother, 2-7 ms less from release to flash in\n"
                                                      "games whose frames waited). With it off, frames ready more than\n"
                                                      "8 ms before the pick are still moved to 5 ms before it once (ReflexAlignCoarse=1 in the ini).");
        if (!st.rfxGameUses) { ImGui::SameLine(); ImGui::TextDisabled("(not used: no Reflex in this game)"); }
        else if (s.reflexAlign)
        {
            ImGui::SameLine();
            if (st.alignSlackMs >= 0.0) ImGui::TextDisabled("(frames ready %.1f ms before the pick, moved %+.1f ms)", st.alignSlackMs, st.alignShiftMs);
            else ImGui::TextDisabled(s.pacingMode == 0 ? "(not in use: needs Lowest latency pacing)" : "(not in use)");
        }
        ImGui::SeparatorText("Presenter");
        bool vb = s.pacing != 0, oq = s.ownQueue != 0;
        if (ImGui::Checkbox("Wait for vblank", &vb)) s.pacing = vb ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("One present per refresh, nothing queued: lowest latency. Needed by just-in-time release.");
        bool tb = s.threadBoost != 0;
        if (ImGui::Checkbox("Multimedia scheduler for the presenter thread (applies at the next start)", &tb)) s.threadBoost = tb ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Registers the presenter thread with Windows' Multimedia Class Scheduler (the one audio and\n"
                                                      "the desktop compositor use), which runs it ahead of every normal thread, the game's included.\n"
                                                      "A late strobe flickers; a late game frame is only a stutter. On by default; the log says\n"
                                                      "whether it took effect. Power throttling is turned off for the thread either way.");
        bool qa = s.queueAhead != 0;
        if (ImGui::Checkbox("Queue one refresh ahead (needs 3 presenter buffers, Wait for vblank on)", &qa)) s.queueAhead = qa ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps exactly one extra present queued, so an occasional late slot is covered\nwithout a flash. +1 refresh (8 ms) of latency. Unlike Wait for vblank off, the\nqueue never grows, so the driver doesn't throttle the game.");
        bool qd = s.queueDark != 0;
        if (ImGui::Checkbox("Queue the dark refresh with the lit one (needs 3 presenter buffers, Wait for vblank on)", &qd)) s.queueDark = qd ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The dark refresh shows the same game frame, so it is drawn and presented right after the lit one\n"
                                                      "instead of one refresh later. A stall while it is drawn then has two refreshes of room: fewer\n"
                                                      "late refreshes, and the ones left are mostly dark flickers rather than bright flashes.\n"
                                                      "No added latency. Turns itself off for the session if the swap chain can't take it. On by default.");
        if (s.queueDark && s.enabled && s.frames >= 2)
        {
            // Why it isn't running: the settings that rule it out, then what the presenter found.
            const int bufs = st.presBufs > 0 ? st.presBufs : s.presBuffers;
            const char *why = !s.pacing ? "not in use: needs Wait for vblank on" :
                              s.queueAhead ? "not in use while Queue one refresh ahead is on" :
                              bufs < 3 ? (s.presBuffers >= 3 ? "not in use yet: 3 presenter buffers apply at the game's next resize"
                                                             : "not in use: needs 3 presenter buffers") :
                              GradualSaverOn(s) ? "not in use while an LCD saver is on" :
                              g_darkState.load() == 2 ? "off for now: the queued presents kept waiting (see the log; it tries again later)" :
                              g_darkState.load() == 3 ? "off for now: the overlays (Show overlays) hold up the queued present; "
                                                        "turn Show overlays off to use it" :
                              g_darkState.load() == 4 ? "off for now: a frame limiter holds the presenter's presents (RTSS, the NVIDIA "
                                                        "control panel's Max Frame Rate, or Low Latency Mode Ultra / Reflex with G-SYNC + VSync); "
                                                        "see the log" : nullptr;
            if (why)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
                ImGui::TextWrapped("Dark refresh queue %s.", why);
                ImGui::PopStyleColor(1);
                if (!s.queueAhead && s.pacing && s.presBuffers < 3)
                {
                    if (ImGui::SmallButton("Use 3 presenter buffers")) { s.presBuffers = 3; IniWriteInt("PresenterBuffers", 3); }
                }
            }
            else if (g_darkState.load() == 1)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(in use)");
            }
        }
        ImGui::SliderInt("Presenter buffers (applies at the game's next resize)", &s.presBuffers, 2, 4);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Real swap chain buffers behind the presenter. With 3 or 4 AND 'Wait for vblank' off,\nthe presenter queues frames ahead, so a late slot no longer holds a lit or dark frame\ntwice (a flash) -- at the cost of 1-2 refreshes (8-17 ms) of extra latency.");
        if (ImGui::Checkbox("Present on own queue (applies when the game next creates its swap chain)", &oq)) s.ownQueue = oq ? 1 : 0;
        bool pr = s.selfResize != 0;
        if (ImGui::Checkbox("Resize the game's swap chain to start BFI (for games that never resize it)", &pr)) s.selfResize = pr ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("BFI (and SDR to HDR in DirectX 12) starts inside the game's first resize of its swap chain.\n"
                                                      "Most games do one at start-up; some never do (Hades II, Wreckfest 2, Pragmata), and BFI waits\n"
                                                      "until you change resolution or display mode. With this on, once the game runs steadily for 3 s,\n"
                                                      "the presenter resizes the swap chain itself (same size and format), as RenoDX does.\n"
                                                      "Only on the game's first swap chain: one the game recreates (God of War Ragnarok when frame\n"
                                                      "generation is turned on) still waits for the game's resize. Off by default (PresenterResize in the ini).");
        if (g_deferPresenter.load() && !g_ownPresentQueue && !g_dx11.load() && s.selfResize)
        {
            ImGui::SameLine();
            ImGui::TextDisabled(g_wrapperVtbl.load() ? "(BFI starts in a few seconds)" : "(not available here: no ReShade swap chain to hook)");
        }
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

#include "bfi_prefx.h"      // the frame from before ReShade's effects (FrameWarpStillBeforeEffects)
#include "bfi_crt.h"        // CRT-Standalone's mask and scanlines after frame warp (CrtStandaloneHandoff)

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
    reshade::unregister_event<reshade::addon_event::reshade_overlay>(on_osd);
    pfx::Register(false);
    crt::Register(false);
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
    reshade::register_event<reshade::addon_event::reshade_overlay>(on_osd);
    pfx::Register(true);
    crt::Register(true);
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
        reshade::register_event<reshade::addon_event::reshade_overlay>(on_osd);
        pfx::Register(true);
        crt::Register(true);
        ShaderPrepare();                                // compiled (or loaded) while the game starts up
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
