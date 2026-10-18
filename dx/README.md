# BFI Presenter — DX add-on (Direct3D 12, 11, 9 and OpenGL)

Presenter-clocked black frame insertion (BFI) for PC games. The lit/dark strobe runs on the display's own refresh clock, not the game's. A late game frame becomes a repeated image (CRT-like stutter) instead of a lit or dark refresh held too long (a flash). It includes a just-in-time frame cap that keeps latency low and motion even, an ABL guard for OLED panels, and a rolling-scan mode.

The [Vulkan layer](../vulkan/README.md) covers Vulkan games and DirectX 9–11 games run through DXVK. It reads the same `BFIPresenter.ini`. Both are released together with the same version number; see the [changelog](../CHANGELOG.md).

## How it works

- **Direct3D 12:** the game and ReShade (including CRT-Standalone) render into stand-in back buffers. Each finished frame is copied into a ring of six staging images. A high-priority presenter thread waits for every vblank, picks a game frame, draws the strobe refresh from it, and presents.
- **Direct3D 11:** the game keeps its own DX11 device (so DLSS and ReShade work as usual). Its frames are copied to shared textures, and the add-on's own D3D12 presenter shows them through a window laid over the game's. Use a borderless or windowed mode.
- **Direct3D 9:** ReShade (6.8 or newer) creates the game's device as Direct3D 9Ex, which can share textures, and the game is kept windowed. Each frame is copied into a shared texture, picked up by the add-on's own D3D11 device and shown by the same presenter as DX11 games.
- **The strobe:** each refresh is one sub-frame of the cycle (2 at 120 Hz: one lit, one dark). The shader applies gain in display light (SDR, scRGB or HDR10), BFI with highlight roll-off or the Variable MPRT brightness budget, dark floor, sine BFI, inverted cycle and rolling scan.
- **Frame cap and release:** the presenter lets the game start each frame (through its frame-latency waitable and Present) on its own refresh clock. The game therefore runs exactly in step with the strobe. Just-in-time release times that so frames finish right before the refresh that shows them.

## Requirements

- ReShade **6.4 or newer, the build with full add-on support** (tested on 6.8).
- A **Direct3D 12, 11, 9 or OpenGL** game (ReShade installed for that API; for OpenGL it is `opengl32.dll`). The same add-on handles all four. 64-bit games use `BFIPresenter.addon64`; 32-bit games use `BFIPresenter.addon32` (with the 32-bit ReShade). Direct3D 9 needs ReShade 6.8 or newer. OpenGL needs an NVIDIA GPU (`WGL_NV_DX_interop2`).
- **A fixed refresh rate, VRR off for the game** (G-SYNC off in its NVIDIA profile, or VRR off on the TV), unless the presenter holds the refresh rate on its own (the overlay warns when it doesn't). Tested at 120 Hz; the examples below use it.
- **No frame limiter acting on the presenter:** RTSS limit 0 for BFI games, NVIDIA Max Frame Rate off (also in NVIDIA Profile Inspector).
- **NVIDIA Vertical Sync:** "Use the 3D application setting" or Force on, never Force off or Fast Sync.
- Windows 10/11 (uses the built-in `d3dcompiler_47.dll`).

## Install, update, uninstall

1. Copy `BFIPresenter.addon64` (64-bit game) or `BFIPresenter.addon32` (32-bit game) into the game folder, next to ReShade's DLL. It creates `BFIPresenter.ini` there on first start (`BFIPresenter.example.ini` explains every setting).
2. In CRT-Standalone set **`ENABLE_DECAY=0`**. The add-on does the BFI; leaving both on doubles it.
3. Start the game and open the ReShade overlay. The **BFI Presenter** tab shows the version, whether the swap chain is managed (and why not, if it isn't), and live timing.
4. BFI starts at the game's first swap-chain resize (nearly all games resize at start-up). If it doesn't start, change resolution or display mode once.

**Updating many games:** use the BFI Presenter Manager (`BFIPresenterManager.exe` on the release page): it finds every game with the add-on and updates them all, with backups.

**Uninstall:** delete `BFIPresenter.addon64` / `.addon32` (and the ini) from the game folder.

## Recommended setups

| Situation | Game frame cap | Frame pacing | Release | Notes |
|---|---|---|---|---|
| Game holds 120 fps | Refresh rate | Lowest latency | — | Lowest latency (~29 ms input to screen on the A95L) |
| Game holds 120 fps easily (light) | Refresh rate | Lowest latency | Later release on | ~5 ms less (Cuphead: 16.7 → 11.3 ms release to display) |
| Game runs 60–110 fps | Refresh rate + Automatic frame cap, or Half refresh | Lowest latency | Just-in-time (default) | Every frame lit exactly once at an even 60 |
| Frame generation | Refresh rate + Automatic frame cap | Lowest latency | — | Reflex frame limit at one presented frame per strobe cycle (God of War Ragnarok: a new frame on 60 of 60 flashes, ~21.5 ms release to flash) |
| Game below 60 fps | Quarter refresh | Lowest latency | Just-in-time (default) | Even 30 fps; each frame lit twice (double image in motion) |
| Flashes in a GPU-heavy game | any | any | — | Start the launcher (Steam) as administrator for a realtime presenter queue |

Avoid Smooth at the Refresh cap: it adds a refresh of latency and doesn't make 120 fps motion more even. With frame generation it added a full cycle (God of War Ragnarok: ~38 ms against ~21.5 ms release to flash) for no smoother motion. At Half or Quarter it only helps while just-in-time release keeps hitting its limit.

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
| LCD: prevent image retention | `LcdPhaseSwap` (0), `LcdPhaseSwapSeconds` (5) | **For LCD monitors (IPS, VA, TN).** An LCD drives each refresh with the opposite polarity of the one before; with an even cycle every lit refresh gets the same polarity, a DC bias that can leave temporary image retention. On, after every N seconds of full strobe (1–300) it fades briefly to the steady picture, shifts by one refresh and fades back, so the lit refreshes alternate. Not needed on OLED or with an odd number of frames |
| Rolling scan (Ctrl+Shift+S) | `RollingScan` (0) | One horizontal band per sub-frame, lit in turn from the top down, so the screen never goes fully dark. At 120 Hz this swaps two halves; 240 Hz+ gives a real rolling band |
| Phosphor decay | `RollingPhosphorDecay` (0.2) | Rolling scan: each row lights when the simulated beam passes and fades over this many sub-frames (Blur Busters CRT-simulator style). Hides the seam; 0.2–0.4 is a good start; longer = smoother, more blur |
| Seam softness | `RollingSeam` (0) | Rolling scan with decay 0: width of the crossfade between bands (share of screen height). Pixels in it are lit in both sub-frames, so the seam blurs in motion |

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
| Game frame cap | `CapGameToRefresh` (1) | 0 off, 1 Refresh rate, 2 Half refresh (60 fps at 120 Hz, every frame lit once), 3 Quarter refresh (30 fps, every frame lit twice), 4 one game frame per strobe cycle (refresh ÷ Frames: 60 fps at 360 Hz with 6 frames, 90 fps with 4), 5 custom (refresh ÷ `CapDivisor`, 1–8; use a multiple of Frames). Counted against the presenter's refreshes, so it stays in step with the strobe |
| Automatic frame cap | `AutoFrameCap` (1) | Refresh cap only. After 3 s below 95% of the refresh rate: one game frame per strobe cycle (e.g. 60 fps at 120 Hz with 2 frames), so frames aren't dropped unevenly. Back to Refresh after 10 s in which the game's Reflex reports (GPU, simulation and submit time per frame) stay under 85% of a refresh; without Reflex, a ~3 s try at Refresh after 30 s, 60 s, then every 120 s, kept if the game holds 95% of the refresh rate. In games with NVIDIA Reflex it steps down with Reflex's frame limit (one presented frame per strobe cycle, sent again every 2 s) instead of the presenter's cap, with frame generation or without; the game's own Reflex setting comes back at Refresh. With frame generation (presented frames well above Reflex's rendered frames, compared over the same second; FSR / XeSS frame generation by its DLL) it tries Refresh only every 5 minutes. When the limited frames settle at the moment the presenter picks one for the lit refresh (repeat-then-skip), the limit is shortened by 0.15 ms for a second to move them away. Frame generation without Reflex: stays at Refresh |
| Frame pacing | `FramePacing` (1) | 1 Lowest latency: the newest finished frame (waits up to 3 ms for an expected frame that is about to finish). 0 Smooth: every game frame shown once, in order, one held in reserve. New ini files get 1; existing ones keep their value |
| Just-in-time release at Half / Quarter cap | `CapJustInTime` (1) | Each frame targets a lit refresh and the game is released at that refresh's vblank minus its measured frame time and the margin. No repeat-then-skip at Half, no 1-then-3-cycle frames at Quarter. At its limit it releases as early as the cap allows |
| Later release at Refresh cap | `LaterRelease` (0) | Refresh cap + Lowest latency: releases the game just in time for the pick instead of a full refresh earlier, so its input is read later. Helps light games; does nothing while frames take nearly a whole refresh. Not for frame generation |
| Just-in-time safety margin | `LaterReleaseMarginMs` (1.5) | Added to the measured frame time (95th percentile of the last 128 frames). Widens 1 ms after a second with 2+ misses (up to 3 ms over the base), narrows after 5 clean seconds. The status line shows state, frame time and margin |
| Smooth: catch up after a hitch | `SmoothCatchUp` (0) | Smooth only. Off: after a game hitch the presenter stays one frame behind (about one cycle more latency) and never skips a frame. On: once two finished frames have waited for 3 cycles, one is skipped to get back to the lowest latency, which shows as a second stutter (Thief, The Midnight Walk: about one repeat and one skip a second with it on, none with it off) |
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
| `DX11GamePresent` (0) | 1 = the game's own DX11 swap chain still presents each frame (sync interval 0, hidden under the presenter's window, past overlay hooks). For games whose GPU frames get slower and slower while the presenter runs (Wreckfest) |
| `DX9Support` (1) | Manage Direct3D 9 games: the device is created as 9Ex (ReShade 6.8+), kept windowed, and shown by the same presenter |
| `DX9Ex` (1) | 0 = keep a plain Direct3D 9 device (windowed, no BFI); for checking whether 9Ex is what breaks a game |
| `DX9Diag` (0) | 1 = log failing D3D9 and D3DX calls and resource creations to `ReShade.log` |
| `OpenGLSupport` (1) | Manage OpenGL games (NVIDIA): frames are shared into the add-on's D3D11 device and shown by the same presenter |
| `OpenGLSwapInterval0` (1) | Set the OpenGL game's swap interval to 0 so its own SwapBuffers doesn't wait for vblank; 0 = keep the game's interval |
| `BorderlessFullscreen` (1) | Where exclusive fullscreen is held off (DX11, DX9), make the game's window borderless over its monitor; 0 = leave the window as the game made it |
| `PresenterQueuePriority` (2) | 0 normal, 1 high, 2 global realtime when Windows allows it (launcher run as administrator), otherwise high. Try 1 or 0 if a game hangs the GPU. Read at game start |
| `ReuseDrawnBuffers` (1) | A refresh that would redraw the same frame with the same settings re-presents the buffer that already holds it: no GPU work, so a busy GPU can't hold up the strobe |
| `StartBFIAfterGameResize` (1) | Every swap chain shows the game's own frames until its first resize, then BFI starts on the presenter's queue (0 deadlocked Pragmata) |
| `WaitForResizeOnRecreatedSwapChain` (1) | Same for swap chains the game destroys and recreates (God of War Ragnarök on loading) |
| `GameFrameStatistics` (1) | Frame statistics and present counts the game reads describe its own frames, not the presenter's 120 presents a second |
| Show overlays (Advanced) | `ShowOverlays` (0) | 64-bit DirectX games only (ignored in 32-bit and OpenGL games, which crashed or showed nothing). 1 = the presenter's presents go through overlay hooks, so the RTSS OSD and the Steam overlay draw on the strobed frames (the resize stays direct). Steam's Present hook crashed Pragmata and Resident Evil 4 this way: turn it off if a game crashes. Keep RTSS's limiter off. Applies at the next game start |
| `BypassOverlayResizeHooks` (1) | The add-on's own presents and resizes call DXGI directly, past overlay hooks (Steam's overlay hung Pragmata). Overlays then don't draw on the strobed frames |
| `TypedBackBufferDesc` (1) | Stand-in back buffers report the swap chain's typed format, as real ones do (The Last of Us Part I crashed without it) |
| `Diagnostics` (1) | 1 = D3D12 DRED: on a device-removed crash, the last GPU operations go to `ReShade.log` and `BFIPresenter_dred.txt`. 2 = also the D3D12 debug layer into `BFIPresenter_debug.txt` (needs "Graphics Tools"; slower) |
| `CrashLog` (1) | 1 = log serious exceptions anywhere in the game to `BFIPresenter_crash.txt` (runs after the game's own exception handlers). 0 = no crash logger |
| `AllowSmoothMotion` (0) | 1 = manage DX11 games even while NVIDIA Smooth Motion is on (it conflicted in Blasphemous; by default the DX11 path stays off then) |
| `AllowEosOverlay` (0) | 1 = manage DX11 games even while the Epic Online Services overlay is loaded (it crashed the presenter in Silt; by default the DX11 path stays off then) |
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
- **Frame generation:** set Game frame cap to Refresh rate, not Half refresh (Half throws away half the generated frames). With the automatic frame cap on, the presenter sets Reflex's frame limit to one presented frame per strobe cycle in Reflex games, so every flash gets the next frame; the overlay shows the limit in use. DLSS FG 2×–4× works in most tested games, and Lossless Scaling mostly worked with its VSync at 1/2; in a few, Reflex pacing misreads the presenter and throttles the game (Pragmata). OptiScaler FSR FG worked in Shadow of the Tomb Raider.
- **Video memory:** the presenter adds about 0.3 GB at 4K (twice that with 16-bit HDR).
- **Exclusive fullscreen in DX11 games** is not supported; use borderless (windowed) fullscreen. The Rogue Prince of Persia hung its presenter when it left exclusive fullscreen.
- **A presenter that won't stop** (stuck in its present for 5 s while the game resizes or recreates its swap chain): BFI switches off for the rest of the session and the game continues on its own frames. Restart the game to use BFI again.
- **NVIDIA Smooth Motion** (driver frame generation): turn it off for BFI games. The DX11 path stays off while it is active.
- **Epic Online Services overlay** (`EOSOVH-Win64-Shipping.dll`, loaded by games that use EOS, also on Steam): it hooks Direct3D 12 and crashed the DX11 presenter in Silt, so the DX11 path stays off while it is loaded. `ReShade.log` names its path; rename the file to use BFI (`AllowEosOverlay=1` manages the game anyway).
- **32-bit games:** `BFIPresenter.addon32` covers 32-bit Direct3D 11 and 9 games. Prefer it over DXVK + the 32-bit Vulkan layer (through DXVK, Twelve Minutes and Alan Wake flickered).
- **Direct3D 9 games run windowed** (borderless at full size): 9Ex can't create the exclusive-fullscreen device older games ask for. A game that breaks on a 9Ex device can be checked with `DX9Ex=0`, and `DX9Diag=1` logs the failing calls. The game's thread waits on the CPU for each frame's copy (no shared fences in Direct3D 9): about 0.5–1.5 ms at 4K.
- **OpenGL needs an NVIDIA GPU** (`WGL_NV_DX_interop2`). Old OpenGL games that present by exchange (Need for Speed: Porsche Unleashed) block the presenter until the game's NVIDIA profile has *Vulkan/OpenGL present method* set to *Prefer layered on DXGI swapchain*. The overlay warns when that happens, and the BFI Presenter Manager sets it when installing.
- **G-SYNC on a borderless game window** can engage VRR and pull the display below 120 Hz (Clive Barker's Undying at 101 fps). Set G-SYNC off for the game (Fixed refresh, VSync Force on) or turn on Queue one refresh ahead.
- **CRT-Standalone doesn't compile under ReShade's Direct3D 9 path** (shader model 3 limits); the presenter's own BFI works there.

## Building

- **MinGW-w64** (Linux, WSL or MSYS2): run `./build_mingw.sh` in this folder.
- **Visual Studio:** open "x64 Native Tools Command Prompt for VS" in this folder and run `build_msvc.bat`.

Both write `BFIPresenter.addon64` here; `build_mingw.sh` also builds `BFIPresenter.addon32`. Headers come from `../third_party/reshade/include` (ReShade add-on and Dear ImGui headers); see [third_party](../third_party/README.md).
