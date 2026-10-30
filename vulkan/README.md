# BFI Presenter — Vulkan layer

The same presenter-clocked black frame insertion as the [DX add-on](../dx/README.md), for Vulkan games, emulators (PCSX2, RPCS3, shadPS4 and others) and DirectX 8–11 games run through DXVK. It reads the same `BFIPresenter.ini` and the same keys. See the [changelog](../CHANGELOG.md) for what changed.

## How it works

- **It's a Vulkan layer, not a ReShade add-on file.** Vulkan lets small plug-ins ("layers") sit between the game and the driver, and that's the only place the presenter can take over the swap chain. The layer sits *below* ReShade:
  - The game and ReShade render into stand-in images.
  - Each finished frame, with ReShade's effects and the CRT shader already applied, is copied into a small staging ring.
  - The presenter thread strobes it at vblank on its own GPU queue, exactly like the DX version.
- **Settings window:** when ReShade is in the game, the layer adds a **BFI Presenter (Vulkan)** tab to the ReShade overlay. ReShade isn't required: without it, edit the ini (the layer picks up the change within a second; Ctrl+Shift+R reloads at once) (Ctrl+Shift+B toggles BFI, Ctrl+Shift+M the method, Ctrl+Shift+S rolling scan, Ctrl+Shift+O rebuilds the swap chain, Ctrl+Shift+K puts a marker in the log). DOOM Eternal runs this way.
- **Off unless asked:** the layer does nothing at all in a game whose folder has no `BFIPresenter.ini`. It writes no files and changes nothing there.

## Install (once)

1. Unzip anywhere and run **`install_vulkan_layer.bat`**. No admin rights are needed.
   - It copies the layer (64-bit and 32-bit) to `%LOCALAPPDATA%\BFIPresenter\Vulkan`.
   - It registers the layer for your Windows user.
   - **If games run as administrator on your PC** (User Account Control turned off, or Steam or the game set to run as administrator), run it with right-click → **Run as administrator**: it then registers the layer for all users too. Windows' Vulkan loader ignores per-user layers in programs run as administrator, so without this the layer never loads (no `BFIPresenterVK.log`). The BFI Presenter Manager does the same with its **All users** button and warns when it sees these settings.
   - It lists the Vulkan layers in load order. **ReShade must be listed before BFI Presenter.**
2. If you install or reinstall ReShade later, run `install_vulkan_layer.bat` again so BFI Presenter is listed after ReShade.
3. To update, run the new package's `install_vulkan_layer.bat`. Games pick up the new layer on their next start; your `BFIPresenter.ini` files stay as they are.
4. To remove it, run `uninstall_vulkan_layer.bat` (as administrator if it was registered for all users).

## Use in a game

1. Copy `BFIPresenter.ini` from the package into the game folder, next to its `.exe` (the layer doesn't create it; it only switches on where the file exists). The ini from a DX game works as-is. For emulators, put it next to the emulator's `.exe`.
2. Start the game. `BFIPresenterVK.log` appears next to the ini, and the ReShade overlay gets the **BFI Presenter (Vulkan)** tab.
3. Use the same per-game setup as for DX games:
   - G-SYNC off in the game's NVIDIA profile if the display doesn't hold its fixed refresh rate (120 Hz in testing).
   - VSync never Force off or Fast Sync.
   - RTSS limit 0 for the game.
   - Reflex off.
   - `ENABLE_DECAY=0` in CRT-Standalone.

Don't put the DX add-on (`BFIPresenter.addon64`) in Vulkan game folders. It doesn't harm anything, but it adds a second, inactive tab.

## DirectX 8, 9, 10 and 11 games (through DXVK)

[DXVK](https://github.com/doitsujin/dxvk/releases) turns a DirectX 8/9/10/11 game into a Vulkan game, which the layer then presents like any other Vulkan game.

1. **Check the game's bitness.** In Task Manager → Details, 32-bit games show "(32 bit)", or look the game up on PCGamingWiki.
2. **Copy DXVK's DLLs next to the game's exe.** Use the `x64` folder for 64-bit games and `x32` for 32-bit ones:

   | Game uses | Copy |
   |---|---|
   | DirectX 11 / 10 | `d3d11.dll`, `dxgi.dll` (and `d3d10core.dll` for DX10) |
   | DirectX 9 | `d3d9.dll` |
   | DirectX 8 | `d3d8.dll` and `d3d9.dll` |

3. **Install ReShade for Vulkan, not DirectX.** In ReShade setup, pick the game and choose **Vulkan** as the rendering API. If ReShade was installed for DirectX before, uninstall it from that game first. Otherwise ReShade's `dxgi.dll` / `d3d9.dll` clash with DXVK's.
4. **Put `BFIPresenter.ini` next to the exe**, as for any Vulkan game.
5. **Optional `dxvk.conf` next to the exe:** `dxgi.enableHDR = True` lets DX11 games that support HDR output it.
6. **Check `BFIPresenterVK.log`.** It should show the game's exe and `presenter queue: ... (its own)`. DXVK's own log (`<game>_d3d11.log` / `_d3d9.log`) confirms DXVK loaded.

Notes:

- **Leave frame limiting off in DXVK** (no `dxgi.maxFrameRate` / `d3d9.maxFrameRate`) and in the game. Use the presenter's frame cap.
- **SDR games stay SDR under DXVK.** `SdrToHdr` is skipped there: an HDR10 swap chain makes the NVIDIA driver present through DXGI, which in a DXVK game is DXVK's own `dxgi.dll`, and the presenter hung. Use Gain 0.5 or Variable MPRT. (A game's own HDR through DXVK, `dxgi.enableHDR`, is untested.)
- **Online games with anti-cheat may refuse DXVK.**
- **OpenGL games aren't covered.** There is no DXVK-style path to Vulkan on Windows for them.

## Check the log first

`BFIPresenterVK.log` should contain:

- `presents arrive from ReShade64.dll -- the layer is below ReShade (correct)`.
  - If it says the layer is **ABOVE** ReShade instead, ReShade's effects would run on every strobed refresh. Re-run `install_vulkan_layer.bat`.
- `presenter queue: family 0 index N (its own)`: the presenter has its own GPU queue.
- `vblank pacing on \\.\DISPLAYn`.

No log at all means the layer isn't active: check that `BFIPresenter.ini` is in the same folder as the `.exe` that runs (some games keep it in a subfolder such as `x64\` or `bin\`), and that the game isn't running as administrator.

## Features

- **Strobe:** BFI and Variable MPRT; frames per cycle, gain, dark floor, roll-off, sine and invert; rolling scan with phosphor decay or seam softness.
- **Output:** SDR, scRGB and HDR10 (Auto detection), and SDR games presented as HDR10 with highlight expansion.
- **Pacing:** Lowest latency (default for new ini files) and Smooth; game frame cap (Refresh / Half / Quarter); automatic frame cap at Refresh (one game frame per strobe cycle while the game can't hold the refresh rate, its frame start timed by a search across the strobe cycle where frames would otherwise wait for their flash; back after a ~3 s try that holds it, every 30–120 s, or 1–5 minutes in games well under the refresh rate; with frame generation only in Reflex games, through Reflex's frame limit, while it leaves ~30 rendered fps or more); just-in-time release at Half / Quarter; Later release at Refresh; automatic frames ahead; queue one refresh ahead.
- **Guards:** hitch guard and ABL guard.
- **Logs:** `BFIPresenterVK.log` and a timing log, `BFIPresenterVK_log_<date>_<time>.csv`, with columns close to the DX log's.
- **Exclusive fullscreen and recovery:** the presenter's swap chain runs in exclusive fullscreen, and presentation that has slowed down for good is recovered with a swap chain rebuild (see `VulkanExclusiveFullscreen`, `VulkanAutoRecover` below).
- **Swap chain changes:** recreated swap chains (resolution or fullscreen changes) are taken over straight away; there's no "BFI starts after a resize" step in Vulkan.
- **DXVK pacing:** `VK_KHR_present_wait`, which DXVK uses for frame pacing, is reported in terms of the game's own frames.

## ini keys (Vulkan only)

All other keys are shared with the DX add-on; see the comments in `BFIPresenter.ini` and the [DX reference](../dx/README.md).

| Key | Default | Meaning |
|---|---|---|
| `VulkanLayer` | 1 | 0 = the layer stays off in this game even though the ini is there |
| `VulkanOwnQueue` | 1 | 0 = the presenter shares the game's queue (locked) instead of its own |
| `VulkanExclusiveFullscreen` | 1 | The presenter's own swap chain runs in exclusive fullscreen (`VK_EXT_full_screen_exclusive`, application-controlled), so an overlay or alt-tab can't move presentation into the desktop compositor, where the strobe loses its timing and flashes. The game's own window mode is unchanged. Applies at the next game start |
| `VulkanAutoRecover` | 1 | When the display keeps handing the presenter's frames back late for 5 seconds (a window covered the game, such as a launcher's splash screen, or the ReShade menu was open), the game is asked to rebuild its swap chain: a short pause, at most every 20 seconds. The log lists the windows over the game when it happens. Ctrl+Shift+O does it by hand |
| `SdrToHdr` | 1 | SDR games are presented as HDR10 when the display offers it (Windows HDR on), so the lit refresh has headroom. 0 = plain SDR (use Gain 0.5 then). In DXVK games 1 keeps them SDR (HDR10 hung the NVIDIA driver's present there); 2 forces HDR10 there too, together with NVIDIA "Vulkan/OpenGL present method: Prefer native" for the game |
| `SdrWhiteNits` | 200 | Brightness of SDR white in that mode (Windows: 80 + 4 × the SDR content brightness slider) |
| `SdrHighlightNits` | 800 | SDR highlights stretch toward this; 0 = no expansion |
| `SdrHighlightKnee` | 0.65 | SDR signal level where the expansion starts |
| `QueueAhead` | 0 | 1 = keep one extra finished refresh queued; covers late presenter draws in GPU-heavy games at +1 refresh of latency |

Environment variable `DISABLE_BFI_PRESENTER_VK=1` turns the layer off for anything started from that environment. `BFI_PRESENTER_INI=<path>` points the layer at an ini elsewhere.

## Known limits

- **Games run as administrator** don't load per-user Vulkan layers (a Vulkan loader rule): register the layer for all users (see Install).
- **Anti-cheat games** may refuse unknown Vulkan layers.
- **32-bit games through DXVK can't strobe cleanly.** In Twelve Minutes (DX11) and Alan Wake (DX9) the presenter's BFI flickered as if the display showed some refreshes twice, even though its timing log was clean, and the CRT shader's own BFI (decay) flickered the same way without the layer. So the frames don't reach the screen one per refresh through DXVK there; it isn't the layer. HDR10 output also gave a black screen (Alan Wake) or froze the game (Twelve Minutes). For 32-bit Direct3D 11 games use the DX add-on's `BFIPresenter.addon32`; 32-bit Direct3D 9 games have no BFI route for now (CRT-Standalone doesn't run under ReShade's DirectX 9 path either).
- **GPU-heavy games** can still flash when the GPU is saturated (Indiana Jones). Give the GPU headroom, raise the dark floor slightly, or try `QueueAhead=1`.
- **Not yet in the Vulkan layer:** the VRR warning (Vulkan has no frame statistics to drive it), the crash and hang loggers, and realtime GPU priority for the presenter queue.

## Building

From this folder, with MinGW-w64 (Linux, WSL or MSYS2):

```sh
./build_mingw.sh
```

- It builds `BFIPresenterVK.dll` (64-bit) and `BFIPresenterVK32.dll` (32-bit).
- If `glslangValidator` is installed, `shaders/` are recompiled to SPIR-V and embedded through `shaders_spv.h` (`tools/genspv.py`); otherwise the committed `shaders_spv.h` is used.
- Headers come from `../third_party`: the Vulkan headers, the ReShade add-on and Dear ImGui headers, and two case-fix shims for building on case-sensitive file systems.
- The same source builds as a Linux `.so` for testing with a software Vulkan driver.
