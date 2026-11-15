#!/bin/sh
# Frame warp test (Linux, WSL or MSYS2 with MinGW-w64; Wine with a Vulkan driver to run it, or copy warp_test.exe and
# the .rgba frames to Windows). Needs python3 with numpy and scipy, and d3dcompiler_47.dll next to warp_test.exe.
#   ./run.sh [finest level: 1 = 8x8 blocks (default), 0 = 4x4]
set -e
cd "$(dirname "$0")"
g++ -O2 -std=c++17 clock_test.cpp -o clock_test && ./clock_test
python3 extract_presenter.py
[ -f f0.rgba ] || python3 make_frames.py
x86_64-w64-mingw32-g++ -O1 -std=c++17 -static -I ../../third_party/mingw-shim warp_test.cpp -o warp_test.exe -ld3d12 -ldxgi -ldxguid
WINEDEBUG=-all WINEDLLOVERRIDES="d3dcompiler_47=n" ${WINE:-wine} warp_test.exe ${1:-1}
if [ "${1:-1}" = "0" ]; then python3 check.py 480 270; else python3 check.py 240 135; fi
