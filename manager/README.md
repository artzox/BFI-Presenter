# BFI Presenter Manager

A small Windows app that finds your games, works out what each one needs, and installs, updates or removes BFI Presenter in them. It is one `.exe` with nothing to install; settings and backups live in `%LOCALAPPDATA%\BFIPresenter`.

## What it does

- **Finds games** in Steam, Epic, GOG and Xbox libraries, plus any folders you add (**Game folders...**: each sub-folder is taken as one game).
- **Picks the game's executable.** For Unreal games it takes `<Game>\Binaries\Win64\<Game>-Win64-Shipping.exe`, not the small launcher in the root; any folder that already has ReShade or the add-on wins. You can choose another `.exe` per game.
- **Reads the executable** to tell 32- from 64-bit and to see which graphics API it uses (Direct3D 12, 11, 9, OpenGL, Vulkan; Unity games through `UnityPlayer.dll`). From that it picks the route: `BFIPresenter.addon64`, `BFIPresenter.addon32` or the Vulkan layer. Games that support both Direct3D 12 and Vulkan can be switched with **Install as**.
- **Finds ReShade by what the DLL is, not by its name:** `dxgi.dll`, `d3d9.dll`, `d3d11.dll`, `d3d12.dll`, `opengl32.dll`, `dinput8.dll`, and `ReShade64.dll` / `ReShade32.dll` as loaded by OptiScaler or other loaders. Each candidate's version information is checked, so OptiScaler's `dxgi.dll` isn't mistaken for ReShade. The add-on goes into the folder of the ReShade DLL (or the add-on folder set in `ReShade.ini`).
- **Shows what else is there:** OptiScaler, DXVK, dgVoodoo, Special K, the ReShade version (Direct3D 9 needs 6.8+), and the game's entry from the [tested-games list](../docs/tested-games.md) with its key requirement.
- **Installs, updates, removes.** Every file it replaces or deletes is kept in `%LOCALAPPDATA%\BFIPresenter\backups`; **Undo last change** puts it back (click again to go one more step back). All changes are listed in `manager_log.csv`.
- **Update all** brings every game with an older add-on (or one for the wrong bitness) to the selected release, and the Vulkan layer too.
- **Vulkan layer:** installs it for your Windows user (no admin rights), the same way `install_vulkan_layer.bat` does, and copies `BFIPresenter.ini` next to Vulkan games' executables (that file turns the layer on in a game).

## Where the files come from

- **Check GitHub** downloads the latest release of [artzox/BFI-Presenter](https://github.com/artzox/BFI-Presenter) into `%LOCALAPPDATA%\BFIPresenter\releases`. When you use a downloaded release, the app checks once per start for a newer one (`AutoCheck=0` under `[Manager]` in `manager.ini` turns that off).
- **Use folder...** takes an extracted release or a test build from any folder instead.

## What it doesn't do

- Install ReShade itself: get it from [reshade.me](https://reshade.me) (the build with full add-on support).
- Change NVIDIA driver profiles: the notes say when a game needs a setting there.
- Edit `BFIPresenter.ini`: the add-on writes it on first start, and the overlay in the game changes it.

## Building

`./build_mingw.sh` (MinGW-w64 on Linux, WSL or MSYS2) builds `BFIPresenterManager.exe`. It first turns `docs/tested-games.md` into `tested_games.inc` with `gen_tested.py`. Dear ImGui is vendored in `third_party/imgui`.

`tests/run_selftest.sh` builds a fake game library (D3D11, D3D9, OpenGL, Vulkan, an Unreal-style layout, OptiScaler with `ReShade64.dll`) and checks detection, install, update, wrong-bitness replacement, remove, undo and the Vulkan layer under Wine.
