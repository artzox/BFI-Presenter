// BFI Presenter -- system checks shared by the DX add-on and the Vulkan layer.
//
// Things outside the game that make the strobe miss refreshes, checked on a helper thread (never on the presenter's):
//   - power: Windows' Power saver plan, the "Best power efficiency" power mode, energy / battery saver, a laptop on
//     battery. The CPU and GPU clock down between frames and the presenter's wake-ups come late.
//   - GPUs: the game renders on one GPU and the monitor hangs off another (Optimus laptops, a monitor plugged into the
//     motherboard): every frame is copied across, on a schedule of its own, and the timing wobbles.
//   - displays: more than one display active. Their refresh clocks are never in step (119.999 against 120.001 Hz), and in
//     borderless or windowed mode Windows composes them all at one rate; only exclusive fullscreen keeps a display apart.
//     Reported only while refreshes are missed: plenty of setups strobe fine with a second screen on.
// Only warnings: nothing is changed. Results are logged when they change, and the overlay shows them.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

struct SysCheckResult
{
    bool valid = false;
    bool powerSaver = false;        // Power saver plan, or the "Best power efficiency" power mode
    bool energySaver = false;       // energy saver / battery saver on
    bool onBattery = false;         // laptop running on battery
    bool gpuMismatch = false;       // the monitor is driven by a different GPU than the game renders on
    char renderGpu[128] = "";
    char displayGpu[128] = "";
    int  displays = 0;              // active displays (targets)
};

class SysCheck
{
public:
    // Called by the layer once it knows the GPU the game renders on.
    void SetRenderGpu(LUID luid, const char *name)
    {
        std::lock_guard<std::mutex> l(m_);
        luid_ = luid; haveLuid_ = true;
        snprintf(renderName_, sizeof(renderName_), "%s", name ? name : "");
    }
    // Helper thread, about every 500 ms: checks at the first call and every 10 s. log(level 0 info / 1 warning, text).
    template <class LogFn> void Tick(HWND gameWindow, LogFn log)
    {
        if (ticks_++ % 20 != 0) return;
        SysCheckResult r;
        r.valid = true;
        CheckPower(r);
        CheckDisplays(gameWindow, r);
        SysCheckResult prev;
        {
            std::lock_guard<std::mutex> l(m_);
            prev = res_;
            res_ = r;
        }
        Report(prev, r, log);
    }
    SysCheckResult Get() const { std::lock_guard<std::mutex> l(m_); return res_; }

    // Overlay lines. always: shown whenever they apply; withFlashes: only while refreshes are being missed.
    static void Lines(const SysCheckResult &r, std::string &always, std::string &withFlashes)
    {
        always.clear(); withFlashes.clear();
        if (!r.valid) return;
        if (r.powerSaver)
            always += "Windows is set to save power (Power saver plan or \"Best power efficiency\" mode): the CPU and GPU clock down "
                      "between frames and flashes come late. Use Balanced or Best performance while playing.\n";
        if (r.energySaver)
            always += "Energy saver (battery saver) is on: Windows throttles the game and the presenter. Turn it off while playing.\n";
        if (r.onBattery)
            always += "The laptop is running on battery: the GPU and CPU are held back and flashes come late. Plug it in.\n";
        if (r.gpuMismatch)
        {
            char b[400];
            snprintf(b, sizeof(b), "The game renders on %s, but this monitor is driven by %s: every frame is copied across on a "
                     "schedule of its own and the strobe's timing wobbles. Connect the monitor to %s's outputs (on a laptop: a "
                     "port wired to it, or the MUX switch / \"dGPU only\" mode).\n",
                     r.renderGpu[0] ? r.renderGpu : "one GPU", r.displayGpu[0] ? r.displayGpu : "another GPU",
                     r.renderGpu[0] ? r.renderGpu : "that GPU");
            always += b;
        }
        if (r.displays > 1)
        {
            char b[400];
            snprintf(b, sizeof(b), "%d displays are active. Their refresh clocks are never exactly in step, and in borderless or "
                     "windowed mode Windows composes them all at one rate. If flashes continue, try exclusive fullscreen or turn the "
                     "other displays off while playing.\n", r.displays);
            withFlashes += b;
        }
        while (!always.empty() && always.back() == '\n') always.pop_back();
        while (!withFlashes.empty() && withFlashes.back() == '\n') withFlashes.pop_back();
    }

private:
    mutable std::mutex m_;
    SysCheckResult res_;
    LUID luid_ = {};
    bool haveLuid_ = false;
    char renderName_[128] = "";
    unsigned ticks_ = 0;

    static bool SameGuid(const GUID &a, const char *s)
    {
        unsigned long d1; unsigned d2, d3, b[8];
        if (sscanf(s, "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
            return false;
        if (a.Data1 != d1 || a.Data2 != d2 || a.Data3 != d3) return false;
        for (int i = 0; i < 8; i++) if (a.Data4[i] != b[i]) return false;
        return true;
    }
    static void CheckPower(SysCheckResult &r)
    {
        SYSTEM_POWER_STATUS ps = {};
        if (GetSystemPowerStatus(&ps))
        {
            const bool hasBattery = ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
            r.onBattery = hasBattery && ps.ACLineStatus == 0;
            r.energySaver = ps.SystemStatusFlag == 1;
        }
        static HMODULE pp = LoadLibraryA("powrprof.dll");
        if (!pp) return;
        typedef DWORD (WINAPI *PFN_Active)(HKEY, GUID **);
        typedef DWORD (WINAPI *PFN_Overlay)(GUID *);
        static PFN_Active active = (PFN_Active)(void *)GetProcAddress(pp, "PowerGetActiveScheme");
        static PFN_Overlay overlay = (PFN_Overlay)(void *)GetProcAddress(pp, "PowerGetEffectiveOverlayScheme");
        GUID *g = nullptr;
        if (active && active(nullptr, &g) == ERROR_SUCCESS && g)
        {
            if (SameGuid(*g, "a1841308-3541-4fab-bc81-f71556f20b4a")) r.powerSaver = true;       // Power saver plan
            LocalFree(g);
        }
        GUID o = {};
        if (overlay && overlay(&o) == ERROR_SUCCESS && SameGuid(o, "961cc777-2547-4f9d-8174-7d86181b8a7a"))
            r.powerSaver = true;                                                                // "Best power efficiency" mode
    }
    void CheckDisplays(HWND wnd, SysCheckResult &r)
    {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return;
        DISPLAYCONFIG_PATH_INFO *paths = new DISPLAYCONFIG_PATH_INFO[np];
        DISPLAYCONFIG_MODE_INFO *modes = new DISPLAYCONFIG_MODE_INFO[nm ? nm : 1];
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, nullptr) == ERROR_SUCCESS)
        {
            r.displays = (int)np;
            char want[CCHDEVICENAME] = "";
            HMONITOR mon = wnd ? MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST) : nullptr;
            MONITORINFOEXA mi; mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoA(mon, &mi)) snprintf(want, sizeof(want), "%s", mi.szDevice);
            LUID rl; bool haveL; char rn[128];
            {
                std::lock_guard<std::mutex> l(m_);
                rl = luid_; haveL = haveLuid_;
                snprintf(rn, sizeof(rn), "%s", renderName_);
            }
            for (UINT32 i = 0; want[0] && haveL && i < np; i++)
            {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME sn = {};
                sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                sn.header.size = sizeof(sn);
                sn.header.adapterId = paths[i].sourceInfo.adapterId;
                sn.header.id = paths[i].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;
                char gdi[64];
                WideCharToMultiByte(CP_UTF8, 0, sn.viewGdiDeviceName, -1, gdi, sizeof(gdi), nullptr, nullptr);
                if (_stricmp(gdi, want) != 0) continue;
                const LUID dl = paths[i].sourceInfo.adapterId;
                if (dl.LowPart != rl.LowPart || dl.HighPart != rl.HighPart)
                {
                    r.gpuMismatch = true;
                    snprintf(r.renderGpu, sizeof(r.renderGpu), "%s", rn);
                    // The display source's entry (\\.\DISPLAYn) names the GPU driving it.
                    DISPLAY_DEVICEA ad = {}; ad.cb = sizeof(ad);
                    for (DWORD k = 0; EnumDisplayDevicesA(nullptr, k, &ad, 0); k++)
                        if (!_stricmp(ad.DeviceName, want)) { snprintf(r.displayGpu, sizeof(r.displayGpu), "%s", ad.DeviceString); break; }
                }
                break;
            }
        }
        delete[] paths;
        delete[] modes;
    }
    template <class LogFn> static void Report(const SysCheckResult &p, const SysCheckResult &r, LogFn log)
    {
        const bool first = !p.valid;
        if (r.powerSaver != p.powerSaver && (r.powerSaver || !first))
            log(r.powerSaver ? 1 : 0, r.powerSaver ? "system check: Windows is set to save power (Power saver plan or \"Best power efficiency\" mode) -- flashes can come late; use Balanced or Best performance"
                                                    : "system check: Windows' power setting no longer saves power");
        if (r.energySaver != p.energySaver && (r.energySaver || !first))
            log(r.energySaver ? 1 : 0, r.energySaver ? "system check: energy saver (battery saver) is on -- Windows throttles the game and the presenter"
                                                      : "system check: energy saver off");
        if (r.onBattery != p.onBattery && (r.onBattery || !first))
            log(r.onBattery ? 1 : 0, r.onBattery ? "system check: running on battery -- the GPU and CPU are held back" : "system check: back on mains power");
        if (r.gpuMismatch != p.gpuMismatch && (r.gpuMismatch || !first))
        {
            char b[400];
            if (r.gpuMismatch)
                snprintf(b, sizeof(b), "system check: the game renders on %s but its monitor is driven by %s -- frames are copied "
                         "across GPUs and the strobe's timing wobbles", r.renderGpu[0] ? r.renderGpu : "one GPU",
                         r.displayGpu[0] ? r.displayGpu : "another GPU");
            else snprintf(b, sizeof(b), "system check: the monitor is now driven by the GPU the game renders on");
            log(r.gpuMismatch ? 1 : 0, b);
        }
        if (r.displays != p.displays && r.displays > 0)
        {
            char b[160];
            snprintf(b, sizeof(b), "system check: %d display%s active%s", r.displays, r.displays == 1 ? "" : "s",
                     r.displays > 1 ? " (their refresh clocks aren't in step; in borderless / windowed mode Windows composes them at one rate)" : "");
            log(0, b);
        }
    }
};
#endif
