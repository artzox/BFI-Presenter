#!/usr/bin/env python3
"""Set the format of every storage image declared as Rgba32f in a SPIR-V module (glslang's HLSL front end declares
RWTexture2D<float4> that way and this version has no [[vk::image_format]]): spvfmt.py <file.spv> <format enum>
(11 = Rgb10A2, 2 = Rgba16f). The image views bound there must have that format."""
import struct, sys
p, fmt = sys.argv[1], int(sys.argv[2])
b = bytearray(open(p, 'rb').read())
w = list(struct.unpack('<%dI' % (len(b) // 4), b))
i, n = 5, 0
while i < len(w):
    wc, op = w[i] >> 16, w[i] & 0xFFFF
    if wc == 0: raise SystemExit('bad SPIR-V')
    if op == 25 and wc >= 9 and w[i + 7] == 2 and w[i + 8] == 1:   # OpTypeImage, Sampled = 2 (storage), Rgba32f
        w[i + 8] = fmt; n += 1
    i += wc
open(p, 'wb').write(struct.pack('<%dI' % len(w), *w))
print('%s: %d storage image format(s) set' % (p, n), file=sys.stderr)
