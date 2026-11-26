#!/usr/bin/env python3
"""Generate dx/bfi_crt_gen.h from CRT-Standalone.fx: the shader's pattern stage (mask, scanlines, spot size, beam
bloom, re-encode, brightboost, vignette, corner shadow, halation and glow added) as HLSL for the presenter, plus the
table of CRT-Standalone uniforms it reads (name, type, slot in the presenter's constant array).

Usage: gen_crt_hlsl.py <CRT-Standalone.fx> <out.h>
The code is copied, not rewritten: uniforms become macros reading the presenter's constant array, ReShade's samplers
become small functions the presenter defines (CrtBB, CrtHalo, CrtGlow, CrtGlowWide)."""
import re, sys

src = open(sys.argv[1], encoding='utf-8').read().splitlines()
def find(pat, start=0):
    for i in range(start, len(src)):
        if pat in src[i]: return i
    raise SystemExit('not found: ' + pat)
def block(a, b): return src[a:b + 1]

out = []
# helpers: Bezier (+ its matrix), gauss .. crt_scanline_roll
i = find('static const float4x4 kCubicBezier'); j = find('return dot(tv, mul(kCubicBezier, cp));', i) + 1
out += block(i, j)
i = find('float gauss(float x, float sigma)'); j = find('return v0 * 0.5 + (v1 + vm) * 0.25;', i) + 1
out += block(i, j)
# the mask
i = find('#define CRT_REFERENCE_WIDTH 3840.0'); j = find('return saturate(lerp(1.0, mask, strength));', i) + 1
out += block(i, j)
i = find('float megatron_scanline('); j = find('}', i)
out += block(i, j)
for name in ('uint grain_uhash(uint x)', 'float  grain_unorm1(uint u)', '#define glin(x)'):
    out.append(src[find(name)])
out.append(src[find('float3 crt_to_linear(float3 x)')])
out.append(src[find('float3 crt_from_linear(float3 x)')])

# the main pass: burn-in offsets, then from the gamma decode to the glow
m = find('void crt_main_PS(')
b0 = find('#if ENABLE_BURNIN_PHASE', m); b1 = find('float2 orbit_offs   = float2(0.0, 0.0);', b0) + 1
g0 = find('// -- CRT gamma decode --', m); g1 = find('color = float4(c, 1.0);', g0) - 1
tail = [l for l in block(g0, g1) if 'BFI_PRESENTER_HANDOFF' not in l]  # the shader's own hand-off switch
body = ['float3 CrtPattern(float3 c, float2 fc, float2 texcoord)', '{'] + block(b0, b1) + tail + ['    return c;', '}']
out += body


# The passes after the main pass that work on the patterned picture (1.0.49): with the hand-off they are drawn by the
# presenter too, after the pattern, each a full-screen pass (they read the pixels around each one). Each is copied with
# the shader's own functions it calls; ReShade's back buffer is the pass before (CrtPrev), and a pass that is off
# ("discard": the back buffer left as it was) passes the picture on.
POST = [('ENABLE_SHARPEN', 'crt_sharpen_PS', 'Sharpen'), ('ENABLE_SCANLINE_SOFTEN', 'crt_soften_PS', 'Soften'),
        ('ENABLE_GRAIN', 'crt_grain_merged_PS', 'GrainRaw'), ('ENABLE_GRAIN', 'crt_grain_diffuse_PS', 'Grain'),
        ('ENABLE_NOISE_FLOOR', 'crt_noise_floor_PS', 'NoiseFloor'), ('ENABLE_GAMUT_EXPAND', 'crt_gamut_expand_PS', 'Gamut'),
        ('ENABLE_CORNER_ROUND', 'crt_corner_round_PS', 'Corner')]
fdef = {}
for i, l in enumerate(src):
    mm = re.match(r'^(?:static\s+)?(?:float[234]?(?:x[234])?|void|int[234]?|uint[234]?|bool[234]?)\s+([A-Za-z_]\w*)\s*\(', l)
    if mm: fdef.setdefault(mm.group(1), i)
def func_lines(i):
    d = 0; seen = False
    for j in range(i, len(src)):
        d += src[j].count('{') - src[j].count('}')
        if '{' in src[j]: seen = True
        if seen and d == 0: return src[i:j + 1]
    raise SystemExit('unbalanced at %d' % i)
have = set(re.findall(r'^(?:static\s+)?(?:float[234]?(?:x[234])?|void|int[234]?|uint[234]?|bool[234]?)\s+([A-Za-z_]\w*)\s*\(', '\n'.join(out), re.M))
have |= set(re.findall(r'^\s*#define\s+([A-Za-z_]\w*)', '\n'.join(out), re.M))
mdef = {}
for i, l in enumerate(src):
    mm = re.match(r'^\s*#define\s+([A-Za-z_]\w*)', l)
    if mm and not mm.group(1).startswith('ENABLE_') and mm.group(1) not in ('BFI_PRESENTER_HANDOFF', 'PIPELINE'): mdef.setdefault(mm.group(1), i)
cdef = {}
for i, l in enumerate(src):
    mm = re.match(r'^static\s+const\s+\w+\s+([A-Za-z_]\w*)', l)
    if mm: cdef.setdefault(mm.group(1), i)
have |= set(re.findall(r'^static\s+const\s+\w+\s+([A-Za-z_]\w*)', '\n'.join(out), re.M))
def const_lines(i):
    for j in range(i, len(src)):
        if ';' in src[j]: return src[i:j + 1]
deps = []
def need(lines):
    for name in re.findall(r'\b([A-Za-z_]\w*)\s*\(', '\n'.join(lines)) + re.findall(r'\b([A-Za-z_]\w*)\b', '\n'.join(lines)):
        if name in have: continue
        if name in fdef and not name.endswith('_PS'):
            have.add(name); body = func_lines(fdef[name]); need(body); deps.append(body)
        elif name in cdef:
            have.add(name); ln = const_lines(cdef[name]); need(ln); deps.append(ln)
        elif name in mdef:
            have.add(name); ln = [src[mdef[name]]]; need(ln); deps.append(ln)
pbodies = []
for sw, fn, nm in POST:
    body = func_lines(fdef[fn]); need(body)
    pbodies.append((sw, nm, body))
ptext = []
for d in deps: ptext += d
for sw, nm, body in pbodies:
    outn = re.search(r'out\s+float4\s+(\w+)', '\n'.join(body[:6])).group(1)
    body = [re.sub(r'\bdiscard\s*;', '{ ' + outn + ' = CrtPrev(texcoord); return; }', l) for l in body]
    ptext += [f'#if {sw}'] + body + [f'float4 PSCrt{nm}(float4 pos : SV_Position) : SV_Target',
              '{', '    float4 o;', f'    {body[0].split("(")[0].split()[-1]}(pos, pos.xy * CrtPixelSize, o);', '    return o;', '}', '#endif']
ptext = '\n'.join(ptext)
ptext = ptext.replace('tex2D(ReShade::BackBuffer, ', 'CrtPrev(').replace('tex2D(crt_grain_raw_samp, ', 'CrtGrainRaw(')
ptext = ptext.replace('ReShade::PixelSize', 'CrtPixelSize')
ptext = re.sub(r'\bdiscard\s*;', '{ color = CrtPrev(texcoord); return; }', ptext)
ptext = ptext.replace('tex2D(ReShade::BackBuffer, ', 'CrtPrev(')
if 'tex2D(' in ptext or 'ReShade::' in ptext:
    raise SystemExit('post passes: unhandled sampler: ' + '\n'.join(l for l in ptext.splitlines() if 'tex2D(' in l or 'ReShade::' in l))
out.append('#if CRT_POST')
out.append(ptext)
out.append('#endif')
post_names = [(sw, nm) for sw, fn, nm in POST]
text = '\n'.join(out)
text = text.replace('tex2D(ReShade::BackBuffer, ', 'CrtBB(').replace('ReShade::PixelSize', 'CrtPixelSize')
text = text.replace('tex2D(crt_halation_v_sampler, ', 'CrtHalo(').replace('tex2D(crt_glow_v_sampler, ', 'CrtGlow(')
text = text.replace('tex2D(crt_glow_wide_v_sampler, ', 'CrtGlowWide(')
if 'tex2D(' in text or 'ReShade::' in text:
    bad = [l for l in text.splitlines() if 'tex2D(' in l or 'ReShade::' in l]
    # the supersampling block (ENABLE_SCANLINE_SS) samples the frame itself: not supported, compiled out
    print('note: left for ENABLE_SCANLINE_SS=0 only:', len(bad), 'lines', file=sys.stderr)

# uniforms used, with their types from the .fx
decl = {}
for l in src:
    mm = re.match(r'\s*uniform\s+(float3|float2|float4|float|int|uint|bool)\s+([A-Za-z_][A-Za-z0-9_]*)', l)
    if mm: decl.setdefault(mm.group(2), mm.group(1))
# their initial values (ReShade's performance mode compiles them in as constants: the presenter then takes them from the
# preset, and where the preset has none, from here)
full = '\n'.join(src)
init = {}
for mm in re.finditer(r'uniform\s+(?:float[234]?|int|uint|bool)\s+([A-Za-z_][A-Za-z0-9_]*)\s*', full):
    i = mm.end()
    if full[i] == '<':                       # the annotations, to their closing '>' outside string literals
        q = False
        while True:
            i += 1
            ch = full[i]
            if ch == '\\' and q: i += 1
            elif ch == '"': q = not q
            elif ch == '>' and not q: i += 1; break
    rest = full[i:full.index(';', i)].strip()
    if not rest.startswith('='): continue    # no initial value (source = "timer" and the like)
    v = rest[1:].strip()
    if v in ('true', 'false'): vals = [1.0 if v == 'true' else 0.0]
    else: vals = [float(x) for x in re.findall(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?', re.sub(r'^\w+\(', '(', v))]
    init.setdefault(mm.group(1), (vals + [0.0] * 4)[:4])
used = sorted(set(re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\b', text)) & set(decl))
slots = []
for k, name in enumerate(used):
    t = decl[name]
    acc = {'float': f'(gCrtP[{k}].x)', 'int': f'((int)gCrtP[{k}].x)', 'uint': f'((uint)gCrtP[{k}].x)',
           'bool': f'(gCrtP[{k}].x > 0.5)', 'float2': f'(gCrtP[{k}].xy)', 'float3': f'(gCrtP[{k}].xyz)', 'float4': f'(gCrtP[{k}])'}[t]
    slots.append((name, t, k, acc))
n = len(slots)
hdr = [f'#define CRT_SLOTS {n + 2}',
       f'cbuffer CrtParams : register(b1) {{ float4 gCrtP[{n + 2}]; }};']
hdr += [f'#define {name} {acc}' for name, t, k, acc in slots]
hdr += [f'#define BUFFER_WIDTH (gCrtP[{n}].x)', f'#define BUFFER_HEIGHT (gCrtP[{n}].y)']
hlsl = '\n'.join(hdr) + '\n' + text + '\n'

def cstr(s):
    return '\n'.join('"' + l.replace('\\', '\\\\').replace('"', '\\"') + '\\n"' for l in s.splitlines())
types = {'float': 'f', 'int': 'i', 'uint': 'u', 'bool': 'b', 'float2': '2', 'float3': '3', 'float4': '4'}
with open(sys.argv[2], 'w') as f:
    f.write('// Generated by tools/crt-handoff/gen_crt_hlsl.py from CRT-Standalone.fx -- do not edit by hand.\n#pragma once\n')
    f.write(f'static const int kCrtParamN = {n};      // CRT-Standalone uniforms read, one float4 slot each\n')
    f.write(f'static const int kCrtSlots = {n + 2};   // + the frame size slot, + a spare\n')
    f.write('struct CrtParam { const char *name; char type; float def[4]; };   // def: its initial value in the .fx\n')
    f.write('static const CrtParam kCrtParams[] = {\n')
    for name, t, k, acc in slots:
        d = init.get(name, [0.0] * 4)
        f.write(f'    {{ "{name}", \'{types[t]}\', {{ {", ".join(repr(float(x)) + "f" for x in d)} }} }},\n')
    f.write('};\n')
    f.write('static const char kCrtHlsl[] =\n' + cstr(hlsl) + ';\n')
    f.write('// the passes after the pattern, in order: the switch each needs, its entry point (PSCrt<name>)\n')
    f.write('struct CrtPost { const char *sw; const char *entry; };\n')
    f.write('static const CrtPost kCrtPost[] = {\n')
    for sw, nm in post_names: f.write(f'    {{ "{sw}", "PSCrt{nm}" }},\n')
    f.write('};\n')
    f.write(f'static const int kCrtPostN = {len(post_names)};\n')
print(n, 'uniforms;', len(hlsl.splitlines()), 'HLSL lines', file=sys.stderr)
