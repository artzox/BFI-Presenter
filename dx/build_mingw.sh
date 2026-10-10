#!/bin/sh
# Cross-build the DX add-on with MinGW-w64 (Linux, WSL or MSYS2). Output: BFIPresenter.addon64 here.
set -e
cd "$(dirname "$0")"
TP=../third_party
x86_64-w64-mingw32-g++ -O2 -std=c++17 -shared -static \
    -I "$TP/reshade/include" -I "$TP/mingw-shim" \
    bfi_presenter.cpp -o BFIPresenter.addon64 \
    -ld3d12 -ldxgi -luser32 -ldxguid -lole32 -ladvapi32
echo "built dx/BFIPresenter.addon64"
