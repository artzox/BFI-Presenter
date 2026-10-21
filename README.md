# BFI Presenter

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/artzox)

**Variable MPRT strobing for PC games on fixed-refresh displays: the motion clarity of black frame insertion without giving up brightness.** Developed and tested on a 120 Hz OLED (Sony A95L); higher refresh rates work the same way, with more refreshes per strobe cycle and a real rolling band for rolling scan at 240 Hz and above.

Classic black frame insertion (BFI) shows each frame for one refresh and then blacks out the next. Motion gets sharper because each image is on screen for less time (a lower MPRT, motion picture response time), but the screen is dark half the time, so the picture loses half its brightness.

BFI Presenter implements the **Variable MPRT** method described by [Blur Busters](https://blurbusters.com) instead. Each pixel's light for the whole strobe cycle is front-loaded into the lit refresh, up to the panel's ceiling, and only what doesn't fit spills into the following refresh:

- **Dark and mid-tone pixels**, nearly all of a typical image, fit in the lit refresh and get the full motion-clarity gain of BFI.
- **Bright highlights** that would clip spill over, so they stay on screen a little longer (a variable MPRT per pixel) instead of being dimmed or clipped.
- **Brightness is kept.** At `Gain 1.0` the average light matches the game without strobing. In HDR the panel's headroom (`DisplayPeakNits`) leaves room for most content to fit entirely in the lit refresh, and an **ABL guard** keeps the boosted refresh within an OLED's full-screen power budget so the panel doesn't dim it.

On top of that comes the Blur Busters CRT-simulator approach: **rolling scan with phosphor fade**. The strobe can be split into horizontal bands lit in turn from the top down, each row fading out like CRT phosphor after the simulated beam passes, so the screen never goes fully dark and flicker is softer. At 120 Hz this alternates two halves with the fade hiding the seam; at 240 Hz and above it becomes a real rolling band.

### Why a presenter

Software strobing usually runs on the game's frame clock, so any late frame leaves a lit or dark refresh on screen too long: a flash. BFI Presenter takes over presentation and runs the strobe on the **display's own vblank clock**. A late game frame becomes a repeated image (a CRT-like stutter) instead of a flash, and the game is paced to the strobe so each frame lands on the refresh that shows it.

### Features

- **Strobe:** Variable MPRT (the default) or classic BFI with hue-preserving highlight roll-off (Ctrl+Shift+M switches); frames per cycle, gain, dark-frame floor, sine and inverted cycles; rolling scan with phosphor fade or a soft seam.
- **HDR:** SDR, scRGB and HDR10 output with a display-peak ceiling and the ABL guard.
- **Pacing:** game frame cap at Refresh / Half / Quarter, Smooth or Lowest-latency pacing, just-in-time release so each frame finishes right before its lit refresh (or, as an opt-in for Reflex games with irregular frame delivery, Reflex's frame limit), automatic frames ahead, hitch guard.
- **APIs:** Direct3D 12, 11 and 9 and OpenGL through a ReShade add-on; Vulkan (and DXVK) through a Vulkan layer. Works with frame generation (DLSS FG, FSR FG, Lossless Scaling) at the Refresh cap.
- **Settings** live in the ReShade overlay and in one `BFIPresenter.ini` per game.

## Which download do I need?

Each [release](../../releases) has two packages with the same version number:

| Package | For | What's in it |
|---|---|---|
| `BFIPresenter-DX-v<version>.zip` | **Direct3D 12, 11 and 9 and OpenGL** games, 64- and 32-bit (Direct3D 9 needs ReShade 6.8+; OpenGL needs an NVIDIA GPU) | `BFIPresenter.addon64` / `BFIPresenter.addon32` (ReShade add-ons), example ini, `BFIPresenter_Shader.fx` (the BFI shader, see below) |
| `BFIPresenter-Vulkan-v<version>.zip` | **Vulkan** games, emulators (PCSX2, RPCS3, shadPS4…), and **DirectX 8–11** games run through [DXVK](https://github.com/doitsujin/dxvk) — 64- and 32-bit | `BFIPresenterVK.dll` / `BFIPresenterVK32.dll` (a Vulkan layer), installer, a ready `BFIPresenter.ini` to copy into games |

Both read the same `BFIPresenter.ini` with the same keys, so one ini works for either.

**The BFI shader** (`BFIPresenter_Shader.fx`, in the DX zip) is the same strobe as a ReShade effect, clocked by the game's own frames instead of the display's. It is a fallback for games that hold a rock-steady 120 fps but whose engine stutters with the presenter, or that reach 120 only with NVIDIA Smooth Motion, which the presenter can't use (Blasphemous). Otherwise use the add-on: with the shader every late frame flashes. See [shader/README.md](shader/README.md) and the [tested-games list](docs/tested-games.md#bfi-shader-game-clocked-bfi-as-a-reshade-effect-v105).

**Or let the app do it:** `BFIPresenterManager.exe` (on the same release page) finds your Steam, Epic, GOG, Xbox and other games, works out whether each needs the 64-bit or 32-bit add-on or the Vulkan layer, checks that ReShade is there (also as `ReShade64.dll` behind OptiScaler), and installs, updates or removes BFI Presenter with a backup of every replaced file. See the [manager README](manager/README.md).

## Quick start

**Requirements:**

- Windows 10/11.
- [ReShade](https://reshade.me) 6.4 or newer, the build with full add-on support. The DX add-on needs it; the Vulkan layer also runs without ReShade (settings then come from the ini alone).
- The display at a **fixed refresh rate** for the game (tested at 120 Hz). The presenter's steady presents usually hold it there; where they don't, set G-SYNC off (and Vertical Sync Force on) in the game's NVIDIA profile. The overlay warns when VRR takes over.
- NVIDIA Vertical Sync never "Force off" or "Fast Sync", and no frame limiter acting on the game (RTSS limit 0, NVIDIA Max Frame Rate off).

**Direct3D 12 / 11 / 9 and OpenGL games** (the same add-on covers all of them)

1. Install ReShade for the game's API (for OpenGL, ReShade's DLL is `opengl32.dll`), then copy `BFIPresenter.addon64` into the game folder, next to ReShade's DLL (`BFIPresenter.addon32` for a 32-bit game, with the 32-bit ReShade). OpenGL needs an NVIDIA GPU.
2. The add-on creates `BFIPresenter.ini` next to itself on first start. (`BFIPresenter.example.ini` explains every setting.)
3. Start the game and open the ReShade overlay: the **BFI Presenter** tab shows the status and all settings. BFI starts at the game's first swap-chain resize; if it doesn't, change resolution or display mode once.

**Vulkan games**

1. Run `install_vulkan_layer.bat` once (no admin rights needed).
2. Copy `BFIPresenter.ini` from the package next to the game's `.exe`. The layer stays inactive in any game without this file, and it doesn't create one itself.
3. Start the game: the ReShade overlay gets a **BFI Presenter (Vulkan)** tab, and `BFIPresenterVK.log` appears next to the ini.

**Recommended settings:**

- **Method Variable MPRT, Gain 1.0** for full brightness (Gain 0.5, the default, keeps the lit refresh as the game drew it, at half the average brightness). In HDR, set `AblGuardBudgetNits` to the panel's full-white brightness (250 for the Sony A95L).
- Game holds 120 fps: Refresh cap + Lowest latency (the defaults).
- Game holds 60 or more but not 120: leave the Refresh cap and the automatic frame cap on (it steps down to one frame per strobe cycle by itself), or pick Half refresh.
- **With frame generation** (DLSS FG, FSR FG, Lossless Scaling): the **Refresh** cap when frame generation's output is well above the strobe rate (Half refresh would throw half the generated frames away). In NVIDIA Reflex games the automatic frame cap sets Reflex's frame limit to one presented frame per strobe cycle. When the output is only about the strobe rate (e.g. 60 at 120 Hz: 2× at about 30 rendered fps, 3× at about 20), use **Half refresh + Smooth**: every generated frame gets a flash, smoother and quicker than Reflex's limit (Alan Wake 2 with 3×: 60 of 60 flashes, ~99 ms from input to flash against ~130 ms). Lossless Scaling worked with its VSync set to 1/2.
- **The formula:** strobe rate = refresh rate ÷ frames per cycle, and the game needs at least that many fps for a new image on every flash (with frame generation, count the final fps, generated frames included). E.g. 120 Hz ÷ 2 = 60 fps, 240 Hz ÷ 2 = 120 fps.

More in the [DX reference](dx/README.md#recommended-setups).

If you use a CRT shader with its own BFI (for example CRT-Standalone), turn that off (`ENABLE_DECAY=0`) so the two don't stack.

> [!WARNING]
> **LCD monitors (IPS, VA, TN): turn on "LCD: prevent image retention" (`LcdPhaseSwap=1`).** With an even number of frames per cycle every lit refresh gets the same LCD drive polarity, and long sessions can leave a temporary ghost of the image. The option moves the strobe to the other refresh from time to time, so this evens out: **Immediate** fades and shifts it every few seconds, **Gradual** slides it slowly with no fade or stutter (see the [settings guide](docs/settings-guide.md)). OLED displays don't need it.

> [!IMPORTANT]
> **Special K's SDR-to-HDR (HDR retrofit) does not work with the presenter.** Special K applies its HDR processing, and draws its menu, in its own Present hook, and the presenter takes the game's frames before that hook runs. The image then reaches the display unprocessed but flagged as HDR, and looks heavily over-contrasted, and Special K's menu doesn't open. For SDR games in HDR use **RTX HDR** or a ReShade AutoHDR effect (for example Pumbo's AutoHDR add-on with AdvancedAutoHDR) instead, and turn Special K off for that game (in SKIF, stop global injection or blacklist the game's `.exe`).

## Documentation

- [DX add-on reference](dx/README.md): how it works, every overlay setting, ini-only keys, hotkeys, files written, known limits.
- [Vulkan layer reference](vulkan/README.md): install, DXVK, log checks, Vulkan-only keys.
- [Settings guide](docs/settings-guide.md): every setting in plain words, what you'd notice and when to change it.
- [Tested games](docs/tested-games.md) and per-game notes.
- [Latency estimates](docs/latency.md) for the main setups.
- [Changelog](CHANGELOG.md).

## Updating many games

Use the [manager app](manager/README.md): point it at the new release (**Check GitHub**, or **Use folder...** with both zips extracted into one folder) and press **Update outdated**. Every replaced file is backed up, and **Undo last change** puts it back per game.

The older PowerShell scripts (`scripts/Find-BFIPresenter.ps1`, `scripts/Update-BFIPresenter.ps1`) stay in the repository for anyone who prefers them, but are no longer part of the release zips.

## Building from source

| Part | MinGW-w64 (Linux, WSL, MSYS2) | Visual Studio |
|---|---|---|
| DX add-on | `dx/build_mingw.sh` | `dx/build_msvc.bat` from an x64 Native Tools prompt |
| Manager app | `manager/build_mingw.sh` | — |
| Vulkan layer | `vulkan/build_mingw.sh` (64- and 32-bit; recompiles the shaders if `glslangValidator` is installed) | — |

All headers needed are in [`third_party`](third_party/README.md).

## Repository layout

```
dx/          DX add-on source, build scripts, reference
vulkan/      Vulkan layer source, GLSL shaders, manifests, installer, reference
manager/     BFIPresenterManager.exe source (finds games, installs and updates)
shader/      BFIPresenter_Shader.fx (the BFI shader: game-clocked BFI as a ReShade effect)
scripts/     Find-/Update-BFIPresenter.ps1 (older command-line updater)
docs/        tested games, latency estimates
third_party/ ReShade, Dear ImGui and Vulkan headers
BFIPresenter.example.ini   shared settings reference
```

## Licence

BFI Presenter is free software under the [GNU General Public License v3.0](LICENSE): you may use, study, share and modify it. If you distribute a modified version, you must release its source under the same licence. The vendored headers in [`third_party`](third_party/README.md) keep their own (permissive) licences.

## Safety

Strobing flickers at 60 Hz by design (rolling scan with phosphor fade softens it). If you are sensitive to flicker or flashing images, take care, and use the hotkey (Ctrl+Shift+B by default) to turn the strobe off.

## No warranty — use at your own risk

BFI Presenter is provided "as is", without warranty of any kind, and the author is not liable for any damage or loss arising from its use, as set out in sections 15 and 16 of the [GPL](LICENSE). You install it into your own games, together with third-party tools such as ReShade, and you are responsible for your own hardware, drivers and settings.

What it does and doesn't touch: it only changes the frames a game shows while it runs. It does not change clocks, voltages, firmware, boot settings or the display's own settings. The manager writes only to game folders (every replaced file is backed up, with undo), one per-user registry entry for the Vulkan layer, ReShade's own files (`ReShade.ini`, its preset, the `reshade-shaders` folder, `ReShadeApps.ini`), the games' NVIDIA driver profiles (the settings NVIDIA Profile Inspector changes) and its own folder in `%LOCALAPPDATA%\BFIPresenter`.
