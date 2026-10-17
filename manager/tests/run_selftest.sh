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
x86_64-w64-mingw32-g++ $F/g_gl.cpp -o "$B/ggl_64.exe" -lopengl32
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
printf 'int main(){return 0;}\n' > "$B/plain.c"; i686-w64-mingw32-gcc "$B/plain.c" -o "$B/plain_32.exe"; x86_64-w64-mingw32-gcc "$B/plain.c" -o "$B/plain_64.exe"
printf '#include <windows.h>\nBOOL WINAPI DllMain(HINSTANCE a, DWORD b, LPVOID c){ if (b == 99) LoadLibraryA("opengl32"); return TRUE; }\n' > "$B/refgl.c"; i686-w64-mingw32-gcc -shared "$B/refgl.c" -o "$B/ref_gl.dll"
mk QuakeRef plain_32.exe QuakeRef.exe; cp "$B/ref_gl.dll" "$G/QuakeRef/ref_gl.dll"
mk HlGame plain_64.exe HlGame.exe; cp "$B/g11_64.exe" "$G/HlGame/directx.hdll"
printf '#include <stdio.h>\nint main(){ puts("vulkan-1.dll"); return 0; }\n' > "$B/vkstr.c"; x86_64-w64-mingw32-gcc "$B/vkstr.c" -o "$B/vkstr_64.exe"
printf 'const char *n[] = {"d3d11.dll","d3d12.dll","opengl32.dll","vulkan-1.dll"};\n' > "$B/sdl.c"; x86_64-w64-mingw32-gcc -shared "$B/sdl.c" -o "$B/SDL3.dll"
mk SdlVk vkstr_64.exe SdlVk.exe; cp "$B/SDL3.dll" "$G/SdlVk/SDL3.dll"; cp "$B/rs_64.dll" "$G/SdlVk/dxgi.dll"
printf '#include <windows.h>\n#include <GL/gl.h>\nint main(){ if (GetTickCount()==1) { glFlush(); LoadLibraryA("d3d11.dll"); } return 0; }\n' > "$B/gld3d.c"; x86_64-w64-mingw32-gcc "$B/gld3d.c" -o "$B/gld3d_64.exe" -lopengl32
mk "The Witcher 3" gld3d_64.exe witcher3.exe
mkdir -p "$G/Witcher GOG/bin/x64_dx12"; cp "$B/ggl_64.exe" "$G/Witcher GOG/bin/x64_dx12/witcher3.exe"
printf '#include <windows.h>\nint main(){ if (GetTickCount()==1) LoadLibraryA("d3d12.dll"); return 0; }\n' > "$B/d3dstr.c"; x86_64-w64-mingw32-gcc "$B/d3dstr.c" -o "$B/d3dstr_64.exe"
printf '#include <windows.h>\n#include <GL/gl.h>\nBOOL WINAPI DllMain(HINSTANCE a, DWORD b, LPVOID c){ if (b == 99) glFlush(); return TRUE; }\n' > "$B/gldll.c"; x86_64-w64-mingw32-gcc -shared "$B/gldll.c" -o "$B/gldll.dll" -lopengl32
mk "GlDllGame" d3dstr_64.exe GlDllGame.exe; cp "$B/gldll.dll" "$G/GlDllGame/helper_gl.dll"
mk OldUnity ggl_32.exe OldUnity.exe; mkdir -p "$G/OldUnity/OldUnity_Data/Managed"
mk ShaderGame g11_64.exe ShaderGame.exe; cp "$B/rs_64.dll" "$G/ShaderGame/dxgi.dll"
printf '[GENERAL]\r\nEffectSearchPaths=.\\fx\\Shaders\\**,.\\other\r\nPresetPath=.\\MyPreset.ini\r\n' > "$G/ShaderGame/ReShade.ini"
printf 'Techniques=CRT@CRT-Standalone.fx\r\nTechniqueSorting=CRT@CRT-Standalone.fx\r\n\r\n[CRT-Standalone.fx]\r\nfoo=1\r\n' > "$G/ShaderGame/MyPreset.ini"
python3 -c "
import zipfile,sys
z=zipfile.ZipFile(sys.argv[1]+'/rs.zip','w',zipfile.ZIP_DEFLATED)
z.write(sys.argv[1]+'/rs_64.dll','ReShade64.dll'); z.write(sys.argv[1]+'/rs_32.dll','ReShade32.dll'); z.close()" "$B"
cat "$B/g11_64.exe" "$B/rs.zip" > "$B/rssetup.exe"
mk "Skyrim VR" g11_64.exe SkyrimVR.exe
# Steam library (VR-only flag from appinfo.vdf) and Battle.net games, found through the registry
S="$W/Steam"; mkdir -p "$S/steamapps/common" "$S/appcache"
for a in "10:Flat Game:FlatGame" "20:Headset Only:HeadsetOnly" "30:Sim VR:SimVR"; do
  id=${a%%:*}; r=${a#*:}; n=${r%%:*}; d=${r#*:}
  mkdir -p "$S/steamapps/common/$d"; cp "$B/g11_64.exe" "$S/steamapps/common/$d/$d.exe"
  printf '"AppState"\n{\n\t"appid"\t\t"%s"\n\t"name"\t\t"%s"\n\t"installdir"\t\t"%s"\n}\n' "$id" "$n" "$d" > "$S/steamapps/appmanifest_$id.acf"
done
printf '"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n' "$(winepath -w "$S" | sed 's/\\/\\\\/g')" > "$S/steamapps/libraryfolders.vdf"
python3 $F/appinfo.py "$S/appcache/appinfo.vdf" 10:0 20:1 30:0
mkdir -p "$W/BNet/Diablo IV"; cp "$B/g11_64.exe" "$W/BNet/Diablo IV/Diablo IV.exe"
U='HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
WINEDEBUG=-all wine reg add 'HKCU\Software\Valve\Steam' /v SteamPath /d "$(winepath -w "$S")" /f >/dev/null
WINEDEBUG=-all wine reg add "$U\Diablo IV" /v DisplayName /d "Diablo IV" /f >/dev/null
WINEDEBUG=-all wine reg add "$U\Diablo IV" /v InstallLocation /d "$(winepath -w "$W/BNet/Diablo IV")" /f >/dev/null
WINEDEBUG=-all wine reg add "$U\Diablo IV" /v UninstallString /d '"C:\ProgramData\Battle.net\Agent\Blizzard Uninstaller.exe" --uid=fenris' /f >/dev/null
WINEDEBUG=-all wine reg add "$U\Battle.net" /v DisplayName /d "Battle.net" /f >/dev/null
WINEDEBUG=-all wine reg add "$U\Battle.net" /v InstallLocation /d "$(winepath -w "$W/BNet")" /f >/dev/null
WINEDEBUG=-all wine reg add "$U\Battle.net" /v UninstallString /d '"C:\ProgramData\Battle.net\Agent\Blizzard Uninstaller.exe" --uid=battle.net' /f >/dev/null
cp -r "$REL" "$W/release"
cp ../../shader/BFIPresenter_Shader.fx "$W/release/"
(cd .. && python3 gen_tested.py >/dev/null)
x86_64-w64-mingw32-g++ -O1 -std=c++17 -municode -static -I .. selftest.cpp ../core.cpp ../unzip.cpp -o "$B/selftest.exe" \
    -lwinhttp -lversion -lshell32 -lole32 -ladvapi32
WINEDEBUG=-all wine "$B/selftest.exe" "$(winepath -w "$G")" "$(winepath -w "$W/release")"
