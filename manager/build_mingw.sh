#!/bin/sh
# Cross-build the BFI Presenter Manager with MinGW-w64 (Linux, WSL or MSYS2): BFIPresenterManager.exe (64-bit).
set -e
cd "$(dirname "$0")"
IG=../third_party/imgui
python3 gen_tested.py
x86_64-w64-mingw32-windres app.rc -O coff -o app_res.o
x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows -static -s \
    -I "$IG" -I "$IG/backends" -I ../third_party/mingw-shim \
    main.cpp core.cpp \
    "$IG/imgui.cpp" "$IG/imgui_draw.cpp" "$IG/imgui_tables.cpp" "$IG/imgui_widgets.cpp" \
    "$IG/backends/imgui_impl_win32.cpp" "$IG/backends/imgui_impl_dx11.cpp" \
    app_res.o -o BFIPresenterManager.exe \
    -ld3d11 -ld3dcompiler -ldxgi -ldwmapi -lwinhttp -lversion -lshell32 -lole32 -luuid -ladvapi32 -lgdi32 -luser32
rm -f app_res.o
echo "built manager/BFIPresenterManager.exe"
