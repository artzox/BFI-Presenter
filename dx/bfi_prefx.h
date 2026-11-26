// ============================================================================
// BFI Presenter -- the game's frame from before ReShade's effects (FrameWarpStillBeforeEffects, 1.0.48)
//
// Copyright (C) 2026 artzox -- https://github.com/artzox/BFI-Presenter
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frame warp keeps still what stands still on the screen (a HUD, text, a crosshair): pixels unchanged from the frame
// before that the scene's motion doesn't explain. It looked for them in the finished frame -- ReShade's effects on it.
// A CRT shader's pre-blur and glow spread the moving scene into a HUD's pixels, so the HUD changed a little every frame
// and was moved with the scene: it broke up on filled flashes (Onimusha: Way of the Sword with CRT-Standalone).
//
// At reshade_begin_effects the back buffer -- the game's frame before the effects -- is copied (on ReShade's command
// list for the frame, before its effects). At the game's Present it is copied on with the frame into the presenter's
// slot (g_pre[s], next to g_stage[s]), and the presenter's still test reads it there. Direct3D 12 and 11, 64-bit;
// Direct3D 9, OpenGL and 32-bit games keep the test on the finished frame.
// ============================================================================
#pragma once

namespace pfx
{
using namespace reshade::api;

static ID3D12Resource  *clean12 = nullptr;     // Direct3D 12: this frame before the effects (the game's queue writes it)
static ID3D11Texture2D *clean11 = nullptr;     // Direct3D 11: likewise (the game's immediate context)
static bool captured = false;                  // copied since the last Present (the game's thread)
static bool logged = false;

static bool Wanted()
{
#ifndef _WIN64
    return false;                              // 32-bit: ReShade's back buffer call has another return convention there
#else
    return g_shared.frameWarp && g_shared.warpStillPre && g_active.load();
#endif
}

// effect_runtime::get_back_buffer returns its resource by value: MSVC hands it back through a hidden pointer after
// `this`, MinGW in a register -- called through the vtable with the pointer, as ReShade (MSVC-built) expects.
static resource BackBuffer(effect_runtime *rt, uint32_t index)
{
    typedef resource *(*Fn)(effect_runtime *self, resource *ret, uint32_t index);
    resource r = {};
    void **vt = *reinterpret_cast<void ***>(rt);
    reinterpret_cast<Fn>(vt[5])(rt, &r, index);
    return r;
}

static void Release()
{
    if (clean12 && g_gameFence && g_gameFenceVal) WaitFence(g_gameFence, g_gameFenceVal, g_gameFenceEvent);
    SafeRelease(clean12);
    SafeRelease(clean11);
    captured = false;
}

static void on_begin(effect_runtime *rt, command_list *cmd, resource_view, resource_view)
{
    if (!Wanted() || !rt || !cmd) return;
    // Only with an effect on: without one the finished frame is the same picture (checked every 60 frames)
    {
        static int on = -1;
        static unsigned n = 0;
        if (on < 0 || (n++ % 60) == 0)
        {
            int k = 0;
            rt->enumerate_techniques(nullptr, [&](effect_runtime *r, effect_technique t) { if (r->get_technique_state(t)) k++; });
            on = k;
        }
        if (on == 0) return;
    }
    void *native = reinterpret_cast<void *>(rt->get_native());
    const resource bbr = BackBuffer(rt, rt->get_current_back_buffer_index());
    if (!bbr.handle) return;
    if (g_dx11.load())
    {
        if (!IsGame11(native)) return;
        ID3D11Texture2D *bb = reinterpret_cast<ID3D11Texture2D *>(bbr.handle);
        ID3D11DeviceContext *ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
        if (!ctx || !g_dev11) return;
        D3D11_TEXTURE2D_DESC d = {};
        bb->GetDesc(&d);
        D3D11_TEXTURE2D_DESC cd = {};
        if (clean11) clean11->GetDesc(&cd);
        if (!clean11 || cd.Width != d.Width || cd.Height != d.Height || cd.Format != d.Format)
        {
            SafeRelease(clean11);
            D3D11_TEXTURE2D_DESC nd = d;
            nd.SampleDesc.Count = 1; nd.SampleDesc.Quality = 0; nd.MipLevels = 1; nd.ArraySize = 1;
            nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = 0; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
            if (FAILED(g_dev11->CreateTexture2D(&nd, nullptr, &clean11))) { clean11 = nullptr; return; }
        }
        if (d.SampleDesc.Count > 1) ctx->ResolveSubresource(clean11, 0, bb, 0, d.Format);
        else                        ctx->CopyResource(clean11, bb);
    }
    else
    {
        if (native != (void *)g_sc) return;
        ID3D12Resource *bb = reinterpret_cast<ID3D12Resource *>(bbr.handle);
        ID3D12GraphicsCommandList *nl = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
        if (!nl || !g_device) return;
        const D3D12_RESOURCE_DESC d = bb->GetDesc();
        if (d.SampleDesc.Count > 1) return;
        if (clean12)
        {
            const D3D12_RESOURCE_DESC cd = clean12->GetDesc();
            if (cd.Width != d.Width || cd.Height != d.Height || cd.Format != d.Format)
            {
                if (g_gameFence && g_gameFenceVal) WaitFence(g_gameFence, g_gameFenceVal, g_gameFenceEvent);
                SafeRelease(clean12);
            }
        }
        if (!clean12)
        {
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = d;
            rd.Flags = D3D12_RESOURCE_FLAG_NONE; rd.MipLevels = 1; rd.DepthOrArraySize = 1;
            if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&clean12))))
            { clean12 = nullptr; return; }
            clean12->SetName(L"BFI frame before ReShade's effects");
        }
        gt::Mark(nl, gt::Q_PFX0);
        D3D12_RESOURCE_BARRIER b[2] = { Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                        Transition(clean12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
        nl->ResourceBarrier(2, b);
        nl->CopyResource(clean12, bb);
        b[0] = Transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        b[1] = Transition(clean12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        nl->ResourceBarrier(2, b);
        gt::Mark(nl, gt::Q_PFX1);
    }
    captured = true;
    if (!logged) { logged = true; LOGI("BFI Presenter: frame warp: what stands still is found in the frame from before ReShade's effects"); }
}

// At the game's Present: this frame's copy from before the effects, without taking it (the HUD mask pass, before the copy).
static ID3D12Resource *Peek12() { return (captured && Wanted() && !g_dx11.load()) ? clean12 : nullptr; }
// At the game's Present: this frame's copy from before the effects, once (null if there is none this frame).
static ID3D12Resource *Take12() { const bool c = captured; captured = false; return (c && Wanted() && !g_dx11.load()) ? clean12 : nullptr; }
static ID3D11Texture2D *Take11() { const bool c = captured; captured = false; return (c && Wanted() && g_dx11.load()) ? clean11 : nullptr; }

static void on_destroy_rt(effect_runtime *) { Release(); }

// ReShade's own window open (1.0.49-t8): it is drawn after the effects, so only the finished frames have it -- the still
// test then looks there too (t2-t7 always did: twice the work on every moved flash, missed refreshes -- dark flashes).
static std::atomic<bool> overlayOpen{false};
static bool on_open_overlay(effect_runtime *, bool open, input_source) { overlayOpen = open; return false; }
static bool OverlayOpen() { return overlayOpen.load(); }

static void Register(bool on)
{
    if (on)
    {
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_rt);
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(on_open_overlay);
    }
    else
    {
        reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_rt);
        reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(on_open_overlay);
    }
}
}   // namespace pfx
