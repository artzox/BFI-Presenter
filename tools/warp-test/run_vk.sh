#!/bin/sh
# Frame warp test for the Vulkan layer (Linux with a Vulkan driver; Mesa's lavapipe works without a GPU):
# the layer's warp passes and BFI compute shader on the synthetic frames, checked like the DX test.
#   ./run_vk.sh        (needs python3 with numpy and scipy, g++, libvulkan; VK_ICD_FILENAMES picks the driver)
set -e
cd "$(dirname "$0")"
python3 - <<'PY'
import re
s=open('../../vulkan/bfi_presenter_vk.cpp',encoding='utf-8').read()
m=re.search(r'\nstruct Constants\n\{.*?\n\};\n',s,re.S)
open('gen_constants_vk.h','w').write('#pragma once\n'+m.group(0))
PY
[ -f f0.rgba ] || python3 make_frames.py
g++ -O2 -std=c++17 -I ../../third_party/vulkan-headers vk_warp_test.cpp -o vk_warp_test -ldl
./vk_warp_test
python3 check.py 60 34
