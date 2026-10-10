# BFI Presenter — DX add-on (Direct3D 12 and 11)

Presenter-clocked black frame insertion (BFI) for PC games. The lit/dark strobe runs on the display's own refresh clock, not the game's. A late game frame becomes a repeated image (CRT-like stutter) instead of a lit or dark refresh held too long (a flash). It includes a just-in-time frame cap that keeps latency low and motion even, an ABL guard for OLED panels, and a rolling-scan mode.

The [Vulkan layer](../vulkan/README.md) covers Vulkan games and DirectX 9–11 games run through DXVK. It reads the same `BFIPresenter.ini`. Both are released together with the same version number; see the [changelog](../CHANGELOG.md).

## How it works

- **Direct3D 12:** the game and ReShade (including CRT-Standalone) render into stand-in back buffers. Each finished frame is copied into a ring of six staging images. A high-priority presenter thread waits for every vblank, picks a game frame, draws the strobe refresh from it, and presents.
- **Direct3D 11:** the game keeps its own DX11 device (so DLSS and ReShade work as usual). Its frames are copied to shared textures, and the add-on's own D3D12 presenter shows them through a window laid over the game's. Use a borderless or windowed mode.
- **The strobe:** each refresh is one sub-frame of the cycle (2 at 120 Hz: one lit, one dark). The shader applies gain in display light (SDR, scRGB or HDR10), BFI with highlight roll-off or the Variable MPRT brightness budget, dark floor, sine BFI, inverted cycle and rolling scan.
- **Frame cap and release:** the presenter lets the game start each frame (through its frame-latency waitable and Present) on its own refresh clock. The game therefore runs exactly in step with the strobe. Just-in-time release times that so frames finish right before the refresh that shows them.

## Requirements

- ReShade **6.4 or newer, the build with full add-on support** (tested on 6.8).
- A 64-bit **Direct3D 12 or Direct3D 11** game. 32-bit games need the Vulkan layer through DXVK.
- **Fixed refresh (120 Hz), VRR off for the game** (G-SYNC off in its NVIDIA profile, or VRR off on the TV), unless the presenter holds 120 Hz on its own (the overlay warns when it doesn't).
- **No frame limiter acting on the presenter:** RTSS limit 0 for BFI games, NVIDIA Max Frame Rate off (also in NVIDIA Profile Inspector).
- **NVIDIA Vertical Sync:** "Use the 3D application setting" or Force on, never Force off or Fast Sync.
- Windows 10/11 (uses the built-in `d3dcompiler_47.dll`).

## Install, update, uninstall

1. Copy `BFIPresenter.addon64` into the game folder, next to ReShade's DLL. It creates `BFIPresenter.ini` there on first start (`BFIPresenter.example.ini` explains every setting).
2. In CRT-Standalone set **`ENABLE_DECAY=0`**. The add-on does the BFI; leaving both on doubles it.
3. Start the game and open the ReShade overlay. The **BFI Presenter** tab shows the version, whether the swap chain is managed (and why not, if it isn't), and live timing.
4. BFI starts at the game's first swap-chain resize (nearly all games resize at start-up). If it doesn't start, change resolution or display mode once.

**Updating many games:** `Find-BFIPresenter.ps1` lists every game folder with the add-on (and its version) into `BFIPresenter_locations.txt`. `Update-BFIPresenter.ps1 -Source <folder with the new build>` copies the new add-on into all of them; `-DryRun` previews, `-Backup` keeps `.bak` copies. Neither script touches your `BFIPresenter.ini` files.

**Uninstall:** delete `BFIPresenter.addon64` (and the ini) from the game folder.

## Recommended setups

| Situation | Game frame cap | Frame pacing | Release | Notes |
|---|---|---|---|---|
| Game holds 120 fps | Refresh rate | Lowest latency | — | Lowest latency (~29 ms input to screen on the A95L) |
| Game holds 120 fps easily (light) | Refresh rate | Lowest latency | Later release on | ~5 ms less (Cuphead: 16.7 → 11.3 ms release to display) |
| Game runs 60–110 fps | Half refresh | Smooth | Just-in-time (default) | Every frame lit exactly once at an even 60 |
| Game below 60 fps | Quarter refresh | Smooth | Just-in-time (default) | Even 30 fps; each frame lit twice (double image in motion) |
| Flashes in a GPU-heavy game | any | any | — | Start the launcher (Steam) as administrator for a realtime presenter queue |

Avoid Smooth at the Refresh cap: it adds a refresh of latency and doesn't make 120 fps motion more even.

## The overlay

Settings apply immediately. **Save to BFIPresenter.ini** keeps them; **Reload ini** reads the file again.

### Top

- **Version line** and **status line**: whether the swap chain is managed and how.
- **BFI enabled** (Ctrl+Shift+B, `Enabled`): turns the strobe off while the presenter keeps running, for a clean A/B comparison.
- **Warnings** appear only when something needs attention:

| Warning | Meaning / what to do |
|---|---|
| RTSS is hooked / other Present hooks | Keep RTSS's limiter off and don't toggle its OSD while playing |
| Frame generation: … | An FG module was detected; see the tested-games list for what works |
| BFI starts when the game resizes its swap chain | Change resolution or display mode once |
| The display is refreshing slower than it can (VRR) | G-SYNC took over: turn it off for the game, or turn on Queue one refresh ahead |
| A frame limiter is holding the presenter | RTSS limit 0, NVIDIA Max Frame Rate off (global, per game, Profile Inspector) |
| The driver is ignoring VSync | NVIDIA Vertical Sync Force on |
| Missed refreshes break the strobe | Something delays the presenter (VRR, limiter, GPU stalls) |
| Hitch guard: BFI paused | Loading or missed refreshes; BFI returns by itself |
| The game is below the cap's frame rate | Lower settings, or use a lower cap (Half / Quarter) |
| Game frames reach the strobe unevenly | The GPU finishes frames right at the refresh: lower settings or try Smooth |

### Strobe

| Control | ini key (default) | What it does |
|---|---|---|
| Method (Ctrl+Shift+M) | `Method` (1) | 0 = BFI: lit + black refreshes, highlights rolled off at the ceiling. 1 = Variable MPRT (Blur Busters): a brightness budget per pixel; light above the ceiling spills into the dark refresh instead of clipping |
| Frames per cycle | `Frames` (2) | Refreshes per strobe cycle: 2 = 60 Hz strobe at 120 Hz. More = shorter persistence, more flicker, less brightness |
| Gain vs blur | `Gain` (0.5) | Average light kept. 0.5 = lit refresh unchanged (half the average brightness); 1.0 = full brightness (BFI rolls off highlights, MPRT spills them) |
| Dark frame floor | `DarkFloor` (0.0) | Light left in the dark refresh. Higher = less flicker and less visible held refreshes, a bit more blur |
| Highlight roll-off / knee | `HighlightRolloff` (1), `RolloffKnee` (0.75) | BFI method only: hue-preserving shoulder toward the ceiling instead of per-channel clipping |
| Sine BFI | `SineBFI` (0) | Brightness follows a sine over the cycle instead of on/off |
| Invert cycle | `InvertCycle` (0) | Frames > 2 only: one dark refresh per cycle instead of one lit |
| Rolling scan (Ctrl+Shift+S) | `RollingScan` (0) | One horizontal band per sub-frame, lit in turn from the top down, so the screen never goes fully dark. At 120 Hz this swaps two halves; 240 Hz+ gives a real rolling band |
| Phosphor decay | `RollingPhosphorDecay` (0.0) | Rolling scan: each row lights when the simulated beam passes and fades over this many sub-frames (Blur Busters CRT-simulator style). Hides the seam; 0.2–0.4 is a good start; longer = smoother, more blur |
| Seam softness | `RollingSeam` (0.08) | Rolling scan with decay 0: width of the crossfade between bands (share of screen height) |

### Brightness and output

| Control | ini key (default) | What it does |
|---|---|---|
| Output | `Output` (auto) | auto / sdr / scrgb / hdr10. Auto follows the swap chain's format and colour space |
| Display peak (nits) | `DisplayPeakNits` (1400) | HDR ceiling for the lit refresh (roll-off target, MPRT budget) |
| ABL guard | `AblGuard` (1) | HDR only. OLEDs dim a refresh that puts out too much light overall; the lit refresh carries the boost. The ceiling is lowered just enough to keep the lit refresh's average within the budget, and comes back when the scene allows |
| Panel budget | `AblGuardBudgetNits` (250) | Set to the display's full-white (100 % window) brightness: 250 for the A95L, 150–200 for many WOLEDs. The overlay shows the scene's demand and the ceiling in use |

### Frame pacing and latency

| Control | ini key (default) | What it does |
|---|---|---|
| Game frame cap | `CapGameToRefresh` (1) | 0 off, 1 Refresh rate, 2 Half refresh (60 fps at 120 Hz, every frame lit once), 3 Quarter refresh (30 fps, every frame lit twice). Counted against the presenter's refreshes, so it stays in step with the strobe |
| Frame pacing | `FramePacing` (0) | 0 Smooth: every game frame shown once, in order, one held in reserve (catches up after 3 cycles). 1 Lowest latency: the newest finished frame (waits up to 3 ms for an expected frame that is about to finish) |
| Just-in-time release at Half / Quarter cap | `CapJustInTime` (1) | Each frame targets a lit refresh and the game is released at that refresh's vblank minus its measured frame time and the margin. No repeat-then-skip at Half, no 1-then-3-cycle frames at Quarter. At its limit it releases as early as the cap allows |
| Later release at Refresh cap | `LaterRelease` (0) | Refresh cap + Lowest latency: releases the game just in time for the pick instead of a full refresh earlier, so its input is read later. Helps light games; does nothing while frames take nearly a whole refresh. Not for frame generation |
| Just-in-time safety margin | `LaterReleaseMarginMs` (1.5) | Added to the measured frame time (95th percentile of the last 128 frames). Widens 1 ms after a second with 2+ misses (up to 3 ms over the base), narrows after 5 clean seconds. The status line shows state, frame time and margin |
| Automatic frames ahead | `AutoFramesAhead` (1) | 1 frame queued normally, 2 while the game keeps missing strobe cycles; back to 1 once it has headroom, and stays at 2 if it needs 2 again within a minute |
| Max frames ahead | `MaxFramesAhead` (1) | With the automatic mode off: how many frames the game may queue on the GPU (0 = no limit). 1 = lowest input lag |

### Status

Game fps, refreshes per second (should equal the display rate), frame age, uneven strobe cycles (repeats / skips against the expected step), late refreshes and display repeats (both should be 0), output format, buffers, and the **presenter queue priority** (global realtime only when the game's launcher runs as administrator).

### Advanced

| Control | ini key (default) | What it does |
|---|---|---|
| Wait for vblank | `WaitForVBlank` (1) | One present per refresh, nothing queued: lowest latency. Needed by just-in-time release |
| Queue one refresh ahead | `QueueAhead` (0) | Keeps one extra present queued (needs 3 presenter buffers), so an occasional late refresh doesn't flash. +1 refresh (~8 ms) of latency |
| Presenter buffers | `PresenterBuffers` (3) | Real swap-chain buffers behind the presenter (2–4); applies at the game's next resize |
| Present on own queue | `PresentOnOwnQueue` (1) | Present and draw on the add-on's own GPU queue, independent of the game's GPU load; applies when the game next creates its swap chain |
| Hitch guard | `HitchGuard` (1) | While the presenter misses refreshes (3 within half a second) or the game delivers no new frame for the loading time, every refresh is shown at the strobe's average brightness. BFI returns after 1 s without either |
| Loading: no new frame for | `HitchGuardLoadingMs` (1000) | Shorter gameplay hitches keep strobing (the frozen frame is strobed cleanly) |
| Counters | — | Game frames that waited for the presenter (should be 0), presents queued, refreshes reusing an already-drawn buffer, last present age |

### Bottom

- **Timing log** (`Log`): writes `BFIPresenter_log_<date>_<time>.csv` (one line per refresh) and `BFIPresenter_game_<date>_<time>.csv` (one line per game frame) next to the ini. A new pair each time it is switched on. These are the most useful thing to send with a report.

## ini-only keys

| Key (default) | What it does |
|---|---|
| `DX11Support` (1) | Manage Direct3D 11 games through the add-on's own D3D12 presenter |
| `PresenterQueuePriority` (2) | 0 normal, 1 high, 2 global realtime when Windows allows it (launcher run as administrator), otherwise high. Try 1 or 0 if a game hangs the GPU. Read at game start |
| `ReuseDrawnBuffers` (1) | A refresh that would redraw the same frame with the same settings re-presents the buffer that already holds it: no GPU work, so a busy GPU can't hold up the strobe |
| `StartBFIAfterGameResize` (1) | Every swap chain shows the game's own frames until its first resize, then BFI starts on the presenter's queue (0 deadlocked Pragmata) |
| `WaitForResizeOnRecreatedSwapChain` (1) | Same for swap chains the game destroys and recreates (God of War Ragnarök on loading) |
| `GameFrameStatistics` (1) | Frame statistics and present counts the game reads describe its own frames, not the presenter's 120 presents a second |
| `BypassOverlayResizeHooks` (1) | The add-on's own presents and resizes call DXGI directly, past overlay hooks (Steam's overlay hung Pragmata). Overlays then don't draw on the strobed frames |
| `TypedBackBufferDesc` (1) | Stand-in back buffers report the swap chain's typed format, as real ones do (The Last of Us Part I crashed without it) |
| `Diagnostics` (1) | 1 = D3D12 DRED: on a device-removed crash, the last GPU operations go to `ReShade.log` and `BFIPresenter_dred.txt`. 2 = also the D3D12 debug layer into `BFIPresenter_debug.txt` (needs "Graphics Tools"; slower) |
| `CrashLog` (1) | 1 = log serious exceptions anywhere in the game to `BFIPresenter_crash.txt` (runs after the game's own exception handlers). 0 = no crash logger |
| `AllowSmoothMotion` (0) | 1 = manage DX11 games even while NVIDIA Smooth Motion is on (it conflicted in Blasphemous; by default the DX11 path stays off then) |
| `AllowRTSS` (1) | 1 = manage games with RTSS hooked (with a warning); 0 = refuse |

## Hotkeys

| Hotkey | Action |
|---|---|
| Ctrl+Shift+B | BFI on / off (the presenter keeps running) |
| Ctrl+Shift+M | BFI ↔ Variable MPRT |
| Ctrl+Shift+S | Rolling scan on / off |
| Ctrl+Shift+R | Reload `BFIPresenter.ini` |

## Files the add-on writes

| File | When |
|---|---|
| `ReShade.log` lines starting `BFI Presenter:` | Always: version, swap-chain handling, warnings, just-in-time state changes |
| `BFIPresenter_log_*.csv`, `BFIPresenter_game_*.csv` | Timing log on |
| `BFIPresenter_dred.txt` | Device removed, with `Diagnostics=1` |
| `BFIPresenter_debug.txt` | `Diagnostics=2` |
| `BFIPresenter_crash.txt` | Serious exceptions in the game process (harmless probes from mods like REFramework appear there too) |

## Known limits

- **Below the cap's frame rate, motion is uneven.** The strobe shows 60 lit images a second at 120 Hz; use the cap that the game can hold.
- **Game hitches and GPU stalls show as flashes.** A stall longer than a refresh holds the current refresh on screen. Realtime queue priority and GPU headroom help.
- **NVIDIA DLSS Frame Generation** relies on Reflex pacing, which can misread the presenter (Pragmata). OptiScaler FSR FG worked in Shadow of the Tomb Raider.
- **Video memory:** the presenter adds about 0.3 GB at 4K (twice that with 16-bit HDR).
- **Exclusive fullscreen in DX11 games** is not supported; use borderless.
- **NVIDIA Smooth Motion** (driver frame generation): turn it off for BFI games. The DX11 path stays off while it is active.
- 32-bit games: not supported by the add-on (use DXVK + the 32-bit Vulkan layer).

## Building

- **MinGW-w64** (Linux, WSL or MSYS2): run `./build_mingw.sh` in this folder.
- **Visual Studio:** open "x64 Native Tools Command Prompt for VS" in this folder and run `build_msvc.bat`.

Both write `BFIPresenter.addon64` here. Headers come from `../third_party/reshade/include` (ReShade add-on and Dear ImGui headers); see [third_party](../third_party/README.md).
