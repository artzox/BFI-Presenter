# BFI Presenter

Presenter-clocked black frame insertion (BFI) for PC games on fixed-refresh displays, built for 120 Hz OLEDs.

Software BFI usually strobes on the game's frame clock, so any late frame leaves a lit or dark refresh on screen too long: a flash. BFI Presenter takes over presentation and runs the strobe on the **display's own vblank clock**. A late game frame becomes a repeated image (a CRT-like stutter) instead of a flash, and the game is paced to the strobe so frames land on the refresh that shows them.

- **Strobe:** classic BFI with highlight roll-off, or the Variable MPRT brightness budget (highlights spill into the dark refresh instead of clipping); dark floor, sine BFI, inverted cycle, rolling scan with phosphor decay.
- **HDR:** SDR, scRGB and HDR10 output with a display-peak ceiling and an **ABL guard** that keeps bright scenes within an OLED's full-screen power budget.
- **Pacing:** game frame cap at Refresh / Half / Quarter, Smooth or Lowest-latency pacing, just-in-time release so each frame finishes right before its lit refresh, automatic frames ahead, hitch guard.
- **Settings** live in the ReShade overlay and in one `BFIPresenter.ini` per game.

## Which download do I need?

Each [release](../../releases) has two packages with the same version number:

| Package | For | What's in it |
|---|---|---|
| `BFIPresenter-DX-v<version>.zip` | **Direct3D 12 and Direct3D 11** games: 64-bit, and 32-bit Direct3D 11 | `BFIPresenter.addon64` / `BFIPresenter.addon32` (ReShade add-ons), example ini, update scripts |
| `BFIPresenter-Vulkan-v<version>.zip` | **Vulkan** games, emulators (PCSX2, RPCS3, shadPS4…), and **DirectX 8–11** games run through [DXVK](https://github.com/doitsujin/dxvk) — 64- and 32-bit | `BFIPresenterVK.dll` / `BFIPresenterVK32.dll` (a Vulkan layer), installer, a ready `BFIPresenter.ini` to copy into games |

Both read the same `BFIPresenter.ini` with the same keys, so one ini works for either.

## Quick start

**Requirements:**

- Windows 10/11.
- [ReShade](https://reshade.me) 6.4 or newer, the build with full add-on support. The DX add-on needs it; the Vulkan layer also runs without ReShade (settings then come from the ini alone).
- The display at a **fixed 120 Hz** for the game. The presenter's steady presents usually hold it there; where they don't, set G-SYNC off (and Vertical Sync Force on) in the game's NVIDIA profile. The overlay warns when VRR takes over.
- NVIDIA Vertical Sync never "Force off" or "Fast Sync", and no frame limiter acting on the game (RTSS limit 0, NVIDIA Max Frame Rate off).

**DX games**

1. Copy `BFIPresenter.addon64` into the game folder, next to ReShade's DLL (`BFIPresenter.addon32` for a 32-bit game, with the 32-bit ReShade).
2. The add-on creates `BFIPresenter.ini` next to itself on first start. (`BFIPresenter.example.ini` explains every setting.)
3. Start the game and open the ReShade overlay: the **BFI Presenter** tab shows the status and all settings. BFI starts at the game's first swap-chain resize; if it doesn't, change resolution or display mode once.

**Vulkan games**

1. Run `install_vulkan_layer.bat` once (no admin rights needed).
2. Copy `BFIPresenter.ini` from the package next to the game's `.exe`. The layer stays inactive in any game without this file, and it doesn't create one itself.
3. Start the game: the ReShade overlay gets a **BFI Presenter (Vulkan)** tab, and `BFIPresenterVK.log` appears next to the ini.

**Recommended settings:** Refresh cap + Lowest latency when the game holds 120 fps; Half refresh + Smooth (just-in-time release on, the default) when it holds 60 or more but not 120. More in the [DX reference](dx/README.md#recommended-setups).

If you use a CRT shader with its own BFI (for example CRT-Standalone), turn that BFI off (`ENABLE_DECAY=0`) so the two don't stack.

## Documentation

- [DX add-on reference](dx/README.md): how it works, every overlay setting, ini-only keys, hotkeys, files written, known limits.
- [Vulkan layer reference](vulkan/README.md): install, DXVK, log checks, Vulkan-only keys.
- [Tested games](docs/tested-games.md) and per-game notes.
- [Latency estimates](docs/latency.md) for the main setups.
- [Changelog](CHANGELOG.md).

## Updating many games

`scripts/Find-BFIPresenter.ps1` finds every game folder with the add-on or an ini (Steam, Epic, GOG and Xbox folders, plus any `-Roots` you pass) and shows each one's version. `scripts/Update-BFIPresenter.ps1 -Source <extracted release>` copies a new build into all of them and updates the installed Vulkan layer; `-DryRun` previews. Your `BFIPresenter.ini` files are never changed.

**Easiest:** double-click `Find-BFIPresenter.bat`, then `Update-BFIPresenter.bat`, from the extracted release folder (the new build next to the scripts). The `.bat` files start PowerShell with the right options, so Windows' script policy and the "downloaded from the internet" block don't stop them.

From a command prompt, extra options pass through the `.bat` files:

```bat
Find-BFIPresenter.bat -Roots "D:\Games"
Update-BFIPresenter.bat -DryRun
Update-BFIPresenter.bat -Source "D:\Downloads\BFIPresenter-1.0.0" -Backup
```

## Building from source

| Part | MinGW-w64 (Linux, WSL, MSYS2) | Visual Studio |
|---|---|---|
| DX add-on | `dx/build_mingw.sh` | `dx/build_msvc.bat` from an x64 Native Tools prompt |
| Vulkan layer | `vulkan/build_mingw.sh` (64- and 32-bit; recompiles the shaders if `glslangValidator` is installed) | — |

All headers needed are in [`third_party`](third_party/README.md).

## Repository layout

```
dx/          DX add-on source, build scripts, reference
vulkan/      Vulkan layer source, GLSL shaders, manifests, installer, reference
scripts/     Find-/Update-BFIPresenter.ps1
docs/        tested games, latency estimates
third_party/ ReShade, Dear ImGui and Vulkan headers
BFIPresenter.example.ini   shared settings reference
```

## Licence

BFI Presenter is free software under the [GNU General Public License v3.0](LICENSE): you may use, study, share and modify it. If you distribute a modified version, you must release its source under the same licence. The vendored headers in [`third_party`](third_party/README.md) keep their own (permissive) licences.

## Safety

BFI flickers at 60 Hz by design. If you are sensitive to flicker or flashing images, take care, and use the hotkey (Ctrl+Shift+B by default) to turn BFI off.
