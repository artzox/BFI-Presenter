#!/bin/sh
# Cross-build the BFI Motion Test with MinGW-w64 (Linux, WSL or MSYS2): BFIMotionTest.exe (64-bit).
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows -static -s bfi_motion_test.cpp -o BFIMotionTest.exe -ld3d12 -ldxgi -luser32
echo "built tools/motion-test/BFIMotionTest.exe"
