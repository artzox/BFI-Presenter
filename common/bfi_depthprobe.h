// Depth probe (test): which depth buffer the game draws into, through ReShade's add-on events.
// Logs, every 10 s, the depth buffers that got draws, their size and format against the back buffer,
// their share of the frame's draws, how they are cleared (the clear value tells standard from reversed
// depth) and, for render passes, whether the depth is kept after the pass. Nothing is changed.
// Used by the DX add-on and the Vulkan layer; each sets dp::g_log before registering the events.
#pragma once
#include <reshade.hpp>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <windows.h>

namespace dp
{
using namespace reshade::api;

inline void (*g_log)(const char *msg) = nullptr;

struct Buf
{
    uint32_t w = 0, h = 0, samples = 1;
    format fmt = format::unknown;
    uint64_t drawsFrame = 0, drawsWin = 0;
    uint32_t framesUsed = 0;
    uint32_t clears = 0, clearsZero = 0, clearsOne = 0, clearsOther = 0;
    uint32_t passes = 0, passesDiscard = 0, passesLoad = 0;
};

struct State
{
    std::mutex lock;
    std::unordered_map<uint64_t, uint64_t> viewRes;      // view -> resource
    std::unordered_map<command_list *, uint64_t> cur;    // command list -> bound depth resource
    std::unordered_map<uint64_t, Buf> bufs;
    uint64_t drawsFrame = 0, drawsWin = 0, drawsNoDepthWin = 0;
    uint32_t frames = 0;
    uint32_t bbW = 0, bbH = 0;
    double winStart = 0.0;
    int reports = 0;
};
inline State g;

inline double Now()
{
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

inline void Log(const char *fmt, ...)
{
    if (!g_log) return;
    char b[1400];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    g_log(b);
}

inline const char *FmtName(format f)
{
    switch (f)
    {
    case format::d16_unorm: return "D16";
    case format::d16_unorm_s8_uint: return "D16S8";
    case format::d24_unorm_x8_uint: return "D24X8";
    case format::d24_unorm_s8_uint: return "D24S8";
    case format::d32_float: return "D32F";
    case format::d32_float_s8_uint: return "D32FS8";
    case format::r16_typeless: return "R16 typeless";
    case format::r24_g8_typeless: return "R24G8 typeless";
    case format::r32_typeless: return "R32 typeless";
    case format::r32_g8_typeless: return "R32G8 typeless";
    case format::r32_float: return "R32F";
    case format::intz: return "INTZ";
    default: return "other";
    }
}

// Resource behind a depth view, looked up once per view (the lock is held).
inline uint64_t ResOf(command_list *cmd, resource_view v)
{
    if (!v.handle) return 0;
    auto it = g.viewRes.find(v.handle);
    if (it != g.viewRes.end()) return it->second;
    device *d = cmd ? cmd->get_device() : nullptr;
    if (!d) return 0;
    const resource r = d->get_resource_from_view(v);
    if (!r.handle) return 0;
    if (g.viewRes.size() > 4096) g.viewRes.clear();
    g.viewRes[v.handle] = r.handle;
    Buf &b = g.bufs[r.handle];
    if (!b.w)
    {
        const resource_desc rd = d->get_resource_desc(r);
        b.w = rd.texture.width; b.h = rd.texture.height; b.fmt = rd.texture.format; b.samples = rd.texture.samples;
    }
    return r.handle;
}

inline void on_bind(command_list *cmd, uint32_t, const resource_view *, resource_view dsv)
{
    std::lock_guard<std::mutex> l(g.lock);
    g.cur[cmd] = ResOf(cmd, dsv);
}

inline void on_begin_pass(command_list *cmd, uint32_t, const render_pass_render_target_desc *, const render_pass_depth_stencil_desc *ds)
{
    std::lock_guard<std::mutex> l(g.lock);
    const uint64_t r = ds ? ResOf(cmd, ds->view) : 0;
    g.cur[cmd] = r;
    if (!r) return;
    Buf &b = g.bufs[r];
    b.passes++;
    if (ds->depth_store_op == render_pass_store_op::discard) b.passesDiscard++;
    if (ds->depth_load_op == render_pass_load_op::load) b.passesLoad++;
    if (ds->depth_load_op == render_pass_load_op::clear)
    {
        b.clears++;
        if (ds->clear_depth == 0.0f) b.clearsZero++; else if (ds->clear_depth == 1.0f) b.clearsOne++; else b.clearsOther++;
    }
}

inline void on_end_pass(command_list *cmd)
{
    std::lock_guard<std::mutex> l(g.lock);
    g.cur[cmd] = 0;
}

// Clears are matched to buffers by their view only, with no calls into ReShade's device: in South of Midnight
// (DX11, dp1) looking up the resource of a cleared view crashed inside ReShade. Views seen while binding are
// already mapped to their buffer; a clear of a view never bound for drawing isn't counted.
inline bool on_clear(command_list *, resource_view dsv, const float *depth, const uint8_t *, uint32_t, const rect *)
{
    if (!dsv.handle || !depth) return false;
    std::lock_guard<std::mutex> l(g.lock);
    auto it = g.viewRes.find(dsv.handle);
    if (it == g.viewRes.end()) return false;
    Buf &b = g.bufs[it->second];
    b.clears++;
    if (*depth == 0.0f) b.clearsZero++; else if (*depth == 1.0f) b.clearsOne++; else b.clearsOther++;
    return false;
}

inline void Count(command_list *cmd, uint32_t n)
{
    std::lock_guard<std::mutex> l(g.lock);
    auto it = g.cur.find(cmd);
    const uint64_t r = it != g.cur.end() ? it->second : 0;
    g.drawsFrame += n;
    if (!r) { g.drawsNoDepthWin += n; return; }
    g.bufs[r].drawsFrame += n;
}

inline bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t) { Count(cmd, 1); return false; }
inline bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { Count(cmd, 1); return false; }
inline bool on_draw_indirect(command_list *cmd, indirect_command type, resource, uint64_t, uint32_t n, uint32_t)
{
    if (type == indirect_command::draw || type == indirect_command::draw_indexed || type == indirect_command::unknown) Count(cmd, n ? n : 1);
    return false;
}
inline void on_reset(command_list *cmd)
{
    std::lock_guard<std::mutex> l(g.lock);
    g.cur[cmd] = 0;
}

inline void on_present(command_queue *, swapchain *sc, const rect *, const rect *, uint32_t, const rect *)
{
    std::lock_guard<std::mutex> l(g.lock);
    if (sc)
    {
        if (device *d = sc->get_device())
        {
            const resource bb = sc->get_current_back_buffer();
            if (bb.handle) { const resource_desc rd = d->get_resource_desc(bb); g.bbW = rd.texture.width; g.bbH = rd.texture.height; }
        }
    }
    g.frames++;
    g.drawsWin += g.drawsFrame; g.drawsFrame = 0;
    for (auto &kv : g.bufs)
    {
        Buf &b = kv.second;
        if (b.drawsFrame) { b.framesUsed++; b.drawsWin += b.drawsFrame; b.drawsFrame = 0; }
    }
    const double t = Now();
    if (g.winStart <= 0.0) { g.winStart = t; return; }
    if (t - g.winStart < 10.0) return;

    std::vector<std::pair<uint64_t, Buf *>> v;
    for (auto &kv : g.bufs) if (kv.second.drawsWin) v.push_back({ kv.first, &kv.second });
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second->drawsWin > b.second->drawsWin; });
    const double secs = t - g.winStart;
    const double all = g.drawsWin ? (double)g.drawsWin : 1.0;
    if (g.reports < 60)
    {
        g.reports++;
        if (v.empty())
            Log("BFI Presenter: depth probe: no draws into a depth buffer in the last %.0f s (%.0f presents a second, %.0f draws a second%s) -- no depth to use",
                secs, g.frames / secs, g.drawsWin / secs, g.drawsWin ? "" : "; ReShade reported no draws");
        else
        {
            int full = 0;
            for (auto &e : v) if (e.second->w * 2 >= g.bbW && e.second->h * 2 >= g.bbH && e.second->drawsWin / secs >= 300.0) full++;
            Log("BFI Presenter: depth probe: %d depth buffers drawn into in the last %.0f s (%.0f presents a second, back buffer %ux%u, %.0f draws a "
                "second, %.0f%% of them with a depth buffer); %d at least half the back buffer's size with 300+ draws a second",
                (int)v.size(), secs, g.frames / secs, g.bbW, g.bbH, g.drawsWin / secs, 100.0 * (1.0 - g.drawsNoDepthWin / all), full);
            for (size_t i = 0; i < v.size() && i < 3; i++)
            {
                const Buf &b = *v[i].second;
                char cl[160];
                if (b.clears)
                    snprintf(cl, sizeof(cl), "cleared %.0f times a second to %s", b.clears / secs,
                             b.clearsZero > b.clearsOne ? "0 (reversed depth: near = 1)" : b.clearsOne ? "1 (standard depth: near = 0)" : "another value");
                else snprintf(cl, sizeof(cl), "not cleared through ReShade");
                char rp[160] = "";
                if (b.passes)
                    snprintf(rp, sizeof(rp), "; %u render passes, depth discarded after %u (%.0f%%), loaded in %u", b.passes, b.passesDiscard,
                             100.0 * b.passesDiscard / b.passes, b.passesLoad);
                Log("BFI Presenter: depth probe:   #%d %ux%u %s%s, %.0f draws a second (%.0f%% of all), drawn into between %u of %u presents, %s%s",
                    (int)i + 1, b.w, b.h, FmtName(b.fmt), b.samples > 1 ? " MSAA" : "", b.drawsWin / secs, 100.0 * b.drawsWin / all,
                    b.framesUsed, g.frames, cl, rp);
            }
        }
    }
    // A new window: counts start again, the buffers' sizes stay known.
    g.frames = 0; g.drawsWin = g.drawsNoDepthWin = 0; g.winStart = t;
    for (auto it = g.bufs.begin(); it != g.bufs.end();)
    {
        Buf &b = it->second;
        if (!b.drawsWin && !b.clears && !b.passes) { it = g.bufs.erase(it); continue; }
        b.drawsWin = 0; b.framesUsed = 0; b.clears = b.clearsZero = b.clearsOne = b.clearsOther = 0; b.passes = b.passesDiscard = b.passesLoad = 0;
        ++it;
    }
}

// Registration through ReShade's exports, so it works from the add-on and from the Vulkan layer.
template <class Reg>
inline void Register(Reg reg)
{
    reg(reshade::addon_event::bind_render_targets_and_depth_stencil, (void *)&on_bind);
    reg(reshade::addon_event::begin_render_pass, (void *)&on_begin_pass);
    reg(reshade::addon_event::end_render_pass, (void *)&on_end_pass);
    reg(reshade::addon_event::clear_depth_stencil_view, (void *)&on_clear);
    reg(reshade::addon_event::draw, (void *)&on_draw);
    reg(reshade::addon_event::draw_indexed, (void *)&on_draw_indexed);
    reg(reshade::addon_event::draw_or_dispatch_indirect, (void *)&on_draw_indirect);
    reg(reshade::addon_event::reset_command_list, (void *)&on_reset);
    reg(reshade::addon_event::present, (void *)&on_present);
}
} // namespace dp
