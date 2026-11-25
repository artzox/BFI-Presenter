// ============================================================================
// BFI Presenter -- draw ahead (test build, Direct3D 12 64-bit; FrameWarpDrawAhead): ReShade's effects drawn after frame
// warp's move instead of before it
//
// Copyright (C) 2026 artzox -- https://github.com/artzox/BFI-Presenter
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frame warp moves the finished frame -- ReShade's effects included: a CRT shader's mask and scanlines move with the
// picture on a moved flash, its curve is moved by vectors made for the flat frame, and its grain and mask break the
// "unchanged from the frame before" test that keeps a HUD still. The effects replay check (dx/bfi_fxreplay.h, fx4)
// showed ReShade draws the effects, technique by technique, on a copy of the frame taken before them exactly as on the
// frame itself (0 difference in 20 checks, 007 First Light with CRT-Standalone).
//
// So, at the game's present, on the game's thread (where ReShade can be called):
//   1. reshade_begin_effects: the frame is copied before the effects (and the one before it is kept).
//   2. In Present, after the frame's motion texture is made (gmo::AtPresent): the copy is drawn moved by the game's
//      motion to the moves the presenter will most likely show it with (fxa::predFirst / predRepeat: its first flash
//      with even pacing, and a repeat), with the same shader as a moved flash (PSMoveOnly).
//   3. ReShade's enabled techniques are drawn on each (effect_runtime::render_technique, on ReShade's immediate command
//      list on the game's queue), before the frame is published.
// The presenter shows one of those, as rendered, on a moved flash whose move is within 0.02 frames of it; otherwise it
// moves the finished frame as before. The log counts both every 10 s.
// ============================================================================
#pragma once

namespace fxa
{
using namespace reshade::api;

struct Gpu
{
    effect_runtime       *rt = nullptr;
    device               *dev = nullptr;
    ID3D12Resource       *clean[2] = {};          // the frame before the effects, this one and the one before (ping-pong)
    int                   cleanCur = -1;          // which of them holds this frame's (set at reshade_begin_effects)
    bool                  capturedNow = false;    // this frame's was copied (since the last Present)
    UINT64                capturedN = 0, presentN = 0;   // which Present each was copied for
    UINT64                cleanN[2] = {};
    ID3D12RootSignature  *rootSig = nullptr;
    ID3D12PipelineState  *pso = nullptr;
    ID3D12DescriptorHeap *srvHeap = nullptr;      // 8 sets of 5 (t0-t4), one per Present in turn
    ID3D12DescriptorHeap *rtvHeap = nullptr;      // one per frame drawn ahead
    UINT                  srvStride = 0, rtvStride = 0;
    resource_view         apiRtv[kVar] = {};      // ReShade's views of them (render_technique)
    UINT                  w = 0, h = 0;
    DXGI_FORMAT           bbFmt = DXGI_FORMAT_UNKNOWN, fmt = DXGI_FORMAT_UNKNOWN;
    int                   pendN = 0, pendVar[3] = {};
    float                 pendAhead[3] = {};
    UINT64                waitRead = 0;           // the presenter's last read of the textures about to be drawn into
    bool                  failed = false, logged = false;
};
static Gpu G;

// The frame from before the effects is also wanted without draw ahead: frame warp finds what stands still in it
// (FrameWarpStillBeforeEffects, 1.0.48 test).
static bool Capturing()
{
#ifndef _WIN64
    return false;
#else
    return g_shared.frameWarp && ((g_shared.warpDrawAhead && g_shared.warpGame) || g_shared.warpStillPre) && !G.failed && !g_dx11.load() && g_sc;
#endif
}
static bool Enabled()
{
#ifndef _WIN64
    return false;
#else
    return g_shared.frameWarp && g_shared.warpDrawAhead && g_shared.warpGame && !G.failed && !g_dx11.load() && g_sc;
#endif
}

static void ReleaseTextures()
{
    if (G.dev)
        for (int v = 0; v < kVar; v++) if (G.apiRtv[v].handle) { G.dev->destroy_resource_view(G.apiRtv[v]); G.apiRtv[v] = {}; }
    EnterCriticalSection(&g_lock);
    for (int v = 0; v < kVar; v++) { SafeRelease(var[v].res); var[v].frameId = 0; var[v].readFence = 0; }
    LeaveCriticalSection(&g_lock);
    varGen++;
    for (int i = 0; i < 2; i++) SafeRelease(G.clean[i]);
    G.cleanCur = -1; G.cleanN[0] = G.cleanN[1] = 0; G.w = G.h = 0;
}

// Everything off and released (the swap chain or ReShade's runtime going away). The GPU must be done with them.
static void Release()
{
    if (g_gameFence && g_gameFenceVal) WaitFence(g_gameFence, g_gameFenceVal, g_gameFenceEvent);
    UINT64 rf = 0;
    for (int v = 0; v < kVar; v++) rf = std::max(rf, var[v].readFence);
    if (g_presFence && rf) WaitFence(g_presFence, rf, nullptr);
    ReleaseTextures();
    SafeRelease(G.pso); SafeRelease(G.rootSig); SafeRelease(G.srvHeap); SafeRelease(G.rtvHeap);
    G.rt = nullptr; G.dev = nullptr; G.pendN = 0; G.capturedNow = false;
}

static ID3D12Resource *Tex(DXGI_FORMAT f, bool rt, const wchar_t *name)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = G.w; rd.Height = G.h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = f; rd.SampleDesc.Count = 1;
    rd.Flags = rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    D3D12_CLEAR_VALUE cv = {}; cv.Format = G.fmt;
    ID3D12Resource *r = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, rt ? &cv : nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    r->SetName(name);
    return r;
}

// The pipeline: the presenter's layout (b0 constants, t0-t4, s0 linear clamp) and its shaders, drawing PSMoveOnly.
static bool EnsurePipeline()
{
    if (G.pso) return true;
    if (!g_shDone || !g_shBlob[0] || !g_shBlob[kShMoveOnly]) return false;
    D3D12_DESCRIPTOR_RANGE rg[5] = {};
    D3D12_ROOT_PARAMETER p[6] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.ShaderRegister = 0;
    p[0].Constants.Num32BitValues = sizeof(Constants) / 4;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (int i = 0; i < 5; i++)
    {
        rg[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg[i].NumDescriptors = 1; rg[i].BaseShaderRegister = (UINT)i;
        p[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        p[1 + i].DescriptorTable.pDescriptorRanges = &rg[i];
        p[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 6; rsd.pParameters = p; rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &smp;
    ID3DBlob *blob = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) &&
              SUCCEEDED(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&G.rootSig)));
    SafeRelease(blob); SafeRelease(err);
    if (ok)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = G.rootSig;
        pd.VS = { g_shBlob[0]->GetBufferPointer(), g_shBlob[0]->GetBufferSize() };
        pd.PS = { g_shBlob[kShMoveOnly]->GetBufferPointer(), g_shBlob[kShMoveOnly]->GetBufferSize() };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = G.fmt;
        pd.SampleDesc.Count = 1;
        ok = SUCCEEDED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&G.pso)));
    }
    if (ok)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 8 * 5; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ok = SUCCEEDED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&G.srvHeap)));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kVar; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ok = ok && SUCCEEDED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&G.rtvHeap)));
        G.srvStride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        G.rtvStride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    if (!ok)
    {
        LOGW("BFI Presenter: draw ahead: its pipeline could not be made -- off for this session");
        SafeRelease(G.pso); SafeRelease(G.rootSig); SafeRelease(G.srvHeap); SafeRelease(G.rtvHeap);
        G.failed = true;
    }
    return ok;
}

// The textures for the frame's size and format: the two copies before the effects and the frames drawn ahead.
static bool EnsureTextures(ID3D12Resource *bb)
{
    const D3D12_RESOURCE_DESC d = bb->GetDesc();
    const bool wantVar = Enabled();
    if (G.clean[0] && (UINT)d.Width == G.w && d.Height == G.h && d.Format == G.bbFmt && (!wantVar || var[0].res)) return true;
    if (G.clean[0])                                       // the size changed: the GPU must be done with the old ones
    {
        if (g_gameFenceVal) WaitFence(g_gameFence, g_gameFenceVal, g_gameFenceEvent);
        UINT64 rf = 0;
        for (int v = 0; v < kVar; v++) rf = std::max(rf, var[v].readFence);
        if (rf) WaitFence(g_presFence, rf, nullptr);
    }
    ReleaseTextures();
    G.w = (UINT)d.Width; G.h = d.Height; G.bbFmt = d.Format; G.fmt = StageFormat();
    bool ok = (G.clean[0] = Tex(G.bbFmt, false, L"BFI draw ahead: frame before the effects")) &&
              (G.clean[1] = Tex(G.bbFmt, false, L"BFI draw ahead: frame before the effects"));
    for (int v = 0; ok && wantVar && v < kVar; v++)
    {
        ID3D12Resource *r = Tex(G.fmt, true, L"BFI draw ahead: frame drawn ahead");
        ok = r != nullptr;
        if (!ok) break;
        D3D12_CPU_DESCRIPTOR_HANDLE h = G.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)v * G.rtvStride;
        D3D12_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = G.fmt; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_device->CreateRenderTargetView(r, &rv, h);
        ok = G.dev->create_resource_view(resource{ (uint64_t)(uintptr_t)r }, resource_usage::render_target,
                                         resource_view_desc((format)G.fmt), &G.apiRtv[v]);
        EnterCriticalSection(&g_lock);
        var[v].res = r; var[v].frameId = 0; var[v].readFence = 0;
        LeaveCriticalSection(&g_lock);
    }
    varGen++;
    if (!ok)
    {
        LOGW("BFI Presenter: draw ahead: its textures could not be made -- off for this session");
        ReleaseTextures();
        G.failed = true;
        return false;
    }
    if (wantVar) LOGI("BFI Presenter: draw ahead: on (%ux%u, format %d) -- ReShade's effects drawn on the moved frames", G.w, G.h, (int)G.fmt);
    return true;
}

// reshade_begin_effects: this frame, before the effects.
static void on_begin(effect_runtime *rt, command_list *cmd, resource_view, resource_view)
{
    if (!Capturing() || reinterpret_cast<void *>(rt->get_native()) != (void *)g_sc) return;
    if (fxr::g.guard) return;
    G.rt = rt; G.dev = rt->get_device();
    if (Enabled() && !EnsurePipeline()) return;
    const resource bbr = fxr::BackBuffer(rt, rt->get_current_back_buffer_index());
    ID3D12Resource *bb = reinterpret_cast<ID3D12Resource *>(bbr.handle);
    if (!bb || !EnsureTextures(bb)) return;
    const int i = G.cleanCur < 0 ? 0 : 1 - G.cleanCur;
    ID3D12GraphicsCommandList *nl = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    D3D12_RESOURCE_BARRIER b[2] = { Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                    Transition(G.clean[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
    nl->ResourceBarrier(2, b);
    nl->CopyResource(G.clean[i], bb);
    b[0] = Transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    b[1] = Transition(G.clean[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    nl->ResourceBarrier(2, b);
    G.cleanCur = i; G.cleanN[i] = G.presentN + 1; G.capturedNow = true;
}

// Present (game's thread), right after gmo::AtPresent on the frame copy list: the moved frames, drawn into `list`.
static void Record(ID3D12GraphicsCommandList *list, UINT s)
{
    G.pendN = 0; G.waitRead = 0;
    const bool cap = G.capturedNow;
    G.capturedNow = false;
    G.presentN++;
    if (!cap || !Enabled() || !G.pso || !G.clean[0] || g_status.fgActive) return;
    // Not while ReShade's window or the OSD is shown: they are drawn on the frame after the effects, so frames drawn
    // ahead (from before them) lack them, and they flickered (da2, Oblivion). Not without an effect on either: the frame
    // moved as before is the same picture (da2: Oblivion drew them with no technique on).
    if (overlayOpen.load() || g_shared.osd || !G.rt) return;
    {
        int on = 0;
        G.rt->enumerate_techniques(nullptr, [&](effect_runtime *r, effect_technique t) { if (r->get_technique_state(t)) on++; });
        if (!on) return;
    }
    const int cur = G.cleanCur, prev = 1 - cur;
    if (G.cleanN[cur] != G.presentN || G.cleanN[prev] + 1 != G.presentN) return;   // this frame's and the one just before
    const int gsrc = gmo::PendingFor(s);
    ID3D12Resource *fl = gmo::FlowRes((int)s);
    if (!gsrc || !fl) return;
    float want[3]; int n = 0;
    for (const float f : { predFirst.load(), predLate.load(), predRepeat.load() })
    {
        bool dup = f <= 0.004f;
        for (int j = 0; j < n && !dup; j++) if (std::fabs(f - want[j]) <= 0.004f) dup = true;
        if (!dup) want[n++] = f;
    }
    if (!n) return;
    // The oldest frames drawn ahead are drawn over (not selectable from here until published again).
    EnterCriticalSection(&g_lock);
    for (int k = 0; k < n; k++)
    {
        int best = -1;
        for (int v = 0; v < kVar; v++)
        {
            bool taken = false;
            for (int j = 0; j < k; j++) if (G.pendVar[j] == v) taken = true;
            if (!taken && var[v].res && (best < 0 || var[v].frameId < var[best].frameId)) best = v;
        }
        if (best < 0) break;
        G.pendVar[k] = best; G.pendAhead[k] = want[k];
        var[best].frameId = 0;
        G.waitRead = std::max(G.waitRead, var[best].readFence);
        G.pendN = k + 1;
    }
    LeaveCriticalSection(&g_lock);
    if (!G.pendN) return;

    // Views: t0 this frame, t3 its motion, t4 the frame before; t1, t2 unused (this frame). One set per Present in turn
    // (a set in use by a list still on the GPU is never written).
    const UINT set = (UINT)(G.presentN % 8) * 5;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = G.fmt; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC fv = sv; fv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D12Resource *srcs[5] = { G.clean[cur], G.clean[cur], G.clean[cur], fl, G.clean[prev] };
    for (UINT t = 0; t < 5; t++)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE hc = G.srvHeap->GetCPUDescriptorHandleForHeapStart();
        hc.ptr += (SIZE_T)(set + t) * G.srvStride;
        g_device->CreateShaderResourceView(srcs[t], t == 3 ? &fv : &sv, hc);
    }
    D3D12_RESOURCE_BARRIER b[3 + kVar];
    UINT nb = 0;
    b[nb++] = Transition(G.clean[cur], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    b[nb++] = Transition(G.clean[prev], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    b[nb++] = Transition(fl, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (int k = 0; k < G.pendN; k++)
        b[nb++] = Transition(var[G.pendVar[k]].res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(nb, b);
    list->SetGraphicsRootSignature(G.rootSig);
    list->SetDescriptorHeaps(1, &G.srvHeap);
    list->SetPipelineState(G.pso);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (UINT t = 0; t < 5; t++)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE hg = G.srvHeap->GetGPUDescriptorHandleForHeapStart();
        hg.ptr += (UINT64)(set + t) * G.srvStride;
        list->SetGraphicsRootDescriptorTable(1 + t, hg);
    }
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)G.w, (float)G.h, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)G.w, (LONG)G.h };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    for (int k = 0; k < G.pendN; k++)
    {
        Constants c = {};
        c.srcPerOutX = c.srcPerOutY = 1.0f;
        c.warp = G.pendAhead[k];
        c.warpSrc = (uint32_t)gsrc;
        c.warpPrevN = 1.0f;
        c.warpEase = (g_shared.warpEase && g_shared.warpEaseNoFg) ? 1.0f : 0.0f;
        if (G.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) c.flags |= 1024u;
        if (g_shared.warpEdge == 0) c.flags |= 8192u; else if (g_shared.warpEdge == 1) c.flags |= 4096u;
        list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
        D3D12_CPU_DESCRIPTOR_HANDLE rh = G.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rh.ptr += (SIZE_T)G.pendVar[k] * G.rtvStride;
        list->OMSetRenderTargets(1, &rh, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
    }
    nb = 0;
    b[nb++] = Transition(G.clean[cur], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    b[nb++] = Transition(G.clean[prev], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    b[nb++] = Transition(fl, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(nb, b);                         // the frames drawn ahead stay render targets for the effects
}

// Present, after the copy list ran on the game's queue: ReShade's enabled techniques on the frames drawn ahead, on its
// immediate command list (the same queue), then flushed -- before the frame's fence is signalled.
static void Effects()
{
    if (!G.pendN || !G.rt) return;
    command_queue *q = G.rt->get_command_queue();
    command_list *cmd = q->get_immediate_command_list();
    effect_technique list[64];
    int n = 0, on = 0;
    G.rt->enumerate_techniques(nullptr, [&](effect_runtime *, effect_technique t) { if (n < 64) list[n++] = t; });
    const double t0 = Now();
    fxr::g.guard = true;                                  // the replay check leaves these alone
    for (int k = 0; k < G.pendN; k++)
        for (int i = 0; i < n; i++)
            if (G.rt->get_technique_state(list[i])) { G.rt->render_technique(list[i], cmd, G.apiRtv[G.pendVar[k]]); if (k == 0) on++; }
    fxr::g.guard = false;
    ID3D12GraphicsCommandList *nl = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
    D3D12_RESOURCE_BARRIER b[kVar];
    for (int k = 0; k < G.pendN; k++)
        b[k] = Transition(var[G.pendVar[k]].res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    nl->ResourceBarrier((UINT)G.pendN, b);
    q->flush_immediate_command_list();
    if (!G.logged)
    {
        G.logged = true;
        char mv[48];
        if (G.pendN > 1) snprintf(mv, sizeof(mv), "moves %.3f and %.3f", G.pendAhead[0], G.pendAhead[1]);
        else snprintf(mv, sizeof(mv), "move %.3f", G.pendAhead[0]);
        LOGI("BFI Presenter: draw ahead: first frames drawn ahead (%d, %s), %d technique%s each, %.2f ms on the CPU",
             G.pendN, mv, on, on == 1 ? "" : "s", (Now() - t0) * 1000.0);
    }
}

// Present, under g_lock, as the frame is published: the frames drawn ahead are for it.
static void Publish(UINT64 id)
{
    for (int k = 0; k < G.pendN; k++) { var[G.pendVar[k]].frameId = id; var[G.pendVar[k]].ahead = G.pendAhead[k]; }
}
static UINT64 WaitRead() { return G.pendN ? G.waitRead : 0; }
static ID3D12Resource *CleanNow()
{
    if (!G.capturedNow || G.cleanCur < 0 || !Capturing()) return nullptr;
    return G.cleanN[G.cleanCur] == G.presentN + 1 ? G.clean[G.cleanCur] : nullptr;
}

static void on_destroy_rt(effect_runtime *rt) { if (rt == G.rt) Release(); }
static bool on_open(effect_runtime *, bool open, input_source) { overlayOpen = open; return false; }

static void Register(bool on)
{
    if (on)
    {
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_rt);
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(on_open);
    }
    else
    {
        reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_rt);
        reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(on_open);
    }
}
}   // namespace fxa
