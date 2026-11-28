#!/bin/sh
# Cross-build the Vulkan layer with MinGW-w64 (Linux, WSL or MSYS2).
# Output: BFIPresenterVK.dll (64-bit) and BFIPresenterVK32.dll (32-bit) here.
# If glslangValidator is installed, the shaders are recompiled into shaders_spv.h first;
# otherwise the committed shaders_spv.h is used.
set -e
cd "$(dirname "$0")"
TP=../third_party
if command -v glslangValidator >/dev/null 2>&1; then
    tmp=$(mktemp -d)
    for f in fullscreen.vert bfi.frag measure.frag measure.comp apl.comp gameflow.comp; do
        glslangValidator -V "shaders/$f" -o "$tmp/$f.spv" >/dev/null
    done
    # BFI compute pass (realtime queue), one per storage format
    glslangValidator -V -DOUT_FMT=rgba8 shaders/bfi.comp -o "$tmp/bfi8.comp.spv" >/dev/null
    glslangValidator -V -DOUT_FMT=rgb10_a2 shaders/bfi.comp -o "$tmp/bfi10.comp.spv" >/dev/null
    glslangValidator -V -DOUT_FMT=rgba16f shaders/bfi.comp -o "$tmp/bfi16.comp.spv" >/dev/null
    # frame warp's passes (brightness, pyramid level, motion, evening out)
    for n in 0 1 2 3; do glslangValidator -V -DPASS=$n shaders/warp.comp -o "$tmp/warp$n.comp.spv" >/dev/null; done
    # CRT hand-off (CrtStandaloneHandoff): the warp into a texture, then CRT-Standalone's own code (HLSL) as compute passes
    glslangValidator -V shaders/crtwarp.comp -o "$tmp/crtwarp.comp.spv" >/dev/null
    python3 tools/crtgen.py ../dx/bfi_crt_gen.h shaders/crt_gen.hlsl
    for e in Sharpen Soften GrainRaw Tail; do
        glslangValidator -V -D -S comp -e main --source-entrypoint CS$e -DBEAM=1 shaders/crt.hlsl -o "$tmp/crt$e.spv" >/dev/null
        python3 tools/spvfmt.py "$tmp/crt$e.spv" 4 2>/dev/null
    done
    for b in 0 1; do
        glslangValidator -V -D -S comp -e main --source-entrypoint CSPattern -DBEAM=$b shaders/crt.hlsl -o "$tmp/crtPattern$b.spv" >/dev/null
        python3 tools/spvfmt.py "$tmp/crtPattern$b.spv" 4 2>/dev/null
    done
    # frame interpolation: the newer frame's motion pushed forward to the moment shown (clear, land, write, fill)
    for n in 0 1 2 3; do glslangValidator -V -DPASS=$n shaders/ipsplat.comp -o "$tmp/ipsplat$n.comp.spv" >/dev/null; done
    python3 tools/genspv.py "$tmp" shaders_spv.h
    rm -rf "$tmp"
    echo "shaders recompiled into shaders_spv.h"
fi
F="-std=c++17 -O2 -shared -static -D_WIN32_WINNT=0x0A00 -I $TP/vulkan-headers -I $TP/reshade/include -I $TP/mingw-shim
   -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Wno-attributes"
x86_64-w64-mingw32-g++ $F bfi_presenter_vk.cpp -o BFIPresenterVK.dll -luser32 -lpsapi
i686-w64-mingw32-g++ $F -msse2 -mfpmath=sse -Wl,--kill-at bfi_presenter_vk.cpp -o BFIPresenterVK32.dll -luser32 -lpsapi
echo "built vulkan/BFIPresenterVK.dll and BFIPresenterVK32.dll"
