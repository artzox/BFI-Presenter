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
print(n, 'uniforms;', len(hlsl.splitlines()), 'HLSL lines', file=sys.stderr)
