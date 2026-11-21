// ============================================================================
// BFI Presenter -- effects replay check (test build, Direct3D 12, ini only: EffectsReplayTest=1)
//
// Copyright (C) 2026 artzox -- https://github.com/artzox/BFI-Presenter
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Can ReShade's effects be rendered again, by the add-on, on a copy of the game's frame taken before them? If so,
// frame warp could move the game's own frame (flat, without the CRT shader's curve, mask and grain) and have the
// effects drawn on the moved picture: the game's HUD-less picture would then match exactly, the motion vectors
// would fit the picture they move, and a CRT mask would stay where it is on filled flashes.
//
// Every 3 s (20 times): the frame is copied at reshade_begin_effects (before the effects); at reshade_present (the
// effects done) the copy is put into a texture of the add-on's own and ReShade's effects are rendered on it there
// (effect_runtime::render_effects). A 512x512 patch from the middle of that, of the frame as shown and of the frame
// before the effects is read back, and the log says how far apart they are. Nothing is shown from it.
// ============================================================================
#pragma once

namespace fxr
{
using namespace reshade::api;

struct State
{
    device       *dev = nullptr;
    effect_runtime *rt = nullptr;
    resource      clean = {}, test = {}, post = {}, rb[3] = {}, bb = {};
    resource_view testRtv = {}, testRtvSrgb = {};
    uint32_t      w = 0, h = 0, pw = 0, ph = 0, bpp = 0;
    format        fmt = format::unknown, viewFmt = format::unknown, viewFmtSrgb = format::unknown;
    int           phase = 0;              // 0 idle, 1 copied before the effects, 2 copied after, 3 read back pending
    bool          guard = false;          // inside our own render_effects call
    double        nextT = 0.0, rbT = 0.0, cpuMs = 0.0;
    int           probes = 0;
    bool          failed = false;
};
static State g;

static void Release()
{
    if (!g.dev) return;
    if (g.testRtv.handle) g.dev->destroy_resource_view(g.testRtv);
    if (g.testRtvSrgb.handle) g.dev->destroy_resource_view(g.testRtvSrgb);
    for (resource r : { g.clean, g.test, g.post, g.rb[0], g.rb[1], g.rb[2] }) if (r.handle) g.dev->destroy_resource(r);
    g = State{};
}

static uint32_t Bpp(format f)
{
    switch ((int)f)
    {
    case 10: return 8;                                     // R16G16B16A16_FLOAT
    case 23: case 24: case 27: case 28: case 29: case 87: case 88: case 90: case 91: return 4;
    default: return 0;
    }
}

// One pixel as 0..1 per channel (float formats: as stored).
static void Unpack(const uint8_t *p, format f, float o[3])
{
    const int v = (int)f;
    if (v == 10)
    {
        const uint16_t *q = (const uint16_t *)p;
        for (int c = 0; c < 3; c++)
        {
            const uint16_t x = q[c];
            const int e = (x >> 10) & 31, m = x & 1023;
            float r = e == 0 ? m / 16777216.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * std::ldexp(1.0f, e - 15);
            o[c] = (x & 0x8000) ? -r : r;
        }
        return;
    }
    const uint32_t x = *(const uint32_t *)p;
    if (v == 23 || v == 24) { o[0] = (x & 1023) / 1023.0f; o[1] = ((x >> 10) & 1023) / 1023.0f; o[2] = ((x >> 20) & 1023) / 1023.0f; return; }
    const bool bgr = v == 87 || v == 88 || v == 90 || v == 91;
    const float a = (x & 255) / 255.0f, b = ((x >> 8) & 255) / 255.0f, c = ((x >> 16) & 255) / 255.0f;
    o[0] = bgr ? c : a; o[1] = b; o[2] = bgr ? a : c;
}

// The typed formats for a back buffer format: the view without and with sRGB (unknown = none).
static void ViewFormats(int f, format &vf, format &vfs)
{
    vfs = format::unknown;
    switch (f)
    {
    case 23: vf = (format)24; break;                                  // R10G10B10A2
    case 9:  vf = (format)10; break;                                  // R16G16B16A16_FLOAT
    case 27: case 28: case 29: vf = (format)28; vfs = (format)29; break;   // R8G8B8A8
    case 87: case 90: case 91: vf = (format)87; vfs = (format)91; break;   // B8G8R8A8
    default: vf = (format)f; break;
    }
}

// The ReShade calls that return a description (get_resource_desc, get_resource_view_desc) return a structure, which
// MSVC (ReShade) and MinGW (this add-on) pass differently: ReShade crashed reading from the stack (fx1, 007 First
// Light). The back buffer comes from the runtime and its description from D3D12 itself.
static bool Ensure(resource bb, resource_view rtvSrgb)
{
    const D3D12_RESOURCE_DESC d = reinterpret_cast<ID3D12Resource *>(bb.handle)->GetDesc();
    format vf, vfs;
    ViewFormats((int)d.Format, vf, vfs);
    if (!rtvSrgb.handle) vfs = format::unknown;
    if (g.clean.handle && (uint32_t)d.Width == g.w && d.Height == g.h && (format)d.Format == g.fmt) return true;
    device *dv = g.dev; effect_runtime *rt = g.rt; const int probes = g.probes; const double nextT = g.nextT;
    Release();
    g.dev = dv; g.rt = rt; g.probes = probes; g.nextT = nextT;
    g.w = (uint32_t)d.Width; g.h = d.Height; g.fmt = (format)d.Format; g.viewFmt = vf; g.viewFmtSrgb = vfs;
    g.bpp = Bpp(vf);
    if (!g.bpp) { LOGW("BFI Presenter: effects replay check: frame format %d not handled -- check off", (int)vf); return false; }
    g.pw = std::min(512u, g.w); g.ph = std::min(512u, g.h);
    const resource_usage tu = resource_usage::render_target | resource_usage::shader_resource | resource_usage::copy_dest | resource_usage::copy_source;
    bool ok = g.dev->create_resource(resource_desc(g.w, g.h, 1, 1, g.fmt, 1, memory_heap::gpu_only, tu), nullptr, resource_usage::copy_dest, &g.clean) &&
              g.dev->create_resource(resource_desc(g.w, g.h, 1, 1, g.fmt, 1, memory_heap::gpu_only, tu), nullptr, resource_usage::copy_dest, &g.post) &&
              g.dev->create_resource(resource_desc(g.w, g.h, 1, 1, g.fmt, 1, memory_heap::gpu_only, tu), nullptr, resource_usage::render_target, &g.test) &&
              g.dev->create_resource_view(g.test, resource_usage::render_target, resource_view_desc(vf), &g.testRtv);
    if (ok && vfs != format::unknown) ok = g.dev->create_resource_view(g.test, resource_usage::render_target, resource_view_desc(vfs), &g.testRtvSrgb);
    for (int i = 0; ok && i < 3; i++)
        ok = g.dev->create_resource(resource_desc((uint64_t)g.pw * g.ph * g.bpp, memory_heap::gpu_to_cpu, resource_usage::copy_dest), nullptr,
                                    resource_usage::copy_dest, &g.rb[i]);
    if (!ok) { LOGW("BFI Presenter: effects replay check: its textures could not be made -- check off"); Release(); return false; }
    LOGI("BFI Presenter: effects replay check: on (%ux%u, format %d, view %d%s), every 3 s", g.w, g.h, (int)g.fmt, (int)vf,
         vfs != format::unknown ? ", with an sRGB view" : "");
    return true;
}

// effect_runtime::get_back_buffer, called as MSVC compiles it. MSVC returns a structure from a class method -- even an
// 8-byte handle like resource -- through a hidden pointer after `this`; MinGW returns one that small in RAX. Called the
// MinGW way, ReShade wrote the handle to the address in the index's place (fx2: "writing address 0x3"). Slot 5 of the
// vtable: get_native, get_private_data, set_private_data (api_object), get_device (device_object), get_hwnd, get_back_buffer.
static resource BackBuffer(effect_runtime *rt, uint32_t index)
{
    typedef resource *(*Fn)(effect_runtime *self, resource *ret, uint32_t index);
    resource r = {};
    void **vt = *reinterpret_cast<void ***>(rt);
    reinterpret_cast<Fn>(vt[5])(rt, &r, index);
    return r;
}

static bool Mine(effect_runtime *rt)
{
#ifndef _WIN64
    return false;                                          // the MSVC call above is written for 64-bit only
#endif
    if (!g_shared.fxReplayTest || g.failed || g.probes >= 20) return false;
    if (!g_sc || g_dx11.load() || reinterpret_cast<void *>(rt->get_native()) != (void *)g_sc) return false;
    if (rt->get_device()->get_api() != device_api::d3d12) return false;
    return true;
}

static void on_begin(effect_runtime *rt, command_list *cmd, resource_view rtv, resource_view rtvSrgb)
{
    if (g.guard || !Mine(rt) || g.phase != 0 || Now() < g.nextT) return;
    g.dev = rt->get_device(); g.rt = rt;
    (void)rtv;
    const resource bb = BackBuffer(rt, rt->get_current_back_buffer_index());
    if (!bb.handle || !Ensure(bb, rtvSrgb)) { g.failed = true; return; }
    g.bb = bb;
    cmd->barrier(bb, resource_usage::render_target, resource_usage::copy_source);
    cmd->copy_resource(bb, g.clean);
    cmd->barrier(bb, resource_usage::copy_source, resource_usage::render_target);
    g.phase = 1;
}

static void on_finish(effect_runtime *rt, command_list *cmd, resource_view, resource_view)
{
    if (g.guard || g.phase != 1 || rt != g.rt) return;
    const resource bb = g.bb;
    cmd->barrier(bb, resource_usage::render_target, resource_usage::copy_source);
    cmd->copy_resource(bb, g.post);
    cmd->barrier(bb, resource_usage::copy_source, resource_usage::render_target);
    g.phase = 2;
}

static void Report()
{
    const uint8_t *p[3] = {};
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++) ok = g.dev->map_buffer_region(g.rb[i], 0, UINT64_MAX, map_access::read_only, (void **)&p[i]);
    if (ok)
    {
        // rb 0: the effects rendered again on the copy, 1: the frame as shown, 2: the frame before the effects
        double sumR = 0.0, maxR = 0.0, sumE = 0.0; uint64_t offR = 0;
        const uint64_t n = (uint64_t)g.pw * g.ph;
        for (uint64_t i = 0; i < n; i++)
        {
            float a[3], b[3], c[3];
            Unpack(p[0] + i * g.bpp, g.viewFmt, a); Unpack(p[1] + i * g.bpp, g.viewFmt, b); Unpack(p[2] + i * g.bpp, g.viewFmt, c);
            float dr = 0.0f, de = 0.0f;
            for (int k = 0; k < 3; k++) { dr = std::max(dr, std::fabs(a[k] - b[k])); de = std::max(de, std::fabs(c[k] - b[k])); }
            sumR += dr; sumE += de; if (dr > maxR) maxR = dr; if (dr > 2.5f / 255.0f) offR++;
        }
        g.probes++;
        LOGI("BFI Presenter: effects replay check %d: effects rendered again on the frame from before them, against the frame shown "
             "(%ux%u from the middle): mean %.4f, max %.4f, %.2f%% of pixels off by more than 2/255; the effects themselves change it by "
             "%.4f on average; render_effects call %.2f ms on the CPU",
             g.probes, g.pw, g.ph, sumR / n, maxR, 100.0 * offR / n, sumE / n, g.cpuMs);
    }
    else LOGW("BFI Presenter: effects replay check: reading back failed");
    for (int i = 0; i < 3; i++) if (p[i]) g.dev->unmap_buffer_region(g.rb[i]);
    if (!ok) g.failed = true;
}

static void on_present(effect_runtime *rt)
{
    if (rt != g.rt) return;
    if (g.phase == 3 && Now() - g.rbT > 0.5) { Report(); g.phase = 0; g.nextT = Now() + 3.0; return; }
    if (g.phase != 2) return;
    command_queue *q = rt->get_command_queue();
    command_list *cmd = q->get_immediate_command_list();
    cmd->barrier(g.test, resource_usage::render_target, resource_usage::copy_dest);
    cmd->barrier(g.clean, resource_usage::copy_dest, resource_usage::copy_source);
    cmd->copy_resource(g.clean, g.test);
    cmd->barrier(g.test, resource_usage::copy_dest, resource_usage::render_target);
    const double t0 = Now();
    g.guard = true;
    rt->render_effects(cmd, g.testRtv, g.testRtvSrgb);
    g.guard = false;
    g.cpuMs = (Now() - t0) * 1000.0;
    // the patches from the middle
    subresource_box box = {};
    box.left = (g.w - g.pw) / 2; box.top = (g.h - g.ph) / 2; box.front = 0;
    box.right = box.left + g.pw; box.bottom = box.top + g.ph; box.back = 1;
    cmd->barrier(g.test, resource_usage::render_target, resource_usage::copy_source);
    cmd->barrier(g.post, resource_usage::copy_dest, resource_usage::copy_source);
    cmd->copy_texture_to_buffer(g.test, 0, &box, g.rb[0], 0, g.pw, g.ph);
    cmd->copy_texture_to_buffer(g.post, 0, &box, g.rb[1], 0, g.pw, g.ph);
    cmd->copy_texture_to_buffer(g.clean, 0, &box, g.rb[2], 0, g.pw, g.ph);
    cmd->barrier(g.test, resource_usage::copy_source, resource_usage::render_target);
    cmd->barrier(g.post, resource_usage::copy_source, resource_usage::copy_dest);
    cmd->barrier(g.clean, resource_usage::copy_source, resource_usage::copy_dest);
    q->flush_immediate_command_list();
    g.phase = 3; g.rbT = Now();
    if (g.probes == 0) LOGI("BFI Presenter: effects replay check: effects rendered on the add-on's copy (%.2f ms on the CPU); reading back", g.cpuMs);
}

static void on_destroy_rt(effect_runtime *rt) { if (rt == g.rt) { g.dev = rt->get_device(); Release(); } }

static void Register(bool on)
{
    if (on)
    {
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish);
        reshade::register_event<reshade::addon_event::reshade_present>(on_present);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_rt);
    }
    else
    {
        reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin);
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(on_finish);
        reshade::unregister_event<reshade::addon_event::reshade_present>(on_present);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_rt);
    }
}
}   // namespace fxr
