# BFI Presenter — Tested Games

Last updated October 2026. "Min. version" is the first BFI Presenter version each game was confirmed working with; "VK" marks the Vulkan layer.

## Required setup (all games)

Every game below was tested on an RTX 5080 with a Sony A95L at a fixed 120 Hz, with BFI Presenter v0.12–v0.82 (Direct3D 12 and Direct3D 11) and the Vulkan layer v0.8 on ReShade 6.8.

- **G-SYNC and VSync: check per game.** The presenter's steady 120 presents per second usually hold the display at a fixed 120 Hz even with G-SYNC on globally. Only where that fails (for example God of War Ragnarök with 2× FG, Control in heavy scenes, or Resonance ignoring VSync) set G-SYNC off and Vertical Sync Force on in the game's NVIDIA Profile Inspector profile. The BFI Presenter window should show 120 refreshes per second with no display repeats.
- **Vertical Sync: never Force off or Fast Sync** in the game's NVIDIA profile: the strobe loses its cadence. “Use the 3D application setting” works in most games; Force on where the game ignores VSync.
- **No frame-rate limiter that touches the presenter:** NVIDIA Max Frame Rate off, globally and in NVIDIA Profile Inspector. With a global RTSS profile (for example 118 fps for VRR), give each BFI game its own RTSS profile with the frame limit set to 0: a 118 cap caused a flash twice a second, and even 120 can drop an occasional refresh. The overlay warns when a limiter holds the presenter below the display rate.
- **CRT-Standalone:** set `ENABLE_DECAY=0`, so the shader's own BFI doesn't stack on the presenter's.
- **ABL guard budget (HDR, v0.52+): set it to the display's full-white brightness.** `AblGuardBudgetNits=250` for the A95L (spec 250–260 nits at a 100 % window). South of Midnight's sky still flashed at 300 and was clean at 250; God of War Ragnarök's brightest scene passed at 300. One value covers all games.
- **Recommended presenter settings (v0.82):** PresenterBuffers 3, Wait for vblank on, Queue one refresh ahead off, Automatic frames ahead on.
  - Game holds 120 fps: Game frame cap Refresh rate + Frame pacing Lowest latency. Light games: add Later release (Cuphead: \~5 ms less latency).
  - Game can't hold 120: Game frame cap Half refresh + Frame pacing Smooth, with Just-in-time release on (the default).
- **DirectX 11 games: borderless or windowed, not exclusive fullscreen.** The DX11 path presents through its own window over the game (Mirror's Edge Catalyst crashed on exit in exclusive fullscreen).
- **Realtime GPU priority (DirectX):** start Steam (or the game's launcher) as administrator for the session. The presenter queue then runs at global realtime priority, which removed the \~30 ms stalls behind God of War Ragnarök's flashes. The overlay's Status section shows the queue priority.
- **Vulkan games:** install the Vulkan layer once with `install_vulkan_layer.bat` and put `BFIPresenter.ini` next to the game's .exe. The layer loads only when the game runs without administrator rights.
- **BFI starts at the game's first swap-chain resize.** If it doesn't start, change resolution or display mode once in the game.

## Compatibility

21 of 25 tested games run with BFI (four of them only without frame generation or a conflicting mod). Two are partial, Sifu hasn't been retested since DirectX 11 support arrived, and one 32-bit game doesn't load the presenter.

| Game | Status | Frame generation with the presenter | Min. version | Key requirement |
| --- | --- | --- | --- | --- |
| Shadow of the Tomb Raider | Working | Works (OptiScaler FSR 3.1 FG) | v0.2 | Standard setup |
| Alan Wake 2 | Working | Not tested | v0.19 | Standard setup |
| God of War Ragnarök | Working | Runs, but judders below 60 output fps | v0.24 | Standard setup; RTSS limiter off |
| Riven (2023) | Working | Not tested | v0.13 | A steady 60 fps for smooth motion |
| Onimusha | Working | Not tested | v0.27 | Standard setup |
| Resonance: A Plague Tale | Working without FG | Game won't start with FG + presenter | v0.19 | Force VSync on, Reflex off, Queue ahead on |
| Returnal | Working without FG | Menu freezes, flickering UI | v0.27 | FG off |
| Pragmata | Working without FG | DLSS FG throttled to \~30 fps (Reflex) | v0.35 | FG off |
| Resident Evil 4 (2023) | Working without OptiScaler | Not tested | v0.40 | No OptiScaler; Quarter refresh below 60 fps |
| Control | Working | Not engaged in testing | v0.54 | G-SYNC off (with it on, VRR took over in heavy scenes and the strobe flickered); G-SYNC-off freeze gone as of v0.54 |
| Marvel's Spider-Man 2 | Partial | Unstable even without the presenter | v0.35 | VRAM-limited at 4K; resolution toggle to start BFI |
| Sifu | Not retested | — | — | DirectX 11 (supported since the DX11 path); borderless window |
| God of War (2018) | Working (DX11) | Not tested | v0.60 | Borderless window |
| No Rest for the Wicked | Working | Not tested | v0.61 | Standard setup (froze on a resize after loading before v0.61) |
| Mirror's Edge Catalyst | Working (DX11) | Not tested | v0.65 | Borderless window, not exclusive fullscreen |
| Grip | Working | Not tested | v0.65 | Standard setup (didn't load in v0.64) |
| Prince of Persia: The Lost Crown | Working | Not tested | v0.64 | Standard setup (was held at 60 fps before v0.64) |
| ABZU | Working | Not tested | v0.66 | Standard setup |
| The Midnight Walk | Working | Not tested | v0.70 | Half refresh; some camera judder is the game's own |
| Cuphead | Working | Not tested | v0.73 | Later release on: \~5 ms less latency at 120 fps |
| Obduction | Working | Not tested | v0.77 | Half refresh + just-in-time release; Lowest latency + Refresh is smoothest at 120 |
| Hades II | Working | Not tested | v0.78 | GPU headroom for 120 fps (4K yes, 5K DSR no) |
| Indiana Jones and the Great Circle | Partial (Vulkan) | Flashes persist with FG | VK 0.8.2 | Vulkan layer; GPU at its limit causes 10–20 flashes a minute |
| shadPS4 | Working (Vulkan) | — | VK 0.7 | Vulkan layer; BFIPresenter.ini next to the emulator's .exe |
| BAJA: Edge of Control HD | Not supported | — | — | 32-bit game: the add-on doesn't load (try DXVK + the 32-bit Vulkan layer) |

## Per-game notes

### Shadow of the Tomb Raider

- Runs with OptiScaler and OptiFG (FSR 3.1) alongside the presenter: the only frame-generation setup confirmed working so far.

### Alan Wake 2

- No issues found. The game calls exclusive fullscreen and changes HDR format several times at start-up; the presenter handles both.

### God of War Ragnarök

- The game destroys and recreates its swap chain when loading a save. BFI pauses during loading and, since the later swap-chain fixes, resumes on its own without a resolution toggle.
- Load crashes and hangs in versions before v0.24 came from Steam's `gameoverlayrenderer64.dll` hook, which is injected even with the Steam overlay and Steam Input disabled.
- An RTSS 118 fps cap (left over from VRR) throttled the strobe to 118 Hz and flashed twice a second: remove it.
- With FG, frames reached the presenter at about 47 fps, so motion judders. Use Half refresh for exactly 60, or play without FG.

* At about 100 fps, Smooth used to cost 20–30 fps against Lowest latency (it handed frame slots back only at the 60 lit picks); fixed in v0.79. Lowest latency + Refresh feels smoother at these frame rates; Half refresh + just-in-time is the even option.
* Occasional flashes with DLSS 5 were \~30 ms GPU stalls the presenter's draw waited behind. Running Steam as administrator (realtime presenter queue, v0.81) removed them: 0 long holds in 126 s against 6 in 176 s.

### Riven (2023)

- Motion stutters whenever the frame rate isn't a steady 60 (it ran at 45–77 fps). Half refresh plus settings that hold 60 give smooth motion; DLSS 5 off helps reach it.

### Onimusha

- About one flash per minute, each matching a game hitch of 33–244 ms. One burst of about a second when presents were briefly limited to about 63 Hz (likely a display-mode change at a transition).
- Ran at 43–46 fps, so Half refresh never capped it; a steady 60 removes the remaining judder.

### Resonance: A Plague Tale

- Needs NVIDIA Vertical Sync Force on: with “Use the 3D application setting” the driver ignored the presenter's VSync after a few seconds (hundreds of presents per second, tearing).
- With Force on, a growing present queue made the driver throttle the game; Queue one refresh ahead (v0.19) fixed it.
- Reflex off measured slightly better: 52 vs 49 fps and 3.5 ms less delay to screen.
- Runs at about 50 fps, so expect judder unless settings reach 60.

### Returnal

- FG off: frame generation caused menu freezes and flickering UI elements.
- Remaining flashes (about 5 per minute) coincide with the game's own GPU hitches of 100–276 ms. Buffer reuse (v0.27) removed the presenter's own GPU stalls.
- A realtime GPU queue needs Steam itself run as administrator (v0.81); not yet measured in this game.

### Pragmata

- Uses REFramework (loads as `dinput8.dll`); its harmless memory-probe exceptions appear at the top of `BFIPresenter_crash.txt`.
- Steam's overlay hooks on Present and ResizeBuffers1 hung and crashed the game until v0.34, which calls DXGI directly past them.
- DLSS Frame Generation drops to about 15 base fps (shown as 30) with the presenter, because Reflex pacing misreads the presenter's presents. Play without FG, or try XeSS or FSR FG through OptiScaler.

### Control

- Needs G-SYNC off. With it on, the display held 120 Hz in light scenes but dropped to 90–104 Hz once the game fell to about 52 fps (GPU near its limit): VRR followed the presenter's slightly late presents and the strobe flickered. G-SYNC off fixed it (v0.54); the G-SYNC-off freeze seen around v0.11 no longer happens.

### Resident Evil 4 (2023)

- With OptiScaler loaded, the GPU hung after a while (device hung, at either presenter queue priority). With OptiScaler removed and only ReShade, no hangs so far.
- Crashed on its first frame inside Steam's Present hook until v0.39.
- Runs at about 47 fps, so Half refresh leaves about 12 lit refreshes per second repeating a frame unevenly, seen as flashes and judder. **Quarter refresh** (a steady 30) shows every frame lit exactly twice and removes them.
- Loads REFramework (as `DINPUT8.dll`); its harmless memory-probe exception appears in `BFIPresenter_crash.txt`.

### Marvel's Spider-Man 2

- BFI starts only after a resolution toggle: the game recreates its swap chain without resizing it.
- The game runs at the limit of 16 GB VRAM at 4K with ReShade. When BFI starts (about 0.3 GB extra), performance can collapse to 5–6 fps.
- Toggling frame generation with ReShade alone already recreates the swap chain twice and destabilises the game. One GPU hang occurred in the game's DirectStorage GPU decompression.
- Not recommended with the presenter at 4K until the lean-memory option exists.

### Sifu

- DirectX 11 is now supported through the add-on's own D3D12 presenter (use a borderless window); Sifu hasn't been retested.

### God of War (2018)

- DirectX 11 game, runs through the add-on's own presenter. Use a borderless window.

### No Rest for the Wicked

- Froze on a resize after a one-second loading pause: the presenter dropped the frames it held and the game waited forever for a frame slot. Fixed in v0.61.

### Mirror's Edge Catalyst

- DirectX 11. Crashed on exit in exclusive fullscreen; works in a borderless window.

### Grip

- Didn't load with v0.64 (the add-on unregistered itself when the engine reloaded ReShade). Fixed in v0.65.

### Prince of Persia: The Lost Crown

- Unity game held at 60 fps in every mode before v0.64, because frame slots came back only at the 60 lit picks. Runs at 120 since.

### ABZU

- Lit refreshes sometimes repeated a frame and then skipped one, when the expected frame finished a moment after the pick. v0.66 waits up to 3 ms for it.

### The Midnight Walk

- Camera-rotation judder that remains in every mode is the game's own. Half refresh + Lowest latency is the best combination; v0.69 releases the game in time for the lit refresh (frames \~5 ms old when shown).

### Cuphead

- Light enough for Later release: frames take \~1.5 ms, so the game is released \~3 ms before each refresh instead of 8.3 ms. Release to display went from 16.7 to 11.3 ms, with no extra repeats.

### Obduction

- Frames need \~4 ms CPU plus GPU time, so at Half refresh they finished right at the pick: repeat-then-skip at Half, frames held 1 then 3 cycles at Quarter. Just-in-time release (v0.75/v0.76) cut uneven lit refreshes at Half to 0.3 %.
- Brief flashes were the driver stalling the game's frame copy for 50–70 ms while the presenter waited on a lock. Fixed in v0.77.
- Lowest latency + Refresh at 120 fps has \~7 ms less latency than Half + just-in-time (16.7 vs 23.8 ms release to display).

### Hades II

- At 5K DSR the GPU was at its limit at 120 fps: dips to 94–112 fps and uneven lit refreshes. At 4K it holds \~118 fps cleanly.
- The "does not match the strobe" warning was on permanently at 120 fps until v0.78.

### Indiana Jones and the Great Circle (Vulkan)

- Flashes 10–20 times a minute at Half refresh, more at Refresh: with the GPU fully loaded, the presenter's draw sometimes misses its vblank and the present call blocks a refresh. Frame generation doesn't help.
- Queue one refresh ahead (VK 0.8.2) doesn't hold here: the NVIDIA Vulkan present path accepts only one pending present, with 3 or 4 buffers. "Prefer native" present method made it worse.
- To try: a lower Texture Pool Size (VRAM paging stalls the GPU at 16 GB), lower settings, Dark frame floor 0.1–0.15.

### shadPS4

- Runs through the Vulkan layer: install the layer once and put `BFIPresenter.ini` (section `[BFI]`) next to the emulator's .exe.

### BAJA: Edge of Control HD

- 32-bit game: the 64-bit add-on can't load. Untested route: DXVK plus the 32-bit Vulkan layer.

## Known issues across games

- **Below 60 fps, motion judders.** The strobe shows 60 lit images per second; a game at 45 fps repeats frames. Clarity is unaffected.
- **NVIDIA DLSS Frame Generation** relies on Reflex pacing, which misreads the presenter's own presents and throttles the game (Pragmata, likely Returnal). OptiScaler FSR FG worked in Shadow of the Tomb Raider.
- **Game hitches show as single flashes:** when the whole GPU stalls for 100 ms or more, no refresh can be presented on time. With the GPU at its limit, shorter stalls of one to four refreshes flash too (God of War Ragnarök, Indiana Jones). GPU headroom helps everywhere; in DirectX, a realtime presenter queue (launcher run as administrator) removed them.
- **Video memory:** the presenter adds about 0.3 GB at 4K (twice that with 16-bit HDR). Games already at the limit can collapse in performance.
- **Overlays:** the RTSS OSD and the Steam overlay don't draw on the strobed frames (their hooks are bypassed for the presenter's own presents since v0.34). Any RTSS or driver frame limiter must stay off.
- **APIs:** Direct3D 12 and 11 through the add-on (64-bit). Vulkan through the Vulkan layer (64- and 32-bit), which also covers DirectX 9–11 games run through DXVK. OpenGL is untested.
- **Below the cap's frame rate, motion is uneven.** The overlay warns when strobe cycles step unevenly for more than a moment and says whether the game is below the cap. At \~100 fps on 120 Hz, Half refresh with just-in-time release gives even motion.
