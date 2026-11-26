// ============================================================================
// BFI Presenter -- CRT-Standalone's mask and scanlines after frame warp (CrtStandaloneHandoff, 1.0.49 test)
//
// Copyright (C) 2026 artzox -- https://github.com/artzox/BFI-Presenter
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frame warp moves the finished frame, CRT-Standalone's mask and scanlines included. Moved by different amounts over
// the screen (the game's motion, the measured grid), the fine pattern drifts in and out of step with the pixels:
// curved bands of moire, in the sky above all (Onimusha: Way of the Sword). With BFI_PRESENTER_HANDOFF=1 in the shader,
// CRT-Standalone stops its main pass before the CRT gamma decode; the presenter draws the rest of that pass after
// frame warp, before the strobe: mask, scanlines, spot size, beam bloom, re-encode, brightboost, vignette, corner
// shadow, halation and glow added. The code is CRT-Standalone's own, copied by tools/crt-handoff/gen_crt_hlsl.py into
// dx/bfi_crt_gen.h; its settings are read from the shader every frame, so they stay in the shader's own window.
//
//   Game's thread, reshade_finish_effects: the shader's preprocessor switches and uniforms, and its halation and glow
//   textures (ReShade's resources, this frame's).
//   Game's thread, Present: those textures copied with the frame into its staging slot (as the frame from before the
//   effects, g_pre).
//   Presenter: a second build of its main shader with the CRT code in (compiled on a thread of its own when the
//   switches change), drawn instead of the plain one while the hand-off is on; t7-t9 the slot's textures, b1 the settings.
// Direct3D 12, 64-bit, PIPELINE 0, ENABLE_SCANLINE_SS=0.
// ============================================================================
#pragma once
#include "bfi_crt_gen.h"

namespace crt
{
using namespace reshade::api;

static const char *const kEffect = "CRT-Standalone.fx";
static const char *const kTexName[3] = { "crt_halation_v_tex", "crt_glow_v_tex", "crt_glow_wide_v_tex" };

// The shader's switches the copied code depends on, with CRT-Standalone 1.7.0's defaults (used when the preset doesn't set
// them: ReShade only reports the definitions set in the preset or globally).
struct Def { const char *name; int def; };
static const Def kDefs[] = {
    { "BFI_PRESENTER_HANDOFF", 0 }, { "PIPELINE", 0 }, { "ENABLE_MASK", 1 }, { "ENABLE_BEAM_MODULATION", 1 },
    { "ENABLE_HALATION", 1 }, { "ENABLE_GLOW", 1 }, { "ENABLE_VIGNETTE", 1 }, { "ENABLE_CORNER_ROUND", 0 },
    { "ENABLE_BURNIN_PHASE", 1 }, { "ENABLE_BURNIN_ORBIT", 1 }, { "ENABLE_INTERLACE", 0 }, { "ENABLE_SCANLINE_SS", 0 },
    { "ENABLE_DECAY", 0 }, { "ENABLE_PREBLUR", 1 }, { "ENABLE_GEOMETRY", 0 }, { "SCANLINE_REFERENCE_HEIGHT", 0 },
    // the passes after the pattern (kCrtPost): drawn by the presenter after it (1.0.49)
    { "ENABLE_SHARPEN", 1 }, { "ENABLE_SCANLINE_SOFTEN", 1 }, { "ENABLE_GRAIN", 1 }, { "ENABLE_NOISE_FLOOR", 0 },
    { "ENABLE_GAMUT_EXPAND", 0 },
};
static int DefIndex(const char *name) { for (int i = 0; i < (int)(sizeof(kDefs) / sizeof(kDefs[0])); i++) if (!strcmp(kDefs[i].name, name)) return i; return -1; }
static const int kDefN = (int)(sizeof(kDefs) / sizeof(kDefs[0]));

// Game's thread -> presenter
static std::mutex lk;
static float params[kCrtSlots * 4] = {};        // one float4 per uniform (kCrtParams), then the frame size
static std::string defines;                     // "#define NAME value\n" for each switch, the last read
static std::atomic<int> state{0};               // 0 off, 1 the shader isn't handing off, 2 handing off, -1 not supported
// The presenter's own copies of this frame's halation, glow and wide glow (ReShade's textures) and of the finished
// frame before ReShade's window is drawn on it (kBase), made on ReShade's command list at reshade_finish_effects.
// (t1 kept ReShade's texture pointers until the game's Present and copied from them there: switching ReShade's
// performance mode rebuilt the effects in between, and the copy read freed textures -- a crash in the driver.)
static const int kBase = 3, kTexN = 4;
static ID3D12Resource *cur[kTexN] = {};
static bool curHas[kTexN] = {};                 // found this frame (a texture compiled out: none)
static bool curOk = false;                      // copied since the last Present
static std::atomic<UINT> slotGen{0};            // counts the slot textures being made (the presenter rewrites its views)

// Per staging slot (next to g_stage[s])
static ID3D12Resource *slotTex[kTexN][kStage] = {};
static bool slotOk[kStage] = {};

// The presenter's CRT build of its main shader
static std::mutex blobLk;
// compiled for blobDefs: [0] the main shader (PSMain), [1] the pattern pass (PSCrtPattern, with passes after it),
// [2 + k] kCrtPost[k] (null: its switch off)
// (kBlobN = 2 + kCrtPostN: declared with the presenter's forward declarations)
static ID3DBlob *blobs[kBlobN] = {};
static std::string blobDefs, wantDefs;
static std::atomic<UINT> blobGen{0};
static std::atomic<bool> compiling{false};

// effect_runtime and device calls that return a handle return it through a hidden pointer after `this` in MSVC
// (ReShade's build); MinGW expects it in a register. Called through the vtable with the pointer, as ReShade expects. The
// vtable slot is taken from the member pointer (Itanium ABI: 1 + the byte offset in the vtable), so it can't be miscounted.
template <class PMF> static size_t Slot(PMF pmf)
{
    uintptr_t raw[2] = {};
    memcpy(raw, &pmf, sizeof(raw) < sizeof(pmf) ? sizeof(raw) : sizeof(pmf));
    return (raw[0] - 1) / sizeof(void *);
}
static effect_uniform_variable FindUniform(effect_runtime *rt, const char *effect, const char *name)
{
    typedef effect_uniform_variable *(*Fn)(const effect_runtime *, effect_uniform_variable *, const char *, const char *);
    static const size_t k = Slot(&effect_runtime::find_uniform_variable);
    effect_uniform_variable v = { 0 };
    reinterpret_cast<Fn>((*reinterpret_cast<void ***>(rt))[k])(rt, &v, effect, name);
    return v;
}
static effect_texture_variable FindTexture(effect_runtime *rt, const char *effect, const char *name)
{
    typedef effect_texture_variable *(*Fn)(const effect_runtime *, effect_texture_variable *, const char *, const char *);
    static const size_t k = Slot(&effect_runtime::find_texture_variable);
    effect_texture_variable v = { 0 };
    reinterpret_cast<Fn>((*reinterpret_cast<void ***>(rt))[k])(rt, &v, effect, name);
    return v;
}
static resource ResourceFromView(device *dev, resource_view view)
{
    typedef resource *(*Fn)(const device *, resource *, resource_view);
    static const size_t k = Slot(&device::get_resource_from_view);
    resource r = { 0 };
    reinterpret_cast<Fn>((*reinterpret_cast<void ***>(dev))[k])(dev, &r, view);
    return r;
}

static int DefValue(effect_runtime *rt, const Def &d)
{
    char v[64] = ""; size_t n = sizeof(v);
    if (rt->get_preprocessor_definition_for_effect(kEffect, d.name, v, &n) && v[0]) return atoi(v);
    n = sizeof(v); v[0] = 0;
    if (rt->get_preprocessor_definition(d.name, v, &n) && v[0]) return atoi(v);
    return d.def;
}

static bool Wanted()
{
#ifndef _WIN64
    return false;
#else
    return g_shared.crtHandoff && g_active.load() && !g_dx11.load();
#endif
}

static void StartCompile();

// ReShade's performance mode compiles the settings into the effect as constants: CRT-Standalone then has no uniform
// variables to read (t3: every setting read as 0 -- a black picture). They are taken from the preset, as ReShade
// compiles them in, and where the preset has none, from the shader's initial values (kCrtParams[].def).
static std::map<std::string, std::vector<float>> presetVals;
static std::string presetPath;
static FILETIME presetTime = {};
static double presetChecked = -1.0;
static void ReadPreset(effect_runtime *rt)
{
    const double t = Now();
    if (presetChecked >= 0.0 && t - presetChecked < 0.5) return;
    presetChecked = t;
    char path[1024] = ""; size_t n = sizeof(path);
    rt->get_current_preset_path(path, &n);
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!path[0] || !GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) { presetVals.clear(); presetPath.clear(); return; }
    if (presetPath == path && CompareFileTime(&fa.ftLastWriteTime, &presetTime) == 0) return;
    presetPath = path; presetTime = fa.ftLastWriteTime;
    presetVals.clear();
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[4096];
    bool in = false;
    while (fgets(line, sizeof(line), f))
    {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        char *b = line;
        while (*b == ' ' || *b == '\t') b++;
        if (*b == '[') { in = _stricmp(b, "[CRT-Standalone.fx]") == 0; continue; }
        if (!in) continue;
        char *eq = strchr(b, '=');
        if (!eq) continue;
        *eq = 0;
        std::string key(b);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        std::vector<float> v;
        for (char *c = eq + 1; *c;)
        {
            while (*c == ' ' || *c == ',') c++;
            if (!*c) break;
            if (!_strnicmp(c, "true", 4)) { v.push_back(1.0f); c += 4; continue; }
            if (!_strnicmp(c, "false", 5)) { v.push_back(0.0f); c += 5; continue; }
            char *end = nullptr;
            const float x = strtof(c, &end);
            if (end == c) break;
            v.push_back(x); c = end;
        }
        if (!v.empty()) presetVals[key] = v;
    }
    fclose(f);
}

static void WaitGame() { if (g_gameFence && g_gameFenceVal) WaitFence(g_gameFence, g_gameFenceVal, g_gameFenceEvent); }

// A copy of r in cur[k] (made or remade to its size and format), on ReShade's list; r is in state rs and left in it.
static bool CopyCur(ID3D12GraphicsCommandList *nl, int k, ID3D12Resource *r, D3D12_RESOURCE_STATES rs)
{
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.SampleDesc.Count > 1 || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    if (cur[k])
    {
        const D3D12_RESOURCE_DESC h = cur[k]->GetDesc();
        if (h.Width != d.Width || h.Height != d.Height || h.Format != d.Format) { WaitGame(); SafeRelease(cur[k]); }
    }
    if (!cur[k])
    {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = d;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE; rd.MipLevels = 1; rd.DepthOrArraySize = 1;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&cur[k]))))
        { cur[k] = nullptr; return false; }
        cur[k]->SetName(L"BFI CRT-Standalone hand-off (this frame)");
    }
    D3D12_RESOURCE_BARRIER b[2] = { Transition(r, rs, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                    Transition(cur[k], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
    nl->ResourceBarrier(2, b);
    if (d.MipLevels > 1)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = cur[k]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        src.pResource = r;      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        nl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    else nl->CopyResource(cur[k], r);
    b[0] = Transition(r, D3D12_RESOURCE_STATE_COPY_SOURCE, rs);
    b[1] = Transition(cur[k], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    nl->ResourceBarrier(2, b);
    return true;
}

// reshade_finish_effects (the game's thread): the shader's state for this frame.
static void on_finish(effect_runtime *rt, command_list *cmd, resource_view, resource_view)
{
    if (!Wanted() || !rt) { if (!Wanted()) state = 0; return; }
    if (reinterpret_cast<void *>(rt->get_native()) != (void *)g_sc) return;
    // CRT-Standalone on at all?
    bool on = false;
    rt->enumerate_techniques(kEffect, [&](effect_runtime *r, effect_technique t) { if (r->get_technique_state(t)) on = true; });
    int vals[kDefN];
    for (int i = 0; i < kDefN; i++) vals[i] = DefValue(rt, kDefs[i]);
    static int lastState = -9;
    int st = (!on || vals[0] != 1) ? 1 : 2;
    if (st == 2 && (vals[1] != 0 || vals[11] != 0)) st = -1;     // PIPELINE 1/2 or ENABLE_SCANLINE_SS: not yet
    if (st != lastState)
    {
        lastState = st;
        if (st == 2) LOGI("BFI Presenter: CRT-Standalone hands its mask and scanlines to the presenter -- drawn after frame warp");
        else if (st == 1) LOGI("BFI Presenter: CRT hand-off on, but CRT-Standalone %s", on ? "doesn't hand off (BFI_PRESENTER_HANDOFF=1 in its preprocessor definitions)" : "isn't on");
        else LOGW("BFI Presenter: CRT hand-off: not with CRT-Standalone's PIPELINE 1/2 or ENABLE_SCANLINE_SS=1 yet -- the shader's own pass is needed");
    }
    state = st;
    if (st != 2) { curOk = false; return; }

    float p[kCrtSlots * 4] = {};
    bool presetRead = false;
    int constants = 0;
    for (int i = 0; i < kCrtParamN; i++)
    {
        float *o = &p[i * 4];
        const char *name = kCrtParams[i].name;
        if (!strcmp(name, "CRT_TIMER") || !strcmp(name, "crt_timer")) { o[0] = (float)(fmod(Now(), 86400.0) * 1000.0); continue; }
        if (!strcmp(name, "FRAMECOUNT")) { static uint32_t fc = 0; o[0] = (float)(fc++ & 0xFFFFFF); continue; }
        effect_uniform_variable v = FindUniform(rt, kEffect, name);
        if (!v.handle)
        {
            // compiled in as a constant (performance mode), or out (its switch off: its code is too)
            if (!presetRead) { ReadPreset(rt); presetRead = true; }
            auto it = presetVals.find(name);
            for (int j = 0; j < 4; j++)
                o[j] = (it != presetVals.end() && j < (int)it->second.size()) ? it->second[j] : kCrtParams[i].def[j];
            constants++;
            continue;
        }
        switch (kCrtParams[i].type)
        {
        case 'f': rt->get_uniform_value_float(v, o, 1); break;
        case '2': rt->get_uniform_value_float(v, o, 2); break;
        case '3': rt->get_uniform_value_float(v, o, 3); break;
        case '4': rt->get_uniform_value_float(v, o, 4); break;
        case 'i': { int32_t x = 0; rt->get_uniform_value_int(v, &x, 1); o[0] = (float)x; break; }
        case 'u': { uint32_t x = 0; rt->get_uniform_value_uint(v, &x, 1); o[0] = (float)x; break; }
        case 'b': { bool x = false; rt->get_uniform_value_bool(v, &x, 1); o[0] = x ? 1.0f : 0.0f; break; }
        }
    }
    // The halation and glow textures: ReShade's own, rendered this frame.
    // (halation or glow compiled out: no texture, and its code is out of the presenter's build too -- nothing bound)
    device *dev = rt->get_device();
    ID3D12GraphicsCommandList *nl = cmd ? reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native()) : nullptr;
    bool ok = dev != nullptr && nl != nullptr && g_device != nullptr;
    bool has[kTexN] = {};
    gt::Mark(nl, gt::Q_FIN0);
    struct FinMark { ID3D12GraphicsCommandList *l; ~FinMark() { gt::Mark(l, gt::Q_FIN1); } } finMark{ nl };
    const D3D12_RESOURCE_STATES rs = (D3D12_RESOURCE_STATES)g_shared.crtTexState;   // ReShade's effect textures between passes
    for (int k = 0; ok && k < 3; k++)
    {
        effect_texture_variable tv = FindTexture(rt, kEffect, kTexName[k]);
        resource_view srv = { 0 }, srgb = { 0 };
        if (tv.handle) rt->get_texture_binding(tv, &srv, &srgb);
        const resource r = srv.handle ? ResourceFromView(dev, srv) : resource{ 0 };
        if (!r.handle) continue;
        has[k] = CopyCur(nl, k, reinterpret_cast<ID3D12Resource *>(r.handle), rs);
        if (!has[k]) ok = false;
    }
    // the finished frame as it is now: ReShade's window and overlay are drawn on it after this (shown without the CRT pattern)
    // (only while ReShade's window is open, 1.0.49-t10: two full-frame copies a frame otherwise)
    if (ok && pfx::OverlayOpen())
    {
        const resource bbr = pfx::BackBuffer(rt, rt->get_current_back_buffer_index());
        ok = bbr.handle && CopyCur(nl, kBase, reinterpret_cast<ID3D12Resource *>(bbr.handle), D3D12_RESOURCE_STATE_RENDER_TARGET);
        has[kBase] = ok;
    }
    {
        static bool constLogged = false;
        if (constants > kCrtParamN / 2 && !constLogged)
        {
            constLogged = true;
            LOGI("BFI Presenter: CRT hand-off: CRT-Standalone's settings are compiled in (ReShade's performance mode) -- taken from the preset %s",
                 presetPath.empty() ? "(none: the shader's initial values)" : presetPath.c_str());
        }
    }
    std::string d;
    char line[96];
    for (int i = 0; i < kDefN; i++) { snprintf(line, sizeof(line), "#define %s %d\n", kDefs[i].name, vals[i]); d += line; }
    {
        int post = 0;
        for (int k = 0; k < kCrtPostN; k++) { const int di = DefIndex(kCrtPost[k].sw); if (di >= 0 && vals[di]) post++; }
        (void)post;
        // always: the pattern is drawn into a texture (with the frame, dx/bfi_crt.h Bake), the main shader reads it
        snprintf(line, sizeof(line), "#define CRT_POST %d\n", 1); d += line;
    }
    {
        std::lock_guard<std::mutex> l(lk);
        memcpy(params, p, sizeof(params) - 8 * sizeof(float));   // the frame size slot is the presenter's
        defines = d;
    }
    curOk = ok;
    for (int k = 0; k < kTexN; k++) curHas[k] = ok && has[k];
    static bool texLogged = false;
    if (!texLogged && ok && (!has[0] || !has[1]))
    {
        texLogged = true;
        LOGI("BFI Presenter: CRT hand-off: CRT-Standalone's textures: halation %s, glow %s, wide glow %s", has[0] ? "found" : "none",
             has[1] ? "found" : "none", has[2] ? "found" : "none");
    }
    {
        std::lock_guard<std::mutex> l(blobLk);
        if (d != blobDefs && d != wantDefs) { wantDefs = d; }
    }
    StartCompile();
}

// The game's Present, on the frame copy list: this frame's halation and glow textures into the slot.
static void CountBytes();
static void CopyAtPresent(ID3D12GraphicsCommandList *list, UINT s)
{
    // The copies of the frame before ReShade's window (one per slot, 33 MB each at 4K) are needed only while the window
    // is open: released 5 s after it closes (1.0.49-t14: Returnal went over its video memory budget, and with part of it
    // moved to system memory every copy and pass on the GPU took ten times as long).
    {
        static double closedT = -1.0;
        if (pfx::OverlayOpen()) closedT = -1.0;
        else if (closedT < 0.0) closedT = Now();
        else if (Now() - closedT > 5.0)
        {
            bool any = cur[kBase] != nullptr;
            for (UINT k = 0; k < kStage && !any; k++) if (slotTex[kBase][k]) any = true;
            if (any)
            {
                WaitGame();
                if (g_presFence && g_presFenceVal) WaitFence(g_presFence, g_presFenceVal, g_gameFenceEvent);
                SafeRelease(cur[kBase]); curHas[kBase] = false;
                for (UINT k = 0; k < kStage; k++) SafeRelease(slotTex[kBase][k]);
                slotGen++;
            }
        }
    }
    bool ok = false;
    if (state.load() == 2 && curOk && Wanted())
    {
        ok = true;
        for (int k = 0; k < kTexN && ok; k++)
        {
            if (!curHas[k])
            {
                if (k != kBase && slotTex[k][s]) { SafeRelease(slotTex[k][s]); slotGen++; }   // compiled out
                continue;                                                                    // the frame before ReShade's window: kept (not read while it is closed)
            }
            const D3D12_RESOURCE_DESC d = cur[k]->GetDesc();
            if (slotTex[k][s])
            {
                const D3D12_RESOURCE_DESC h = slotTex[k][s]->GetDesc();
                if (h.Width != d.Width || h.Height != d.Height || h.Format != d.Format) ok = false;   // resized: until the slots are remade
            }
            else
            {
                D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slotTex[k][s]))))
                { slotTex[k][s] = nullptr; ok = false; }
                else { slotTex[k][s]->SetName(L"BFI CRT-Standalone hand-off (slot)"); slotGen++; }
            }
        }
        if (ok)
            for (int k = 0; k < kTexN; k++)
            {
                if (!curHas[k]) continue;
                D3D12_RESOURCE_BARRIER b[2] = { Transition(cur[k], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                                Transition(slotTex[k][s], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
                list->ResourceBarrier(2, b);
                list->CopyResource(slotTex[k][s], cur[k]);
                b[0] = Transition(cur[k], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                b[1] = Transition(slotTex[k][s], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                list->ResourceBarrier(2, b);
            }
    }
    slotOk[s] = ok;
    curOk = false;
    CountBytes();
}

// The presenter: the texture bound for slot s, view k (null: compiled out of the shader).
static ID3D12Resource *SlotTex(UINT s, int k) { return s < kStage ? slotTex[k][s] : nullptr; }
static bool SlotReady(UINT s) { return s < kStage && slotOk[s]; }

static void ReleaseCur()
{
    curOk = false;
    bool any = false;
    for (int k = 0; k < kTexN; k++) { curHas[k] = false; if (cur[k]) any = true; }
    if (any) WaitGame();
    for (int k = 0; k < kTexN; k++) SafeRelease(cur[k]);
}

static void BakeRelease();
static void ReleaseSlots()
{
    BakeRelease();
    for (int k = 0; k < kTexN; k++) for (UINT s = 0; s < kStage; s++) SafeRelease(slotTex[k][s]);
    for (UINT s = 0; s < kStage; s++) slotOk[s] = false;
    slotGen++;
}

// The presenter's main shader with the CRT code in: kShader, the switches defined in front, and at its marker the CRT
// stage (dx/bfi_crt_gen.h) with the samplers it calls. Compiled on a thread of its own (a few seconds at the highest
// optimisation level); the plain shader is drawn until it is ready.
static const char kCrtGlue[] =
    "#define CrtPixelSize (1.0 / float2(BUFFER_WIDTH, BUFFER_HEIGHT))\n"
    "Texture2D<float4> crtHalo : register(t7);\n"
    "Texture2D<float4> crtGlow : register(t8);\n"
    "Texture2D<float4> crtGlowWide : register(t9);\n"
    "Texture2D<float4> crtBase : register(t10);\n"
    "float4 CrtBB(float2 uv) { return src.SampleLevel(linClamp, uv, 0); }\n"
    "float4 CrtHalo(float2 uv) { return crtHalo.SampleLevel(linClamp, uv, 0); }\n"
    "float4 CrtGlow(float2 uv) { return crtGlow.SampleLevel(linClamp, uv, 0); }\n"
    "float4 CrtGlowWide(float2 uv) { return crtGlowWide.SampleLevel(linClamp, uv, 0); }\n"
    "#if CRT_POST\n"
    "Texture2D<float4> crtPrev : register(t11);\n"       // the passes after the pattern: the pass before
    "Texture2D<float4> crtGrainRaw : register(t12);\n"   // and the grain pass's own texture
    "float4 CrtPrev(float2 uv) { return crtPrev.SampleLevel(linClamp, uv, 0); }\n"
    "float4 CrtGrainRaw(float2 uv) { return crtGrainRaw.SampleLevel(linClamp, uv, 0); }\n"
    "#endif\n";
static const char kCrtApply[] =
    "// what was drawn after the effects (ReShade's window, an overlay) is shown as it is, without the pattern\n"
    "bool CrtOverlay(float2 cp)\n"
    "{\n"
    "    if (!CRT_OVERLAY_CHECK) return false;\n"
    "    const int2 q = int2(cp);\n"
    "    return any(abs(src.Load(int3(q, 0)).rgb - crtBase.Load(int3(q, 0)).rgb) > 0.0005);\n"
    "}\n"
    "// the mask and scanlines at the screen's pixel; the picture, halation and glow from where it came from (moved)\n"
    "float3 CrtApply(float3 c, float2 cp) { return CrtOverlay(cp) ? c : CrtPattern(c, cp, gSrcPos * CrtPixelSize); }\n"
    "#if CRT_POST\n"
    "// with passes after the pattern: the pattern drawn first, at the game frame's pixels, then those passes\n"
    "float4 PSCrtPattern(float4 pos : SV_Position) : SV_Target\n"
    "{\n"
    "    const float3 c = Fetch(pos.xy / srcPerOut);\n"
    "    return float4(CrtApply(c, pos.xy), 1.0);\n"
    "}\n"
    "float3 CrtColour(float2 pos)\n"
    "{\n"
    "    const float2 cp = pos * srcPerOut;\n"
    "    if (CrtOverlay(cp)) return Fetch(pos);\n"
    "    return crtPrev.SampleLevel(linClamp, cp * CrtPixelSize, 0).rgb;   // the last pass's picture\n"
    "}\n"
    "#else\n"
    "float3 CrtColour(float2 pos) { const float3 c = Fetch(pos); return CrtApply(c, pos * srcPerOut); }\n"
    "#endif\n";

// The textures between the pattern and the passes after it: 10-bit like the frame where the frame is 8- or 10-bit (as
// ReShade's own back buffer: half the memory traffic of 16-bit float), else 16-bit float (1.0.49-t10)
static DXGI_FORMAT PassFormat()
{
    switch (StageFormat())
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
}

static DWORD WINAPI CompileThread(void *)
{
    for (;;)
    {
        std::string defs;
        {
            std::lock_guard<std::mutex> l(blobLk);
            if (wantDefs.empty() || wantDefs == blobDefs) { compiling = false; return 0; }
            defs = wantDefs;
        }
        const double t0 = Now();
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        PFN_D3DCompile compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : nullptr;
        std::string srcText(kShader);
        const char *marker = "// @CRT_HANDOFF@";
        const size_t at = srcText.find(marker);
        ID3DBlob *got[kBlobN] = {};
        bool failed = !compile || at == std::string::npos;
        if (!failed)
        {
            std::string full = "#define CRT_HANDOFF 1\n" + defs + srcText.substr(0, at) + kCrtGlue + kCrtHlsl + kCrtApply + srcText.substr(at + strlen(marker));
            const bool post = defs.find("#define CRT_POST 1") != std::string::npos;
            for (int e = 0; e < kBlobN && !failed; e++)
            {
                const char *entry = e == 0 ? "PSMain" : e == 1 ? "PSCrtPattern" : kCrtPost[e - 2].entry;
                if (e >= 1 && !post) continue;
                if (e >= 2)
                {
                    char sw[96]; snprintf(sw, sizeof(sw), "#define %s 1\n", kCrtPost[e - 2].sw);
                    if (defs.find(sw) == std::string::npos) continue;      // its switch off: not drawn
                }
                ID3DBlob *b = nullptr, *err = nullptr;
                HRESULT hr = compile(full.data(), full.size(), "bfi_presenter_crt", nullptr, nullptr, entry, "ps_5_0",
                                     e <= 1 ? D3DCOMPILE_OPTIMIZATION_LEVEL3 : D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &b, &err);
                if (FAILED(hr))
                {
                    LOGE("BFI Presenter: CRT hand-off: the shader (%s) failed to compile: %s", entry, err ? (const char *)err->GetBufferPointer() : "?");
                    SafeRelease(b);
                    failed = true;
                }
                SafeRelease(err);
                got[e] = b;
            }
        }
        if (failed) for (int e = 0; e < kBlobN; e++) SafeRelease(got[e]);
        {
            std::lock_guard<std::mutex> l(blobLk);
            for (int e = 0; e < kBlobN; e++) { SafeRelease(blobs[e]); blobs[e] = got[e]; }
            blobDefs = defs;                                        // tried: not again until the switches change
            blobGen++;
        }
        ID3DBlob *b = got[0];
        if (b) LOGI("BFI Presenter: CRT hand-off: shader built in %.0f ms", (Now() - t0) * 1000.0);
    }
}
static void StartCompile()
{
    {
        std::lock_guard<std::mutex> l(blobLk);
        if (wantDefs.empty() || wantDefs == blobDefs) return;
    }
    if (compiling.exchange(true)) return;
    HANDLE h = CreateThread(nullptr, 0, CompileThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h); else compiling = false;
}
// The presenter: the compiled shaders (each AddRef'd, null where not built) if newer than `gen`; false: no news.
static bool TakeBlobs(UINT *gen, ID3DBlob *out[kBlobN])
{
    std::lock_guard<std::mutex> l(blobLk);
    if (blobGen.load() == *gen) return false;
    *gen = blobGen.load();
    for (int e = 0; e < kBlobN; e++) { out[e] = blobs[e]; if (out[e]) out[e]->AddRef(); }
    return true;
}
static bool PostIsGrainRaw(int k) { return !strcmp(kCrtPost[k].entry, "PSCrtGrainRaw"); }
// Pass k of kCrtPost would change the picture with these settings (cp, as ParamsFor gives them): one at its off value
// (the shader's own test: it then leaves the picture as it is) isn't drawn.
static float ParamOf(const float *cp, const char *name)
{
    for (int i = 0; i < kCrtParamN; i++) if (!strcmp(kCrtParams[i].name, name)) return cp[i * 4];
    return 1.0f;                                         // not read: assume on
}
static bool PostOn(int k, const float *cp)
{
    const char *e = kCrtPost[k].entry;
    if (!strcmp(e, "PSCrtSharpen"))    return ParamOf(cp, "crt_sharpen_strength") >= 0.001f;
    if (!strcmp(e, "PSCrtSoften"))     return ParamOf(cp, "crt_soften_strength") >= 0.001f;
    if (!strcmp(e, "PSCrtGrainRaw") || !strcmp(e, "PSCrtGrain")) return ParamOf(cp, "crt_grain_intensity") > 0.001f;
    if (!strcmp(e, "PSCrtNoiseFloor")) return ParamOf(cp, "crt_noise_floor") > 0.001f;
    if (!strcmp(e, "PSCrtGamut"))      return ParamOf(cp, "crt_gamut_expand_strength") >= 0.001f;
    if (!strcmp(e, "PSCrtCorner"))     return ParamOf(cp, "crt_corner_size") >= 0.001f || ParamOf(cp, "crt_corner_border") >= 0.001f;
    return true;
}
// The presenter: this frame's settings, with the frame size (w, h).
static void ParamsFor(float *out, float w, float h)
{
    std::lock_guard<std::mutex> l(lk);
    memcpy(out, params, sizeof(params));
    out[kCrtParamN * 4 + 0] = w;
    out[kCrtParamN * 4 + 1] = h;
    out[kCrtParamN * 4 + 2] = pfx::OverlayOpen() ? 1.0f : 0.0f;   // what is drawn after the effects: only with ReShade's window open
}

static int StateNow() { return state.load(); }
static UINT SlotGen() { return slotGen.load(); }


// ---- The CRT picture made with the game's frame (1.0.49-t7) ----
// t4-t6 drew the pattern (and from t5 the passes after it) in the presenter, just before the refresh it is shown on:
// heavier refreshes on the strobe's own deadline, and more of them missed (dark flashes). Now, at the game's Present, the
// frame's CRT picture is drawn on the game's queue into bOut[s] with the slot (once per game frame, as CRT-Standalone
// itself does), and the presenter only reads it. Only a flash shown moved (frame warp) draws it in the presenter.
static const UINT kBakeCbStride = (UINT)((kCrtSlots * 16 + 255) & ~255);
static ID3D12RootSignature *bRoot = nullptr;
static ID3D12PipelineState *bPso[1 + kCrtPostN] = {};
static UINT bBlobGen = 0;
static ID3D12DescriptorHeap *bHeap = nullptr, *bRtvHeap = nullptr;
static ID3D12Resource *bRt[3] = {};                 // [0] [1] in turn, [2] grain's (the game's queue only)
static ID3D12Resource *bOut[kStage] = {};           // the slot's CRT picture (COMMON between uses)
static bool bOk[kStage] = {};
static std::atomic<UINT> bOutGen{0};
static ID3D12Resource *bCb = nullptr;
static uint8_t *bCbMap = nullptr;
static ID3D12Resource *bSeenSrc[kStage] = {}, *bSeenTex[kStage][kTexN] = {};
static UINT bSrvStride = 0, bRtvStride = 0;
static DXGI_FORMAT bFmt = DXGI_FORMAT_UNKNOWN;     // the format bPso were made for
static bool bFailed = false;
static std::atomic<bool> bSkip{false};              // most flashes are shown moved: not made (SetBakeSkip)
// Video memory (1.0.50): the per-frame CRT picture keeps one full-size texture per slot and three to draw in (about 330 MB
// at 4K). With the game close to its video memory budget it isn't made, and its textures are freed: each refresh then
// draws its CRT picture itself, once per new picture (as 1.0.49-t6), which costs GPU time on the strobe's deadline instead.
static std::atomic<bool> bTight{false};
static bool bFreed = true;
static void SetMemoryTight(bool tight)
{
    if (bTight.exchange(tight) != tight)
        LOGI("BFI Presenter: CRT hand-off: %s", tight ? "the game is close to its video memory budget -- the CRT picture is no longer made with each frame (its textures freed); each refresh draws it"
                                                      : "video memory to spare again -- the CRT picture is made with each frame");
}
static void SetBakeSkip(bool skip)
{
    if (bSkip.exchange(skip) != skip)
        LOGI("BFI Presenter: CRT hand-off: %s", skip ? "most flashes are moved by frame warp -- each draws its CRT picture, none is made with the frame"
                                                     : "the CRT picture is made with each frame again (most flashes shown as rendered)");
}
// heap: [s] the slot's frame (t0..t6), [kStage + 4 s ..] its four textures (t7-t10), [5 kStage + v] the pairs (t11 t12)

static bool BakeSetup()
{
    if (bFailed) return false;
    if (!bRoot)
    {
        D3D12_DESCRIPTOR_RANGE r[10] = {};
        for (int i = 0; i < 10; i++) { r[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[i].NumDescriptors = 1; r[i].BaseShaderRegister = (UINT)i; }
        r[7].NumDescriptors = 4;                     // t7-t10
        r[8].BaseShaderRegister = 11; r[8].NumDescriptors = 2;   // t11-t12
        D3D12_ROOT_PARAMETER pr[11] = {};
        pr[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        pr[0].Constants.ShaderRegister = 0; pr[0].Constants.Num32BitValues = sizeof(Constants) / 4;
        pr[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const int map[10] = { 0, 1, 2, 3, 4, 5, 6, 7, -1, 8 };   // params 1-8 and 10: t0..t6, t7-t10, (9 = b1), t11-t12
        for (int i = 1; i <= 10; i++)
        {
            if (i == 9) continue;
            pr[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            pr[i].DescriptorTable.NumDescriptorRanges = 1;
            pr[i].DescriptorTable.pDescriptorRanges = &r[map[i - 1]];
            pr[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        pr[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; pr[9].Descriptor.ShaderRegister = 1;
        pr[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxLOD = D3D12_FLOAT32_MAX; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 11; rsd.pParameters = pr; rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
        ID3DBlob *rb = nullptr, *re = nullptr;
        bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rb, &re)) &&
                  SUCCEEDED(g_device->CreateRootSignature(0, rb->GetBufferPointer(), rb->GetBufferSize(), IID_PPV_ARGS(&bRoot)));
        SafeRelease(rb); SafeRelease(re);
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 5 * kStage + 4; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ok = ok && SUCCEEDED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&bHeap)));
        D3D12_DESCRIPTOR_HEAP_DESC rh = {}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 3 + kStage;   // bRt, then bOut
        ok = ok && SUCCEEDED(g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&bRtvHeap)));
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (UINT64)kBakeCbStride * kStage; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ok = ok && SUCCEEDED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&bCb)));
        D3D12_RANGE nr = { 0, 0 };
        ok = ok && SUCCEEDED(bCb->Map(0, &nr, (void **)&bCbMap));
        if (!ok)
        {
            bFailed = true;
            LOGE("BFI Presenter: CRT hand-off: the game-side pass could not be set up -- drawn by the presenter");
            return false;
        }
        bSrvStride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        bRtvStride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    ID3DBlob *cbs[kBlobN] = {};
    if (bPso[0] && bFmt != PassFormat()) bBlobGen = 0;   // the frame's format changed: made again
    if (TakeBlobs(&bBlobGen, cbs))
    {
        for (int k = 0; k <= kCrtPostN; k++) SafeRelease(bPso[k]);
        ID3DBlob *vsb = g_shBlob[0];
        bool ok = vsb != nullptr && cbs[1] != nullptr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = bRoot;
        if (vsb) pd.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = PassFormat();
        pd.SampleDesc.Count = 1;
        bFmt = PassFormat();
        for (int e = 1; e < kBlobN && ok; e++)
        {
            if (!cbs[e]) continue;
            pd.PS = { cbs[e]->GetBufferPointer(), cbs[e]->GetBufferSize() };
            if (FAILED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&bPso[e - 1])))) { bPso[e - 1] = nullptr; ok = false; }
        }
        if (!ok) for (int k = 0; k <= kCrtPostN; k++) SafeRelease(bPso[k]);
        for (int e = 0; e < kBlobN; e++) SafeRelease(cbs[e]);
    }
    return bPso[0] != nullptr;
}

static void BakeWaitGame() { WaitGame(); }

// The game's Present, on the frame copy list after CopyAtPresent: slot s's CRT picture into bOut[s].
// waitRead: the presenter's fence value it may still be reading slot s at (its bOut[s] is remade only after it).
static void Bake(ID3D12GraphicsCommandList *list, UINT s, ID3D12Resource *stage, UINT64 waitRead)
{
    bOk[s] = false;
    if (bTight.load())
    {
        if (!bFreed)
        {
            // the presenter may still read a slot's picture: wait until it has drawn all it reserved
            WaitGame();
            if (g_presFence && g_presFenceVal) WaitFence(g_presFence, g_presFenceVal, g_gameFenceEvent);
            for (UINT j = 0; j < kStage; j++) { SafeRelease(bOut[j]); bOk[j] = false; }
            for (int k = 0; k < 3; k++) SafeRelease(bRt[k]);
            bOutGen++;
            bFreed = true;
        }
        return;
    }
    if (bSkip.load() || !(state.load() == 2 && slotOk[s] && Wanted() && stage) || !BakeSetup()) return;
    bFreed = false;
    const D3D12_RESOURCE_DESC sd = stage->GetDesc();
    const UINT fw = (UINT)sd.Width, fh = sd.Height;
    // the working textures (the game's queue only) and the slot's output
    const DXGI_FORMAT pf = PassFormat();
    if (bRt[0] && (bRt[0]->GetDesc().Width != fw || bRt[0]->GetDesc().Height != fh || bRt[0]->GetDesc().Format != pf))
    { WaitGame(); for (int k = 0; k < 3; k++) SafeRelease(bRt[k]); }
    if (bOut[s] && (bOut[s]->GetDesc().Width != fw || bOut[s]->GetDesc().Height != fh || bOut[s]->GetDesc().Format != pf))
    {
        if (waitRead && g_presFence) WaitFence(g_presFence, waitRead, g_gameFenceEvent);
        SafeRelease(bOut[s]);
    }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = fw; rd.Height = fh; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = pf; rd.SampleDesc.Count = 1;
    bool madeRt = false;
    for (int k = 0; k < 3; k++)
    {
        if (bRt[k]) continue;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&bRt[k]))))
        { bRt[k] = nullptr; return; }
        bRt[k]->SetName(L"BFI CRT-Standalone hand-off (game side)");
        D3D12_CPU_DESCRIPTOR_HANDLE h = bRtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)k * bRtvStride;
        g_device->CreateRenderTargetView(bRt[k], nullptr, h);
        madeRt = true;
    }
    if (madeRt)
    {
        const int order[4] = { 0, 2, 1, 2 };
        for (int v = 0; v < 4; v++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = bHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)(5 * kStage + v) * bSrvStride;
            g_device->CreateShaderResourceView(bRt[order[v]], nullptr, h);
        }
    }
    if (!bOut[s])
    {
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;       // the last pass draws into it (t10: drawn, then copied)
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&bOut[s]))))
        { bOut[s] = nullptr; return; }
        bOut[s]->SetName(L"BFI CRT-Standalone hand-off (the slot's CRT picture)");
        D3D12_CPU_DESCRIPTOR_HANDLE h = bRtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(3 + s) * bRtvStride;
        g_device->CreateRenderTargetView(bOut[s], nullptr, h);
        bOutGen++;
    }
    // the slot's views (its frame, its four textures), rewritten when they change (the slot's last use is done)
    if (bSeenSrc[s] != stage)
    {
        bSeenSrc[s] = stage;
        D3D12_SHADER_RESOURCE_VIEW_DESC pv = {};
        pv.Format = StageFormat(); pv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        pv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; pv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = bHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)s * bSrvStride;
        g_device->CreateShaderResourceView(stage, &pv, h);
    }
    for (int t = 0; t < kTexN; t++)
    {
        ID3D12Resource *r = slotTex[t][s];
        if (bSeenTex[s][t] == r && r) continue;
        bSeenTex[s][t] = r;
        D3D12_SHADER_RESOURCE_VIEW_DESC cv = {};
        cv.Format = r ? r->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
        cv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        cv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; cv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = bHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)(kStage + 4 * s + (UINT)t) * bSrvStride;
        g_device->CreateShaderResourceView(r, &cv, h);
    }
    // this frame's settings
    float cp[kCrtSlots * 4];
    ParamsFor(cp, (float)fw, (float)fh);
    memcpy(bCbMap + (size_t)s * kBakeCbStride, cp, sizeof(cp));
    Constants k = {};
    k.srcPerOutX = 1.0f; k.srcPerOutY = 1.0f;                // the game frame's own pixels, as rendered (not moved)

    ID3D12DescriptorHeap *heaps[] = { bHeap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(bRoot);
    list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &k, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE g0 = bHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE src = g0; src.ptr += (UINT64)s * bSrvStride;
    for (UINT i = 1; i <= 7; i++) list->SetGraphicsRootDescriptorTable(i, src);   // t0 (t1-t6: not read here)
    D3D12_GPU_DESCRIPTOR_HANDLE tx = g0; tx.ptr += (UINT64)(kStage + 4 * s) * bSrvStride;
    list->SetGraphicsRootDescriptorTable(8, tx);
    list->SetGraphicsRootConstantBufferView(9, bCb->GetGPUVirtualAddress() + (UINT64)s * kBakeCbStride);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)fw, (float)fh, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)fw, (LONG)fh };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);

    D3D12_RESOURCE_BARRIER in[1 + kTexN]; UINT nin = 0;
    in[nin++] = Transition(stage, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (int t = 0; t < kTexN; t++)
        if (slotTex[t][s]) in[nin++] = Transition(slotTex[t][s], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(nin, in);

    // t: 0 1 2 = bRt, 3 = bOut[s] (the last pass draws straight into it)
    auto into = [&](int t, ID3D12PipelineState *pp, int from)
    {
        ID3D12Resource *r = t < 3 ? bRt[t] : bOut[s];
        const D3D12_RESOURCE_STATES idle = t < 3 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_BARRIER b = Transition(r, idle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &b);
        D3D12_GPU_DESCRIPTOR_HANDLE ph = g0; ph.ptr += (UINT64)(5 * kStage + 2 * (UINT)(from >= 0 ? from : 1)) * bSrvStride;
        list->SetGraphicsRootDescriptorTable(10, ph);
        D3D12_CPU_DESCRIPTOR_HANDLE rh = bRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rh.ptr += (SIZE_T)(t < 3 ? t : 3 + (int)s) * bRtvStride;
        list->OMSetRenderTargets(1, &rh, FALSE, nullptr);
        list->SetPipelineState(pp);
        list->DrawInstanced(3, 1, 0, 0);
        b = Transition(r, D3D12_RESOURCE_STATE_RENDER_TARGET, idle);
        list->ResourceBarrier(1, &b);
    };
    int steps[kCrtPostN], ns = 0, lastMain = -1;      // the passes drawn, in order; the last that isn't grain's first half
    for (int p = 0; p < kCrtPostN; p++)
        if (bPso[1 + p] && PostOn(p, cp)) { steps[ns++] = p; if (!PostIsGrainRaw(p)) lastMain = ns - 1; }
    int last = 0;
    into(lastMain < 0 ? 3 : 0, bPso[0], -1);
    for (int i = 0; i < ns; i++)
    {
        const int p = steps[i];
        if (PostIsGrainRaw(p)) { into(2, bPso[1 + p], last); continue; }
        into(i == lastMain ? 3 : 1 - last, bPso[1 + p], last);
        last = 1 - last;
    }
    for (UINT i = 0; i < nin; i++) std::swap(in[i].Transition.StateBefore, in[i].Transition.StateAfter);
    list->ResourceBarrier(nin, in);
    bOk[s] = true;
    static bool logged = false;
    if (!logged) { logged = true; LOGI("BFI Presenter: CRT hand-off: each frame's CRT picture is drawn with the frame (the game's queue); flashes moved by frame warp draw their own"); }
}
// The game side's video memory, once a second (the settings window and the OSD show it)
static void CountBytes()
{
    static double countT = 0.0;
    const double tn = Now();
    if (tn - countT <= 1.0) return;
    countT = tn;
    UINT64 n = 0;
    for (int k = 0; k < kTexN; k++) { n += ResBytes(cur[k]); for (UINT j = 0; j < kStage; j++) n += ResBytes(slotTex[k][j]); }
    for (UINT j = 0; j < kStage; j++) n += ResBytes(bOut[j]);
    for (int k = 0; k < 3; k++) n += ResBytes(bRt[k]);
    g_bytesCrtGame = n;
}
static ID3D12Resource *BakeOut(UINT s) { return s < kStage ? bOut[s] : nullptr; }
static bool BakeReady(UINT s) { return s < kStage && bOk[s] && bOut[s]; }
static UINT BakeGen() { return bOutGen.load(); }
static void BakeRelease()
{
    for (int k = 0; k <= kCrtPostN; k++) SafeRelease(bPso[k]);
    for (int k = 0; k < 3; k++) SafeRelease(bRt[k]);
    for (UINT s = 0; s < kStage; s++) { SafeRelease(bOut[s]); bOk[s] = false; bSeenSrc[s] = nullptr; for (int t = 0; t < kTexN; t++) bSeenTex[s][t] = nullptr; }
    if (bCb && bCbMap) bCb->Unmap(0, nullptr);
    bCbMap = nullptr;
    SafeRelease(bCb); SafeRelease(bHeap); SafeRelease(bRtvHeap); SafeRelease(bRoot);
    bBlobGen = 0; bFailed = false;
    bOutGen++;
}

static void Release()
{
    ReleaseSlots();
    ReleaseCur();
}
static void on_destroy_rt(effect_runtime *) { ReleaseCur(); }

static void Register(bool on)
{
    if (on)
    {
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_rt);
    }
    else
    {
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(on_finish);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_rt);
    }
}
}   // namespace crt
