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
};
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
static ID3DBlob *blob = nullptr;                // compiled for blobDefs
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
    if (ok)
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
static void CopyAtPresent(ID3D12GraphicsCommandList *list, UINT s)
{
    bool ok = false;
    if (state.load() == 2 && curOk && Wanted())
    {
        ok = true;
        for (int k = 0; k < kTexN && ok; k++)
        {
            if (!curHas[k]) { if (slotTex[k][s]) { SafeRelease(slotTex[k][s]); slotGen++; } continue; }
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

static void ReleaseSlots()
{
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
    "float4 CrtGlowWide(float2 uv) { return crtGlowWide.SampleLevel(linClamp, uv, 0); }\n";
static const char kCrtApply[] =
    "float3 CrtApply(float3 c, float2 cp)\n"
    "{\n"
    "    // the mask and scanlines at the screen's pixel; the picture, halation and glow from where it came from (moved)\n"
    "    // what was drawn after the effects (ReShade's window, an overlay) is shown as it is, without the pattern\n"
    "    const int2 q = int2(cp);\n"
    "    if (any(abs(src.Load(int3(q, 0)).rgb - crtBase.Load(int3(q, 0)).rgb) > 0.0005)) return c;\n"
    "    return CrtPattern(c, cp, gSrcPos * CrtPixelSize);\n"
    "}\n";

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
        ID3DBlob *b = nullptr, *err = nullptr;
        if (compile && at != std::string::npos)
        {
            std::string full = "#define CRT_HANDOFF 1\n" + defs + srcText.substr(0, at) + kCrtGlue + kCrtHlsl + kCrtApply + srcText.substr(at + strlen(marker));
            HRESULT hr = compile(full.data(), full.size(), "bfi_presenter_crt", nullptr, nullptr, "PSMain", "ps_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err);
            if (FAILED(hr))
            {
                LOGE("BFI Presenter: CRT hand-off: the shader failed to compile: %s", err ? (const char *)err->GetBufferPointer() : "?");
                SafeRelease(b);
            }
            SafeRelease(err);
        }
        {
            std::lock_guard<std::mutex> l(blobLk);
            SafeRelease(blob);
            blob = b;
            blobDefs = defs;                                        // tried: not again until the switches change
            blobGen++;
        }
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
// The presenter: the compiled shader (AddRef'd) if newer than `have`.
static ID3DBlob *TakeBlob(UINT *gen)
{
    std::lock_guard<std::mutex> l(blobLk);
    if (blobGen.load() == *gen) return nullptr;
    *gen = blobGen.load();
    if (blob) blob->AddRef();
    return blob;
}
// The presenter: this frame's settings, with the frame size (w, h).
static void ParamsFor(float *out, float w, float h)
{
    std::lock_guard<std::mutex> l(lk);
    memcpy(out, params, sizeof(params));
    out[kCrtParamN * 4 + 0] = w;
    out[kCrtParamN * 4 + 1] = h;
}

static int StateNow() { return state.load(); }
static UINT SlotGen() { return slotGen.load(); }

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
