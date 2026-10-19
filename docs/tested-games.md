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
  - Game can't hold 120: Game frame cap Half refresh + Frame pacing Smooth, with Just-in-time release on (the default). Some games are smoother with Lowest latency instead (Soul Reaver).
  - **With frame generation (DLSS FG, FSR FG, Lossless Scaling): Game frame cap Refresh rate, not Half refresh.** FG doubles (or triples) the game's output, so let the presenter take every generated frame; Half refresh throws half of them away. Exception: when frame generation only reaches about the strobe rate (e.g. 60 at 120 Hz: 2× at about 30 rendered fps, 3× at about 20), use Half refresh + Smooth: it shows every generated frame evenly and was quicker than Reflex's limit at Refresh (Alan Wake 2 with 3×, Pragmata, Until Dawn).
- **DirectX 11 games: borderless or windowed, not exclusive fullscreen.** The DX11 path presents through its own window over the game (Mirror's Edge Catalyst crashed on exit in exclusive fullscreen).
- **Realtime GPU priority (DirectX):** start Steam (or the game's launcher) as administrator for the session. The presenter queue then runs at global realtime priority, which removed the \~30 ms stalls behind God of War Ragnarök's flashes. The overlay's Status section shows the queue priority.
- **Vulkan games:** install the Vulkan layer once with `install_vulkan_layer.bat` and put `BFIPresenter.ini` next to the game's .exe. The layer loads only when the game runs without administrator rights.
- **BFI starts at the game's first swap-chain resize.** If it doesn't start, change resolution or display mode once in the game.

## Compatibility

99 of 101 tested games and emulators run with BFI: the 50 working in the table below (some only with a setting, without frame generation or without a conflicting mod; twelve of them 32-bit; three with the engine's own stutter) and 49 more listed after it. Two are partial.

| Game | Status | Frame generation with the presenter | Min. version | Key requirement |
| --- | --- | --- | --- | --- |
| Shadow of the Tomb Raider | Working | Works (OptiScaler FSR 3.1 FG) | v0.2 | Standard setup |
| Alan Wake 2 | Working | Works (2×, 3×); 3× at \~20 fps: Half refresh + Smooth | v0.19 | Standard setup |
| God of War Ragnarök | Working | Runs, but judders below 60 output fps | v0.24 | Standard setup; RTSS limiter off |
| Riven (2023) | Working | Not tested | v0.13 | A steady 60 fps for smooth motion |
| Onimusha | Working | Works (2×, 3×); Refresh cap | v0.27 | Standard setup |
| Resonance: A Plague Tale | Working without FG | Game won't start with FG + presenter | v0.19 | Force VSync on, Reflex off, Queue ahead on |
| Returnal | Working without FG | Highlights flicker, frame cap can't pace it | v0.27 | FG off. Automatic frame cap works from v1.0.14 |
| Pragmata | Working | Works at Half refresh + Smooth (\~30 rendered, 60 presented); the automatic cap can't hold Refresh | v1.0.16 | Half refresh + Smooth with FG; change resolution once to start BFI |
| Resident Evil 4 (2023) | Working without OptiScaler | Not tested | v0.40 | No OptiScaler; Quarter refresh below 60 fps |
| Control | Working | Works (2×, 3×, 4× with the [Control-FG](https://github.com/Frooglemcdoo/Control-FG) mod, also with DLSS 5); Refresh cap | v0.54 | G-SYNC off (with it on, VRR took over in heavy scenes and the strobe flickered); G-SYNC-off freeze gone as of v0.54 |
| Marvel's Spider-Man 2 | Partial | Unstable even without the presenter | v0.35 | VRAM-limited at 4K; resolution toggle to start BFI |
| Sifu | Working (DX11) | Not tested | — | Borderless window |
| God of War (2018) | Working (DX11) | Not tested | v0.60 | Borderless window |
| No Rest for the Wicked | Working | Not tested | v0.61 | Standard setup (froze on a resize after loading before v0.61) |
| Mirror's Edge Catalyst | Working (DX11) | Not tested | v0.65 | Borderless window, not exclusive fullscreen |
| Grip | Working | Not tested | v0.65 | Standard setup (didn't load in v0.64) |
| Prince of Persia: The Lost Crown | Working | Not tested | v0.64 | Standard setup (was held at 60 fps before v0.64) |
| ABZU | Working | Not tested | v0.66 | Standard setup |
| The Midnight Walk | Working | Not tested | v0.70 | Half refresh + Apply the cap with Reflex's frame limit (v1.0.15) + Lowest latency; DLSS needs `Engine.ini` lines (see notes) |
| Cuphead | Working | Not tested | v0.73 | Later release on: \~5 ms less latency at 120 fps |
| Obduction | Working | Not tested | v0.77 | Half refresh + just-in-time release; Lowest latency + Refresh is smoothest at 120 |
| Hades II | Working | Not tested | v0.78 | GPU headroom for 120 fps (4K yes, 5K DSR no) |
| Indiana Jones and the Great Circle | Partial (Vulkan) | Flashes persist with FG | VK 0.8.2 | Vulkan layer; GPU at its limit causes 10–20 flashes a minute |
| shadPS4 | Working (Vulkan) | — | VK 0.7 | Vulkan layer; BFIPresenter.ini next to the emulator's .exe |
| Blasphemous | Working (DX11) | NVIDIA Smooth Motion must be off with the presenter; works with the BFI shader | v1.0.3 | Locked at 60 fps: Refresh rate + Lowest latency; Smooth Motion off in the NVIDIA app (with it on, the game closed and relaunched itself). Or the BFI shader at 120 Hz with Smooth Motion on |
| Guacamelee! 2 | Working (DX11) | Not tested | v1.0.3 | Standard setup; works with Special K (crashed at start before v1.0.3) |
| Jusant | Working; recommended with FG | Works (DLSS FG 2×): removes the engine's judder; Refresh cap + Automatic frame cap | v1.0.3 (FG: v1.0.13) | Game frame cap Refresh rate + Automatic frame cap + Frame pacing Lowest latency. With RenoDX, use the UE Extended mod (see notes) |
| Moons of Madness | Working (DX11) | Not tested | v1.0.3 | Half refresh + Lowest latency (the engine stutters at Refresh + Smooth); the BFI shader at the game's locked 120 fps also works |
| Ori and the Blind Forest: Definitive Edition | Working (DX11, 32-bit), engine stutter | Not tested | v1.0.3 | `BFIPresenter.addon32`; Half refresh + Lowest latency (a fast game: Smooth adds latency). At Refresh the game's own 60 Hz simulation stutters, with the presenter or the BFI shader |
| Celeste | Working (DX11 through FNA) | — | v1.0.5 | Steam beta "opengl" (the FNA build) + launch option `--graphics D3D11`, ReShade as `dxgi.dll`; game locked at 60 fps: Half refresh |
| Wreckfest | Working (DX11) | Not tested | v1.0.3 | `DX11GamePresent=1` in `BFIPresenter.ini`: without it the game slowed to 5 fps within a minute |
| Silt | Working (DX11) | Not tested | v1.0.3 | Locked 120 fps, so the CRT shader's decay is the simpler choice. For the presenter, rename the Epic Online Services overlay `EOSOVH-Win64-Shipping.dll` (outside the game folder; `ReShade.log` names its path), else it crashes at start |
| The Rogue Prince of Persia | Working (DX11) | Not tested | v1.0.3 | Borderless (windowed) fullscreen, not exclusive fullscreen: leaving exclusive fullscreen hung or crashed the presenter |
| Alan Wake | Working (DX9, 32-bit) | — | v1.0.3 | `BFIPresenter.addon32` with the 32-bit ReShade 6.8+ (not DXVK: flickered); runs windowed |
| Lara Croft and the Guardian of Light | Working (DX9, 32-bit) | — | v1.0.3 | `BFIPresenter.addon32` with the 32-bit ReShade 6.8+; runs windowed |
| BAJA: Edge of Control HD | Working (DX11, 32-bit) | — | v1.0.3 | `BFIPresenter.addon32` with the 32-bit ReShade |
| Twelve Minutes | Working (DX11, 32-bit) | — | v1.0.3 | `BFIPresenter.addon32` with the 32-bit ReShade for DirectX 11 -- not through DXVK (flickered) |
| Burnout Paradise Remastered | Working (DX11, 32-bit) | — | v1.0.3 | `BFIPresenter.addon32`; works with the Luma HDR add-on |
| Dead Cells | Working (DX11, 32-bit) | — | v1.0.3 | `BFIPresenter.addon32` with ReShade 6.8 (6.5.1 with the Steam overlay crashed at start) |
| FlatOut: Ultimate Carnage | Working (DX9, 32-bit) | — | v1.0.3 | `BFIPresenter.addon32`; Refresh + Lowest latency (Half refresh doesn't hold it at 60, and an NVIDIA Profile Inspector cap stutters) |
| Replaced | Working | Not tested | v1.0.9 | Crashes on the first start after installing, until ReShade has created `ReShade2.ini`; from the second start it runs. Good HDR + CRT look with AutoHDR (see notes) |
| Red Dead Redemption 2 | Working (Vulkan) | Not tested | — | Vulkan renderer; Vulkan layer, `BFIPresenter.ini` next to `RDR2.exe` |
| DOOM Eternal | Working (Vulkan) | Not tested | v1.0.3 | Vulkan layer without ReShade (the game didn't start with ReShade); `BFIPresenter.ini` next to `DOOMEternalx64vk.exe`; settings in the ini + hotkeys |
| PT-PC | Working (Vulkan) | — | v1.0.5 | Vulkan layer; ReShade only through its Vulkan layer (manager: Turn on), never as `dxgi.dll` and no DX add-on in the folder (crashed). With ray tracing on: Half refresh + Lowest latency + Just-in-time release |
| Kena: Bridge of Spirits | Working | Not tested | v1.0.3 | Refresh rate + automatic frame cap (v1.0.16) |
| Need for Speed: Porsche Unleashed | Working (OpenGL, 32-bit), engine stutter; not recommended | — | v1.0.3 | `BFIPresenter.addon32`; in the unofficial patch's OGL3 mode: NVIDIA profile Vulkan/OpenGL present method = Prefer layered on DXGI swapchain |
| Legacy of Kain: Soul Reaver 1 & 2 Remastered | Working (OpenGL) | — | v1.0.3 | With DLSS 5: Half refresh + Lowest latency. Without: Refresh + Lowest latency + Later release (Smooth stutters more) |
| Heretic II | Working (OpenGL, 32-bit) | — | v1.0.3 | `BFIPresenter.addon32` |
| DOOM 3: BFG Edition | Working (OpenGL, 32-bit) | — | v1.0.4 | `BFIPresenter.addon32`; NVIDIA profile: Vulkan/OpenGL present method = Prefer layered on DXGI swapchain (else about 30 fps) |
| Clive Barker's Undying | Working (DX11 via dgVoodoo, 32-bit) | — | v1.0.3 | `BFIPresenter.addon32`; G-SYNC off for the game (Fixed refresh + VSync Force on), or Queue one refresh ahead |
| PCSX2 | Working (Vulkan) | — | — | Vulkan layer; Vulkan renderer, `BFIPresenter.ini` next to the emulator's .exe |
| RPCS3 | Working (Vulkan) | — | — | Vulkan layer; `BFIPresenter.ini` next to the emulator's .exe |

### More games tested

Also tested and working with the standard setup above. Their notes weren't recorded in detail, so the API and first working version aren't listed; DLSS 5 and frame generation are noted where tested.

| Game | Notes |
| --- | --- |
| Ashen | Standard setup |
| Assassin's Creed Unity | Standard setup |
| Blade of Darkness (2021 remaster) | Standard setup |
| Blair Witch | Standard setup |
| Castlevania: Belmont's Curse (demo) | Standard setup |
| Cocoon | Standard setup |
| Crash Bandicoot N. Sane Trilogy | Standard setup |
| Days Gone | Standard setup |
| Dead Space (2023) | Standard setup |
| Dishonored 2 | Standard setup |
| FAR: Lone Sails | Standard setup |
| GRIS | Standard setup |
| Hades | Standard setup |
| Hollow Knight | Standard setup |
| Hotshot Racing | Standard setup |
| INSIDE | Standard setup |
| Jotun | Standard setup |
| Journey | Standard setup |
| Layers of Fear 2 | Standard setup |
| Legacy of Kain: Defiance Remastered | Works with DLSS 5 |
| Little Nightmares (Enhanced Edition) | Standard setup |
| Little Nightmares II (Enhanced Edition) | Standard setup |
| Mafia: Definitive Edition | Standard setup |
| Mortal Shell | Works with DLSS 5 |
| Need for Speed (2015) | Standard setup |
| Planet of Lana II | Standard setup |
| Prodeus | Standard setup |
| REANIMAL | Works with DLSS 5 |
| Return to Monkey Island | Standard setup |
| RiME | Standard setup |
| Shadow Man Remastered | Standard setup |
| Silent Hill 2 (2024) | Works with DLSS 5 and FG 2× (Refresh cap) |
| South of Midnight | Half refresh + Lowest latency, or Refresh + automatic frame cap (it can't hold 120; even 60 either way, Smooth repeated \~1.3%); HDR: `AblGuardBudgetNits=250` (the sky flashed at 300) |
| Split Fiction | Standard setup |
| Spyro Reignited Trilogy | Standard setup |
| Stray | Standard setup |
| The Callisto Protocol | Standard setup |
| The Dark Pictures Anthology: Directive 8020 | Works with DLSS 5 and FG 2× (Refresh cap) |
| The Elder Scrolls IV: Oblivion Remastered | Works with DLSS 5 |
| The Last of Us Part I | Works with DLSS 5. Needs `TypedBackBufferDesc=1` (the default; it crashed without it) |
| The Vanishing of Ethan Carter Redux | Standard setup |
| The Witcher 3: Wild Hunt (patch 4.04, DirectX 12) | Works with DLSS 5 |
| Thief (2014) | Half refresh + Smooth; with catch-up on (before v1.0.3) Smooth repeated and then skipped a frame about once a second |
| Tony Hawk's Pro Skater 1 + 2 | Standard setup |
| Trine 5 | Standard setup |
| Unto The End | Standard setup |
| Until Dawn (2024) | Works with DLSS 5 and FG: Refresh cap, or Half refresh at \~30 rendered fps (60 of 60 flashes, \~30 ms more latency) |
| Untitled Goose Game | Standard setup |
| Wreckfest 2 | Standard setup |

## Per-game notes

### Shadow of the Tomb Raider

- Runs with OptiScaler and OptiFG (FSR 3.1) alongside the presenter: the only frame-generation setup confirmed working so far.

### Alan Wake 2

- No issues found. The game calls exclusive fullscreen and changes HDR format several times at start-up; the presenter handles both.
- **With 3× frame generation at about 20 rendered fps (60 presented): Half refresh + Smooth (v1.0.16).** A new frame on 60 of 60 flashes (0.5% repeated) at about 99 ms from input to flash. Reflex's limit (Refresh with the automatic frame cap, or Half with *Apply the cap with Reflex's frame limit*) repeated about 7% of flashes at about 130 ms; Half + Lowest latency repeated or skipped about a third.

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

- FG off: frame generation caused menu freezes and flickering UI elements, and with v1.0.13 bright highlights flicker under BFI (the lit refresh mixes real and generated frames unevenly).
- Any Reflex frame limit collapses the game to 7–8 fps, including the game's own 16,666 µs limit, which it switches on and off by itself. From v1.0.14 the automatic frame cap notices the collapse, takes the limit out (the game's own too) for the rest of the session and caps with the presenter instead: a steady 60 fps without FG. Without a Reflex limit nothing paces DLSS-G here, which is another reason to keep FG off.
- Remaining flashes (about 5 per minute) coincide with the game's own GPU hitches of 100–276 ms. Buffer reuse (v0.27) removed the presenter's own GPU stalls.
- A realtime GPU queue needs Steam itself run as administrator (v0.81); not yet measured in this game.

### Pragmata

- Uses REFramework (loads as `dinput8.dll`); its harmless memory-probe exceptions appear at the top of `BFIPresenter_crash.txt`.
- Steam's overlay hooks on Present and ResizeBuffers1 hung and crashed the game until v0.34, which calls DXGI directly past them.
- **BFI starts only after the game rebuilds its swap chain:** change the resolution once in the game's settings (and back) to start it.
- **Recommended with frame generation (v1.0.16): Half refresh + Frame pacing Smooth.** With DLSS FG at about 30 rendered fps, every flash showed the next generated frame (60 of 60 a second), at about 108 ms from the game's input to the flash.
- Refresh rate + Smooth is the next best: also 60 of 60, but frame generation delivers about 65 a second, so about 5 a second are dropped (small skips), at about 117 ms. Refresh + Lowest latency repeated a frame on about 40% of flashes.
- The automatic frame cap can't hold Refresh with frame generation: the game collapses under any Reflex frame limit (e.g. 50 → 8 fps), so the presenter drops the limit for the session and frame generation runs uncapped at about 65–70 fps. The in-game limit is inactive with FG, and NVIDIA's Max Frame Rate also limits the presenter's own presents and breaks the strobe, so pick Half refresh yourself.
- Before v1.0.14, DLSS Frame Generation dropped to about 15 base fps (shown as 30) with the presenter, because Reflex pacing misread the presenter's presents. OptiScaler limited the frame rate in a v1.0.15 test.

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

- DirectX 11 game, runs through the add-on's own presenter. Use a borderless window.

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

- **DLSS needs an `Engine.ini` edit.** Without it the game's DLSS options have no effect (the engine stays on its own anti-aliasing). Add at the bottom of `%LOCALAPPDATA%\TheMidnightWalk\Saved\Config\Windows\Engine.ini`:

  ```ini
  [SystemSettings]
  r.TemporalAA.Upscaler=1
  r.AntiAliasingMethod=4
  r.NGX.DLSS.Enable=1
  r.Streamline.InitializePlugin=1
  r.Streamline.DLSSG.Enable=1
  t.Streamline.Reflex.Enable=1
  r.FidelityFX.FSR3.UseNativeDX12=1
  r.FidelityFX.FSR3.UseRHI=0
  ```

  `r.AntiAliasingMethod` defaults to 2 (TAA), which is why the upscalers do nothing.
- **Recommended (v1.0.15): Half refresh + Apply the cap with Reflex's frame limit + Frame pacing Lowest latency**, no in-game limit, frame generation off, DLSS Balanced. 1.7% of flashes off-cadence against 15.6% with the presenter's own Half cap (the engine hands frames over too irregularly for just-in-time release), ~35 ms release to flash. The engine's frame delivery is the reason: in most games leave the option off.
- **Alternative (v1.0.14):** the game's own 60 fps limit (it sets Reflex's frame limit), frame generation off, DLSS Balanced; presenter Game frame cap Refresh rate, Automatic frame cap on, Frame pacing Lowest latency, automatic frames ahead. A new frame on about 98% of flashes, ~36 ms release to flash. Uncapped, the engine hands over frames unevenly even at a full 120 fps (about half the flashes repeat or skip), and heavier DLSS modes (Quality, DLAA) vary more from frame to frame, so repeat/skip pairs come back.
- Smooth with the v1.0.3 default (`SmoothCatchUp=0`) shows every frame once at 60: no repeats or skips, against about one of each a second with catch-up on.
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
- 1.0.11: with exclusive fullscreen for the presenter's swap chain (on by default) it stays steady, also after alt-tab, which used to bring the flashes back for good.

### shadPS4

- Runs through the Vulkan layer: install the layer once and put `BFIPresenter.ini` (section `[BFI]`) next to the emulator's .exe.

### Silt

- Unity game on Steam that uses Epic Online Services. The EOS SDK in the game folder loads Epic's overlay helper, `EOSOVH-Win64-Shipping.dll`, from a shared Epic folder elsewhere on the PC, even without the Epic launcher. The overlay hooks Direct3D 12 and crashed the DX11 presenter's own present about a second after BFI started.
- Fix: rename that file (e.g. to `EOSOVH-Win64-Shipping.dll.off`). Since v1.0.3 `ReShade.log` prints its full path. EOS keeps working; only Epic's pop-ups are gone, in every game that uses them, until it is renamed back.

### The Rogue Prince of Persia

- DirectX 11. **Set the display mode to borderless (windowed) fullscreen.** In exclusive fullscreen the game switches to windowed and recreates its swap chain; the presenter got stuck in its present at that moment, which first hung the game and then (v1.0.3 test build) crashed it. Borderless works.
- Since v1.0.3, if the presenter gets stuck like this in any game, BFI switches off for the rest of the session and the game continues; the overlay says so. Restart the game to use BFI.

### Alan Wake

- 32-bit DirectX 9. Works with `BFIPresenter.addon32` (v1.0.3) and the 32-bit ReShade 6.8 or newer, which creates the game's device as Direct3D 9Ex for the add-on. The game runs windowed (borderless at full size) even when set to fullscreen.
- Before v1.0.3 the game showed "DEVICE_CREATE_ERROR - Invalid call" (9Ex in exclusive fullscreen) and then "D3DXCreateTextureFromFileInMemoryEx failed" (D3DX refusing managed textures on 9Ex); both are handled now.
- Don't use DXVK + the 32-bit Vulkan layer: BFI flickered (SDR) or the screen stayed black (HDR10), and the CRT shader's decay flickered the same way through DXVK. CRT-Standalone doesn't compile under ReShade's DirectX 9 path; use the presenter's BFI.

### BAJA: Edge of Control HD

- 32-bit DirectX 11: works with `BFIPresenter.addon32` (v1.0.3) and the 32-bit ReShade.

### Twelve Minutes

- 32-bit Unity game on DirectX 11. Use `BFIPresenter.addon32` with the 32-bit ReShade installed for DirectX 10/11/12.
- Through DXVK and the 32-bit Vulkan layer it first froze (fixed in v1.0.3: present fences, kernel vblank) and then flickered with BFI and even with the CRT shader's decay alone; frames did not reach the screen one per refresh. Don't use DXVK for this game.

### Burnout Paradise Remastered

- 32-bit DirectX 11. Works with `BFIPresenter.addon32`, also together with the Luma HDR add-on (an earlier v1.0.3 test build hung when Luma set the HDR colour space).

### Dead Cells

- 32-bit DirectX 11. With ReShade 6.5.1 the game crashed at start (a stack overflow inside the Steam overlay); it works with ReShade 6.8.

### Lara Croft and the Guardian of Light

- 32-bit DirectX 9. Works with `BFIPresenter.addon32` (v1.0.3) and the 32-bit ReShade 6.8 or newer; the game runs windowed (borderless at full size).

### Wreckfest

- DirectX 11. **Set `DX11GamePresent=1` in `BFIPresenter.ini`.** Without it the game's GPU frames got slower and slower while the presenter ran (14 ms to over 100 ms within a minute, down to 5 fps, with the GPU reported busy at only 70 W); the game ran fine without the add-on. With the setting the game's own swap chain presents each frame too (hidden under the presenter's window) and the game holds 120 fps.
- An earlier test build presented through the overlay hooks and the game crashed when the Steam overlay set itself up; v1.0.3 presents past them.

### Jusant

- **Recommended with frame generation (v1.0.13+): DLSS FG 2×, Game frame cap Refresh rate, Automatic frame cap on, Frame pacing Lowest latency.** The automatic frame cap steps down to one presented frame per strobe cycle with Reflex's frame limit, and the presenter shows 60 even frames a second from 30 rendered, about 20–30 ms from release to flash.
- Without frame generation: Unreal's frame pipeline hands finished frames over with a varying delay (10–33 ms), so a fixed 120 Hz strobe shows some judder at any setting; this is the engine, not the presenter. Queue one refresh ahead removes the flashes.
- With RenoDX, use the UE Extended RenoDX mod. The older RenoDX Unreal Engine mod (0.2025.622) removed the GPU device a few minutes in (DXGI_ERROR_INVALID_CALL, each time right after it hooked a new render target) and hung the GPU at start-up on ReShade 6.8.0; with UE Extended the game no longer crashes.

### Kena: Bridge of Spirits

- **Recommended (v1.0.16): Game frame cap Refresh rate with the automatic frame cap on** (the defaults): it runs smoothly.
- Earlier versions left a stutter that looked like the engine's own; with the automatic frame cap it no longer shows.

### Replaced

- The first start after installing crashes: ReShade creates `ReShade2.ini` on that start, and from the second start the game runs. Start it once more after the crash.
- Otherwise it works great with the presenter. For HDR with a CRT look: the AutoHDR add-on by MajorPainTheCactus with Pumbo's AdvancedAutoHDR shader, plus CRT-Standalone. With AutoHDR, CRT-Standalone must run in Pipeline 1 (`PIPELINE=1`, Soop scRGB).

### Red Dead Redemption 2

- Use the game's Vulkan renderer with the Vulkan layer installed, and put `BFIPresenter.ini` next to `RDR2.exe`.

### PT-PC

- A Vulkan game: use the Vulkan layer (`BFIPresenter.ini` next to `pt.exe`) and nothing else in the folder. Manager 1.2.0 took it for a DirectX game (the SDL library next to it names every renderer) and installed ReShade as `dxgi.dll` and the DX add-on; with those, NVIDIA's Vulkan driver presented through a hidden DXGI swap chain that ReShade, the add-on and RTSS all hooked, and the game crashed in the driver half a second after BFI started.
- ReShade runs through its own Vulkan layer: in the manager, **Turn on** in the game's ReShade row (it writes the `ReShade.ini` next to `pt.exe` that the layer needs; a folder with only `ReShadePreset.ini` and `reshade-shaders` doesn't load it). The BFI Presenter (Vulkan) tab is then in ReShade's overlay. Without ReShade, edit `BFIPresenter.ini`: the layer picks up the change while the game runs.
- With ray tracing on: Half refresh + Lowest latency + Just-in-time release for smooth motion.

### DOOM Eternal

- The game didn't start with ReShade, but the Vulkan layer doesn't need it: install the layer and put `BFIPresenter.ini` next to `DOOMEternalx64vk.exe` (not `idTechLauncher.exe`).
- Without ReShade there's no settings window. Edit `BFIPresenter.ini` and press **Ctrl+Shift+R** in the game to reload it; **Ctrl+Shift+B** turns BFI on and off, **Ctrl+Shift+M** switches BFI / Variable MPRT, **Ctrl+Shift+S** toggles rolling scan. `BFIPresenterVK.log` next to the ini shows the status.

### OpenGL games (v1.0.3)

- The add-on takes each finished OpenGL frame after ReShade's effects and shares it with its own Direct3D 11 device through NVIDIA's `WGL_NV_DX_interop2`, so OpenGL needs an NVIDIA GPU. The game's swap interval is set to 0 so its own SwapBuffers doesn't wait for vblank (`OpenGLSwapInterval0=0` turns that off).
- **Need for Speed: Porsche Unleashed** (32-bit), with the unofficial patch in OGL3 mode: the presenter was blocked (presents of 20–46 ms) until the game's NVIDIA profile had *Vulkan/OpenGL present method* set to *Prefer layered on DXGI swapchain*. The BFI Presenter Manager doesn't set it here, because a dgVoodoo install makes the game look like Direct3D 11 to it: set it in NVIDIA Profile Inspector. Runs after that, but the engine's own stutter remains, so the game isn't recommended.
- **Legacy of Kain: Soul Reaver 1 & 2 Remastered:** works together with the DLSS 5 add-ons, which lower the frame rate well below 120. With DLSS 5 use Half refresh + Lowest latency; without it the game holds 120, so use Refresh + Lowest latency + Later release. Frame pacing Smooth stutters more in this game.
- **Heretic II** (32-bit): works with `BFIPresenter.addon32`.
- **DOOM 3: BFG Edition** (32-bit): the presenter's presents blocked for 30–45 ms and held the game at about 30 fps until the game's NVIDIA profile had *Vulkan/OpenGL present method* set to *Prefer layered on DXGI swapchain* (the Manager sets it when installing). Runs at full rate after that.

### Clive Barker's Undying

- 32-bit, run through dgVoodoo (Direct3D 11 output) with `BFIPresenter.addon32`. The game's fullscreen setting used to end up windowed with the add-on; since v1.0.3 the window is made borderless over the monitor automatically (`BorderlessFullscreen=1`).
- With G-SYNC on, VRR engaged on the borderless window and the game dropped to about 101 fps. Set G-SYNC off for the game (Fixed refresh) with Vertical Sync Force on, or use Queue one refresh ahead.
- If the mouse stops working, change the input setting in `Undying.ini`.
- When dgVoodoo changes display mode (or on quit) the presenter can get stuck; BFI then switches off for the session and the game continues.

### FlatOut: Ultimate Carnage

- 32-bit DirectX 9, works with `BFIPresenter.addon32` and the 32-bit ReShade 6.8+. With Half refresh the game kept running at about 100 fps instead of 60, and capping it at 60 in NVIDIA Profile Inspector made it stutter. Play it at Refresh + Lowest latency.

### Celeste

- The default Windows build runs on XNA (Direct3D 9, 32-bit), and ReShade never loads in it, whatever its name or bitness (`ReShade.log` keeps the setup's placeholder text).
- Use the FNA build instead: Steam → Properties → Betas → **opengl**. Add `--graphics D3D11` to the launch options (the game's `log.txt` then says `FNA3D Driver: D3D11`) and install ReShade as `dxgi.dll` (Direct3D 10/11/12) with the bitness the manager shows. BFI Presenter then works as in any DX11 game.
- The game is locked at 60 fps: use the Half refresh cap.

### BFI shader (game-clocked BFI as a ReShade effect, v1.0.5)

The shader runs the presenter's strobe inside ReShade, one game frame per sub-frame, so the game's own VSync does the pacing. **Use it only when the game holds a rock-steady 120 fps and its engine stutters with the presenter.** Otherwise the presenter is better: with the shader every late game frame shows a lit or a dark frame twice (a flash), which the shader's hitch guard can only soften.

- **Works:** Blasphemous at 120 Hz with NVIDIA Smooth Motion (the game is locked at 60 fps; the presenter can't be used with Smooth Motion, and plays it at Refresh + Lowest latency without it). Moons of Madness at its locked 120 fps (it flickers a little, no more than CRT-Standalone's decay did), though the presenter at Half refresh + Lowest latency is now recommended there.
- **Likely fine, not yet tested with the shader** (CRT-Standalone's decay ran cleanly in them, and they hold 120 fps): Hades, Hades II, FAR: Lone Sails, Layers of Fear 2, Cocoon, Cuphead, Guacamelee! 2, Hollow Knight, Prodeus, Tony Hawk's Pro Skater 1 + 2, Jotun. The presenter works in all of them too.
- **Not recommended:** Stray and Sifu: CRT-Standalone's decay flashed a lot there; use the presenter.
- **Doesn't help:** Ori and the Blind Forest: Definitive Edition: the stutter at 120 comes from the game's own 60 Hz simulation. Use the presenter at Half refresh + Lowest latency.
- The BFI Presenter Manager installs it with **Install as → BFI shader** (it removes the add-on, copies the shader into ReShade's shader folder and turns it on as the last effect in the active preset) and suggests it for the games recommended above. By hand: see [shader/README.md](../shader/README.md).

## Known issues across games

- **Below 60 fps, motion judders.** The strobe shows 60 lit images per second; a game at 45 fps repeats frames. Clarity is unaffected.
- **Frame generation:** use the Refresh rate cap, not Half refresh, unless frame generation only reaches about 60 fps: then Half refresh + Smooth shows every generated frame (Alan Wake 2 with 3×, Pragmata, Until Dawn). DLSS FG (2×–4×) works in Alan Wake 2, Onimusha, Control (Control-FG mod), Silent Hill 2, Directive 8020 and Until Dawn; OptiScaler FSR FG in Shadow of the Tomb Raider. Lossless Scaling frame generation mostly worked with its VSync set to 1/2. A few games collapse under any Reflex frame limit (Pragmata, Returnal); the presenter then drops the limit for the session.
- **Game hitches show as single flashes:** when the whole GPU stalls for 100 ms or more, no refresh can be presented on time. With the GPU at its limit, shorter stalls of one to four refreshes flash too (God of War Ragnarök, Indiana Jones). GPU headroom helps everywhere; in DirectX, a realtime presenter queue (launcher run as administrator) removed them.
- **Video memory:** the presenter adds about 0.3 GB at 4K (twice that with 16-bit HDR). Games already at the limit can collapse in performance.
- **Overlays:** in DirectX games the RTSS OSD and the Steam overlay don't draw on the strobed frames by default (their hooks are bypassed for the presenter's own presents since v0.34). `ShowOverlays=1` (v1.0.9) sends the presents through them again in 64-bit DirectX games (not 32-bit or OpenGL games); turn it off if a game crashes. Vulkan games show them anyway. Any RTSS or driver frame limiter must stay off.
- **APIs:** Direct3D 12, 11 and 9 through the add-on (64-bit, and 32-bit Direct3D 11 and 9 with `BFIPresenter.addon32`; Direct3D 9 needs ReShade 6.8+). OpenGL through the add-on on NVIDIA GPUs (`WGL_NV_DX_interop2`, 64- and 32-bit). Vulkan through the Vulkan layer (64- and 32-bit), which also covers DirectX 9–11 games run through DXVK.
- **Below the cap's frame rate, motion is uneven.** The overlay warns when strobe cycles step unevenly for more than a moment and says whether the game is below the cap. At \~100 fps on 120 Hz, Half refresh with just-in-time release gives even motion.
