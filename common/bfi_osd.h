// In-game OSD (shared by the DX add-on and the Vulkan layer): a small always-on panel drawn through ReShade's overlay
// event, so the game's frame rate, the presenter's lost frames and a frame-time graph can be watched while changing the
// game's settings, without RTSS and without opening ReShade's menu. Drawn into the game's frame (it is strobed with
// it). Include after <imgui.h> and <windows.h>.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdio>

struct OsdData
{
    double gameFps = 0.0;      // game frames a second
    double refreshHz = 0.0;    // presenter refreshes a second
    int    frames = 2;         // refreshes per strobe cycle
    int    lost = 0;           // per second: finished game frames never shown
    int    repeated = 0;       // per second: strobe cycles that showed the previous frame again
    int    missed = 0;         // per second: refreshes the presenter missed
    bool   paused = false;     // hitch guard has BFI paused
    bool   bfiOn = true;
    double relFlashMs = -1.0;  // game released -> lit flash (average), -1 = not measured
    double now = 0.0;          // Now() seconds, for the totals' time
};

namespace Osd
{
// Frame-time history: one entry per game frame, filled where the presenter receives it.
static const int kHist = 240;
static float g_ms[kHist] = {};
static std::atomic<unsigned> g_head{0};
static std::atomic<double> g_lastT{0.0};

inline void FramePush(double tNowSec)
{
    const double last = g_lastT.exchange(tNowSec);
    if (last <= 0.0) return;
    const double d = (tNowSec - last) * 1000.0;
    if (d <= 0.0 || d > 1000.0) return;
    g_ms[g_head.fetch_add(1) % kHist] = (float)d;
}

// Totals since the last reset (Ctrl+Shift+Z or the overlay's button), next to the per-second counts: added once a
// second where those are counted, so a setting can be changed, the totals reset and the two runs compared.
static std::atomic<long long> g_totLost{0}, g_totRep{0}, g_totMiss{0};
static std::atomic<double> g_totSince{0.0};   // when the totals started (Now() seconds), 0 = not yet

inline void AddSecond(int lost, int repeated, int missed, double tNowSec)
{
    double z = 0.0;
    g_totSince.compare_exchange_strong(z, tNowSec);
    if (lost > 0) g_totLost.fetch_add(lost);
    if (repeated > 0) g_totRep.fetch_add(repeated);
    if (missed > 0) g_totMiss.fetch_add(missed);
}
inline void ResetTotals(double tNowSec)
{
    g_totLost.store(0); g_totRep.store(0); g_totMiss.store(0);
    g_totSince.store(tNowSec);
}

// CPU load (all cores), from GetSystemTimes between calls about a second apart.
inline int CpuLoad()
{
    static ULONGLONG lastIdle = 0, lastTotal = 0;
    static int pct = -1;
    static ULONGLONG lastTick = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastTick < 1000 && pct >= 0) return pct;
    lastTick = now;
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return -1;
    auto v = [](const FILETIME &f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    const ULONGLONG idle = v(fi), total = v(fk) + v(fu);   // kernel time includes idle
    if (lastTotal && total > lastTotal)
        pct = (int)(100.0 * (1.0 - (double)(idle - lastIdle) / (double)(total - lastTotal)) + 0.5);
    lastIdle = idle; lastTotal = total;
    return pct;
}

// GPU load through NVIDIA's NVML (nvml.dll comes with the driver); -1 on other GPUs or when it isn't there.
inline int GpuLoad()
{
    typedef int (*PFN_Init)();
    typedef int (*PFN_Handle)(unsigned, void **);
    struct Util { unsigned gpu, memory; };
    typedef int (*PFN_Util)(void *, Util *);
    static int state = 0;                    // 0 not tried, 1 ready, -1 not available
    static void *dev = nullptr;
    static PFN_Util util = nullptr;
    static int pct = -1;
    static ULONGLONG lastTick = 0;
    if (state == 0)
    {
        state = -1;
        HMODULE m = LoadLibraryA("nvml.dll");
        if (!m)
        {
            char p[MAX_PATH];
            if (ExpandEnvironmentStringsA("%ProgramW6432%\\NVIDIA Corporation\\NVSMI\\nvml.dll", p, MAX_PATH)) m = LoadLibraryA(p);
        }
        PFN_Init init = m ? (PFN_Init)(void *)GetProcAddress(m, "nvmlInit_v2") : nullptr;
        PFN_Handle handle = m ? (PFN_Handle)(void *)GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2") : nullptr;
        util = m ? (PFN_Util)(void *)GetProcAddress(m, "nvmlDeviceGetUtilizationRates") : nullptr;
        if (init && handle && util && init() == 0 && handle(0, &dev) == 0 && dev) state = 1;
    }
    if (state != 1) return -1;
    const ULONGLONG now = GetTickCount64();
    if (now - lastTick < 1000 && pct >= 0) return pct;
    lastTick = now;
    Util u = {};
    pct = util(dev, &u) == 0 ? (int)u.gpu : -1;
    return pct;
}

// corner: 0 top left, 1 top right, 2 bottom left, 3 bottom right.
inline void Draw(const OsdData &d, int corner, bool graph, bool loads, float scale)
{
    const ImGuiIO &io = ImGui::GetIO();
    const float pad = 12.0f;
    const ImVec2 pos(corner & 1 ? io.DisplaySize.x - pad : pad, corner & 2 ? io.DisplaySize.y - pad : pad);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(corner & 1 ? 1.0f : 0.0f, corner & 2 ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                ImGuiWindowFlags_NoMove;
    if (!ImGui::Begin("##BFIPresenterOSD", nullptr, fl)) { ImGui::End(); return; }
    ImGui::SetWindowFontScale(scale);
    const ImVec4 kGood(0.55f, 0.9f, 0.55f, 1.0f), kWarn(1.0f, 0.75f, 0.2f, 1.0f), kBad(1.0f, 0.4f, 0.3f, 1.0f);
    const double cycles = d.frames > 0 && d.refreshHz > 1.0 ? d.refreshHz / d.frames : 0.0;
    // the game's rate against one frame per strobe cycle (what BFI shows), or against the refresh rate without BFI
    const double target = d.bfiOn ? cycles : d.refreshHz;
    const ImVec4 fpsCol = target <= 0.0 ? kGood : d.gameFps >= 0.97 * target ? kGood : d.gameFps >= 0.85 * target ? kWarn : kBad;
    ImGui::TextColored(fpsCol, "%5.1f fps", d.gameFps);
    ImGui::SameLine();
    ImGui::Text("%5.1f ms   %3.0f Hz", d.gameFps > 0.5 ? 1000.0 / d.gameFps : 0.0, d.refreshHz);
    if (d.bfiOn && cycles > 0.0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(%.0f strobe cycles/s)", cycles);
    }
    const bool trouble = d.lost > 0 || d.missed > 0 || d.repeated > 0;
    // this second's counts, then the totals since the last reset in brackets
    ImGui::TextColored(trouble ? kWarn : kGood, "lost %d (%lld)   repeated %d (%lld)   missed %d (%lld)", d.lost, g_totLost.load(),
                       d.repeated, g_totRep.load(), d.missed, g_totMiss.load());
    if (d.paused) { ImGui::SameLine(); ImGui::TextColored(kBad, "  BFI paused"); }
    {
        const double since = g_totSince.load();
        const int el = since > 0.0 && d.now > since ? (int)(d.now - since) : 0;
        ImGui::TextDisabled("(totals for %d:%02d:%02d, Ctrl+Shift+Z resets)", el / 3600, el / 60 % 60, el % 60);
    }
    if (d.relFlashMs > 0.0) ImGui::TextDisabled("release to flash %.1f ms", d.relFlashMs);
    if (graph)
    {
        float v[kHist];
        const unsigned h = g_head.load();
        float mx = 0.0f;
        for (int i = 0; i < kHist; i++) { v[i] = g_ms[(h + i) % kHist]; mx = v[i] > mx ? v[i] : mx; }
        const float tgtMs = target > 0.0 ? (float)(1000.0 / target) : 16.7f;
        const float top = mx > 2.0f * tgtMs ? mx : 2.0f * tgtMs;
        ImGui::TextDisabled("frame time, last %d frames (max %.1f ms)", kHist, mx);
        // the panel's full width (it is sized by the text lines), so the graph and its label aren't cut off
        // (GetContentRegionAvail returns ImVec2: ReShade is built with MSVC, which returns it through a hidden
        // pointer, so it is called that way -- a plain call from MinGW writes through a null pointer)
        typedef ImVec2 *(*AvailMsvc)(ImVec2 *);
        ImVec2 av(0.0f, 0.0f);
        reinterpret_cast<AvailMsvc>(imgui_function_table_instance()->GetContentRegionAvail)(&av);
        float w = av.x;
        if (w < 200.0f * scale) w = 200.0f * scale;
        ImGui::PlotLines("##bfiosdft", v, kHist, 0, nullptr, 0.0f, top, ImVec2(w, 48.0f * scale));
    }
    if (loads)
    {
        const int c = CpuLoad(), g = GpuLoad();
        if (g >= 0) ImGui::Text("CPU %3d%%   GPU %3d%%", c, g);
        else ImGui::Text("CPU %3d%%", c);
    }
    ImGui::End();
}
} // namespace Osd
