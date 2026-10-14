// ============================================================================
// BFI Presenter (Vulkan)  --  implicit Vulkan layer + ReShade add-on; version: BFI_VERSION below
//
// Copyright (C) 2026 artzox -- https://github.com/artzox/BFI-Presenter
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Presenter-clocked black frame insertion for Vulkan games (and DirectX 9-11
// games through DXVK). Same idea as the Direct3D 12 add-on:
//
//   * The layer sits between ReShade and the driver. It gives the game (and
//     ReShade above it) a swap chain of stand-in images; every game present
//     copies the finished image (with ReShade's effects already applied) into
//     a small staging ring.
//   * A presenter thread owns the real swap chain. Once per display refresh
//     (at vblank) it draws the newest staged frame through the BFI / Variable
//     MPRT shader into a real image and presents it on the layer's own queue,
//     so the strobe never depends on the game's frame timing.
//   * When ReShade is in the process the layer registers itself as a ReShade
//     add-on, so its settings window appears in the ReShade overlay.
//
// The layer does nothing unless BFIPresenter.ini is next to the game's exe.
//
// Hotkeys: Ctrl+Shift+B toggle BFI, Ctrl+Shift+M method, Ctrl+Shift+R reload ini
// ============================================================================
#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <sys/stat.h>
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define BFI_EXPORT extern "C" __declspec(dllexport)
#else
#include <unistd.h>
#include <dlfcn.h>
#include <limits.h>
#define BFI_EXPORT extern "C" __attribute__((visibility("default")))
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shaders_spv.h"

#define BFI_VERSION "1.0.6"
static const char *kLayerName = "VK_LAYER_BFI_presenter";

// ---------------------------------------------------------------- platform
static double Now()
{
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}
static void SleepSec(double s)
{
    if (s > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(s));
}
// Sub-millisecond sleeps: a high-resolution waitable timer (Windows) for most
// of it, a spin for the last 0.3 ms. Used by just-in-time release.
static void SleepUntil(double target)
{
#ifdef _WIN32
    thread_local HANDLE timer = nullptr;
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    const double left = target - Now();
    if (left > 0.0004 && timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((left - 0.0003) * 1e7);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
    }
#else
    const double left = target - Now();
    if (left > 0.0004) std::this_thread::sleep_for(std::chrono::duration<double>(left - 0.0003));
#endif
    while (Now() < target) std::this_thread::yield();
}
static void SleepShort(double s) { SleepUntil(Now() + s); }
static std::string g_exeDir;     // with trailing separator
static std::string g_outDir;     // logs: the folder of BFIPresenter.ini
static std::string g_exeName;
static void FindExe()
{
    char p[4096] = {};
#ifdef _WIN32
    GetModuleFileNameA(nullptr, p, sizeof(p));
    const char *sep = "\\/";
#else
    ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
    if (n > 0) p[n] = 0;
    const char *sep = "/";
#endif
    std::string s = p;
    size_t k = s.find_last_of(sep);
    g_exeDir  = (k == std::string::npos) ? std::string() : s.substr(0, k + 1);
    g_exeName = (k == std::string::npos) ? s : s.substr(k + 1);
}

// ---------------------------------------------------------------- logging
static std::mutex g_logLock;
static FILE      *g_logFile = nullptr;
static void Log(const char *lvl, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(g_logLock);
    if (!g_logFile)
    {
        std::string path = (g_outDir.empty() ? g_exeDir : g_outDir) + "BFIPresenterVK.log";
        g_logFile = fopen(path.c_str(), "w");
        if (!g_logFile) return;
        fprintf(g_logFile, "BFI Presenter (Vulkan layer) v" BFI_VERSION " -- %s\n", g_exeName.c_str());
    }
    fprintf(g_logFile, "%10.3f %s %s\n", Now(), lvl, msg);
    fflush(g_logFile);
}
#define LOGI(...) Log("INFO ", __VA_ARGS__)
#define LOGW(...) Log("WARN ", __VA_ARGS__)
#define LOGE(...) Log("ERROR", __VA_ARGS__)

// ---------------------------------------------------------------- ini (portable, [BFI] section)
static std::string g_iniPath;
static bool IEq(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}
static std::map<std::string, std::string> ReadIni()
{
    std::map<std::string, std::string> kv;
    FILE *f = fopen(g_iniPath.c_str(), "r");
    if (!f) return kv;
    char line[512];
    bool inBfi = false;
    while (fgets(line, sizeof(line), f))
    {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        s = s.substr(b);
        if (s[0] == ';' || s[0] == '#') continue;
        if (s[0] == '[') { inBfi = (s.substr(0, 5) == "[BFI]" || s.substr(0, 5) == "[bfi]"); continue; }
        if (!inBfi) continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        size_t vb = v.find_first_not_of(" \t");
        v = vb == std::string::npos ? std::string() : v.substr(vb);
        size_t sc = v.find(';');                       // trailing comment
        if (sc != std::string::npos) { v = v.substr(0, sc); while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back(); }
        std::string kl = k;
        for (char &c : kl) c = (char)tolower((unsigned char)c);
        kv[kl] = v;
    }
    fclose(f);
    return kv;
}
// Rewrites the given keys in [BFI], keeping every other line (and comments).
[[maybe_unused]] static bool WriteIni(const std::vector<std::pair<std::string, std::string>> &keys, bool onlyMissing)
{
    std::vector<std::string> lines;
    if (FILE *f = fopen(g_iniPath.c_str(), "r"))
    {
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            lines.push_back(s);
        }
        fclose(f);
    }
    std::vector<bool> done(keys.size(), false);
    int bfiStart = -1, bfiEnd = -1;
    bool inBfi = false;
    for (size_t i = 0; i < lines.size(); i++)
    {
        std::string t = lines[i];
        size_t b = t.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        t = t.substr(b);
        if (t[0] == '[')
        {
            if (inBfi) bfiEnd = (int)i;
            inBfi = (t.substr(0, 5) == "[BFI]" || t.substr(0, 5) == "[bfi]");
            if (inBfi) { bfiStart = (int)i; bfiEnd = -1; }
            continue;
        }
        if (!inBfi || t[0] == ';' || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = t.substr(0, eq);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        for (size_t j = 0; j < keys.size(); j++)
            if (!done[j] && IEq(k, keys[j].first))
            {
                if (!onlyMissing) lines[i] = keys[j].first + "=" + keys[j].second;
                done[j] = true;
            }
    }
    if (bfiStart < 0) { lines.push_back("[BFI]"); bfiStart = (int)lines.size() - 1; bfiEnd = -1; }
    std::vector<std::string> add;
    for (size_t j = 0; j < keys.size(); j++)
        if (!done[j]) add.push_back(keys[j].first + "=" + keys[j].second);
    if (bfiEnd < 0) bfiEnd = (int)lines.size();
    // insert before trailing blank lines of the section
    int at = bfiEnd;
    while (at - 1 > bfiStart && lines[at - 1].find_first_not_of(" \t") == std::string::npos) at--;
    lines.insert(lines.begin() + at, add.begin(), add.end());
    FILE *f = fopen(g_iniPath.c_str(), "w");
    if (!f) return false;
    for (auto &l : lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    return true;
}

// ---------------------------------------------------------------- settings
// Same keys as the Direct3D 12 add-on, so one BFIPresenter.ini works for both.
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
    float rollDecay = 0.0f;   // rolling scan phosphor decay, in sub-frames (0 = hard bands + seam)
    float knee     = 0.75f;
    float peakNits = 1400.0f; // display peak (HDR ceiling)
    int   output   = -1;      // -1 auto, 0 SDR, 1 scRGB, 2 HDR10
    int   gameCap  = 1;       // 0 off, 1 refresh rate, 2 half refresh, 3 quarter refresh
    int   pacingMode = 0;     // 0 smooth (every game frame once, in order), 1 lowest latency (newest)
    int   capJit   = 1;       // Half / Quarter cap: release just in time for the lit refresh
    int   laterRelease = 0;   // Refresh cap + Lowest latency: release just in time for the pick
    float laterReleaseMs = 1.5f; // just-in-time safety margin (widened automatically)
    int   maxAhead = 1;       // game may run this many frames ahead of the GPU (0 = no limit)
    int   smoothCatchUp = 0;  // Smooth: 1 = skip a frame when two have waited for 3 cycles (standing latency)
    int   autoAhead = 1;      // automatic: 1 frame ahead, 2 while the game keeps missing strobe cycles
    int   presBuffers = 3;    // real swap chain images (read when the swap chain is created)
    int   ablGuard = 1;       // HDR: keep the lit refresh within the panel's full-screen budget
    int   ablBudget = 250;    // panel budget: average nits of a lit refresh after capping
    int   hitchGuard = 1;     // show every refresh lit while refreshes are missed or the game stalls
    int   hitchGapMs = 1000;  // no new game frame for this long counts as loading
    int   pacing   = 1;       // wait for vblank before each present
    int   queueAhead = 0;     // keep one extra refresh queued (covers a late draw; +1 refresh latency)
    int   log      = 0;
    int   ownQueue = 1;       // present on a queue of the layer's own (else share the game's)
    int   sdrToHdr = 1;       // SDR game + HDR display: present HDR10 so the lit refresh has headroom
    float sdrWhite = 200.0f;  // nits of SDR white in that mode (Windows' SDR content brightness)
    float sdrHiNits = 800.0f; // highlight expansion: SDR white highlights stretch to this (0 = off)
    float sdrKnee  = 0.65f;   // expansion starts at this SDR signal level (0..1, as the game outputs it)
};

static std::mutex g_cfgLock;            // guards g_shared and g_status
static Settings   g_shared;

static int   KvInt(const std::map<std::string, std::string> &kv, const char *k, int def)
{
    std::string kl = k;
    for (char &c : kl) c = (char)tolower((unsigned char)c);
    auto it = kv.find(kl);
    return it == kv.end() || it->second.empty() ? def : atoi(it->second.c_str());
}
static float KvFloat(const std::map<std::string, std::string> &kv, const char *k, float def)
{
    std::string kl = k;
    for (char &c : kl) c = (char)tolower((unsigned char)c);
    auto it = kv.find(kl);
    return it == kv.end() || it->second.empty() ? def : (float)atof(it->second.c_str());
}
template <class T> static T Clamp(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }

static void LoadSettings(Settings &s)
{
    const auto kv = ReadIni();
    s.enabled  = KvInt(kv, "Enabled", 1);
    s.method   = KvInt(kv, "Method", 1) ? 1 : 0;
    s.frames   = Clamp(KvInt(kv, "Frames", 2), 2, 8);
    s.gain     = Clamp(KvFloat(kv, "Gain", 0.5f), 0.1f, 1.0f);
    s.dfloor   = Clamp(KvFloat(kv, "DarkFloor", 0.0f), 0.0f, 1.0f);
    s.sine     = KvInt(kv, "SineBFI", 0);
    s.invert   = KvInt(kv, "InvertCycle", 0);
    s.rolloff  = KvInt(kv, "HighlightRolloff", 1);
    s.rolling  = KvInt(kv, "RollingScan", 0) ? 1 : 0;
    s.rollSeam = Clamp(KvFloat(kv, "RollingSeam", 0.08f), 0.0f, 0.5f);
    s.rollDecay = Clamp(KvFloat(kv, "RollingPhosphorDecay", 0.0f), 0.0f, 1.0f);
    s.knee     = KvFloat(kv, "RolloffKnee", 0.75f);
    s.peakNits = Clamp(KvFloat(kv, "DisplayPeakNits", 1400.0f), 80.0f, 10000.0f);
    s.gameCap  = KvInt(kv, "CapGameToRefresh", 1);
    if (s.gameCap < 0 || s.gameCap > 3) s.gameCap = 1;
    s.pacingMode = KvInt(kv, "FramePacing", 0) ? 1 : 0;
    s.capJit = KvInt(kv, "CapJustInTime", 1) ? 1 : 0;
    s.laterRelease = KvInt(kv, "LaterRelease", 0) ? 1 : 0;
    s.laterReleaseMs = Clamp(KvFloat(kv, "LaterReleaseMarginMs", 1.5f), 0.5f, 5.0f);
    s.maxAhead = Clamp(KvInt(kv, "MaxFramesAhead", 1), 0, 3);
    s.autoAhead = KvInt(kv, "AutoFramesAhead", 1) ? 1 : 0;
    s.smoothCatchUp = KvInt(kv, "SmoothCatchUp", 0) ? 1 : 0;
    s.presBuffers = Clamp(KvInt(kv, "PresenterBuffers", 3), 2, 4);
    s.ablGuard = KvInt(kv, "AblGuard", 1) ? 1 : 0;
    s.ablBudget = Clamp(KvInt(kv, "AblGuardBudgetNits", 250), 100, 1500);
    s.hitchGuard = KvInt(kv, "HitchGuard", 1) ? 1 : 0;
    s.hitchGapMs = Clamp(KvInt(kv, "HitchGuardLoadingMs", 1000), 150, 5000);
    s.pacing   = KvInt(kv, "WaitForVBlank", 1);
    s.queueAhead = KvInt(kv, "QueueAhead", 0) ? 1 : 0;
    s.log      = KvInt(kv, "Log", 0);
    s.ownQueue = KvInt(kv, "VulkanOwnQueue", 1) ? 1 : 0;
    s.sdrToHdr = Clamp(KvInt(kv, "SdrToHdr", 1), 0, 2);   // 2 = also in DXVK games
    s.sdrWhite = Clamp(KvFloat(kv, "SdrWhiteNits", 200.0f), 80.0f, 600.0f);
    s.sdrHiNits = KvFloat(kv, "SdrHighlightNits", 800.0f);
    if (s.sdrHiNits < 0.0f) s.sdrHiNits = 0.0f;
    if (s.sdrHiNits > 4000.0f) s.sdrHiNits = 4000.0f;
    s.sdrKnee = Clamp(KvFloat(kv, "SdrHighlightKnee", 0.65f), 0.3f, 0.95f);
    auto it = kv.find("output");
    std::string out = it == kv.end() ? "auto" : it->second;
    for (char &c : out) c = (char)tolower((unsigned char)c);
    s.output = out == "sdr" ? 0 : out == "scrgb" ? 1 : out == "hdr10" ? 2 : -1;
}
[[maybe_unused]] static std::vector<std::pair<std::string, std::string>> SettingsKeys(const Settings &s)
{
    char b[64];
    std::vector<std::pair<std::string, std::string>> k;
    auto I = [&](const char *n, int v) { snprintf(b, sizeof(b), "%d", v); k.push_back({n, b}); };
    auto F = [&](const char *n, float v) { snprintf(b, sizeof(b), "%.3f", v); k.push_back({n, b}); };
    I("Enabled", s.enabled); I("Method", s.method); I("Frames", s.frames); F("Gain", s.gain);
    F("DarkFloor", s.dfloor); I("SineBFI", s.sine); I("InvertCycle", s.invert); I("HighlightRolloff", s.rolloff);
    I("RollingScan", s.rolling); F("RollingSeam", s.rollSeam); F("RollingPhosphorDecay", s.rollDecay);
    I("CapJustInTime", s.capJit); I("LaterRelease", s.laterRelease); F("LaterReleaseMarginMs", s.laterReleaseMs);
    F("RolloffKnee", s.knee); F("DisplayPeakNits", s.peakNits);
    k.push_back({"Output", s.output == 0 ? "sdr" : s.output == 1 ? "scrgb" : s.output == 2 ? "hdr10" : "auto"});
    I("CapGameToRefresh", s.gameCap); I("FramePacing", s.pacingMode); I("MaxFramesAhead", s.maxAhead);
    I("AutoFramesAhead", s.autoAhead); I("SmoothCatchUp", s.smoothCatchUp); I("PresenterBuffers", s.presBuffers); I("AblGuard", s.ablGuard);
    I("AblGuardBudgetNits", s.ablBudget); I("HitchGuard", s.hitchGuard); I("HitchGuardLoadingMs", s.hitchGapMs);
    I("WaitForVBlank", s.pacing); I("QueueAhead", s.queueAhead); I("Log", s.log); I("VulkanOwnQueue", s.ownQueue);
    I("SdrToHdr", s.sdrToHdr); F("SdrWhiteNits", s.sdrWhite); F("SdrHighlightNits", s.sdrHiNits);
    F("SdrHighlightKnee", s.sdrKnee);
    return k;
}
static void LoadShared()
{
    Settings s;
    LoadSettings(s);
    std::lock_guard<std::mutex> l(g_cfgLock);
    g_shared = s;
}

// ---------------------------------------------------------------- status (overlay)
struct Status
{
    int    managed = 0;
    char   reason[256] = "Waiting for the game's swap chain.";
    int    running = 0;
    int    outType = -1;
    uint32_t width = 0, height = 0, images = 0, realImages = 0;
    int    format = 0, colorSpace = 0;
    double refreshHz = 0.0, gameFps = 0.0, frameAgeMs = 0.0;
    int    late = 0, frameRepeats = 0, frameSkips = 0, slotWaits = 0, reused = 0;
    int    unevenRep = 0, unevenSkip = 0;   // per second: strobe cycles short of / ahead of the expected step
    int    lrState = 0;                     // just-in-time release: 0 off, 1 measuring, 2 active, 3 at its limit
    double lrEst = 0.0, lrMargin = 0.0;
    int    lrMisses = 0;
    int    qaPrimes = 0, qaSkips = 0;           // queue ahead: extra refreshes queued / ticks skipped (per second)
    bool   hitchPaused = false;
    bool   limitSuspect = false;   // something caps the presenter below the display refresh
    int    aheadInUse = 1;
    double rawAvg = -1.0, litAvg = -1.0, ceilInUse = 0.0;
    int    sharedQueue = 0;
    int    layerOrder = 0;      // 0 unknown, 1 below ReShade (good), 2 ReShade not above the layer
    char   caller[64] = "";
};
static Status g_status;
static void SetReason(int managed, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(g_cfgLock);
    g_status.managed = managed;
    snprintf(g_status.reason, sizeof(g_status.reason), "%s", b);
}

// ---------------------------------------------------------------- dispatch
template <class T> static void *Key(T h) { return *(void **)h; }

#define BFI_INSTANCE_FUNCS(X) \
    X(DestroyInstance) X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceSurfaceSupportKHR) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceFormatProperties) X(DestroySurfaceKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR)

struct InstanceData
{
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
#define X(n) PFN_vk##n n = nullptr;
    BFI_INSTANCE_FUNCS(X)
#undef X
#ifdef _WIN32
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR = nullptr;
#endif
};

#define BFI_DEVICE_FUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) X(QueueWaitIdle) X(QueueSubmit) \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(AllocateMemory) X(FreeMemory) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
    X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateGraphicsPipelines) X(DestroyPipeline) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(ResetCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) \
    X(CmdCopyImageToBuffer) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(GetFenceStatus) \
    X(CreateSemaphore) X(DestroySemaphore)

// Optional entry points (may be absent: extension or version not enabled).
#define BFI_DEVICE_OPT_FUNCS(X) \
    X(QueueSubmit2) X(QueueSubmit2KHR) X(QueueBindSparse) X(AcquireNextImage2KHR) X(WaitForPresentKHR) \
    X(GetSwapchainStatusKHR) X(ReleaseSwapchainImagesEXT) X(GetDeviceQueue2)

struct Swapchain;
struct DeviceData
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    InstanceData *inst = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
#define X(n) PFN_vk##n n = nullptr;
    BFI_DEVICE_FUNCS(X)
    BFI_DEVICE_OPT_FUNCS(X)
#undef X
    bool     ok = false;               // all required functions found
    uint32_t family = UINT32_MAX;      // queue family of the presenter queue
    VkQueue  queue = VK_NULL_HANDLE;   // presenter queue
    bool     sharedQueue = false;      // presenter queue is one of the game's (locked in every queue call)
    std::vector<uint32_t> families;    // every family the device created queues in
    VkPhysicalDeviceMemoryProperties mem = {};
    std::recursive_mutex qlock;        // external synchronisation of 'queue'
};

static std::mutex g_mapLock;
static std::unordered_map<void *, InstanceData *> g_instances;   // instance / physical device key
static std::unordered_map<void *, DeviceData *>   g_devices;     // device / queue / command buffer key
static std::unordered_map<uint64_t, void *>       g_surfaceWindow; // VkSurfaceKHR -> HWND

static InstanceData *GetInst(void *key)
{
    std::lock_guard<std::mutex> l(g_mapLock);
    auto it = g_instances.find(key);
    return it == g_instances.end() ? nullptr : it->second;
}
static DeviceData *GetDev(void *key)
{
    std::lock_guard<std::mutex> l(g_mapLock);
    auto it = g_devices.find(key);
    return it == g_devices.end() ? nullptr : it->second;
}

static bool g_active = false;      // BFIPresenter.ini found: the layer manages swap chains in this process
static std::atomic<bool> g_colorspaceExt{false};   // VK_EXT_swapchain_colorspace enabled (HDR10 presentation possible)
static int  g_dumpFrames = 0;      // diagnostics: write the first N presented refreshes as PPM files (8-bit formats)

// ---------------------------------------------------------------- swap chain state
static const uint32_t kMaxImg = 8;
static const uint32_t kStage  = 6;       // ring of finished game frames

struct Constants
{
    uint32_t outType, slot, frames, method;
    float    gain, dfloor, knee, ceilLin;
    uint32_t flags;
    float    sdrScale;    // SDR in, HDR10 out: SDR white in 80-nit units
    float    sdrKnee;     // highlight expansion knee (linear, 1 = SDR white)
    float    sdrPeak;     // expanded SDR white (80-nit units); <= sdrScale = off
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


static const int kLr = 64;               // just-in-time release: per-frame release records

struct CopyCmds
{
    VkCommandPool   pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[kStage] = {};
};

struct Swapchain
{
    DeviceData    *d = nullptr;
    VkSwapchainKHR real = VK_NULL_HANDLE;   // also the handle the game sees
    VkSurfaceKHR   surface = VK_NULL_HANDLE;
    void          *hwnd = nullptr;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D     extent = {};
    VkImageUsageFlags realUsage = 0;
    VkFormat       realFormat = VK_FORMAT_UNDEFINED;     // differs from 'format' when SDR is presented as HDR10
    bool           hdrUpgrade = false;

    // stand-in images the game renders to
    uint32_t       nVirt = 0;
    VkImage        virt[kMaxImg] = {};
    VkDeviceMemory virtMem[kMaxImg] = {};
    VkSemaphore    virtSem[kMaxImg] = {};   // signalled when the copy out of the image is done
    bool           virtPending[kMaxImg] = {};
    uint32_t       nextVirt = 0;

    // staging ring (game frames ready to be shown)
    VkImage        stage[kStage] = {};
    VkDeviceMemory stageMem[kStage] = {};
    VkImageView    stageView[kStage] = {};
    VkFence        copyFence[kStage] = {};
    bool           copySubmitted[kStage] = {};
    bool           copyDone[kStage] = {};
    uint64_t       stageId[kStage] = {};        // game frame number (0 = empty / being rewritten)
    double         stageTime[kStage] = {};
    uint64_t       stagePresentId[kStage] = {};
    uint64_t       slotReadSub[kStage] = {};    // presenter submission that last read the slot
    int            presCurSlot = -1;
    std::map<uint32_t, CopyCmds> copyCmds;      // per queue family the game presents from
    std::mutex     lock;                        // stage arrays
    std::mutex     submitLock;                  // game-side presents
    uint64_t       frameId = 0;
    uint64_t       recentId[8] = {};
    int            recentSlot[8] = {};
    uint64_t       nextTick = 0;

    // presenter
    uint32_t       nReal = 0;
    VkImage        realImg[kMaxImg] = {};
    std::thread    thread;
    std::atomic<bool> running{false}, failed{false}, stopReq{false};
    std::atomic<uint64_t> presDoneSub{0};
    std::atomic<uint64_t> ticks{0};
    std::mutex     tickLock;
    std::condition_variable tickCv;
    std::atomic<uint64_t> shownPresentId{0};
    std::mutex     presIdLock;
    std::condition_variable presIdCv;
    std::atomic<int>  status{VK_SUCCESS};       // VK_ERROR_OUT_OF_DATE_KHR etc. from the real swap chain
    std::atomic<bool> suboptimal{false};
    std::atomic<bool> retired{false};
    std::atomic<int>  maxAhead{1};
    std::atomic<long long> capWaitUs{0};
    std::atomic<int>  capWaits{0}, slotWaits{0}, staged{0};
    std::atomic<double> lastPresentT{0.0};
    bool           firstFrameLogged = false;

    // Frame cap alignment and just-in-time release (DX12 add-on v0.69, v0.73-v0.76)
    std::atomic<int>    tickSlot{0}, strobeFrames{2};   // strobe slot of the refresh drawn at the last tick
    std::atomic<double> tickT{0.0};                     // Now() of the last tick
    std::atomic<double> period{1.0 / 120.0};
    std::atomic<bool>   lrAllowed{false};
    std::atomic<double> lrLead{0.0};                    // release this long before the target refresh (0 = as before)
    std::atomic<double> lrWindow{1.0 / 120.0};          // time between releases at the current cap
    std::atomic<float>  lrBaseMs{1.5f};
    uint64_t            jitTarget = 0;                  // tick of the lit refresh the last frame was released for
    double              lrRelT[kLr] = {}, lrDeadline[kLr] = {};
    uint64_t            lrRelId[kLr] = {};
    int                 lrSlot[kLr] = {};
    std::atomic<uint64_t> lrStaged{0};
    std::thread         lrThread;
    std::atomic<bool>   lrRun{false};
    std::mutex          lrMx;
    std::condition_variable lrCv;
};
static std::unordered_map<uint64_t, Swapchain *> g_swapchains;   // by real handle (g_mapLock)
static Swapchain *GetSC(VkSwapchainKHR h)
{
    std::lock_guard<std::mutex> l(g_mapLock);
    auto it = g_swapchains.find((uint64_t)h);
    return it == g_swapchains.end() ? nullptr : it->second;
}
static std::unordered_map<void *, uint32_t> g_queueFamily;        // VkQueue -> family (g_mapLock)
static uint32_t QueueFamilyOf(VkQueue q)
{
    std::lock_guard<std::mutex> l(g_mapLock);
    auto it = g_queueFamily.find((void *)q);
    return it == g_queueFamily.end() ? UINT32_MAX : it->second;
}

// ---------------------------------------------------------------- helpers
static uint32_t FindMemType(DeviceData *d, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (d->mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static bool CreateImageMem(DeviceData *d, const VkImageCreateInfo &ici, VkImage *img, VkDeviceMemory *mem)
{
    if (d->CreateImage(d->device, &ici, nullptr, img) != VK_SUCCESS) { *img = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements mr;
    d->GetImageMemoryRequirements(d->device, *img, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = FindMemType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = FindMemType(d, mr.memoryTypeBits, 0);
    if (ai.memoryTypeIndex == UINT32_MAX || d->AllocateMemory(d->device, &ai, nullptr, mem) != VK_SUCCESS)
    { *mem = VK_NULL_HANDLE; return false; }
    return d->BindImageMemory(d->device, *img, *mem, 0) == VK_SUCCESS;
}
static VkImageMemoryBarrier ImgBarrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return b;
}
static bool IsSrgbFormat(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_R8G8B8_SRGB: case VK_FORMAT_B8G8R8_SRGB: return true;
    default: return false;
    }
}
static int ResolveOutput(const Settings &s, const Swapchain *sc)
{
    const bool srgbFmt = IsSrgbFormat(sc->format);
    if (sc->hdrUpgrade) return srgbFmt ? 5 : 4;          // SDR game, HDR10 presentation
    if (s.output >= 0) return (s.output == 0 && srgbFmt) ? 3 : s.output;
    // A 16-bit float swap chain is scRGB whatever colour space it reports
    // (mods that upgrade SDR games to HDR leave it at sRGB).
    if (sc->format == VK_FORMAT_R16G16B16A16_SFLOAT) return 1;
    if (sc->colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) return 2;
    if (sc->colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) return 1;
    return srgbFmt ? 3 : 0;
}
static const char *OutName(int t)
{
    static const char *k[] = { "SDR", "HDR scRGB", "HDR10 (PQ)", "SDR (sRGB format)", "SDR shown as HDR10", "SDR (sRGB format) shown as HDR10" };
    return (t >= 0 && t <= 5) ? k[t] : "?";
}

// DXVK's dxgi.dll / d3d11.dll in the game's folder (or loaded from outside
// the system folder): the game runs through DXVK.
static bool DxvkInProcess()
{
#ifdef _WIN32
    wchar_t sys[MAX_PATH] = {}, exe[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t *bs = wcsrchr(exe, L'\\')) bs[1] = 0;
    for (const wchar_t *dll : { L"dxgi.dll", L"d3d11.dll" })
    {
        std::wstring f = std::wstring(exe) + dll;
        if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
        HMODULE m = GetModuleHandleW(dll);
        wchar_t path[MAX_PATH] = {};
        if (m && GetModuleFileNameW(m, path, MAX_PATH) && _wcsnicmp(path, sys, wcslen(sys)) != 0) return true;
    }
#endif
    return false;
}

// ---------------------------------------------------------------- vblank
struct VBlank
{
#ifdef _WIN32
    // The kernel's vblank event (D3DKMT, gdi32), not DXGI: in a game run
    // through DXVK, "dxgi.dll" in the process is DXVK's. Its WaitForVBlank
    // only estimates the vblank, and its factory brings up another Vulkan
    // instance and device from inside the presenter thread (Twelve Minutes
    // crashed or froze right after BFI started).
    bool      warned = false;
    struct OpenFromName { WCHAR DeviceName[32]; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
    struct WaitVbl      { UINT hAdapter; UINT hDevice; UINT VidPnSourceId; };
    struct CloseAd      { UINT hAdapter; };
    typedef LONG (APIENTRY *PFN_Open)(OpenFromName *);
    typedef LONG (APIENTRY *PFN_Wait)(const WaitVbl *);
    typedef LONG (APIENTRY *PFN_Close)(const CloseAd *);
    PFN_Open  pOpen = nullptr;
    PFN_Wait  pWait = nullptr;
    PFN_Close pClose = nullptr;
    UINT      adapter = 0, source = 0;
    HMONITOR  monitor = nullptr;
    double    modeHz = 0.0;     // the display mode's refresh rate
    void Close()
    {
        if (adapter && pClose) { CloseAd c = { adapter }; pClose(&c); }
        adapter = 0;
    }
    void Find(void *hwnd)
    {
        HMONITOR m = hwnd ? MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTONEAREST)
                          : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        if (m == monitor && adapter) return;
        Close();
        monitor = m;
        if (!pOpen)
        {
            HMODULE g = GetModuleHandleA("gdi32.dll");
            if (!g) g = LoadLibraryA("gdi32.dll");
            if (g)
            {
                pOpen  = (PFN_Open)(void *)GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName");
                pWait  = (PFN_Wait)(void *)GetProcAddress(g, "D3DKMTWaitForVerticalBlankEvent");
                pClose = (PFN_Close)(void *)GetProcAddress(g, "D3DKMTCloseAdapter");
            }
            if (!pOpen || !pWait) { pOpen = nullptr; if (!warned) { warned = true; LOGW("BFI Presenter: kernel vblank functions not found -- pacing by the present queue only"); } return; }
        }
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(m, &mi)) { if (!warned) { warned = true; LOGW("BFI Presenter: no monitor for the game's window -- pacing by the present queue only"); } return; }
        OpenFromName o = {};
        wcsncpy(o.DeviceName, mi.szDevice, 31);
        const LONG st = pOpen(&o);
        if (st != 0) { if (!warned) { warned = true; LOGW("BFI Presenter: vblank source for the game's monitor not opened (0x%08lX) -- pacing by the present queue only", (unsigned long)st); } return; }
        adapter = o.hAdapter;
        source = o.VidPnSourceId;
        DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            modeHz = (double)dm.dmDisplayFrequency;
        char name[64];
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, name, sizeof(name), nullptr, nullptr);
        LOGI("BFI Presenter: vblank pacing on %s (%.0f Hz, kernel vblank event)", name, modeHz);
    }
    bool Wait()
    {
        if (!adapter) return false;
        WaitVbl w = { adapter, 0, source };
        if (pWait(&w) == 0) return true;
        Close();                        // display changed: reopened at the next Find
        monitor = nullptr;
        return false;
    }
    void Release() { Close(); monitor = nullptr; }
#else
    // Test builds: a simulated 120 Hz vblank.
    double next = 0.0;
    double modeHz = 120.0;
    void Find(void *) {}
    bool Wait()
    {
        const double p = 1.0 / 120.0, t = Now();
        if (next < t - p) next = std::ceil(t / p) * p;
        SleepSec(next - t);
        next += p;
        return true;
    }
    void Release() {}
#endif
};

static bool KeyCombo(int vk)
{
#ifdef _WIN32
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(vk) & 0x8000);
#else
    (void)vk;
    return false;
#endif
}

// ---------------------------------------------------------------- presenter resources
struct PresenterRes
{
    VkRenderPass          rp = VK_NULL_HANDLE, rpMeas = VK_NULL_HANDLE;
    VkImageView           realView[kMaxImg] = {};
    VkFramebuffer         fb[kMaxImg] = {};
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout      pl = VK_NULL_HANDLE;
    VkShaderModule        vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, fsMeas = VK_NULL_HANDLE;
    VkPipeline            pso = VK_NULL_HANDLE, psoMeas = VK_NULL_HANDLE;
    VkSampler             sampler = VK_NULL_HANDLE;
    VkDescriptorPool      dp = VK_NULL_HANDLE;
    VkDescriptorSet       ds[kStage] = {};
    VkCommandPool         pool = VK_NULL_HANDLE;
    VkCommandBuffer       cmd[kMaxImg] = {};
    VkFence               fence[kMaxImg] = {};
    VkSemaphore           done[kMaxImg] = {};
    VkSemaphore           acq[kMaxImg + 1] = {};
    VkImage               measImg = VK_NULL_HANDLE;
    VkDeviceMemory        measMem = VK_NULL_HANDLE;
    VkImageView           measView = VK_NULL_HANDLE;
    VkFramebuffer         measFb = VK_NULL_HANDLE;
    VkBuffer              measBuf = VK_NULL_HANDLE;
    VkDeviceMemory        measBufMem = VK_NULL_HANDLE;
    float                *measPtr = nullptr;
    VkBuffer              dumpBuf = VK_NULL_HANDLE;
    VkDeviceMemory        dumpMem = VK_NULL_HANDLE;
    uint8_t              *dumpPtr = nullptr;
};

static VkRenderPass MakeRenderPass(DeviceData *d, VkFormat fmt, VkImageLayout finalLayout)
{
    VkAttachmentDescription a = {};
    a.format = fmt;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = finalLayout;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = {};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = 0;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    ci.dependencyCount = 2;
    ci.pDependencies = dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (d->CreateRenderPass(d->device, &ci, nullptr, &rp) != VK_SUCCESS) rp = VK_NULL_HANDLE;
    return rp;
}
static VkShaderModule MakeShader(DeviceData *d, const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (d->CreateShaderModule(d->device, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    return m;
}
static VkPipeline MakePipeline(DeviceData *d, VkRenderPass rp, VkPipelineLayout pl, VkShaderModule vs, VkShaderModule fs)
{
    VkPipelineShaderStageCreateInfo st[2] = {};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {};
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = pl;
    ci.renderPass = rp;
    VkPipeline p = VK_NULL_HANDLE;
    if (d->CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return p;
}

// Creates what the presenter needs. Returns false only if nothing can be
// shown at all; without the BFI pipeline the presenter falls back to copying
// frames straight to the screen (copy mode).
static bool CreatePresenterRes(Swapchain *sc, PresenterRes &R)
{
    DeviceData *d = sc->d;
    VkDevice dev = d->device;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d->family;
    if (d->CreateCommandPool(dev, &pci, nullptr, &R.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = R.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = sc->nReal;
    if (d->AllocateCommandBuffers(dev, &cai, R.cmd) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < sc->nReal; i++)
        if (d->setLoaderData) d->setLoaderData(dev, R.cmd[i]);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < sc->nReal; i++)
    {
        if (d->CreateFence(dev, &fci, nullptr, &R.fence[i]) != VK_SUCCESS) return false;
        if (d->CreateSemaphore(dev, &sci, nullptr, &R.done[i]) != VK_SUCCESS) return false;
    }
    for (uint32_t i = 0; i <= sc->nReal; i++)
        if (d->CreateSemaphore(dev, &sci, nullptr, &R.acq[i]) != VK_SUCCESS) return false;

    // BFI pipeline (optional: copy mode without it)
    bool p = true;
    R.rp = MakeRenderPass(d, sc->realFormat, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    p = R.rp != VK_NULL_HANDLE && (sc->realUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    for (uint32_t i = 0; p && i < sc->nReal; i++)
    {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = sc->realImg[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = sc->realFormat;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        p = d->CreateImageView(dev, &vci, nullptr, &R.realView[i]) == VK_SUCCESS;
        if (!p) break;
        VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbi.renderPass = R.rp;
        fbi.attachmentCount = 1;
        fbi.pAttachments = &R.realView[i];
        fbi.width = sc->extent.width;
        fbi.height = sc->extent.height;
        fbi.layers = 1;
        p = d->CreateFramebuffer(dev, &fbi, nullptr, &R.fb[i]) == VK_SUCCESS;
    }
    if (p)
    {
        VkDescriptorSetLayoutBinding b = {};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dli.bindingCount = 1;
        dli.pBindings = &b;
        p = d->CreateDescriptorSetLayout(dev, &dli, nullptr, &R.dsl) == VK_SUCCESS;
    }
    if (p)
    {
        VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants) };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &R.dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        p = d->CreatePipelineLayout(dev, &pli, nullptr, &R.pl) == VK_SUCCESS;
    }
    if (p)
    {
        R.vs = MakeShader(d, kVertSpv, sizeof(kVertSpv));
        R.fs = MakeShader(d, kBfiSpv, sizeof(kBfiSpv));
        R.fsMeas = MakeShader(d, kMeasSpv, sizeof(kMeasSpv));
        p = R.vs && R.fs;
    }
    if (p) { R.pso = MakePipeline(d, R.rp, R.pl, R.vs, R.fs); p = R.pso != VK_NULL_HANDLE; }
    if (p)
    {
        VkSamplerCreateInfo smi = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        smi.magFilter = smi.minFilter = VK_FILTER_NEAREST;
        smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        smi.maxLod = 0.0f;
        p = d->CreateSampler(dev, &smi, nullptr, &R.sampler) == VK_SUCCESS;
    }
    if (p)
    {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kStage };
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = kStage;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        p = d->CreateDescriptorPool(dev, &dpi, nullptr, &R.dp) == VK_SUCCESS;
        VkDescriptorSetLayout l[kStage];
        for (uint32_t i = 0; i < kStage; i++) l[i] = R.dsl;
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool = R.dp;
        dai.descriptorSetCount = kStage;
        dai.pSetLayouts = l;
        p = p && d->AllocateDescriptorSets(dev, &dai, R.ds) == VK_SUCCESS;
        for (uint32_t i = 0; p && i < kStage; i++)
        {
            VkDescriptorImageInfo ii = { R.sampler, sc->stageView[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = R.ds[i];
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &ii;
            d->UpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        }
    }
    if (!p)
    {
        if (R.pso) { d->DestroyPipeline(dev, R.pso, nullptr); R.pso = VK_NULL_HANDLE; }
        LOGE("BFI Presenter: BFI pipeline unavailable -- frames are copied to the screen without BFI");
        return (sc->realUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 && sc->realFormat == sc->format;
    }

    if (g_dumpFrames && (sc->realUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = (VkDeviceSize)sc->extent.width * sc->extent.height * 8;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkMemoryRequirements mr;
        if (d->CreateBuffer(dev, &bci, nullptr, &R.dumpBuf) == VK_SUCCESS)
        {
            d->GetBufferMemoryRequirements(dev, R.dumpBuf, &mr);
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = FindMemType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (ai.memoryTypeIndex != UINT32_MAX && d->AllocateMemory(dev, &ai, nullptr, &R.dumpMem) == VK_SUCCESS &&
                d->BindBufferMemory(dev, R.dumpBuf, R.dumpMem, 0) == VK_SUCCESS)
                d->MapMemory(dev, R.dumpMem, 0, VK_WHOLE_SIZE, 0, (void **)&R.dumpPtr);
        }
    }

    // ABL guard: 32x18 float target + host-visible readback (optional)
    bool m = R.fsMeas != VK_NULL_HANDLE;
    R.rpMeas = m ? MakeRenderPass(d, VK_FORMAT_R32_SFLOAT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) : VK_NULL_HANDLE;
    m = m && R.rpMeas;
    if (m)
    {
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R32_SFLOAT;
        ici.extent = { 32, 18, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        m = CreateImageMem(d, ici, &R.measImg, &R.measMem);
    }
    if (m)
    {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = R.measImg;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R32_SFLOAT;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        m = d->CreateImageView(dev, &vci, nullptr, &R.measView) == VK_SUCCESS;
    }
    if (m)
    {
        VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbi.renderPass = R.rpMeas;
        fbi.attachmentCount = 1;
        fbi.pAttachments = &R.measView;
        fbi.width = 32;
        fbi.height = 18;
        fbi.layers = 1;
        m = d->CreateFramebuffer(dev, &fbi, nullptr, &R.measFb) == VK_SUCCESS;
    }
    if (m) { R.psoMeas = MakePipeline(d, R.rpMeas, R.pl, R.vs, R.fsMeas); m = R.psoMeas != VK_NULL_HANDLE; }
    if (m)
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = 32 * 18 * sizeof(float);
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        m = d->CreateBuffer(dev, &bci, nullptr, &R.measBuf) == VK_SUCCESS;
        VkMemoryRequirements mr;
        if (m) d->GetBufferMemoryRequirements(dev, R.measBuf, &mr);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        if (m)
        {
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = FindMemType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            m = ai.memoryTypeIndex != UINT32_MAX && d->AllocateMemory(dev, &ai, nullptr, &R.measBufMem) == VK_SUCCESS;
        }
        m = m && d->BindBufferMemory(dev, R.measBuf, R.measBufMem, 0) == VK_SUCCESS;
        m = m && d->MapMemory(dev, R.measBufMem, 0, VK_WHOLE_SIZE, 0, (void **)&R.measPtr) == VK_SUCCESS;
    }
    if (!m)
    {
        LOGW("BFI Presenter: ABL guard unavailable (could not create its measurement objects)");
        if (R.psoMeas) d->DestroyPipeline(dev, R.psoMeas, nullptr);
        R.psoMeas = VK_NULL_HANDLE;
        R.measPtr = nullptr;
    }
    return true;
}

static void DestroyPresenterRes(DeviceData *d, PresenterRes &R)
{
    VkDevice dev = d->device;
    for (uint32_t i = 0; i < kMaxImg; i++)
    {
        if (R.fb[i]) d->DestroyFramebuffer(dev, R.fb[i], nullptr);
        if (R.realView[i]) d->DestroyImageView(dev, R.realView[i], nullptr);
        if (R.fence[i]) d->DestroyFence(dev, R.fence[i], nullptr);
        if (R.done[i]) d->DestroySemaphore(dev, R.done[i], nullptr);
    }
    for (uint32_t i = 0; i <= kMaxImg; i++)
        if (R.acq[i]) d->DestroySemaphore(dev, R.acq[i], nullptr);
    if (R.pool) d->DestroyCommandPool(dev, R.pool, nullptr);
    if (R.pso) d->DestroyPipeline(dev, R.pso, nullptr);
    if (R.psoMeas) d->DestroyPipeline(dev, R.psoMeas, nullptr);
    if (R.vs) d->DestroyShaderModule(dev, R.vs, nullptr);
    if (R.fs) d->DestroyShaderModule(dev, R.fs, nullptr);
    if (R.fsMeas) d->DestroyShaderModule(dev, R.fsMeas, nullptr);
    if (R.pl) d->DestroyPipelineLayout(dev, R.pl, nullptr);
    if (R.dsl) d->DestroyDescriptorSetLayout(dev, R.dsl, nullptr);
    if (R.dp) d->DestroyDescriptorPool(dev, R.dp, nullptr);
    if (R.sampler) d->DestroySampler(dev, R.sampler, nullptr);
    if (R.rp) d->DestroyRenderPass(dev, R.rp, nullptr);
    if (R.rpMeas) d->DestroyRenderPass(dev, R.rpMeas, nullptr);
    if (R.measFb) d->DestroyFramebuffer(dev, R.measFb, nullptr);
    if (R.measView) d->DestroyImageView(dev, R.measView, nullptr);
    if (R.measImg) d->DestroyImage(dev, R.measImg, nullptr);
    if (R.measMem) d->FreeMemory(dev, R.measMem, nullptr);
    if (R.measBuf) d->DestroyBuffer(dev, R.measBuf, nullptr);
    if (R.measBufMem) d->FreeMemory(dev, R.measBufMem, nullptr);
    if (R.dumpBuf) d->DestroyBuffer(dev, R.dumpBuf, nullptr);
    if (R.dumpMem) d->FreeMemory(dev, R.dumpMem, nullptr);
    R = PresenterRes();
}

// ---------------------------------------------------------------- presenter thread
static void PresenterThread(Swapchain *sc)
{
    DeviceData *d = sc->d;
    VkDevice dev = d->device;
    PresenterRes R;
    const bool ok = CreatePresenterRes(sc, R);
    const bool copyMode = ok && R.pso == VK_NULL_HANDLE;
    if (!ok)
    {
        LOGE("BFI Presenter: presenter could not start -- nothing can be shown");
        DestroyPresenterRes(d, R);
        sc->failed.store(true);
        sc->running.store(false);
        sc->tickCv.notify_all();
        return;
    }
    VBlank vb;
    vb.Find(sc->hwnd);
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    LOGI("BFI Presenter: presenter running (%s, %s queue, %u real images)", copyMode ? "copy mode" : "BFI",
         d->sharedQueue ? "the game's" : "its own", sc->nReal);
    {
        std::lock_guard<std::mutex> l(g_cfgLock);
        g_status.running = 1;
    }

    Settings s;
    {
        std::lock_guard<std::mutex> l(g_cfgLock);
        s = g_shared;
    }
    uint64_t presents = 0, lastShownId = 0, presSub = 0, measSub = 0;
    uint64_t bufSub[kMaxImg] = {}, bufFrame[kMaxImg] = {};
    Constants bufConst[kMaxImg] = {};
    bool bufValid[kMaxImg] = {};
    uint32_t acqIdx = 0;
    int secFrameRepeats = 0, secFrameSkips = 0, extraCycles = 0, secReused = 0;
    int secRepU = 0, secSkipU = 0;      // strobe cycles short of / ahead of the expected step (DX12 add-on v0.78)
    double llStep = 0.0;                // recent game frames per strobe cycle
    // Queue one refresh ahead: one extra refresh is presented right after a
    // vblank ("prime"), so the display always has the next refresh queued and
    // a draw delayed by the game's GPU work (Indiana Jones: the present call
    // blocked a refresh ~10 times a minute -- a held lit or dark refresh, a
    // flash) is covered. The 3-image FIFO swap chain caps the depth; if a
    // present blocks anyway the queue is deeper than wanted and one tick is
    // skipped, and after a missed tick the extra refresh is primed again.
    bool qaPrimed = false, qaSkipWait = false, qaSkipNext = false;
    int  secQaSkips = 0, secQaPrimes = 0;
    int autoAhead = 1, missSecs = 0, spareSecs = 0;
    int lastOutType = -1, lastShown = -1;
    double ceilNow = -1.0, ceilTarget = -1.0, litAvg = -1.0, rawAvg = -1.0, ablSolve = -1.0, ablT = Now(), ablLowT = 0.0;
    double aheadDropT = -1.0;
    bool aheadLocked = false;
    double lastT = Now(), secStart = Now(), ageSum = 0.0;
    int secPresents = 0, secLate = 0;
    int limitSecs = 0;
    bool limitLogged = false;
    double lateTimes[4] = {};
    int lateIdx = 0;
    double lastNewFrameT = 0.0, hitchUntil = 0.0;
    bool hitchPaused = false;
    bool kb = false, km = false, kr = false, krs = false;
    FILE *logf = nullptr;
    double monitorCheckT = Now();
    int dumpLeft = g_dumpFrames;

    while (!sc->stopReq.load())
    {
        // hotkeys (edge-triggered) act on the shared settings
        const bool b = KeyCombo('B'), m = KeyCombo('M'), r = KeyCombo('R'), rs = KeyCombo('S');
        if ((b && !kb) || (m && !km) || (rs && !krs))
        {
            std::lock_guard<std::mutex> l(g_cfgLock);
            if (b && !kb) g_shared.enabled = !g_shared.enabled;
            if (m && !km) g_shared.method = !g_shared.method;
            if (rs && !krs) g_shared.rolling = !g_shared.rolling;
        }
        krs = rs;
        if (r && !kr) { LoadShared(); LOGI("BFI Presenter: settings reloaded"); }
        // BFIPresenter.ini edited while the game runs (no ReShade overlay in the game): picked up within a second.
        {
            static double iniCheckT = 0.0;
            static long long iniStamp = -1;
            const double tc = Now();
            if (tc - iniCheckT > 1.0)
            {
                iniCheckT = tc;
                struct stat st;
                const long long stamp = stat(g_iniPath.c_str(), &st) == 0 ? (long long)st.st_mtime * 1000 + (long long)st.st_size % 1000 : -2;
                if (iniStamp != -1 && stamp != iniStamp && stamp != -2) { LoadShared(); LOGI("BFI Presenter: BFIPresenter.ini changed -- settings reloaded"); }
                iniStamp = stamp;
            }
        }
        kb = b; km = m; kr = r;
        {
            std::lock_guard<std::mutex> l(g_cfgLock);
            s = g_shared;
        }
        // BFI off: a plain presenter, a new game frame every refresh. Kept at
        // the strobe's cycle, every second frame of a 120 fps game was dropped
        // and each shown twice (60 Hz motion), which also made a CRT shader's
        // own BFI flicker (Twelve Minutes).
        if (!s.enabled) s.frames = 1;

        // timing log: a new time-stamped file each time it is switched on
        if (s.log && !logf)
        {
            time_t now = time(nullptr);
            struct tm lt;
#ifdef _WIN32
            localtime_s(&lt, &now);
#else
            localtime_r(&now, &lt);
#endif
            char name[96];
            snprintf(name, sizeof(name), "BFIPresenterVK_log_%04d%02d%02d_%02d%02d%02d.csv", lt.tm_year + 1900, lt.tm_mon + 1,
                     lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
            std::string path = g_outDir + name;
            logf = fopen(path.c_str(), "w");
            if (logf)
            {
                fprintf(logf, "time_ms,present,slot,frame_id,frame_age_ms,interval_ms,bfi,method,frames,gain,floor,rolloff,"
                              "pacing,cap,ahead,vblank,wait_vblank_ms,acquire_ms,wait_gpu_ms,submit_ms,present_ms,reused,"
                              "hitch_paused,ceiling_nits,lit_avg_nits,raw_avg_nits,lr_state,lr_lead_ms,lr_est_ms,lr_margin_ms,queue_ahead,smooth_catchup\n");
                LOGI("BFI Presenter: timing log started: %s", path.c_str());
            }
        }
        else if (!s.log && logf)
        {
            fclose(logf);
            logf = nullptr;
            LOGI("BFI Presenter: timing log stopped");
        }

        const double tLoop0 = Now();
        if (tLoop0 - monitorCheckT > 2.0) { vb.Find(sc->hwnd); monitorCheckT = tLoop0; }   // window moved?
        bool vblank = false;
        const bool qa = s.queueAhead && s.pacing;
        if (!qa) { qaPrimed = false; qaSkipWait = false; qaSkipNext = false; }
        const bool primeIter = qaSkipWait;
        qaSkipWait = false;
        if (s.pacing && !primeIter) vblank = vb.Wait();
        // Release the game's frame cap at the vblank itself, not after this
        // refresh's present: the present's varying length shifted each release
        // and games timed their animation to the uneven gaps (DX12 add-on v0.67).
        if (vblank)
        {
            sc->strobeFrames.store(s.frames);
            sc->tickSlot.store((int)(presents % (uint64_t)s.frames));
            sc->tickT.store(Now());
            sc->ticks.fetch_add(1);
            sc->tickCv.notify_all();
        }
        const double tVb = Now();
        {
            // Just-in-time release: Half / Quarter cap (any pacing) when the
            // cap interval is a whole number of strobe cycles, or the Refresh
            // cap with Lowest latency + Later release. Needs vblank ticks.
            const int per = s.gameCap == 2 ? 2 : s.gameCap == 3 ? 4 : 1;
            const bool capOk = s.capJit && s.gameCap >= 2 && s.frames >= 2 && per % s.frames == 0;
            const bool llOk  = s.laterRelease && s.pacingMode == 1 && s.gameCap == 1;
            const double hz = vb.modeHz > 0.0 ? vb.modeHz : 120.0;
            sc->period.store(1.0 / hz);
            sc->lrWindow.store(per / hz);
            sc->lrBaseMs.store(s.laterReleaseMs);
            if (!primeIter) sc->lrAllowed.store((capOk || llOk) && vblank);
        }
        if (qa && qaSkipNext && vblank)
        {
            // The queue is deeper than one extra refresh: let this vblank take
            // a queued refresh without adding one.
            qaSkipNext = false;
            secQaSkips++;
            lastT = Now();
            continue;
        }
        // The frame this cycle should show (1 or 2 after the last one, from the
        // recent average) sometimes finishes a moment after the pick: the
        // refresh then repeats a frame and the next jumps two. If that frame
        // is already submitted, give it up to 3 ms (DX12 add-on v0.66).
        if ((presents % (uint64_t)s.frames) == 0 && lastShownId)
        {
            const int step = (int)std::floor(llStep + 0.5);
            const uint64_t want = lastShownId + (uint64_t)std::max(step, 1);
            const double w0 = Now();
            while (step >= 1 && Now() - w0 < 0.003)
            {
                bool pending = false;
                {
                    std::lock_guard<std::mutex> l(sc->lock);
                    for (uint32_t i = 0; i < kStage; i++)
                        if (sc->stageId[i] == want && sc->copySubmitted[i] && !sc->copyDone[i])
                        {
                            if (d->GetFenceStatus(dev, sc->copyFence[i]) == VK_SUCCESS) sc->copyDone[i] = true;
                            else pending = true;
                        }
                }
                if (!pending) break;
                SleepShort(0.0002);
            }
        }

        // Which game frame this refresh shows: the same rules as the D3D12
        // presenter (a new frame only at the start of each strobe cycle;
        // Smooth = oldest unseen, one in reserve; Lowest latency = newest).
        const uint32_t slotNow = (uint32_t)(presents % (uint64_t)s.frames);
        int pick = -1;
        uint64_t pickId = 0;
        double pickTime = 0.0;
        uint64_t pickPresentId = 0;
        {
            std::lock_guard<std::mutex> l(sc->lock);
            for (uint32_t i = 0; i < kStage; i++)
                if (sc->stageId[i] && sc->copySubmitted[i] && !sc->copyDone[i] &&
                    d->GetFenceStatus(dev, sc->copyFence[i]) == VK_SUCCESS)
                    sc->copyDone[i] = true;
            int newest = -1, oldestNew = -1, unseen = 0;
            uint64_t newestId = 0, oldestNewId = ~0ull;
            for (uint32_t i = 0; i < kStage; i++)
            {
                if (sc->stageId[i] == 0 || !sc->copyDone[i]) continue;
                if (sc->stageId[i] > newestId) { newest = (int)i; newestId = sc->stageId[i]; }
                if (sc->stageId[i] > lastShownId)
                {
                    unseen++;
                    if (sc->stageId[i] < oldestNewId) { oldestNew = (int)i; oldestNewId = sc->stageId[i]; }
                }
            }
            const bool cycleStart = (slotNow == 0) || lastShown < 0;
            if (!cycleStart && lastShown >= 0 && sc->stageId[lastShown] == lastShownId && sc->copyDone[lastShown])
                pick = lastShown;
            else if (s.pacingMode == 0 && oldestNew >= 0)
            {
                pick = oldestNew;
                if (cycleStart) extraCycles = (unseen >= 2) ? extraCycles + 1 : 0;
                if (s.smoothCatchUp && unseen <= 2 && extraCycles >= 3 && newest >= 0)
                {
                    // Two frames waiting for 3 cycles in a row is standing
                    // latency: show the newer one (DX12 add-on v0.70; picking
                    // "the newest but one" kept Smooth a full cycle behind).
                    pick = newest;
                    extraCycles = 0;
                }
                else if (unseen > 2)
                {
                    uint64_t bestId = 0;
                    for (uint32_t i = 0; i < kStage; i++)
                        if (sc->stageId[i] > lastShownId && sc->stageId[i] < newestId && sc->copyDone[i] && sc->stageId[i] > bestId)
                        { pick = (int)i; bestId = sc->stageId[i]; }
                    extraCycles = 0;
                }
            }
            else if (newest >= 0)
                pick = (s.pacingMode == 0 && lastShown >= 0 && sc->stageId[lastShown] == lastShownId) ? lastShown : newest;
            if (pick >= 0)
            {
                pickId = sc->stageId[pick];
                pickTime = sc->stageTime[pick];
                pickPresentId = sc->stagePresentId[pick];
                sc->presCurSlot = pick;
                if (cycleStart)
                {
                    if (pickId == lastShownId) secFrameRepeats++;
                    else if (lastShownId && pickId > lastShownId + 1) secFrameSkips += (int)(pickId - lastShownId - 1);
                    if (lastShownId && pickId >= lastShownId)
                    {
                        const int step = (int)(pickId - lastShownId);
                        if (s.gameCap == 3) { if (step >= 2) secSkipU++; }   // Quarter: 0/1 alternate normally
                        else
                        {
                            const int expect = std::max(1, (int)std::floor(llStep + 0.5));
                            if (step < expect) secRepU++;
                            else if (step > expect) secSkipU++;
                        }
                    }
                    if (lastShownId && pickId > lastShownId)
                    {
                        const double dd = (double)(pickId - lastShownId);
                        llStep = llStep > 0.0 ? 0.95 * llStep + 0.05 * std::min(dd, 8.0) : dd;
                    }
                }
            }
        }
        if (pick < 0)
        {
            if (!vblank) SleepSec(0.001);
            continue;                                      // nothing to show yet
        }

        Constants c = {};
        c.outType = (uint32_t)ResolveOutput(s, sc);
        if ((int)c.outType != lastOutType)
        {
            LOGI("BFI Presenter: output %s (%s, format %d, colour space %d)", OutName((int)c.outType),
                 s.output >= 0 ? "set in the settings" : "Auto", (int)sc->format, (int)sc->colorSpace);
            lastOutType = (int)c.outType;
        }
        c.frames = (uint32_t)s.frames;
        c.slot   = slotNow;
        c.method = (!s.enabled || copyMode) ? 2u : hitchPaused ? 3u : (uint32_t)s.method;

        // ABL guard (HDR): read the last measurement once its draw is done and
        // find the highest lit-refresh ceiling whose screen average stays
        // within the panel budget (see the D3D12 presenter, v0.52/v0.53).
        if (measSub && R.measPtr && sc->presDoneSub.load() >= measSub)
        {
            static float cells[32 * 18];
            for (int i = 0; i < 32 * 18; i++) cells[i] = std::max(R.measPtr[i], 0.0f) * 80.0f;
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
            litAvg = avgAt(ablSolve);
            rawAvg = avgAt(1.0e9);
            measSub = 0;
        }
        const bool ablOn = s.ablGuard && R.psoMeas && s.enabled && c.outType != 0 && c.outType != 3;   // HDR outputs (1, 2, 4, 5)
        ceilTarget = (ablOn && ablSolve > 0.0) ? std::min(ablSolve, (double)s.peakNits) : (double)s.peakNits;
        {
            const double nowT = Now(), dt = std::min(nowT - ablT, 0.1);
            ablT = nowT;
            if (ceilNow < 0.0 || !ablOn) ceilNow = ceilTarget;
            else if (ceilTarget < ceilNow - 5.0)
            {
                ablLowT = nowT;
                ceilNow += (ceilTarget - ceilNow) * (1.0 - std::exp(-dt / 0.08));
            }
            else if (nowT - ablLowT > 1.5)
                ceilNow += (ceilTarget - ceilNow) * (1.0 - std::exp(-dt / 2.5));
        }
        c.gain    = s.gain;
        c.dfloor  = s.dfloor;
        c.knee    = s.knee;
        c.ceilLin = (c.outType == 0 || c.outType == 3) ? 1.0f : (float)(std::floor(ceilNow / 10.0 + 0.5) * 10.0 / 80.0);
        c.flags   = (s.rolloff ? 1u : 0u) | (s.sine ? 2u : 0u) | (s.invert ? 4u : 0u) | (s.rolling ? 8u : 0u);
        c.seam    = s.rollSeam;
        c.decay   = s.rollDecay;
        c.sdrScale = s.sdrWhite / 80.0f;
        {
            // knee given as signal level (what the game writes); the shader works in linear light
            const float k = s.sdrKnee;
            c.sdrKnee = k <= 0.04045f ? k / 12.92f : std::pow((k + 0.055f) / 1.055f, 2.4f);
            const float hi = std::min(s.sdrHiNits, s.peakNits);
            c.sdrPeak = hi > s.sdrWhite ? hi / 80.0f : 0.0f;
        }

        // real image
        VkSemaphore acqSem = R.acq[acqIdx];
        acqIdx = (acqIdx + 1) % (sc->nReal + 1);
        uint32_t idx = 0;
        VkResult ar = d->AcquireNextImageKHR(dev, sc->real, 100000000ull, acqSem, VK_NULL_HANDLE, &idx);
        const double tAcq = Now();
        if (ar == VK_TIMEOUT || ar == VK_NOT_READY) continue;
        if (ar == VK_SUBOPTIMAL_KHR) sc->suboptimal.store(true);
        else if (ar != VK_SUCCESS)
        {
            LOGW("BFI Presenter: real swap chain acquire returned %d -- the game will be asked to recreate it", (int)ar);
            sc->status.store((int)ar);
            break;
        }

        bool dumpNow = false;
        const bool reuse = !(R.dumpPtr && dumpLeft > 0) &&
                           bufValid[idx] && bufFrame[idx] == pickId && memcmp(&bufConst[idx], &c, sizeof(c)) == 0;
        VkSemaphore presentWait = acqSem;
        double tFence = Now();
        if (reuse) secReused++;
        else
        {
            d->WaitForFences(dev, 1, &R.fence[idx], VK_TRUE, 1000000000ull);
            d->ResetFences(dev, 1, &R.fence[idx]);
            tFence = Now();
            VkCommandBuffer cb = R.cmd[idx];
            d->ResetCommandBuffer(cb, 0);
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            d->BeginCommandBuffer(cb, &bi);
            if (copyMode)
            {
                VkImageMemoryBarrier br2[2] = {
                    ImgBarrier(sc->realImg[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
                    ImgBarrier(sc->stage[pick], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT) };
                d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, br2);
                VkImageCopy ic = {};
                ic.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                ic.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                ic.extent = { sc->extent.width, sc->extent.height, 1 };
                d->CmdCopyImage(cb, sc->stage[pick], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->realImg[idx],
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
                br2[0] = ImgBarrier(sc->realImg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                    VK_ACCESS_TRANSFER_WRITE_BIT, 0);
                br2[1] = ImgBarrier(sc->stage[pick], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_ACCESS_TRANSFER_READ_BIT, 0);
                d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, br2);
            }
            else
            {
                VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                rbi.renderPass = R.rp;
                rbi.framebuffer = R.fb[idx];
                rbi.renderArea = { { 0, 0 }, sc->extent };
                d->CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
                d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pso);
                d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pl, 0, 1, &R.ds[pick], 0, nullptr);
                d->CmdPushConstants(cb, R.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(c), &c);
                VkViewport vp = { 0.0f, 0.0f, (float)sc->extent.width, (float)sc->extent.height, 0.0f, 1.0f };
                VkRect2D scis = { { 0, 0 }, sc->extent };
                d->CmdSetViewport(cb, 0, 1, &vp);
                d->CmdSetScissor(cb, 0, 1, &scis);
                d->CmdDraw(cb, 3, 1, 0, 0);
                d->CmdEndRenderPass(cb);
                if (R.dumpPtr && dumpLeft > 0)
                {
                    VkImageMemoryBarrier db = ImgBarrier(sc->realImg[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &db);
                    VkBufferImageCopy bic = {};
                    bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    bic.imageExtent = { sc->extent.width, sc->extent.height, 1 };
                    d->CmdCopyImageToBuffer(cb, sc->realImg[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, R.dumpBuf, 1, &bic);
                    db = ImgBarrier(sc->realImg[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
                    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &db);
                    dumpNow = true;
                }
                if (ablOn && !measSub && pickId != lastShownId && c.method < 2)
                {
                    Constants mc = c;
                    mc.gain = s.gain;                          // brightness at the configured boost
                    mc.ceilLin = s.peakNits / 80.0f;           // and the full ceiling
                    rbi.renderPass = R.rpMeas;
                    rbi.framebuffer = R.measFb;
                    rbi.renderArea = { { 0, 0 }, { 32, 18 } };
                    d->CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
                    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, R.psoMeas);
                    d->CmdPushConstants(cb, R.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mc), &mc);
                    VkViewport mvp = { 0.0f, 0.0f, 32.0f, 18.0f, 0.0f, 1.0f };
                    VkRect2D mrc = { { 0, 0 }, { 32, 18 } };
                    d->CmdSetViewport(cb, 0, 1, &mvp);
                    d->CmdSetScissor(cb, 0, 1, &mrc);
                    d->CmdDraw(cb, 3, 1, 0, 0);
                    d->CmdEndRenderPass(cb);
                    VkBufferImageCopy bic = {};
                    bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    bic.imageExtent = { 32, 18, 1 };
                    d->CmdCopyImageToBuffer(cb, R.measImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, R.measBuf, 1, &bic);
                    VkBufferMemoryBarrier hb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    hb.buffer = R.measBuf;
                    hb.size = VK_WHOLE_SIZE;
                    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
                    measSub = presSub + 1;
                }
            }
            d->EndCommandBuffer(cb);
            presSub++;
            {
                std::lock_guard<std::mutex> l(sc->lock);
                sc->slotReadSub[pick] = presSub;
            }
            bufSub[idx] = presSub;
            VkPipelineStageFlags waitStage = copyMode ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &acqSem;
            si.pWaitDstStageMask = &waitStage;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &R.done[idx];
            VkResult sr;
            {
                std::lock_guard<std::recursive_mutex> l(d->qlock);
                sr = d->QueueSubmit(d->queue, 1, &si, R.fence[idx]);
            }
            if (sr != VK_SUCCESS)
            {
                LOGE("BFI Presenter: presenter submit failed (%d)", (int)sr);
                sc->status.store((int)sr);
                break;
            }
            bufFrame[idx] = pickId;
            bufConst[idx] = c;
            bufValid[idx] = true;
            presentWait = R.done[idx];
            if (dumpNow)
            {
                d->WaitForFences(dev, 1, &R.fence[idx], VK_TRUE, 1000000000ull);
                char name[96];
                snprintf(name, sizeof(name), "BFIPresenterVK_dump_%03d_frame%llu_slot%u.ppm", g_dumpFrames - dumpLeft,
                         (unsigned long long)pickId, c.slot);
                if (FILE *df = fopen((g_outDir + name).c_str(), "wb"))
                {
                    const uint32_t w = sc->extent.width, h = sc->extent.height;
                    const bool bgr = sc->format == VK_FORMAT_B8G8R8A8_UNORM || sc->format == VK_FORMAT_B8G8R8A8_SRGB;
                    fprintf(df, "P6\n%u %u\n255\n", w, h);
                    std::vector<uint8_t> row(w * 3);
                    for (uint32_t y = 0; y < h; y++)
                    {
                        const uint8_t *p = R.dumpPtr + (size_t)y * w * 4;
                        for (uint32_t x = 0; x < w; x++)
                        {
                            row[x * 3 + 0] = p[x * 4 + (bgr ? 2 : 0)];
                            row[x * 3 + 1] = p[x * 4 + 1];
                            row[x * 3 + 2] = p[x * 4 + (bgr ? 0 : 2)];
                        }
                        fwrite(row.data(), 1, row.size(), df);
                    }
                    fclose(df);
                }
                dumpLeft--;
            }
        }
        const double tSub = Now();
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &presentWait;
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc->real;
        pi.pImageIndices = &idx;
        VkResult pr;
        {
            std::lock_guard<std::recursive_mutex> l(d->qlock);
            pr = d->QueuePresentKHR(d->queue, &pi);
        }
        const double tPres = Now();
        sc->lastPresentT.store(tPres);
        if (presents == 0) LOGI("BFI Presenter: first frame presented (%d)", (int)pr);
        if (pr == VK_SUBOPTIMAL_KHR) sc->suboptimal.store(true);
        else if (pr != VK_SUCCESS)
        {
            LOGW("BFI Presenter: real swap chain present returned %d -- the game will be asked to recreate it", (int)pr);
            sc->status.store((int)pr);
            break;
        }
        // Presenter submissions known to be finished (game side may reuse
        // staging slots read by them).
        {
            uint64_t doneSub = presSub;
            for (uint32_t i = 0; i < sc->nReal; i++)
                if (bufSub[i] && bufSub[i] <= doneSub && d->GetFenceStatus(dev, R.fence[i]) != VK_SUCCESS)
                    doneSub = bufSub[i] - 1;
            if (doneSub > sc->presDoneSub.load()) sc->presDoneSub.store(doneSub);
        }
        presents++;
        if (!vblank && !primeIter)
        {
            sc->tickT.store(Now());
            sc->ticks.fetch_add(1);
            sc->tickCv.notify_all();
        }
        if (qa)
        {
            const double per = 1.0 / (vb.modeHz > 0.0 ? vb.modeHz : 120.0);
            const double blocked = (tAcq - tVb) + (tPres - tSub);
            if (qaPrimed && !primeIter && blocked > 0.5 * per) qaSkipNext = true;       // queue too deep
            if (!primeIter && presents > 1 && Now() - lastT > 1.5 * per) qaPrimed = false;  // fell a refresh behind
            if (!qaPrimed && presents > 0 && !qaSkipNext)
            {
                qaPrimed = true;
                qaSkipWait = true;                       // next iteration: present again without waiting
                secQaPrimes++;
            }
        }

        const double t = Now();
        secPresents++;
        ageSum += (t - pickTime) * 1000.0;
        if (t - lastT > 1.5 / (vb.modeHz > 0.0 ? vb.modeHz : 120.0) && presents > 1)   // a missed refresh at the display's own rate
        {
            secLate++;
            lateTimes[lateIdx++ & 3] = t;
        }
        if (s.hitchGuard && s.enabled)
        {
            const double third = lateTimes[(lateIdx + 1) & 3];
            const bool lateBurst = lateIdx >= 3 && t - third < 0.5;
            const bool stalled = lastNewFrameT > 0.0 && (t - lastNewFrameT) * 1000.0 > s.hitchGapMs;
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
            std::lock_guard<std::mutex> l(g_cfgLock);
            g_status.refreshHz  = secPresents / (t - secStart);
            g_status.gameFps    = sc->staged.exchange(0) / (t - secStart);
            g_status.frameAgeMs = secPresents ? ageSum / secPresents : 0.0;
            g_status.late       = secLate;
            // A frame limiter acting on the presenter's own presents (RTSS,
            // the NVIDIA "Max Frame Rate" setting): a little under the display
            // rate, with no single refresh late enough to count as missed. The
            // strobe then slips a refresh every half second or so (flicker).
            {
                const double prate = secPresents / (t - secStart);
                const double hz = vb.modeHz;
                limitSecs = (s.pacing && hz > 0.0 && prate < 0.985 * hz && prate > 0.5 * hz && secLate < 3) ? limitSecs + 1 : 0;
                if (limitSecs == 3 && !limitLogged)
                {
                    limitLogged = true;
                    LOGW("BFI Presenter: the presenter is held to %.1f presents/s on a %.0f Hz display -- a frame limiter is "
                         "acting on it (RTSS limit, or NVIDIA Control Panel / NVIDIA app 'Max Frame Rate', global or for this exe). "
                         "Turn it off for this program: the strobe slips a refresh about %.0f times a second, which shows as flicker.",
                         prate, hz, hz - prate);
                }
                g_status.limitSuspect = limitSecs >= 3;
            }
            g_status.frameRepeats = secFrameRepeats;
            g_status.frameSkips   = secFrameSkips;
            g_status.unevenRep    = secRepU;
            g_status.qaPrimes     = secQaPrimes;
            g_status.qaSkips      = secQaSkips;
            secQaPrimes = secQaSkips = 0;
            g_status.unevenSkip   = secSkipU;
            g_status.slotWaits    = sc->slotWaits.exchange(0);
            g_status.reused       = secReused;
            g_status.hitchPaused  = hitchPaused;
            g_status.litAvg       = (s.ablGuard && R.psoMeas) ? litAvg : -1.0;
            g_status.rawAvg       = (s.ablGuard && R.psoMeas) ? rawAvg : -1.0;
            g_status.ceilInUse    = c.ceilLin * 80.0;
            g_status.outType      = (int)c.outType;
            // Automatic frames ahead (as the D3D12 presenter): 1 normally, 2
            // while the game keeps missing strobe cycles, back to 1 when it has
            // headroom; settles at 2 if it needs 2 again within a minute.
            const long long us = sc->capWaitUs.exchange(0);
            const int n = sc->capWaits.exchange(0);
            const double capWaitMs = n ? us / 1000.0 / n : 0.0;
            if (s.autoAhead && s.maxAhead != 0)
            {
                if (autoAhead == 1)
                {
                    missSecs = (secRepU >= 2 && s.gameCap >= 1) ? missSecs + 1 : 0;
                    if (missSecs >= 3)
                    {
                        autoAhead = 2; missSecs = 0; spareSecs = 0;
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
                sc->maxAhead.store(autoAhead);
            }
            else sc->maxAhead.store(s.maxAhead);
            g_status.aheadInUse = sc->maxAhead.load();
            secFrameRepeats = secFrameSkips = secReused = 0;
            secRepU = secSkipU = 0;
            secStart = t; secPresents = 0; secLate = 0; ageSum = 0.0;
        }
        if (logf)
        {
            int lrSt; double lrE, lrM;
            {
                std::lock_guard<std::mutex> l(g_cfgLock);
                lrSt = g_status.lrState; lrE = g_status.lrEst; lrM = g_status.lrMargin;
            }
            fprintf(logf, "%.3f,%llu,%u,%llu,%.3f,%.3f,%d,%d,%d,%.2f,%.2f,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%.0f,%.1f,%.1f,%d,%.3f,%.3f,%.3f,%d,%d\n",
                    t * 1000.0, (unsigned long long)presents, c.slot, (unsigned long long)pickId, (t - pickTime) * 1000.0,
                    (t - lastT) * 1000.0, s.enabled, s.method, s.frames, s.gain, s.dfloor, s.rolloff, s.pacingMode, s.gameCap,
                    sc->maxAhead.load(), vblank ? 1 : 0, (tVb - tLoop0) * 1000.0, (tAcq - tVb) * 1000.0, (tFence - tAcq) * 1000.0,
                    (tSub - tFence) * 1000.0, (tPres - tSub) * 1000.0, reuse ? 1 : 0, hitchPaused ? 1 : 0, c.ceilLin * 80.0f,
                    litAvg, rawAvg, lrSt, sc->lrLead.load() * 1000.0, lrE, lrM, qa ? (primeIter ? 2 : qaPrimed ? 1 : 0) : -1, s.smoothCatchUp);
            if ((presents % 120) == 0) fflush(logf);
        }
        lastT = t;
        if (pickId != lastShownId)
        {
            lastNewFrameT = t;
            if (pickPresentId)
            {
                std::lock_guard<std::mutex> l(sc->presIdLock);
                if (pickPresentId > sc->shownPresentId.load()) sc->shownPresentId.store(pickPresentId);
            }
            sc->presIdCv.notify_all();
        }
        lastShown = pick;
        lastShownId = pickId;
    }

    // drain and release
    {
        std::lock_guard<std::recursive_mutex> l(d->qlock);
        d->QueueWaitIdle(d->queue);
    }
    if (logf) fclose(logf);
    DestroyPresenterRes(d, R);
    vb.Release();
    {
        std::lock_guard<std::mutex> l(g_cfgLock);
        g_status.running = 0;
    }
    sc->running.store(false);
    sc->tickCv.notify_all();
    sc->presIdCv.notify_all();
    LOGI("BFI Presenter: presenter stopped after %llu refreshes", (unsigned long long)presents);
}

// ---------------------------------------------------------------- just-in-time release
// Times each game frame from its release to its copy finishing on the GPU and
// turns that into the release lead once a second (95th percentile of the last
// 128 frames + a margin that widens after misses). The copy fence is polled
// under the stage lock, as the game resets it under that lock.
static void LrWatchThread(Swapchain *sc)
{
    DeviceData *d = sc->d;
    VkDevice dev = d->device;
    uint64_t next = sc->lrStaged.load() + 1;
    double builds[128] = {}, sorted[128];
    int nb = 0, wb = 0, state = 0, misses = 0, frames = 0, clean = 0, good = 0;
    double margin = sc->lrBaseMs.load() * 0.001, est = 0.0, secT = Now(), lastWindow = 0.0;
    while (sc->lrRun.load())
    {
        const uint64_t staged = sc->lrStaged.load();
        if (staged < next)
        {
            std::unique_lock<std::mutex> lk(sc->lrMx);
            sc->lrCv.wait_for(lk, std::chrono::milliseconds(50));
        }
        else
        {
            if (staged > next + kLr / 2) next = staged;
            const uint64_t id = next++;
            const int k = (int)(id % kLr);
            const int slot = sc->lrSlot[k];
            double fin = -1.0;
            const double w0 = Now();
            while (sc->lrRun.load() && Now() - w0 < 0.1)
            {
                bool gone = false, done = false;
                {
                    std::lock_guard<std::mutex> l(sc->lock);
                    if (slot < 0 || slot >= (int)kStage || sc->stageId[slot] != id) gone = true;
                    else done = sc->copyDone[slot] || (sc->copySubmitted[slot] && d->GetFenceStatus(dev, sc->copyFence[slot]) == VK_SUCCESS);
                }
                if (gone) break;
                if (done) { fin = Now(); break; }
                SleepShort(0.00025);
            }
            if (fin > 0.0 && sc->lrRelId[k] == id)
            {
                const double b = fin - sc->lrRelT[k];
                if (b > 0.0 && b < 0.1) { builds[wb] = b; wb = (wb + 1) % 128; if (nb < 128) nb++; }
                if (sc->lrDeadline[k] > 0.0) { frames++; if (fin > sc->lrDeadline[k]) misses++; }
            }
        }
        const double now = Now();
        if (now - secT < 1.0) continue;
        secT = now;
        const bool allowed = sc->lrAllowed.load();
        const double window = sc->lrWindow.load(), base = sc->lrBaseMs.load() * 0.001;
        const bool capMode = window > 1.5 * sc->period.load();      // Half / Quarter cap
        if (std::fabs(window - lastWindow) > 1e-4) { lastWindow = window; if (state != 0) { state = 0; nb = wb = 0; margin = base; } }
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
        else if (state == 1) { if (nb >= 60) state = (est + margin < window - 0.002) ? 2 : 3; }
        else
        {
            if (misses >= 2)
            {
                const double m0 = margin;
                margin = std::min(margin + 0.001, base + 0.003); clean = 0;
                if (margin > m0)
                    LOGI("BFI Presenter: just-in-time release: %d of %d frames missed their refresh -- margin widened to %.1f ms",
                         misses, frames, margin * 1000.0);
            }
            else if (misses == 0) { if (++clean >= 5 && margin > base) { margin = std::max(base, margin - 0.0005); clean = 0; } }
            else clean = 0;
            if (state == 2) { if (est + margin > window - 0.001) { state = 3; good = 0; } }
            else { good = (est + margin < window - 0.002) ? good + 1 : 0; if (good >= 3) { state = 2; good = 0; } }
        }
        if (state != prev && state == 2)
            LOGI("BFI Presenter: just-in-time release active -- game frames take %.1f ms (95th percentile), released %.1f ms "
                 "before the refresh that shows them (cap interval %.1f ms)", est * 1000.0, (est + margin) * 1000.0, window * 1000.0);
        else if (state != prev && state == 3)
            LOGI("BFI Presenter: just-in-time release at its limit -- game frames take %.1f ms, close to the %.1f ms cap interval; %s",
                 est * 1000.0, window * 1000.0, capMode ? "released as early as the cap allows" : "released at the refresh as before");
        // At its limit: at the Half / Quarter cap every frame still targets a
        // lit refresh, released as early as the cap allows; at the Refresh cap
        // the earliest release is the vblank, i.e. the normal behaviour.
        double lead = 0.0;
        if (state == 2) lead = est + margin;
        else if (state == 3 && capMode) lead = window - 0.001;
        sc->lrLead.store(lead);
        {
            std::lock_guard<std::mutex> l(g_cfgLock);
            g_status.lrState = state;
            g_status.lrEst = est * 1000.0;
            g_status.lrMargin = margin * 1000.0;
            g_status.lrMisses = misses;
        }
        misses = frames = 0;
    }
    sc->lrLead.store(0.0);
}

// Record when the game is let go for its next frame (the watcher times it).
static void LrRecordRelease(Swapchain *sc, double deadline)
{
    const uint64_t id = sc->frameId + 1;
    const int k = (int)(id % kLr);
    sc->lrRelT[k] = Now();
    sc->lrDeadline[k] = deadline;
    sc->lrRelId[k] = id;
}

// Wait for the presenter's next tick (vblank); returns the tick count.
static uint64_t WaitTick(Swapchain *sc, uint64_t after, double waitStart)
{
    uint64_t now = sc->ticks.load();
    std::unique_lock<std::mutex> l(sc->tickLock);
    while (now <= after && Now() - waitStart < 0.25 && sc->running.load())
    {
        sc->tickCv.wait_for(l, std::chrono::milliseconds(20));
        now = sc->ticks.load();
    }
    return now;
}

// Half / Quarter cap, just in time: each game frame targets a lit refresh (per
// refreshes after the previous frame's) and is released at that refresh's
// vblank minus the measured build time and margin (DX12 add-on v0.75/v0.76).
static bool CapWaitJit(Swapchain *sc, uint64_t per, int frames, double lead)
{
    const double period = sc->period.load();
    uint64_t c = 0; int sl = 0; double tT = 0.0;
    for (int i = 0; i < 4; i++)
    {
        c = sc->ticks.load(); sl = sc->tickSlot.load(); tT = sc->tickT.load();
        if (sc->ticks.load() == c) break;
    }
    if (c == 0 || tT <= 0.0) return false;
    auto timeOf = [&](uint64_t x) { return tT + (double)(int64_t)(x - c) * period; };
    const double now = Now();
    uint64_t want = sc->jitTarget ? sc->jitTarget + per : 0;
    if (want == 0 || want <= c || want > c + 2 * per || timeOf(want) <= now)
    {
        uint64_t k = 1;
        for (; k < 4 * per + (uint64_t)frames; k++)
            if ((sl + (int)k) % frames == 0 && timeOf(c + k) - lead >= now - 0.0005) break;
        want = c + k;
    }
    sc->jitTarget = want;
    const double vbT = timeOf(want), rel = vbT - lead;
    bool held = false;
    if (rel > now && rel - now < (double)(per + 1) * period) { SleepUntil(rel); held = true; }
    LrRecordRelease(sc, held ? vbT + 0.0005 : 0.0);
    return true;
}

// Frame cap: release the game every 1, 2 or 4 refreshes, counted against the
// presenter's refreshes so it stays in step with the strobe.
static void CapWait(Swapchain *sc, uint64_t per)
{
    const double waitStart = Now();
    if (per >= 2 && sc->lrAllowed.load())
    {
        const int fr = sc->strobeFrames.load();
        const double lead = sc->lrLead.load();
        if (fr >= 2 && per % (uint64_t)fr == 0 && lead > 0.0 && CapWaitJit(sc, per, fr, lead))
        {
            sc->capWaitUs.fetch_add((long long)((Now() - waitStart) * 1e6));
            sc->capWaits.fetch_add(1);
            return;
        }
    }
    uint64_t now = sc->ticks.load();
    if (sc->nextTick == 0 || sc->nextTick > now + per) sc->nextTick = now;
    const bool onTime = now <= sc->nextTick;
    while (now < sc->nextTick && Now() - waitStart < 0.25 && sc->running.load())
        now = WaitTick(sc, now, waitStart);
    const int frames = sc->strobeFrames.load();
    if (onTime && (int)per == frames && frames >= 2)
    {
        // Release at the refresh before the lit one, so the frame can make it
        // (DX12 add-on v0.69): released at the lit refresh's own vblank it was
        // shown a whole cycle later.
        const uint64_t limit = now + per - 1;
        while (sc->tickSlot.load() != frames - 1 && now < limit && Now() - waitStart < 0.25 && sc->running.load())
            now = WaitTick(sc, now, waitStart);
        sc->nextTick = now + per;
    }
    else
        sc->nextTick = (now >= sc->nextTick + per ? now : sc->nextTick) + per;
    if (per >= 2 && sc->lrAllowed.load()) LrRecordRelease(sc, 0.0);     // measuring / paused: released as before
    if (per == 1 && sc->lrAllowed.load())
    {
        // Later release: hold the game until just in time for the pick at the
        // next vblank, so the frame it renders reads its input later.
        const double lead = sc->lrLead.load(), period = sc->period.load();
        const double pickVb = sc->tickT.load() + period, target = pickVb - lead, t0 = Now();
        bool held = false;
        if (lead > 0.0 && target > t0 && target - t0 < period) { SleepUntil(target); held = true; }
        LrRecordRelease(sc, held ? pickVb + 0.0005 : 0.0);
    }
    sc->capWaitUs.fetch_add((long long)((Now() - waitStart) * 1e6));
    sc->capWaits.fetch_add(1);
}

static void StartPresenter(Swapchain *sc)
{
    if (sc->running.load() || sc->failed.load()) return;
    sc->stopReq.store(false);
    sc->running.store(true);
    sc->thread = std::thread(PresenterThread, sc);
    if (!sc->lrThread.joinable())
    {
        sc->lrRun.store(true);
        sc->lrThread = std::thread(LrWatchThread, sc);
    }
}
static void StopPresenter(Swapchain *sc)
{
    if (sc->lrThread.joinable())
    {
        sc->lrRun.store(false);
        sc->lrCv.notify_all();
        sc->lrThread.join();
    }
    sc->lrLead.store(0.0);
    sc->lrAllowed.store(false);
    sc->stopReq.store(true);
    if (sc->thread.joinable()) sc->thread.join();
    sc->running.store(false);
}

// ---------------------------------------------------------------- game side
static CopyCmds *GetCopyCmds(Swapchain *sc, uint32_t family)
{
    DeviceData *d = sc->d;
    auto it = sc->copyCmds.find(family);
    if (it != sc->copyCmds.end()) return &it->second;
    CopyCmds cc;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (d->CreateCommandPool(d->device, &pci, nullptr, &cc.pool) != VK_SUCCESS) return nullptr;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = cc.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kStage;
    if (d->AllocateCommandBuffers(d->device, &cai, cc.cmd) != VK_SUCCESS)
    {
        d->DestroyCommandPool(d->device, cc.pool, nullptr);
        return nullptr;
    }
    for (uint32_t i = 0; i < kStage; i++)
        if (d->setLoaderData) d->setLoaderData(d->device, cc.cmd[i]);
    return &(sc->copyCmds[family] = cc);
}

static bool IsOurQueue(DeviceData *d, VkQueue q) { return q == d->queue; }

// Waits on the given semaphores without doing anything else (used when a
// present can't be carried out, so the game's semaphores are still consumed).
static void ConsumeWaits(DeviceData *d, VkQueue queue, uint32_t n, const VkSemaphore *waits)
{
    if (!n) return;
    std::vector<VkPipelineStageFlags> st(n, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = n;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = st.data();
    std::unique_lock<std::recursive_mutex> l(d->qlock, std::defer_lock);
    if (IsOurQueue(d, queue)) l.lock();
    d->QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
}

// One game frame: copy the presented stand-in image into a free staging slot
// (after the game's rendering, via its semaphores), then hold the game to the
// frames-ahead limit and the frame cap.
static VkResult GameFrame(Swapchain *sc, VkQueue queue, uint32_t nWait, const VkSemaphore *waits, uint32_t imageIndex,
                          uint64_t presentId, VkSemaphore chainSignal)
{
    DeviceData *d = sc->d;
    VkDevice dev = d->device;
    std::lock_guard<std::mutex> gl(sc->submitLock);
    const int st = sc->status.load();
    if (st != VK_SUCCESS || sc->retired.load() || imageIndex >= sc->nVirt)
    {
        ConsumeWaits(d, queue, nWait, waits);
        return st != VK_SUCCESS ? (VkResult)st : VK_ERROR_OUT_OF_DATE_KHR;
    }
    if (!sc->running.load() && !sc->failed.load()) StartPresenter(sc);

    uint32_t fam = QueueFamilyOf(queue);
    if (fam == UINT32_MAX) fam = d->family;
    CopyCmds *cc = GetCopyCmds(sc, fam);
    if (!cc)
    {
        LOGE("BFI Presenter: no command pool for queue family %u", fam);
        ConsumeWaits(d, queue, nWait, waits);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Staging slot: the oldest one the presenter is neither showing nor still
    // reading, and whose previous copy is done.
    int slot = -1;
    const double t0 = Now();
    for (;;)
    {
        {
            std::lock_guard<std::mutex> l(sc->lock);
            const uint64_t pdone = sc->presDoneSub.load();
            uint64_t bestId = ~0ull;
            for (uint32_t i = 0; i < kStage; i++)
            {
                if ((int)i == sc->presCurSlot || sc->slotReadSub[i] > pdone) continue;
                if (sc->stageId[i] < bestId) { slot = (int)i; bestId = sc->stageId[i]; }
            }
            if (slot >= 0) { sc->stageId[slot] = 0; sc->copyDone[slot] = false; }   // being rewritten: not selectable
        }
        if (slot >= 0 || Now() - t0 > 0.2) break;
        SleepSec(0.0005);
    }
    if (slot < 0)
    {
        // presenter stuck: take any slot that is not on screen
        sc->slotWaits.fetch_add(1);
        std::lock_guard<std::mutex> l(sc->lock);
        slot = (sc->presCurSlot + 1) % (int)kStage;
        sc->stageId[slot] = 0;
        sc->copyDone[slot] = false;
    }
    else if (Now() - t0 > 0.0005) sc->slotWaits.fetch_add(1);
    if (sc->copySubmitted[slot]) d->WaitForFences(dev, 1, &sc->copyFence[slot], VK_TRUE, 1000000000ull);
    {
        std::lock_guard<std::mutex> l(sc->lock);
        sc->copySubmitted[slot] = false;
        d->ResetFences(dev, 1, &sc->copyFence[slot]);
    }

    VkCommandBuffer cb = cc->cmd[slot];
    d->ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier br[2] = {
        ImgBarrier(sc->virt[imageIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT),
        ImgBarrier(sc->stage[slot], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT) };
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, br);
    VkImageCopy ic = {};
    ic.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    ic.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    ic.extent = { sc->extent.width, sc->extent.height, 1 };
    d->CmdCopyImage(cb, sc->virt[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->stage[slot],
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    br[0] = ImgBarrier(sc->virt[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
    br[1] = ImgBarrier(sc->stage[slot], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, 0);   // the presenter reads it after the copy fence
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, br);
    d->EndCommandBuffer(cb);

    std::vector<VkPipelineStageFlags> stg(nWait, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSemaphore sig[2] = { sc->virtSem[imageIndex], chainSignal };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = nWait;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stg.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = chainSignal ? 2 : 1;
    si.pSignalSemaphores = sig;
    VkResult r;
    {
        std::unique_lock<std::recursive_mutex> l(d->qlock, std::defer_lock);
        if (IsOurQueue(d, queue)) l.lock();
        r = d->QueueSubmit(queue, 1, &si, sc->copyFence[slot]);
    }
    if (r != VK_SUCCESS)
    {
        LOGE("BFI Presenter: frame copy submit failed (%d)", (int)r);
        return r;
    }
    uint64_t id;
    {
        std::lock_guard<std::mutex> l(sc->lock);
        sc->copySubmitted[slot] = true;
        id = ++sc->frameId;
        sc->stageId[slot] = id;
        sc->stageTime[slot] = Now();
        sc->stagePresentId[slot] = presentId;
        sc->recentId[id % 8] = id;
        sc->recentSlot[id % 8] = slot;
        sc->virtPending[imageIndex] = true;
        sc->lrSlot[id % kLr] = slot;
    }
    sc->lrStaged.store(id);
    sc->lrCv.notify_one();
    sc->staged.fetch_add(1);
    if (!sc->firstFrameLogged) { sc->firstFrameLogged = true; LOGI("BFI Presenter: first game frame received"); }

    // Latency limit: before the game starts its next frame, the frame from
    // MaxFramesAhead frames ago must be finished on the GPU.
    const int ahead = sc->maxAhead.load();
    if (ahead > 0 && id > (uint64_t)ahead)
    {
        const uint64_t need = id - (uint64_t)ahead;
        int ns = -1;
        {
            std::lock_guard<std::mutex> l(sc->lock);
            if (sc->recentId[need % 8] == need)
            {
                const int k = sc->recentSlot[need % 8];
                if (sc->stageId[k] == need && sc->copySubmitted[k]) ns = k;
            }
        }
        if (ns >= 0) d->WaitForFences(dev, 1, &sc->copyFence[ns], VK_TRUE, 250000000ull);
    }
    // Frame cap: release the game every 1, 2 or 4 refreshes, counted against
    // the presenter's refreshes so it stays in step with the strobe.
    int cap, pacingLL;
    {
        std::lock_guard<std::mutex> l(g_cfgLock);
        cap = g_shared.gameCap;
        pacingLL = g_shared.pacingMode;
    }
    // Lowest latency, and Smooth with a frame cap: the game's present counts
    // as done once it is handed over, not when the strobe takes it. Completing
    // present waits only at the 60 Hz picks held DXVK and other present-wait
    // games at 60 fps; with a cap on, the cap paces the game anyway (DX12
    // add-on v0.64 / v0.79: Smooth at the Refresh cap cost God of War ~25 fps).
    if ((pacingLL || cap >= 1) && presentId)
    {
        {
            std::lock_guard<std::mutex> l(sc->presIdLock);
            if (presentId > sc->shownPresentId.load()) sc->shownPresentId.store(presentId);
        }
        sc->presIdCv.notify_all();
    }
    const uint64_t per = cap == 1 ? 1 : cap == 2 ? 2 : cap == 3 ? 4 : 0;
    if (per && sc->running.load()) CapWait(sc, per);
    const int st2 = sc->status.load();
    if (st2 != VK_SUCCESS) return (VkResult)st2;
    return sc->suboptimal.load() ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
}

static VkResult AcquireVirtual(Swapchain *sc, VkSemaphore sem, VkFence fence, uint32_t *pIndex)
{
    DeviceData *d = sc->d;
    const int st = sc->status.load();
    if (sc->retired.load()) return VK_ERROR_OUT_OF_DATE_KHR;
    if (st != VK_SUCCESS) return (VkResult)st;
    uint32_t i;
    bool pending;
    {
        std::lock_guard<std::mutex> l(sc->lock);
        i = sc->nextVirt;
        sc->nextVirt = (i + 1) % sc->nVirt;
        pending = sc->virtPending[i];
        sc->virtPending[i] = false;
    }
    if (pending || sem || fence)
    {
        // The image is free once the copy out of its last present is done:
        // wait for that on the GPU, then signal what the game passed in.
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = pending ? 1 : 0;
        si.pWaitSemaphores = &sc->virtSem[i];
        si.pWaitDstStageMask = &ws;
        si.signalSemaphoreCount = sem ? 1 : 0;
        si.pSignalSemaphores = &sem;
        VkResult r;
        {
            std::lock_guard<std::recursive_mutex> l(d->qlock);
            r = d->QueueSubmit(d->queue, 1, &si, fence);
        }
        if (r != VK_SUCCESS) return r;
    }
    *pIndex = i;
    return sc->suboptimal.load() ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
}

// ---------------------------------------------------------------- swap chain create / destroy
static void DestroySCResources(Swapchain *sc)
{
    DeviceData *d = sc->d;
    VkDevice dev = d->device;
    StopPresenter(sc);
    for (uint32_t i = 0; i < kStage; i++)
        if (sc->copySubmitted[i]) d->WaitForFences(dev, 1, &sc->copyFence[i], VK_TRUE, 1000000000ull);
    {
        std::lock_guard<std::recursive_mutex> l(d->qlock);
        d->QueueWaitIdle(d->queue);
    }
    for (auto &kv : sc->copyCmds) d->DestroyCommandPool(dev, kv.second.pool, nullptr);
    sc->copyCmds.clear();
    for (uint32_t i = 0; i < kStage; i++)
    {
        if (sc->stageView[i]) d->DestroyImageView(dev, sc->stageView[i], nullptr);
        if (sc->stage[i]) d->DestroyImage(dev, sc->stage[i], nullptr);
        if (sc->stageMem[i]) d->FreeMemory(dev, sc->stageMem[i], nullptr);
        if (sc->copyFence[i]) d->DestroyFence(dev, sc->copyFence[i], nullptr);
        sc->stageView[i] = VK_NULL_HANDLE; sc->stage[i] = VK_NULL_HANDLE; sc->stageMem[i] = VK_NULL_HANDLE; sc->copyFence[i] = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < kMaxImg; i++)
    {
        if (sc->virt[i]) d->DestroyImage(dev, sc->virt[i], nullptr);
        if (sc->virtMem[i]) d->FreeMemory(dev, sc->virtMem[i], nullptr);
        if (sc->virtSem[i]) d->DestroySemaphore(dev, sc->virtSem[i], nullptr);
        sc->virt[i] = VK_NULL_HANDLE; sc->virtMem[i] = VK_NULL_HANDLE; sc->virtSem[i] = VK_NULL_HANDLE;
    }
}

// Builds the stand-in images and staging ring for a real swap chain that has
// just been created. Returns false (and leaves nothing behind) on failure.
static bool BuildSC(Swapchain *sc, const VkSwapchainCreateInfoKHR *ci)
{
    DeviceData *d = sc->d;
    VkDevice dev = d->device;
    uint32_t n = 0;
    if (d->GetSwapchainImagesKHR(dev, sc->real, &n, nullptr) != VK_SUCCESS || n == 0) return false;
    sc->nReal = std::min(n, kMaxImg);
    if (n > kMaxImg) { LOGE("BFI Presenter: real swap chain has %u images (max %u)", n, kMaxImg); return false; }
    if (d->GetSwapchainImagesKHR(dev, sc->real, &n, sc->realImg) != VK_SUCCESS) return false;

    // Stand-in images: what the game asked for, plus copy-out.
    const VkImageFormatListCreateInfo *fl = nullptr;
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) fl = (const VkImageFormatListCreateInfo *)p;
    VkImageFormatListCreateInfo flc = {};
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    if (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
    {
        ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (fl) { flc = *fl; flc.pNext = nullptr; ici.pNext = &flc; }
    }
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = ci->imageFormat;
    ici.extent = { ci->imageExtent.width, ci->imageExtent.height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = std::max(ci->imageArrayLayers, 1u);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = ci->imageSharingMode;
    ici.queueFamilyIndexCount = ci->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? ci->queueFamilyIndexCount : 0;
    ici.pQueueFamilyIndices = ci->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? ci->pQueueFamilyIndices : nullptr;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    sc->nVirt = Clamp(ci->minImageCount, 2u, kMaxImg);
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    bool ok = true;
    for (uint32_t i = 0; ok && i < sc->nVirt; i++)
    {
        ok = CreateImageMem(d, ici, &sc->virt[i], &sc->virtMem[i]);
        ok = ok && d->CreateSemaphore(dev, &sci, nullptr, &sc->virtSem[i]) == VK_SUCCESS;
    }
    if (!ok) { LOGE("BFI Presenter: could not create the stand-in images (usage 0x%X)", (unsigned)ici.usage); DestroySCResources(sc); return false; }

    // Staging ring: sampled by the presenter's queue, written on the game's.
    VkImageCreateInfo sti = ici;
    sti.pNext = nullptr;
    sti.flags = 0;
    sti.arrayLayers = 1;
    sti.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sti.sharingMode = d->families.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    sti.queueFamilyIndexCount = d->families.size() > 1 ? (uint32_t)d->families.size() : 0;
    sti.pQueueFamilyIndices = d->families.size() > 1 ? d->families.data() : nullptr;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (uint32_t i = 0; ok && i < kStage; i++)
    {
        ok = CreateImageMem(d, sti, &sc->stage[i], &sc->stageMem[i]);
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = sc->stage[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ci->imageFormat;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        ok = ok && d->CreateImageView(dev, &vci, nullptr, &sc->stageView[i]) == VK_SUCCESS;
        ok = ok && d->CreateFence(dev, &fci, nullptr, &sc->copyFence[i]) == VK_SUCCESS;
    }
    if (!ok) { LOGE("BFI Presenter: could not create the staging images"); DestroySCResources(sc); return false; }

    // Initial layouts: stand-ins as if presented, staging as shader input.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = d->family;
    ok = d->CreateCommandPool(dev, &pci, nullptr, &pool) == VK_SUCCESS;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    ok = ok && d->AllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS;
    if (ok)
    {
        if (d->setLoaderData) d->setLoaderData(dev, cb);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        d->BeginCommandBuffer(cb, &bi);
        std::vector<VkImageMemoryBarrier> b;
        for (uint32_t i = 0; i < sc->nVirt; i++)
        {
            VkImageMemoryBarrier x = ImgBarrier(sc->virt[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0);
            x.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
            b.push_back(x);
        }
        for (uint32_t i = 0; i < kStage; i++)
            b.push_back(ImgBarrier(sc->stage[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0));
        d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                              (uint32_t)b.size(), b.data());
        d->EndCommandBuffer(cb);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        std::lock_guard<std::recursive_mutex> l(d->qlock);
        ok = d->QueueSubmit(d->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
        d->QueueWaitIdle(d->queue);
    }
    if (pool) d->DestroyCommandPool(dev, pool, nullptr);
    if (!ok) { LOGE("BFI Presenter: could not prepare the swap chain images"); DestroySCResources(sc); return false; }
    return true;
}

// The real swap chain gets the game's extension structures we know are safe
// with the presenter's own settings. Present-mode lists (swapchain
// maintenance1, used by DXVK) are dropped: the presenter always uses FIFO,
// which such a list need not contain.
struct ChainCopy
{
    std::vector<std::vector<uint8_t>> mem;
    const void *head = nullptr;
};
static void FilterSwapchainChain(const void *pNext, ChainCopy &out, bool dropFormatList)
{
    VkBaseOutStructure *last = nullptr;
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)pNext; p; p = p->pNext)
    {
        size_t sz = 0;
        switch ((int)p->sType)
        {
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: sz = dropFormatList ? 0 : sizeof(VkImageFormatListCreateInfo); break;
        case VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT: sz = sizeof(VkSwapchainCounterCreateInfoEXT); break;
        case VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV: sz = sizeof(VkSwapchainLatencyCreateInfoNV); break;
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT: sz = sizeof(VkSwapchainPresentScalingCreateInfoEXT); break;
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR: sz = sizeof(VkDeviceGroupSwapchainCreateInfoKHR); break;
        case VK_STRUCTURE_TYPE_SWAPCHAIN_DISPLAY_NATIVE_HDR_CREATE_INFO_AMD: sz = sizeof(VkSwapchainDisplayNativeHdrCreateInfoAMD); break;
#ifdef _WIN32
        case VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT: sz = sizeof(VkSurfaceFullScreenExclusiveInfoEXT); break;
        case VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT: sz = sizeof(VkSurfaceFullScreenExclusiveWin32InfoEXT); break;
#endif
        default:
            LOGI("BFI Presenter: swap chain extension structure %d not passed to the real swap chain", (int)p->sType);
            continue;
        }
        if (!sz) continue;
        out.mem.emplace_back(sz);
        memcpy(out.mem.back().data(), p, sz);
        VkBaseOutStructure *c = (VkBaseOutStructure *)out.mem.back().data();
        c->pNext = nullptr;
        if (last) last->pNext = c; else out.head = c;
        last = c;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL Bfi_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                             const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    DeviceData *d = GetDev(Key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    Swapchain *old = ci->oldSwapchain ? GetSC(ci->oldSwapchain) : nullptr;
    if (old)
    {
        // The old one retires: its presenter stops now (the game destroys it later).
        old->retired.store(true);
        StopPresenter(old);
        LOGI("BFI Presenter: swap chain replaced by the game -- presenter for the old one stopped");
    }
    if (!g_active || !d->ok || !d->queue)
        return d->CreateSwapchainKHR(device, ci, alloc, out);

    InstanceData *inst = d->inst;
    VkBool32 sup = VK_FALSE;
    if (inst->GetPhysicalDeviceSurfaceSupportKHR)
        inst->GetPhysicalDeviceSurfaceSupportKHR(d->phys, d->family, ci->surface, &sup);
    VkSurfaceCapabilitiesKHR caps = {};
    if (!sup || !inst->GetPhysicalDeviceSurfaceCapabilitiesKHR ||
        inst->GetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, ci->surface, &caps) != VK_SUCCESS)
    {
        LOGW("BFI Presenter: the presenter's queue family %u can't present to this surface -- swap chain not managed", d->family);
        SetReason(0, "Not managed: the presenter's queue can't present to the game's window.");
        return d->CreateSwapchainKHR(device, ci, alloc, out);
    }
    Settings s;
    {
        std::lock_guard<std::mutex> l(g_cfgLock);
        s = g_shared;
    }
    VkSwapchainCreateInfoKHR rci = *ci;
    uint32_t want = std::max((uint32_t)s.presBuffers, caps.minImageCount);
    if (caps.maxImageCount) want = std::min(want, caps.maxImageCount);
    rci.minImageCount = want;
    rci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (g_dumpFrames) rci.imageUsage |= caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    rci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    rci.queueFamilyIndexCount = 0;
    rci.pQueueFamilyIndices = nullptr;
    rci.presentMode = VK_PRESENT_MODE_FIFO_KHR;          // the presenter paces to vblank itself
    rci.oldSwapchain = old ? old->real : ci->oldSwapchain;
    // SDR game on an HDR display: present HDR10, so the lit refresh can be
    // boosted above SDR white (BFI needs the headroom; in SDR the boost is
    // clipped and Variable MPRT spills it all into the dark refresh).
    bool upgrade = false;
    // Not in DXVK games: an HDR10 swap chain makes the NVIDIA driver present
    // through DXGI / D3D11, which in that process are DXVK's own DLLs -- the
    // presenter's first present never returned (Twelve Minutes).
    static const bool dxvk = DxvkInProcess();
    if (dxvk && s.sdrToHdr == 1 && ci->imageColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
        static bool logged = false;
        if (!logged) { logged = true; LOGI("BFI Presenter: DXVK game -- SDR stays SDR (SdrToHdr=2 forces HDR10; needs NVIDIA 'Vulkan/OpenGL present method: Prefer native')"); }
    }
    const VkFormat gf = ci->imageFormat;
    const bool sdrFmt = gf == VK_FORMAT_B8G8R8A8_UNORM || gf == VK_FORMAT_R8G8B8A8_UNORM || gf == VK_FORMAT_B8G8R8A8_SRGB ||
                        gf == VK_FORMAT_R8G8B8A8_SRGB || gf == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || gf == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    if ((!dxvk || s.sdrToHdr == 2) && s.sdrToHdr && ci->imageColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && sdrFmt && g_colorspaceExt.load() &&
        inst->GetPhysicalDeviceSurfaceFormatsKHR)
    {
        uint32_t nf = 0;
        inst->GetPhysicalDeviceSurfaceFormatsKHR(d->phys, ci->surface, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> sf(nf);
        if (nf) inst->GetPhysicalDeviceSurfaceFormatsKHR(d->phys, ci->surface, &nf, sf.data());
        for (const VkFormat want : { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32 })
            for (const auto &f : sf)
                if (!upgrade && f.format == want && f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
                {
                    upgrade = true;
                    rci.imageFormat = want;
                    rci.imageColorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
                    rci.flags &= ~VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
                }
        if (!upgrade) LOGI("BFI Presenter: SDR game; the display offers no HDR10 output (Windows HDR off?) -- staying SDR, use Gain 0.5");
    }
    if (!upgrade && getenv("BFI_PRESENTER_FAKEHDR")) upgrade = true;   // test builds: PQ into the SDR swap chain
    ChainCopy chainCopy;
    FilterSwapchainChain(ci->pNext, chainCopy, upgrade);
    rci.pNext = chainCopy.head;
    VkSwapchainKHR real = VK_NULL_HANDLE;
    VkResult r = d->CreateSwapchainKHR(device, &rci, alloc, &real);
    if (r != VK_SUCCESS && upgrade)
    {
        LOGW("BFI Presenter: HDR10 swap chain refused (%d) -- staying SDR", (int)r);
        upgrade = false;
        rci.imageFormat = ci->imageFormat;
        rci.imageColorSpace = ci->imageColorSpace;
        rci.flags = ci->flags;
        ChainCopy c2;
        FilterSwapchainChain(ci->pNext, c2, false);
        rci.pNext = c2.head;
        r = d->CreateSwapchainKHR(device, &rci, alloc, &real);
    }
    if (r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
    {
        // An earlier swap chain of ours still holds the window (the game made
        // a new one without passing the old as oldSwapchain): retire it.
        Swapchain *prev = nullptr;
        {
            std::lock_guard<std::mutex> l(g_mapLock);
            for (auto &kv : g_swapchains)
                if (kv.second->surface == ci->surface && kv.second != old && kv.second->d == d) { prev = kv.second; break; }
        }
        if (prev)
        {
            prev->retired.store(true);
            StopPresenter(prev);
            rci.oldSwapchain = prev->real;
            r = d->CreateSwapchainKHR(device, &rci, alloc, &real);
            LOGI("BFI Presenter: the window was still held by the previous swap chain -- retired it (%d)", (int)r);
        }
    }
    if (r != VK_SUCCESS)
    {
        LOGE("BFI Presenter: creating the real swap chain failed (%d) -- not managed", (int)r);
        SetReason(0, "Not managed: the real swap chain could not be created (%d).", (int)r);
        return d->CreateSwapchainKHR(device, ci, alloc, out);
    }
    Swapchain *sc = new Swapchain();
    sc->d = d;
    sc->real = real;
    sc->surface = ci->surface;
    {
        std::lock_guard<std::mutex> l(g_mapLock);
        auto it = g_surfaceWindow.find((uint64_t)ci->surface);
        sc->hwnd = it == g_surfaceWindow.end() ? nullptr : it->second;
    }
    sc->format = ci->imageFormat;
    sc->colorSpace = ci->imageColorSpace;
    sc->extent = ci->imageExtent;
    sc->realUsage = rci.imageUsage;
    sc->realFormat = rci.imageFormat;
    sc->hdrUpgrade = upgrade;
    if (upgrade) LOGI("BFI Presenter: SDR game presented as HDR10 (SDR white %.0f nits) for BFI headroom", s.sdrWhite);
    sc->maxAhead.store(s.autoAhead && s.maxAhead ? 1 : s.maxAhead);
    if (!BuildSC(sc, ci))
    {
        d->DestroySwapchainKHR(device, real, alloc);
        delete sc;
        SetReason(0, "Not managed: the stand-in images could not be created.");
        return d->CreateSwapchainKHR(device, ci, alloc, out);
    }
    {
        std::lock_guard<std::mutex> l(g_mapLock);
        g_swapchains[(uint64_t)real] = sc;
    }
    *out = real;
    LOGI("BFI Presenter: managing swap chain %ux%u format %d colour space %d: %u stand-in images for the game, %u real "
         "(game asked for %u, present mode %d)", ci->imageExtent.width, ci->imageExtent.height, (int)ci->imageFormat,
         (int)ci->imageColorSpace, sc->nVirt, sc->nReal, ci->minImageCount, (int)ci->presentMode);
    {
        std::lock_guard<std::mutex> l(g_cfgLock);
        g_status.width = ci->imageExtent.width;
        g_status.height = ci->imageExtent.height;
        g_status.images = sc->nVirt;
        g_status.realImages = sc->nReal;
        g_status.format = (int)ci->imageFormat;
        g_status.colorSpace = (int)ci->imageColorSpace;
        g_status.sharedQueue = d->sharedQueue ? 1 : 0;
    }
    SetReason(1, "Managing the swap chain (%ux%u, %u images).", ci->imageExtent.width, ci->imageExtent.height, sc->nVirt);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL Bfi_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *alloc)
{
    DeviceData *d = GetDev(Key(device));
    if (!d) return;
    Swapchain *sc = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mapLock);
        auto it = g_swapchains.find((uint64_t)swapchain);
        if (it != g_swapchains.end()) { sc = it->second; g_swapchains.erase(it); }
    }
    if (sc)
    {
        DestroySCResources(sc);
        LOGI("BFI Presenter: swap chain destroyed");
        delete sc;
        // Games recreate a swap chain by creating the new one first and then
        // destroying the old one: only go back to "waiting" when no managed
        // swap chain is left (it overwrote the new one's status).
        bool any;
        {
            std::lock_guard<std::mutex> l(g_mapLock);
            any = !g_swapchains.empty();
        }
        if (!any) SetReason(0, "Waiting for the game's swap chain.");
    }
    d->DestroySwapchainKHR(device, swapchain, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL Bfi_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images)
{
    DeviceData *d = GetDev(Key(device));
    Swapchain *sc = GetSC(swapchain);
    if (!sc) return d->GetSwapchainImagesKHR(device, swapchain, count, images);
    if (!images) { *count = sc->nVirt; return VK_SUCCESS; }
    const uint32_t n = std::min(*count, sc->nVirt);
    for (uint32_t i = 0; i < n; i++) images[i] = sc->virt[i];
    *count = n;
    return n < sc->nVirt ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL Bfi_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                              VkSemaphore sem, VkFence fence, uint32_t *idx)
{
    Swapchain *sc = GetSC(swapchain);
    if (!sc) return GetDev(Key(device))->AcquireNextImageKHR(device, swapchain, timeout, sem, fence, idx);
    return AcquireVirtual(sc, sem, fence, idx);
}
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *idx)
{
    Swapchain *sc = GetSC(info->swapchain);
    if (!sc) return GetDev(Key(device))->AcquireNextImage2KHR(device, info, idx);
    return AcquireVirtual(sc, info->semaphore, info->fence, idx);
}
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_GetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR swapchain)
{
    Swapchain *sc = GetSC(swapchain);
    if (!sc) return GetDev(Key(device))->GetSwapchainStatusKHR(device, swapchain);
    if (sc->retired.load()) return VK_ERROR_OUT_OF_DATE_KHR;
    const int st = sc->status.load();
    if (st != VK_SUCCESS) return (VkResult)st;
    return sc->suboptimal.load() ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_ReleaseSwapchainImagesEXT(VkDevice device, const VkReleaseSwapchainImagesInfoEXT *info)
{
    Swapchain *sc = GetSC(info->swapchain);
    if (!sc) return GetDev(Key(device))->ReleaseSwapchainImagesEXT(device, info);
    return VK_SUCCESS;                                     // stand-in images need no release
}
// VK_KHR_present_wait (DXVK uses it for pacing): a game present counts as
// done once its frame (or a later one) has been shown.
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout)
{
    Swapchain *sc = GetSC(swapchain);
    if (!sc) return GetDev(Key(device))->WaitForPresentKHR(device, swapchain, presentId, timeout);
    const double until = Now() + (timeout == UINT64_MAX ? 1e9 : (double)timeout * 1e-9);
    std::unique_lock<std::mutex> l(sc->presIdLock);
    while (sc->shownPresentId.load() < presentId)
    {
        if (sc->retired.load() || sc->status.load() != VK_SUCCESS) return VK_ERROR_OUT_OF_DATE_KHR;
        if (!sc->running.load() && sc->failed.load()) return VK_SUCCESS;
        const double left = until - Now();
        if (left <= 0.0) return VK_TIMEOUT;
        sc->presIdCv.wait_for(l, std::chrono::duration<double>(std::min(left, 0.05)));
    }
    return VK_SUCCESS;
}

// ---------------------------------------------------------------- present
static std::atomic<bool> g_callerChecked{false};
static void CheckCaller(void *ret)
{
    if (g_callerChecked.exchange(true)) return;
    char mod[64] = "?";
    bool reshadeLoaded = false;
#ifdef _WIN32
    HMODULE h = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &h) && h)
    {
        char p[MAX_PATH];
        GetModuleFileNameA(h, p, MAX_PATH);
        const char *b = strrchr(p, '\\');
        snprintf(mod, sizeof(mod), "%s", b ? b + 1 : p);
    }
    reshadeLoaded = GetModuleHandleA("ReShade64.dll") != nullptr || GetModuleHandleA("ReShade32.dll") != nullptr;
    // ReShade anywhere above us on the call stack is what matters (other
    // layers, e.g. Steam's overlay, may sit between ReShade and this layer).
    bool onStack = false;
    {
        void *frames[48];
        const USHORT nfr = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT k = 0; k < nfr && !onStack; k++)
        {
            HMODULE fm = nullptr;
            char fp[MAX_PATH];
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[k], &fm) &&
                fm && GetModuleFileNameA(fm, fp, MAX_PATH))
            {
                for (char *c = fp; *c; c++) *c = (char)tolower((unsigned char)*c);
                const char *bn = strrchr(fp, '\\');
                if (strstr(bn ? bn : fp, "reshade")) onStack = true;
            }
        }
    }
#else
    Dl_info di;
    if (dladdr(ret, &di) && di.dli_fname)
    {
        const char *b = strrchr(di.dli_fname, '/');
        snprintf(mod, sizeof(mod), "%s", b ? b + 1 : di.dli_fname);
    }
#endif
    std::string lm = mod;
    for (char &c : lm) c = (char)tolower((unsigned char)c);
    bool fromReshade = lm.find("reshade") != std::string::npos;
#ifdef _WIN32
    if (onStack) fromReshade = true;
#endif
    int order = fromReshade ? 1 : (reshadeLoaded ? 2 : 0);
    if (fromReshade) LOGI("BFI Presenter: presents arrive from %s with ReShade above -- the layer is below ReShade (correct)", mod);
    else if (reshadeLoaded)
        LOGE("BFI Presenter: presents arrive from %s, not from ReShade -- the layer is ABOVE ReShade, so ReShade's effects "
             "would run on every strobed refresh. Re-run install_vulkan_layer.bat (it must be registered after ReShade).", mod);
    else LOGI("BFI Presenter: presents arrive from %s (ReShade is not loaded)", mod);
    std::lock_guard<std::mutex> l(g_cfgLock);
    g_status.layerOrder = order;
    snprintf(g_status.caller, sizeof(g_status.caller), "%s", mod);
}

#if defined(_MSC_VER)
#include <intrin.h>
#define BFI_RETADDR() _ReturnAddress()
#else
#define BFI_RETADDR() __builtin_return_address(0)
#endif

static VKAPI_ATTR VkResult VKAPI_CALL Bfi_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    DeviceData *d = GetDev(Key(queue));
    if (!d) return VK_ERROR_DEVICE_LOST;
    std::vector<uint32_t> ours, others;
    if (g_active)
        for (uint32_t i = 0; i < pi->swapchainCount; i++)
            (GetSC(pi->pSwapchains[i]) ? ours : others).push_back(i);
    if (ours.empty())
    {
        std::unique_lock<std::recursive_mutex> ql(d->qlock, std::defer_lock);
        if (IsOurQueue(d, queue)) ql.lock();
        return d->QueuePresentKHR(queue, pi);
    }
    // No queue lock held here: GameFrame locks around its own submit only (it
    // may wait for the presenter in the frame cap).
    CheckCaller(BFI_RETADDR());

    const uint64_t *ids = nullptr;
    const VkFence *presentFences = nullptr;     // swapchain maintenance1 (DXVK 3)
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)pi->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR)
        {
            const VkPresentIdKHR *pid = (const VkPresentIdKHR *)p;
            if (pid->swapchainCount == pi->swapchainCount) ids = pid->pPresentIds;
        }
        else if ((int)p->sType == 1000275001)   // VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR / _EXT
        {
            const VkSwapchainPresentFenceInfoEXT *pf = (const VkSwapchainPresentFenceInfoEXT *)p;
            if (pf->swapchainCount == pi->swapchainCount) presentFences = pf->pFences;
        }

    // Other swap chains in the same call wait on our copy instead of the
    // game's semaphores (which the copy consumes).
    static thread_local VkSemaphore chain = VK_NULL_HANDLE;
    if (!others.empty() && !chain)
    {
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        d->CreateSemaphore(d->device, &sci, nullptr, &chain);
    }
    VkResult worst = VK_SUCCESS;
    auto merge = [&](VkResult r) {
        if (r < 0 && worst >= 0) worst = r;
        else if (r == VK_SUBOPTIMAL_KHR && worst == VK_SUCCESS) worst = r;
    };
    bool first = true;
    for (uint32_t i : ours)
    {
        Swapchain *sc = GetSC(pi->pSwapchains[i]);
        const VkResult r = GameFrame(sc, queue, first ? pi->waitSemaphoreCount : 0, first ? pi->pWaitSemaphores : nullptr,
                                     pi->pImageIndices[i], ids ? ids[i] : 0, (first && !others.empty()) ? chain : VK_NULL_HANDLE);
        if (pi->pResults) pi->pResults[i] = r;
        merge(r);
        first = false;
        // Present fence: the game waits on it before it reuses the semaphores
        // and image of this present (DXVK 3 does). Those were consumed by the
        // copy just submitted, so an empty submit behind it signals the fence
        // at the right time. Never signalled, DXVK waited for good (Twelve
        // Minutes froze right after BFI started).
        if (presentFences && presentFences[i])
        {
            std::unique_lock<std::recursive_mutex> ql(d->qlock, std::defer_lock);
            if (IsOurQueue(d, queue)) ql.lock();
            const VkResult fr = d->QueueSubmit(queue, 0, nullptr, presentFences[i]);
            if (fr != VK_SUCCESS) LOGW("BFI Presenter: present fence submit failed (%d)", (int)fr);
            static bool logged = false;
            if (!logged) { logged = true; LOGI("BFI Presenter: the game uses present fences (swapchain maintenance1) -- signalled after each frame's copy"); }
        }
    }
    if (!others.empty())
    {
        std::vector<VkSwapchainKHR> scs;
        std::vector<uint32_t> idx;
        std::vector<VkResult> res(others.size(), VK_SUCCESS);
        for (uint32_t i : others) { scs.push_back(pi->pSwapchains[i]); idx.push_back(pi->pImageIndices[i]); }
        VkPresentInfoKHR p2 = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        p2.waitSemaphoreCount = 1;
        p2.pWaitSemaphores = &chain;
        p2.swapchainCount = (uint32_t)scs.size();
        p2.pSwapchains = scs.data();
        p2.pImageIndices = idx.data();
        p2.pResults = res.data();
        std::vector<VkFence> otherFences;
        VkSwapchainPresentFenceInfoEXT pf2 = { (VkStructureType)1000275001 };
        if (presentFences)
        {
            for (uint32_t i : others) otherFences.push_back(presentFences[i]);
            pf2.swapchainCount = (uint32_t)otherFences.size();
            pf2.pFences = otherFences.data();
            p2.pNext = &pf2;
        }
        {
            std::unique_lock<std::recursive_mutex> ql(d->qlock, std::defer_lock);
            if (IsOurQueue(d, queue)) ql.lock();
            merge(d->QueuePresentKHR(queue, &p2));
        }
        for (size_t k = 0; k < others.size(); k++)
        {
            if (pi->pResults) pi->pResults[others[k]] = res[k];
            merge(res[k]);
        }
    }
    return worst;
}

// Queue calls are locked only when the presenter shares the game's queue.
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{
    DeviceData *d = GetDev(Key(q));
    std::unique_lock<std::recursive_mutex> l(d->qlock, std::defer_lock);
    if (IsOurQueue(d, q)) l.lock();
    return d->QueueSubmit(q, n, s, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f)
{
    DeviceData *d = GetDev(Key(q));
    std::unique_lock<std::recursive_mutex> l(d->qlock, std::defer_lock);
    if (IsOurQueue(d, q)) l.lock();
    return d->QueueSubmit2(q, n, s, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f)
{
    DeviceData *d = GetDev(Key(q));
    std::unique_lock<std::recursive_mutex> l(d->qlock, std::defer_lock);
    if (IsOurQueue(d, q)) l.lock();
    return d->QueueSubmit2KHR(q, n, s, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo *s, VkFence f)
{
    DeviceData *d = GetDev(Key(q));
    std::unique_lock<std::recursive_mutex> l(d->qlock, std::defer_lock);
    if (IsOurQueue(d, q)) l.lock();
    return d->QueueBindSparse(q, n, s, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_QueueWaitIdle(VkQueue q)
{
    DeviceData *d = GetDev(Key(q));
    std::unique_lock<std::recursive_mutex> l(d->qlock, std::defer_lock);
    if (IsOurQueue(d, q)) l.lock();
    return d->QueueWaitIdle(q);
}
// vkDeviceWaitIdle needs every queue externally synchronised -- ours included.
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_DeviceWaitIdle(VkDevice device)
{
    DeviceData *d = GetDev(Key(device));
    std::lock_guard<std::recursive_mutex> l(d->qlock);
    return d->DeviceWaitIdle(device);
}
static VKAPI_ATTR void VKAPI_CALL Bfi_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *q)
{
    DeviceData *d = GetDev(Key(device));
    d->GetDeviceQueue(device, family, index, q);
    if (*q)
    {
        std::lock_guard<std::mutex> l(g_mapLock);
        g_queueFamily[(void *)*q] = family;
    }
}
static VKAPI_ATTR void VKAPI_CALL Bfi_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *q);
static VKAPI_ATTR void VKAPI_CALL Bfi_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *q)
{
    DeviceData *d = GetDev(Key(device));
    d->GetDeviceQueue2(device, info, q);
    if (*q)
    {
        std::lock_guard<std::mutex> l(g_mapLock);
        g_queueFamily[(void *)*q] = info->queueFamilyIndex;
    }
}

// ---------------------------------------------------------------- ReShade settings window (Windows)
#ifdef _WIN32
static HMODULE g_module = nullptr;
static void TryRegisterReShade();
static void UnregisterReShade();
#else
static void TryRegisterReShade() {}
static void UnregisterReShade() {}
#endif

// ---------------------------------------------------------------- activation
static std::once_flag g_initOnce;
static void InitProcess()
{
    std::call_once(g_initOnce, [] {
        FindExe();
        const char *iniEnv = getenv("BFI_PRESENTER_INI");
        g_iniPath = iniEnv && *iniEnv ? iniEnv : g_exeDir + "BFIPresenter.ini";
        {
            const size_t k = g_iniPath.find_last_of("\\/");
            g_outDir = k == std::string::npos ? g_exeDir : g_iniPath.substr(0, k + 1);
        }
        FILE *f = fopen(g_iniPath.c_str(), "r");
        const bool haveIni = f != nullptr;
        if (f) fclose(f);
        const char *force = getenv("BFI_PRESENTER_VK");
        bool enabled = haveIni && KvInt(ReadIni(), "VulkanLayer", 1) != 0;
        if (force && *force) enabled = atoi(force) != 0;
        g_active = enabled;
        if (!g_active) return;                       // no ini: stay silent (no log file)
        if (const char *dmp = getenv("BFI_PRESENTER_DUMP")) g_dumpFrames = atoi(dmp);
        LoadShared();
        LOGI("BFI Presenter: active in %s (settings: %s%s)", g_exeName.c_str(), g_iniPath.c_str(), haveIni ? "" : ", not found -- defaults");
    });
}

// ---------------------------------------------------------------- instance / device
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out)
{
    InitProcess();
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    // HDR10 presentation of SDR games needs VK_EXT_swapchain_colorspace; add
    // it when the game didn't (and drop it again if the driver refuses).
    std::vector<const char *> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    bool hasCs = false;
    for (const char *e : exts) if (!strcmp(e, "VK_EXT_swapchain_colorspace")) hasCs = true;
    VkResult r = VK_ERROR_EXTENSION_NOT_PRESENT;
    if (g_active && !hasCs)
    {
        VkInstanceCreateInfo mci = *ci;
        exts.push_back("VK_EXT_swapchain_colorspace");
        mci.enabledExtensionCount = (uint32_t)exts.size();
        mci.ppEnabledExtensionNames = exts.data();
        r = create(&mci, a, out);
        if (r == VK_SUCCESS) hasCs = true;
    }
    if (r != VK_SUCCESS) r = create(ci, a, out);
    if (r != VK_SUCCESS) return r;
    if (g_active) g_colorspaceExt.store(hasCs);
    InstanceData *id = new InstanceData();
    id->instance = *out;
    id->gipa = gipa;
#define X(n) id->n = (PFN_vk##n)gipa(*out, "vk" #n);
    BFI_INSTANCE_FUNCS(X)
#undef X
#ifdef _WIN32
    id->CreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)gipa(*out, "vkCreateWin32SurfaceKHR");
#endif
    std::lock_guard<std::mutex> l(g_mapLock);
    g_instances[Key(*out)] = id;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL Bfi_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *a)
{
    InstanceData *id = nullptr;
    bool last = false;
    {
        std::lock_guard<std::mutex> l(g_mapLock);
        auto it = g_instances.find(Key(instance));
        if (it == g_instances.end()) return;
        id = it->second;
        g_instances.erase(it);
        last = g_instances.empty();
    }
    if (last && g_active) UnregisterReShade();
    id->DestroyInstance(instance, a);
    delete id;
}

#ifdef _WIN32
static VKAPI_ATTR VkResult VKAPI_CALL Bfi_CreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR *ci,
                                                                const VkAllocationCallbacks *a, VkSurfaceKHR *out)
{
    InstanceData *id = GetInst(Key(instance));
    VkResult r = id->CreateWin32SurfaceKHR(instance, ci, a, out);
    if (r == VK_SUCCESS)
    {
        std::lock_guard<std::mutex> l(g_mapLock);
        g_surfaceWindow[(uint64_t)*out] = (void *)ci->hwnd;
    }
    return r;
}
#endif

static VKAPI_ATTR VkResult VKAPI_CALL Bfi_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                                       const VkAllocationCallbacks *a, VkDevice *out)
{
    InstanceData *id = GetInst(Key(phys));
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)ci->pNext, *cb = nullptr;
    for (VkLayerDeviceCreateInfo *p = chain; p; p = (VkLayerDeviceCreateInfo *)p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && p->function == VK_LOADER_DATA_CALLBACK && !cb) cb = p;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    if (!id || !chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(id->instance, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    // Presenter queue: one more queue in the game's graphics family, so the
    // presenter never has to share (or lock) the game's queues.
    VkDeviceCreateInfo mci = *ci;
    std::vector<VkDeviceQueueCreateInfo> qci(ci->pQueueCreateInfos, ci->pQueueCreateInfos + ci->queueCreateInfoCount);
    std::vector<float> prio;
    uint32_t family = UINT32_MAX, index = 0;
    bool shared = false;
    if (g_active)
    {
        uint32_t nf = 0;
        id->GetPhysicalDeviceQueueFamilyProperties(phys, &nf, nullptr);
        std::vector<VkQueueFamilyProperties> fp(nf);
        id->GetPhysicalDeviceQueueFamilyProperties(phys, &nf, fp.data());
        // Diagnostics for presenter-queue priority work: families and whether
        // the driver offers queue global priority.
        {
            uint32_t ne = 0;
            id->EnumerateDeviceExtensionProperties(phys, nullptr, &ne, nullptr);
            std::vector<VkExtensionProperties> ep(ne);
            if (ne) id->EnumerateDeviceExtensionProperties(phys, nullptr, &ne, ep.data());
            bool gpK = false, gpE = false;
            for (auto &e : ep) { if (!strcmp(e.extensionName, "VK_KHR_global_priority")) gpK = true; if (!strcmp(e.extensionName, "VK_EXT_global_priority")) gpE = true; }
            std::string fams;
            for (uint32_t i = 0; i < nf; i++)
            {
                char b[64];
                snprintf(b, sizeof(b), "%s%u:%s%s%s x%u", i ? ", " : "", i, (fp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? "G" : "",
                         (fp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "C" : "", (fp[i].queueFlags & VK_QUEUE_TRANSFER_BIT) ? "T" : "", fp[i].queueCount);
                fams += b;
            }
            LOGI("BFI Presenter: queue families %s; global priority: KHR %s, EXT %s", fams.c_str(), gpK ? "yes" : "no", gpE ? "yes" : "no");
        }
        int which = -1;
        for (size_t i = 0; i < qci.size(); i++)
            if (qci[i].queueFamilyIndex < nf && (fp[qci[i].queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                !(qci[i].flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT)) { which = (int)i; break; }
        Settings s;
        {
            std::lock_guard<std::mutex> l(g_cfgLock);
            s = g_shared;
        }
        if (which >= 0)
        {
            family = qci[which].queueFamilyIndex;
            if (s.ownQueue && fp[family].queueCount > qci[which].queueCount)
            {
                prio.assign(qci[which].pQueuePriorities, qci[which].pQueuePriorities + qci[which].queueCount);
                prio.push_back(1.0f);
                index = qci[which].queueCount;
                qci[which].queueCount++;
                qci[which].pQueuePriorities = prio.data();
                mci.pQueueCreateInfos = qci.data();
            }
            else { shared = true; index = 0; }
        }
    }
    VkResult r = create(phys, &mci, a, out);
    if (r != VK_SUCCESS && mci.pQueueCreateInfos != ci->pQueueCreateInfos)
    {
        LOGW("BFI Presenter: device creation with an extra queue failed (%d) -- sharing the game's queue", (int)r);
        shared = true; index = 0;
        r = create(phys, ci, a, out);
    }
    if (r != VK_SUCCESS) return r;

    DeviceData *d = new DeviceData();
    d->device = *out;
    d->phys = phys;
    d->inst = id;
    d->gdpa = gdpa;
    d->setLoaderData = cb ? cb->u.pfnSetDeviceLoaderData : nullptr;
    bool ok = true;
#define X(n) d->n = (PFN_vk##n)gdpa(*out, "vk" #n); if (!d->n) { ok = false; LOGE("BFI Presenter: vk" #n " missing"); }
    BFI_DEVICE_FUNCS(X)
#undef X
#define X(n) d->n = (PFN_vk##n)gdpa(*out, "vk" #n);
    BFI_DEVICE_OPT_FUNCS(X)
#undef X
    d->ok = ok;
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
        if (std::find(d->families.begin(), d->families.end(), ci->pQueueCreateInfos[i].queueFamilyIndex) == d->families.end())
            d->families.push_back(ci->pQueueCreateInfos[i].queueFamilyIndex);
    id->GetPhysicalDeviceMemoryProperties(phys, &d->mem);
    if (g_active && ok && family != UINT32_MAX)
    {
        d->family = family;
        d->sharedQueue = shared;
        d->GetDeviceQueue(*out, family, index, &d->queue);
        if (d->queue && d->setLoaderData) d->setLoaderData(*out, d->queue);
        VkPhysicalDeviceProperties pp;
        id->GetPhysicalDeviceProperties(phys, &pp);
        LOGI("BFI Presenter: device %s -- presenter queue: family %u index %u (%s)", pp.deviceName, family, index,
             shared ? "shared with the game, locked" : "its own");
        if (shared) LOGW("BFI Presenter: no spare queue in family %u -- the presenter shares the game's queue", family);
    }
    {
        std::lock_guard<std::mutex> l(g_mapLock);
        g_devices[Key(*out)] = d;
        if (d->queue) g_queueFamily[(void *)d->queue] = family;
    }
    if (g_active) TryRegisterReShade();
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL Bfi_DestroyDevice(VkDevice device, const VkAllocationCallbacks *a)
{
    DeviceData *d = nullptr;
    std::vector<Swapchain *> left;
    {
        std::lock_guard<std::mutex> l(g_mapLock);
        auto it = g_devices.find(Key(device));
        if (it == g_devices.end()) return;
        d = it->second;
        g_devices.erase(it);
        for (auto s = g_swapchains.begin(); s != g_swapchains.end();)
            if (s->second->d == d) { left.push_back(s->second); s = g_swapchains.erase(s); }
            else ++s;
    }
    for (Swapchain *sc : left)
    {
        DestroySCResources(sc);
        d->DestroySwapchainKHR(device, sc->real, nullptr);
        delete sc;
    }
    d->DestroyDevice(device, a);
    delete d;
}

static VKAPI_ATTR VkResult VKAPI_CALL Bfi_EnumerateDeviceExtensionProperties(VkPhysicalDevice phys, const char *layer,
                                                                             uint32_t *count, VkExtensionProperties *props)
{
    if (layer && !strcmp(layer, kLayerName)) { *count = 0; return VK_SUCCESS; }
    InstanceData *id = GetInst(Key(phys));
    return id->EnumerateDeviceExtensionProperties(phys, layer, count, props);
}

// ---------------------------------------------------------------- proc addr
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Bfi_GetDeviceProcAddr(VkDevice device, const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Bfi_GetInstanceProcAddr(VkInstance instance, const char *name);

static PFN_vkVoidFunction DeviceHook(const char *name, DeviceData *d)
{
#define H(n, f) if (!strcmp(name, "vk" #n)) return (d && !d->n) ? nullptr : (PFN_vkVoidFunction)f;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)Bfi_GetDeviceProcAddr;
    H(DestroyDevice, Bfi_DestroyDevice)
    if (!g_active) return nullptr;
    H(CreateSwapchainKHR, Bfi_CreateSwapchainKHR)
    H(DestroySwapchainKHR, Bfi_DestroySwapchainKHR)
    H(GetSwapchainImagesKHR, Bfi_GetSwapchainImagesKHR)
    H(AcquireNextImageKHR, Bfi_AcquireNextImageKHR)
    H(AcquireNextImage2KHR, Bfi_AcquireNextImage2KHR)
    H(QueuePresentKHR, Bfi_QueuePresentKHR)
    H(GetSwapchainStatusKHR, Bfi_GetSwapchainStatusKHR)
    H(ReleaseSwapchainImagesEXT, Bfi_ReleaseSwapchainImagesEXT)
    H(WaitForPresentKHR, Bfi_WaitForPresentKHR)
    H(QueueSubmit, Bfi_QueueSubmit)
    H(QueueSubmit2, Bfi_QueueSubmit2)
    H(QueueSubmit2KHR, Bfi_QueueSubmit2KHR)
    H(QueueBindSparse, Bfi_QueueBindSparse)
    H(QueueWaitIdle, Bfi_QueueWaitIdle)
    H(DeviceWaitIdle, Bfi_DeviceWaitIdle)
    H(GetDeviceQueue, Bfi_GetDeviceQueue)
    H(GetDeviceQueue2, Bfi_GetDeviceQueue2)
#undef H
    return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Bfi_GetDeviceProcAddr(VkDevice device, const char *name)
{
    DeviceData *d = GetDev(Key(device));
    if (!d) return nullptr;
    if (PFN_vkVoidFunction f = DeviceHook(name, d)) return f;
    return d->gdpa(device, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Bfi_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    InitProcess();
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)Bfi_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)Bfi_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)Bfi_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)Bfi_CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)Bfi_GetDeviceProcAddr;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)Bfi_EnumerateDeviceExtensionProperties;
    if (!instance) return nullptr;
    InstanceData *id = GetInst(Key(instance));
    if (!id) return nullptr;
#ifdef _WIN32
    if (g_active && !strcmp(name, "vkCreateWin32SurfaceKHR") && id->CreateWin32SurfaceKHR)
        return (PFN_vkVoidFunction)Bfi_CreateWin32SurfaceKHR;
#endif
    if (g_active)
    {
        // device-level functions queried through the instance
        PFN_vkVoidFunction next = id->gipa(instance, name);
        if (next)
            if (PFN_vkVoidFunction f = DeviceHook(name, nullptr)) return f;
        return next;
    }
    return id->gipa(instance, name);
}

BFI_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion >= 2)
    {
        v->pfnGetInstanceProcAddr = Bfi_GetInstanceProcAddr;
        v->pfnGetDeviceProcAddr = Bfi_GetDeviceProcAddr;
        v->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    return VK_SUCCESS;
}
BFI_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL BFI_vkGetInstanceProcAddr(VkInstance i, const char *n) { return Bfi_GetInstanceProcAddr(i, n); }
BFI_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL BFI_vkGetDeviceProcAddr(VkDevice d, const char *n) { return Bfi_GetDeviceProcAddr(d, n); }

// ---------------------------------------------------------------- ReShade settings window (Windows)
#ifdef _WIN32
#include <imgui.h>
#include <reshade.hpp>

extern "C" __declspec(dllexport) const char *NAME;
const char *NAME = "BFI Presenter (Vulkan)";
extern "C" __declspec(dllexport) const char *DESCRIPTION;
const char *DESCRIPTION = "Presenter-clocked black frame insertion for Vulkan games (implicit Vulkan layer).";

static const char *kOutputNames = "Auto\0SDR (sRGB)\0HDR scRGB\0HDR10 (PQ)\0";

static void draw_overlay(reshade::api::effect_runtime *)
{
    Status st;
    Settings s;
    {
        std::lock_guard<std::mutex> l(g_cfgLock);
        st = g_status;
        s = g_shared;
    }
    const Settings before = s;
    const ImVec4 kRed(1.0f, 0.4f, 0.3f, 1.0f), kAmber(1.0f, 0.75f, 0.2f, 1.0f);

    // ================================================================ header
    ImGui::TextDisabled("BFI Presenter (Vulkan layer) v" BFI_VERSION);
    if (st.managed) ImGui::TextWrapped("%s", st.reason);
    else            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", st.reason);
    bool en = s.enabled != 0;
    if (ImGui::Checkbox("BFI enabled (Ctrl+Shift+B)", &en)) s.enabled = en ? 1 : 0;

    // ================================================================ warnings (only when something needs attention)
    if (st.layerOrder == 2)
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "The layer is ABOVE ReShade (presents come from %s).\n"
                           "Re-run install_vulkan_layer.bat so it is registered after ReShade.", st.caller);
    if (st.managed)
    {
        if (st.limitSuspect)
            ImGui::TextColored(kRed, "A frame limiter is holding the presenter below the display's refresh rate.\n"
                               "Turn off the RTSS limit and NVIDIA's Max Frame Rate (global and for this exe; check NVIDIA Profile Inspector).");
        if (st.late > 0)
            ImGui::TextColored(kAmber, "Missed refreshes break the strobe: check VRR is off and nothing caps the presenter.");
        if (st.hitchPaused)
            ImGui::TextColored(kAmber, "Hitch guard: BFI paused (loading or missed refreshes)");
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("OLED panels dim the picture when a refresh puts out too much light overall (area x brightness).\nWith BFI the lit refresh carries the boost, so bright skies and white screens can trigger it.\nThe lit-refresh ceiling is lowered just enough to stay within the budget. HDR only.");
        if (s.ablGuard)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderInt("Panel budget (average nits of a lit refresh)", &s.ablBudget, 100, 1500);
            if (st.litAvg >= 0.0)
                ImGui::Text("Scene asks for: %.0f nits average   After ceiling: %.0f   Ceiling in use: %.0f nits", st.rawAvg, st.litAvg, st.ceilInUse);
            ImGui::Unindent(16.0f);
        }
        bool sh = s.sdrToHdr != 0;
        if (ImGui::Checkbox("SDR games: present as HDR10 (applies when the swap chain is next created)", &sh)) s.sdrToHdr = sh ? 1 : 0;
        if (s.sdrToHdr)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderFloat("SDR white (nits)", &s.sdrWhite, 80.0f, 600.0f, "%.0f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Windows: 80 + 4 x the 'SDR content brightness' slider (40 -> 240).");
            ImGui::SliderFloat("SDR highlights expand to (nits, 0 = off)", &s.sdrHiNits, 0.0f, 2000.0f, "%.0f");
            ImGui::SliderFloat("Highlight expansion starts at (SDR signal)", &s.sdrKnee, 0.3f, 0.95f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Below this level the image is unchanged; above it the brightest parts\nstretch toward the highlight value (an HDR look for SDR games).");
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
        ImGui::Text("Game: %.1f fps   Refreshes: %.1f/s   Frame age: %.1f ms", st.gameFps, st.refreshHz, st.frameAgeMs);
        ImGui::Text("Uneven strobe cycles: %d/s (a frame repeated %d, frames skipped %d)",
                    st.unevenRep + st.unevenSkip, st.unevenRep, st.unevenSkip);
        ImGui::Text("Late refreshes: %d/s", st.late);
        ImGui::Text("Presenter: %s   Output: %s   Format: %d   %ux%u, %u game images, %u real",
                    st.running ? "running" : "stopped", OutName(st.outType), st.format, st.width, st.height, st.images, st.realImages);
        ImGui::Text("Presenter queue: %s", st.sharedQueue ? "shared with the game (no spare queue)" : "its own");
    }

    // ================================================================ 5. advanced
    if (ImGui::CollapsingHeader("Advanced"))
    {
        bool vb = s.pacing != 0;
        if (ImGui::Checkbox("Wait for vblank", &vb)) s.pacing = vb ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("One present per refresh, nothing queued: lowest latency. Needed by just-in-time release.");
        bool qa = s.queueAhead != 0;
        if (ImGui::Checkbox("Queue one refresh ahead (Wait for vblank on, 3 presenter buffers)", &qa)) s.queueAhead = qa ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps one extra finished refresh queued, so a draw delayed by the game's GPU work is\n"
                                                      "covered instead of the display holding a lit or dark refresh (a flash). +1 refresh\n"
                                                      "(~8 ms) of latency. For GPU-heavy games that flash (Indiana Jones).");
        if (s.queueAhead && st.managed)
            ImGui::Text("Queue ahead: %d re-primed, %d drained per second", st.qaPrimes, st.qaSkips);
        ImGui::SliderInt("Presenter buffers (applies when the swap chain is next created)", &s.presBuffers, 2, 4);
        bool hg = s.hitchGuard != 0;
        if (ImGui::Checkbox("Hitch guard (pause BFI during loading and missed refreshes)", &hg)) s.hitchGuard = hg ? 1 : 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("While the presenter misses refreshes or the game delivers no new frame for the loading\ntime, every refresh is shown at the strobe's average brightness instead of strobing.");
        if (s.hitchGuard)
        {
            ImGui::Indent(16.0f);
            ImGui::SliderInt("Loading: no new frame for (ms)", &s.hitchGapMs, 150, 5000);
            ImGui::Unindent(16.0f);
        }
        ImGui::Text("Game frames that waited for the presenter: %d/s (should be 0)", st.slotWaits);
        ImGui::Text("Refreshes reusing an already-drawn buffer: %d/s", st.reused);
    }

    // ================================================================ footer
    ImGui::Separator();
    bool lg = s.log != 0;
    if (ImGui::Checkbox("Timing log (new BFIPresenterVK_log_<date>_<time>.csv each time it is switched on)", &lg)) s.log = lg ? 1 : 0;
    if (memcmp(&s, &before, sizeof(Settings)) != 0)
    {
        std::lock_guard<std::mutex> l(g_cfgLock);
        g_shared = s;
    }
    if (ImGui::Button("Save to BFIPresenter.ini")) { WriteIni(SettingsKeys(s), false); LOGI("BFI Presenter: settings saved"); }
    ImGui::SameLine();
    if (ImGui::Button("Reload ini")) LoadShared();
    ImGui::TextDisabled("Use ENABLE_DECAY=0 in CRT-Standalone. Fixed 120 Hz, VRR off.");
}

// Registration goes straight to ReShade's exports (not the reshade.hpp
// helpers, which remember the first ReShade module for good): emulators
// destroy and recreate their Vulkan instance per game, the loader unloads and
// reloads ReShade's layer with it, and the settings window must be added to
// the new ReShade each time.
static std::mutex g_rsLock;
static HMODULE    g_rsModule = nullptr;     // ReShade we are registered with
static HMODULE FindReShade()
{
    HMODULE mods[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return nullptr;
    const DWORD n = std::min<DWORD>(bytes, sizeof(mods)) / sizeof(HMODULE);
    for (DWORD i = 0; i < n; i++)
        if (GetProcAddress(mods[i], "ReShadeRegisterAddon") && GetProcAddress(mods[i], "ReShadeRegisterOverlay")) return mods[i];
    return nullptr;
}
static void TryRegisterReShade()
{
    std::lock_guard<std::mutex> l(g_rsLock);
    HMODULE rs = FindReShade();
    if (!rs)
    {
        static bool logged = false;
        if (!logged) { logged = true; LOGI("BFI Presenter: ReShade (with add-on support) not found -- no settings window; edit BFIPresenter.ini"); }
        return;
    }
    if (rs == g_rsModule) return;
    auto reg   = (bool (*)(HMODULE, uint32_t))(void *)GetProcAddress(rs, "ReShadeRegisterAddon");
    auto imgui = (const imgui_function_table *(*)(uint32_t))(void *)GetProcAddress(rs, "ReShadeGetImGuiFunctionTable");
    auto ov    = (void (*)(const char *, void (*)(reshade::api::effect_runtime *)))(void *)GetProcAddress(rs, "ReShadeRegisterOverlay");
    const imgui_function_table *table = imgui ? imgui(IMGUI_VERSION_NUM) : nullptr;
    if (!reg || !ov || !table || !reg(g_module, RESHADE_API_VERSION))
    {
        LOGW("BFI Presenter: this ReShade version does not accept the add-on -- no settings window");
        return;
    }
    imgui_function_table_instance() = table;
    ov("BFI Presenter (Vulkan)", draw_overlay);
    static bool pinned = false;
    if (!pinned)
    {
        // ReShade calls into this module: never let the loader unload it.
        HMODULE h = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)(void *)&TryRegisterReShade, &h);
        pinned = true;
    }
    LOGI("BFI Presenter: settings window added to the ReShade overlay%s", g_rsModule ? " (ReShade was reloaded)" : "");
    g_rsModule = rs;
}
// Called when the game's last Vulkan instance goes: ReShade's layer may be
// unloaded right after, so take the window off while it is still there.
static void UnregisterReShade()
{
    std::lock_guard<std::mutex> l(g_rsLock);
    if (!g_rsModule) return;
    auto uov = (void (*)(const char *, void (*)(reshade::api::effect_runtime *)))(void *)GetProcAddress(g_rsModule, "ReShadeUnregisterOverlay");
    auto ureg = (void (*)(HMODULE))(void *)GetProcAddress(g_rsModule, "ReShadeUnregisterAddon");
    if (uov) uov("BFI Presenter (Vulkan)", draw_overlay);
    if (ureg) ureg(g_module);
    g_rsModule = nullptr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { g_module = hModule; DisableThreadLibraryCalls(hModule); }
    return TRUE;
}
#endif
