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
    for f in fullscreen.vert bfi.frag measure.frag; do
        glslangValidator -V "shaders/$f" -o "$tmp/$f.spv" >/dev/null
    done
    python3 tools/genspv.py "$tmp" shaders_spv.h
    rm -rf "$tmp"
    echo "shaders recompiled into shaders_spv.h"
fi
F="-std=c++17 -O2 -shared -static -D_WIN32_WINNT=0x0A00 -I $TP/vulkan-headers -I $TP/reshade/include -I $TP/mingw-shim
   -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Wno-attributes"
x86_64-w64-mingw32-g++ $F bfi_presenter_vk.cpp -o BFIPresenterVK.dll -ldxgi -luser32 -lpsapi
i686-w64-mingw32-g++ $F -Wl,--kill-at bfi_presenter_vk.cpp -o BFIPresenterVK32.dll -ldxgi -luser32 -lpsapi
echo "built vulkan/BFIPresenterVK.dll and BFIPresenterVK32.dll"
