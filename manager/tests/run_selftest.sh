#!/bin/sh
# Builds a fake game library and runs the manager's core self-test under Wine.
# Needs MinGW-w64, Wine and an extracted release (default: ../../dist).
set -e
cd "$(dirname "$0")"
REL=${1:-../../dist}
W=$(mktemp -d)
F=fixture
B="$W/bin"; G="$W/Games"; mkdir -p "$B" "$G"
rc() { printf '1 VERSIONINFO\nFILEVERSION 1,0,0,0\nBEGIN\n BLOCK "StringFileInfo"\n BEGIN\n  BLOCK "040904b0"\n  BEGIN\n   VALUE "ProductName", "%s"\n   VALUE "ProductVersion", "%s"\n  END\n END\n BLOCK "VarFileInfo"\n BEGIN\n  VALUE "Translation", 0x409, 1200\n END\nEND\n' "$2" "$3" > "$B/$1.rc"; }
rc rs ReShade 6.8.0.2000; rc rs641 ReShade 6.4.1.1900; rc opti OptiScaler 0.7.7
for t in rs rs641 opti; do x86_64-w64-mingw32-windres "$B/$t.rc" -O coff -o "$B/$t.o"; x86_64-w64-mingw32-g++ -shared $F/dll.cpp "$B/$t.o" -o "$B/${t}_64.dll"; done
i686-w64-mingw32-windres "$B/rs.rc" -O coff -o "$B/rs32.o"; i686-w64-mingw32-g++ -shared $F/dll.cpp "$B/rs32.o" -o "$B/rs_32.dll"
x86_64-w64-mingw32-g++ $F/g_d3d11.cpp -o "$B/g11_64.exe" -ld3d11
i686-w64-mingw32-g++ $F/g_d3d9.cpp -o "$B/g9_32.exe" -ld3d9
x86_64-w64-mingw32-g++ $F/g_vk.cpp -o "$B/gvk_64.exe"
i686-w64-mingw32-g++ $F/g_gl.cpp -o "$B/ggl_32.exe" -lopengl32
x86_64-w64-mingw32-g++ $F/g_perf.cpp -o "$B/gperf_64.exe" -ld3d9
pad() { cp "$B/$1" "$2"; head -c ${3}000000 /dev/zero >> "$2"; }   # a bigger copy (data after the PE image)
mk() { mkdir -p "$G/$1"; cp "$B/$2" "$G/$1/$3"; }
mk Wreckfest g11_64.exe Wreckfest_x64.exe; cp "$B/rs_64.dll" "$G/Wreckfest/dxgi.dll"
mk "Alan Wake" g9_32.exe AlanWake.exe; cp "$B/rs_32.dll" "$G/Alan Wake/d3d9.dll"
mk OptiGame g11_64.exe OptiGame.exe; cp "$B/opti_64.dll" "$G/OptiGame/dxgi.dll"; cp "$B/rs641_64.dll" "$G/OptiGame/ReShade64.dll"
mk "Red Dead Redemption 2" gvk_64.exe RDR2.exe
mk "Heretic II" ggl_32.exe Heretic2.exe; cp "$B/rs_32.dll" "$G/Heretic II/opengl32.dll"
mk NoReShade g11_64.exe NoReShade.exe; cp "$B/g11_64.exe" "$G/NoReShade/unins000.exe"
mkdir -p "$G/UEGame/UEGame/Binaries/Win64"; cp "$B/gvk_64.exe" "$G/UEGame/UEGame.exe"
cp "$B/g11_64.exe" "$G/UEGame/UEGame/Binaries/Win64/UEGame-Win64-Shipping.exe"; cp "$B/rs_64.dll" "$G/UEGame/UEGame/Binaries/Win64/dxgi.dll"
mkdir -p "$G/DOOM Eternal"; cp "$B/gvk_64.exe" "$G/DOOM Eternal/DOOMEternalx64vk.exe"; pad gvk_64.exe "$G/DOOM Eternal/idTechLauncher.exe" 20
mkdir -p "$G/Hades II/Ship" "$G/Hades II/Tools" "$G/Hades II/Content"; cp "$B/g11_64.exe" "$G/Hades II/Ship/Hades2.exe"; pad g11_64.exe "$G/Hades II/Tools/F10.exe" 30
mkdir -p "$G/Burnout Paradise Remastered"; cp "$B/g11_64.exe" "$G/Burnout Paradise Remastered/BurnoutPR.exe"; pad g11_64.exe "$G/Burnout Paradise Remastered/BurnoutPR_trial.exe" 5
mk PerfGame gperf_64.exe PerfGame.exe
cp -r "$REL" "$W/release"
x86_64-w64-mingw32-g++ -O1 -std=c++17 -municode -static -I .. selftest.cpp ../core.cpp -o "$B/selftest.exe" \
    -lwinhttp -lversion -lshell32 -lole32 -ladvapi32
WINEDEBUG=-all wine "$B/selftest.exe" "$(winepath -w "$G")" "$(winepath -w "$W/release")"
