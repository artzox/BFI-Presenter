# Changelog

BFI Presenter has two parts, released together with one version number:

- **DX** — the ReShade add-on for Direct3D 12, 11 and 9 and OpenGL games (`BFIPresenter.addon64`, `BFIPresenter.addon32`).
- **Vulkan** — the Vulkan layer for Vulkan games and DirectX 9–11 games run through DXVK (`BFIPresenterVK.dll`).

Entries are tagged **[DX]**, **[Vulkan]** or **[Both]**. Before 1.0.0 the two were versioned separately (DX 0.x, Vulkan 0.x.y); their histories follow the 1.0.0 entry.

## [1.0.26] — 2026-10-29 (Manager 1.3.10)

- **[DX] FSR frame generation through OptiScaler: the frame limit for frame generation without Reflex works with it too.** FSR presents every frame, the game's and the generated ones, from a thread of its own, so the presenter couldn't tell the game's frames apart and FSR read as no frame generation. The game still presents to FSR's swap chain once per rendered frame: the presenter now finds that swap chain (by hooking the DXGI factory the game itself uses, before the game creates its swap chain), counts the game's presents there and holds the game right after each of them. Only for FSR frame generation's swap chain; XeSS frame generation and everything else keep the hook on the real swap chain and the timings tested with them. FSR follows a moved frame start with a lag, so its start-time search gives each start time 2 s to settle, prefers 8 ms to spare, makes no 2 ms steps while held, and looks again after 5 s of lost frames (at most every 20 s). The Callisto Protocol (FSR frame generation with OptiScaler's Async on and HUD fix off): 60 of 60 flashes, about 28–34 ms from release to flash. Riven: 55 of 60 flashes on average, 59–60 in 45% of seconds, against 46 and 20% with OptiScaler's 60 fps limit (FSR's own timing moves against the flashes there now and then; XeSS frame generation is steadier in Riven). `HookOuterSwapChain=0` in the ini turns the hook off.
- **[Vulkan]** No changes; version number only.
- **[Manager] Motion test and log analyzer buttons.** A **Tools** row downloads the tools on first use (from the newest release with a tools zip, so a release without one is fine) and updates them when a newer one comes out. **Motion test** keeps `BFIMotionTest.exe` in a folder of its own, listed as **BFI Motion Test** in the game list: install ReShade and BFI Presenter there like in a game, and the button starts it from then on (updates replace only the exe). **Log analyzer** opens the analyzer page in your browser.
- **[Docs]** Settings guide and example ini: FSR frame generation through OptiScaler. Tested games: The Callisto Protocol with FSR frame generation; Riven with FSR frame generation.

## [1.0.25] — 2026-10-28

- **[DX] Frame limit for frame generation without Reflex (`FrameGenLimitWithoutReflex`, on by default).** Frame generation in games without NVIDIA Reflex (e.g. XeSS or FSR frame generation through OptiScaler) had no limit from the presenter: the game rendered as fast as it could, and generated and real frames reached the flashes unevenly. The presenter now tells the game's own frames from frame generation's (frame generation presents its generated frames from a thread of its own), and with the Refresh rate cap holds the game to one rendered frame per strobe cycle × the frame-generation factor: e.g. 30 rendered / 60 presented at 120 Hz with 2 frames and 2×. The game is held right after its own Present, where Reflex would hold it at the start of the next frame, on the refresh grid; a generated frame submitted together with the game's frame is shown at one flash and the game's at the next; and the start time is found by trying it out: for about 25 s after the limit comes on, start times across one strobe cycle are tried 2 ms apart, and the one with every frame reaching a flash, at least 3 ms to spare, and the shortest release to flash is kept (the log lists them all). The start stays on the strobe-cycle grid through hitches, moves 2 ms earlier if frames start repeating at their flash, and is looked for again when frames keep missing their flash or release to flash grows by 6 ms or more (at most once a minute: the game's GPU work can change, Riven with OptiScaler's denoiser 2.6 → 7 ms a frame). Steering by how long frames wait, as `ReflexAlign` does, hunted with frame generation: frames a pair apart wait a refresh more by design, and in one Riven run the frame start moved 3 ms later every second for minutes and motion stuttered. Only while that leaves about 28 rendered fps or more. Riven with DLSS 5 and XeSS frame generation through OptiScaler (its own frame limit off), a 10-minute run: 59–60 of 60 flashes in 78% of seconds, about 37–40 ms from release to flash, against about 43 ms with OptiScaler's 60 fps limit. While it is in use the automatic frame cap stays at Refresh: the presenter's own cap on top held frame generation's presents too, and the game's window stopped responding for seconds now and then (The Callisto Protocol). With OptiScaler's FSR frame generation all presents come from FSR's own thread, so it isn't recognised yet; keep OptiScaler's own limit there.
- **[Vulkan]** No changes; version number only.
- **[Docs]** Tested games: Riven and The Callisto Protocol with XeSS frame generation and the presenter's limit. Settings guide, latency, example ini.

## [1.0.24] — 2026-10-28 (Manager 1.3.9)

- **[DX] [Vulkan] Video memory warning.** When a game uses more video memory than Windows gives it (3 of 5 seconds over the budget), Windows moves part of it to system memory and the GPU stalls now and then: missed refreshes, frame generation dropping its generated frames, BFI pausing. The status box and the log now say so, with the amount and the budget, and suggest lowering the game's texture or memory settings. Indiana Jones and the Great Circle at 4K with 2× frame generation ran 150–250 MB over: every 20–90 s presentation slowed, the Reflex frame limit went off and BFI flickered; with a lower Texture Pool Size it holds. The flash hints no longer suggest 4 presenter buffers while video memory is over or within 256 MB of the budget (each 4K buffer is about 33 MB).
- **[Vulkan] The Reflex frame limit stays on through short slowdowns.** When presentation slows for a few seconds, frame generation drops its generated frames and comes back by itself; those seconds no longer count as frame generation switching off, so the limit doesn't go off and on again (each time about 3 s uncapped).
- **[Vulkan] Automatic recovery waits 5 seconds** of slowed presentation instead of 3. Slowdowns of 2–3 s (a game close to its video memory budget, settings menus) recover by themselves, and a rebuild there only added a hitch. A window over the game or the ReShade menu still triggers it.
- **[DX] [Vulkan] ReLimiter warning.** When ReLimiter (`relimiter.addon64`) is loaded, the status box and the log say that its frame limit stacks with the presenter's cap, that its G-SYNC handling can switch frame generation's mode back and forth (a ~30 fps lock in Control until G-SYNC was off for the game, #26), and that its smoothness reading looks worse than the screen because the presenter holds each frame for its flash (#27). Remove it from the game's folder or turn its limiter off, and use the presenter's frame cap.
- **[DX] [Vulkan] Ctrl+Shift+K: log marker.** Writes a marked line to the log with the last second's state (frame rate, frame generation, the Reflex limit, video memory), to find a moment you noticed in a long session when sending a log.
- **[Manager] What's new before an update.** A game's Update, Update layer, Update outdated and Update Manager first show what changed since the installed version, from the release's changelog: only the entries for that part (DX add-on, Vulkan layer, shader or Manager), across every version in between, one line each with the details a click away; Update or Cancel. **Show this before updating** turns the window off; a **What's new** button next to the message afterwards still shows the changes, and games and the Vulkan layer with an update available have one too. The built-in shader copy is the current one.
- **[Shader]** Its version now reads 1.0.21, the last release that changed it (the ABL guard's window curve in 1.0.20, the Ko-fi link in 1.0.21). It still read 1.0.10, so the Manager didn't offer those changes as an update to games with the shader.
- **[Docs]** Tested games: Indiana Jones and the Great Circle, video memory and Texture Pool Size. Settings guide, READMEs and example ini: automatic recovery after 5 seconds, Ctrl+Shift+K.

## [1.0.23] — 2026-10-27

- **[Vulkan] Automatic frame cap with frame generation in Reflex games.** The layer now passes the game's NVIDIA Reflex calls (`VK_NV_low_latency2`) through itself: frame generation is recognised from the game's Reflex sleep (one call per rendered frame) against its presents, and with the Refresh rate cap and the automatic frame cap it is capped to one presented frame per strobe cycle by a frame limit in the game's Reflex sleep, as in the DirectX add-on. Only while that leaves about 30 rendered fps or more: Indiana Jones and the Great Circle with 2× at 120 Hz holds a steady 30 rendered / 60 presented with a new frame on every flash; 3× (20 rendered) lagged and presented unevenly, so it stays at Refresh. While the limit paces frame generation, the game's presents are not held (frames ahead, the Refresh cap's release): holding them built a backlog (about 120 ms from frame start to the GPU). The game's own Reflex frame limit is taken out of the driver, which would count the presenter's presents; if the game falls far below the limit, it is dropped for the session. A stall or a rebuilt swap chain (its first second has few presents) does not count as frame generation switching off: in testing that switched the limit off after the layer's automatic swap-chain rebuilds.
- **[DX]** No changes; version number only.
- **[Docs]** Settings guide, example ini, Vulkan README; tested games: Indiana Jones and the Great Circle with 2× frame generation.

## [1.0.22] — 2026-10-26

- **[DX] Line Reflex's frame limit up with the flash (`ReflexAlign`, off by default).** Reflex's frame limit starts each game frame on its own clock, so a finished frame waited up to a whole strobe cycle before the lit refresh showed it, by an amount that changed from launch to launch. With this on, the presenter measures that wait (from the frame's copy finishing on the GPU) and moves the start of each frame until frames are ready about 2 ms before their flash, backing off when frames start missing it. God of War Ragnarök: about 31 ms from release to flash every launch, against 31–44 ms. Games with uneven frame times gain little and can repeat a frame now and then (Spider-Man 2: 1.5 ms quicker, more repeats; Dead Space: no change), so it is off by default. Needs Lowest latency pacing; not with frame generation. The reflex timing log has two new columns: how long frames waited before their flash (also with the option off) and how far the frame start has moved.
- **[DX] Fixed: loading stalls read as frame generation.** The Witcher 3 with frame generation off stopped its Reflex reports in loading stalls at 27–29 fps, which counted as frame generation for about 20 s (the cap at Refresh, Reflex's limit off). DLSS-G on the present path now counts only at a frame rate frame generation would give.
- **[Docs]** Tested games: God of War Ragnarök with `ReflexAlign=1`; Dead Space (2023) with Lowest latency pacing.

## [1.0.21] — 2026-10-26

- **[DX] Fixed: God of War Ragnarok lost Reflex's frame limit at launch.** The game has DLSS Frame Generation on its present path with frame generation off. In the first second of Reflex reports, before the rendered-frame counts came in, that was taken as frame generation, the limit doubled, the game fell to 30 fps, and the collapse check switched Reflex's limit off for the session (the automatic frame cap then used the presenter's own cap: about 45–53 ms from frame start to the GPU against about 15 ms with Reflex's limit). DLSS-G on the present path now counts only once the game's Reflex reports have stopped (Returnal with frame generation), and the frame-generation factor steps up only after 3 seconds in a row agree.
- **[DX] Frame generation switching on or off no longer drops Reflex's limit.** Until the switch is confirmed the limit keeps its frame-generation factor, so the game runs at half the cap for a few seconds (Returnal with frame generation turned off); the collapse check now waits for it. The check for frame-generation DLLs on the present path also runs every quarter second instead of every 30th present: at low frame rates it read frame generation on, off, on... every two seconds.
- **[DX] Status box: the frame-generation line starts with its state** (off, active, or active with its factor, e.g. 2x) and lists the loaded components after it. It only listed the components, which some games load with frame generation off.
- **[DX] [Vulkan] [Shader] Support on Ko-fi.** A **Support on Ko-fi** button at the end of the Save preset / Reload ini row opens ko-fi.com/artzox in the browser; the shader shows the link in a Support section at the end of its settings.
- **[Docs]** Tested games: Marvel's Spider-Man 2 with and without frame generation; Returnal: change frame generation only from the main menu or restart the game after changing it (turning it off mid-game slows the game down, also without the presenter); Onimusha: OptiScaler NR keeps frame generation on in menus.

## [1.0.20] — 2026-10-23

- **[Vulkan] Fixed: SDR games shown as HDR10 (`SdrToHdr`) were oversaturated.** Their BT.709 colours went into the HDR10 output unconverted; they now go through the BT.709 → BT.2020 conversion (ITU-R BT.2087) first. Thanks to speedlemur (#70).
- **[DX] [Vulkan] [Shader] The ABL guard follows the panel's window curve.** It only kept the lit refresh's average under `AblGuardBudgetNits`, but an OLED holds less brightness on a large area than on a small one: a 25% area at 800 nits (average 200) went through and the TV's own limiter dimmed it. Each bright area is now held to what the panel sustains at its size, by default from **Peak brightness** (`DisplayPeakNits`) at 10% of the screen down to the budget at 100% (e.g. about 700 nits at 25% and 420 at 50% with 1400 and 250). `AblCurve=10:1400,25:600,50:400,100:250` takes a panel's measured curve (DX and Vulkan). Thanks to speedlemur (#71).
- **[DX] [Vulkan] [Shader] The ABL guard measures HDR10 with BT.2020 luminance weights.** It used BT.709's, which under-counted red by about 19% in HDR10. SDR and scRGB are unchanged. (#70)
- **[Docs]** Tested games: Mirror's Edge Catalyst with Windows Auto HDR: lower Gain or Auto HDR's intensity; the ABL guard can't see Auto HDR's output.

## [1.0.19] — 2026-10-23

- **[DX] [Vulkan] Flash hints.** When refreshes keep being missed (3 of the last 10 seconds, loading screens not counted), the status box suggests the next step, one at a time, instead of the general "Missed refreshes break the strobe" line: start Steam or the game's launcher as administrator when the presenter's GPU queue fell back from realtime (DX); 4 presenter buffers at 2 or 4 frames per cycle (no extra latency); Queue one refresh ahead (+1 refresh of latency); then lighter game settings or the Half refresh cap. The VRR, frame-limiter and VSync warnings still come first. Buttons apply a step and save it to the game's `BFIPresenter.ini`; nothing is switched on without a click. **Hide flash hints** turns them off for the game (`FlashHints=0`; Advanced > Flash hints brings them back). The hint goes away after 30 s without missed refreshes.

- **[DX] Fixed: games collapsing to about 7–8 fps under a Reflex frame limit (The Witcher 3, Returnal, Pragmata).** The driver's frame limit (Reflex's minimum interval, and NVIDIA's Max Frame Rate while Reflex is on) paces the game by the presents it sees on the swap chain. With the presenter those are its own, 120 a second whatever the game does, so the driver kept holding the game longer until it ended at about 7 rendered fps. The presenter now paces the game itself where Reflex would, in the game's own Reflex sleep call at the start of each frame, and gives the driver no limit. With frame generation it paces the rendered frames (e.g. 30 for 60 presented with 2× frame generation). The Witcher 3 with DLSS frame generation now holds 60 of 60 at the Refresh cap with the automatic frame cap, about 30 ms from release to flash, and at Half refresh with **Apply the cap with Reflex's frame limit**. A game's own Reflex limit (Returnal sets 16,666 µs by itself) is applied the same way and taken from the driver. Pragmata with 2× or 3× frame generation now holds Refresh with the automatic frame cap too. Games that call their Reflex sleep more than once per rendered frame (DLSS 5 injectors with frame generation in Onimusha: Way of the Sword call it twice) keep the driver's limit, which held them at an even 60; ReShade.log says so. `ReflexLimitAtSleep=0` goes back to the driver's limit everywhere. The safety net for collapses stays.
- **[DX] Automatic frame cap in Reflex games: Reflex's limit with frame generation, and by default without it too.** With frame generation, Reflex's limit (now at the game's sleep) paces it: Alan Wake 2 with 3× about 42 ms from frame start to GPU against about 140 ms with the presenter's cap. Without frame generation, frame start to flash was about the same or a little quicker with Reflex's limit (Onimusha about 54 ms against 58, God of War Ragnarök 54–64 ms against about 70). **Presenter's own cap without frame generation** (`AutoCapOwnWithoutFg=1`) uses the presenter's cap with just-in-time release while frame generation is off; the cap then follows frame generation being switched on and off.
- **[Docs]** With frame generation in Reflex games the guides now recommend the Refresh rate cap with the automatic frame cap also when frame generation only reaches about 60 fps (Half refresh + Smooth remains the advice without Reflex). Tested games: The Witcher 3 and Pragmata with frame generation at Refresh + the automatic frame cap; Returnal's own Reflex limit; Alan Wake 2 with 3×; Onimusha: Way of the Sword with DLSS 5 (ShortFuse's injector or OptiScaler NR) and 2× frame generation, which turns frame generation off in its menus.

## [1.0.18] — 2026-10-23

- **[DX] [Vulkan] The overlay's controls no longer move when the status changes.** The status line and the warnings at the top sit in a box of fixed height (7 lines; it scrolls if more are showing), so a warning appearing or going no longer pushes every control below it up or down. **BFI enabled** is now right under the version line, above the box.
- **[DX] Fixed: DirectX 11 games that make a second swap chain for their window hung at start.** DARQ (32-bit Unity) makes a new swap chain about a second after starting (4× MSAA, then 2×) while the first still exists; the presenter stayed on the first, and the game presented to the new one under the presenter's window. The presenter now moves to the new swap chain.
- **[Docs]** Tested games: Observer: System Redux holds 120 fps at 4K with the Refresh rate cap + Lowest latency. Need for Speed: Porsche Unleashed now recommended with the NFS5 Enhanced mod (its Direct3D 11 renderer) and `BFIPresenter.addon32`, at Refresh rate + Lowest latency, with the CRT-Standalone shader for the full retro look. Riven (2023) with DLSS 5 at 4K: XeSS frame generation through OptiScaler, limited to 60 (OptiScaler.ini and Engine.ini settings in the notes), at the Refresh rate cap with the automatic frame cap off. DARQ (32-bit, v1.0.18 or newer); ReShade's overlay can misbehave in it on its own (stray mouse movement and keystrokes).

## [Manager 1.3.8] — 2026-10-21

- **[Manager] Fixed: Observer: System Redux (and Observer) were missing from the list.** Executables with "server" in the name are skipped as dedicated servers, and "Observer" contains it; every exe of the game was skipped, and a game without an exe isn't listed. Words like Observer and Crash Bandicoot are now exempt.

## [Manager 1.3.7] — 2026-10-21

- **[Manager] Direct3D 8 games are recognised** (from PCGamingWiki and from a `d3d8.dll` import) and shown as "D3D8" instead of "?". They get the DX add-on route, through Direct3D 9: **Install ReShade** copies ReShade as `d3d9.dll`, and the game's notes say to download the d3d8to9 converter by crosire and put its `d3d8.dll` next to the executable, with a link to [d3d8to9](https://github.com/crosire/d3d8to9) (or to use ReShade's own setup with Direct3D 8 picked, which downloads it). With dgVoodoo installed they stay on its Direct3D 11 route. Executables are read again once for the new check.
- **[Manager]** Links in a game's notes can be clicked and open in the browser. The labels in the details pane are spaced to the longest one, so "NVIDIA profile" no longer runs into the profile name.
- **[Manager]** A PCGamingWiki page that lists only APIs BFI Presenter doesn't support (e.g. Direct3D 7) now says so in the game's notes, instead of "PCGamingWiki's page lists none".

## [Manager 1.3.6] — 2026-10-21

- **[Manager] Fixed: PCGamingWiki lookups stopped working for everyone.** PCGamingWiki no longer answers anonymous Cargo (database) queries ("You don't have permission to run arbitrary Cargo queries"), so every lookup failed, `pcgw_cache3.txt` stayed empty, and games whose executable doesn't show the API (launchers, APIs loaded at run time) kept "?". The Manager now reads the game pages themselves through the standard MediaWiki API, up to 20 pages per request by title with redirects followed, and takes the Direct3D, Vulkan and OpenGL versions from each page's API box. A Steam game must land on a page that lists its app id; if its name doesn't lead there, the wiki's Steam app id redirect finds the page (one request per such game). Results already in the cache are kept. The first scan of a large library takes a few minutes; after that games are looked up again every 30 days (7 when not found).
- **[Manager]** Requests now identify the Manager by name, version and repository, as wikis ask of API clients, and ask PCGamingWiki for JSON instead of GitHub's media type.

## [1.0.17] — 2026-10-21

- **[Both] New: Gradual mode for the LCD image-retention protection** (`LcdSaverGradual`, Mode: **Immediate** | **Gradual**; Immediate stays the default). Immediate is the existing fade, shift by one refresh, fade back every `LcdPhaseSwapSeconds`. Gradual instead slides the light slowly from one refresh into the next, with the cycle's light kept constant, so there is no fade, no brightness change and no stutter. The design follows the LCD Saver of the [Blur Busters](https://blurbusters.com) CRT beam simulator by Mark Rejhon and Timothy Lottes (MIT), written from scratch for BFI Presenter.
  - **At 2 frames per cycle: hold, drift, hold.** Halfway through a slide the light is split 50/50 over both refreshes, which is the same as no BFI for that moment, so the light stays fully on one refresh for `LcdGradualHoldSeconds` (120), slides to the other over `LcdGradualDriftSeconds` (10) and holds again. Motion is sharp about 92% of the time. Hold 0 = drift continuously.
  - **At 4 frames per cycle and up: continuous drift**, once through the whole cycle every `LcdGradualSeconds` (1000, 20–1200, the Blur Busters default). A 4-frame cycle is still dark half the time mid-slide, so clarity barely changes.
  - Plain strobe only (BFI and Variable MPRT); with Sine BFI, Invert cycle or Rolling scan the Immediate swap is used. With the BFI method the highlights are rolled off once for the whole lit picture and then shared between the refreshes, so the slide doesn't change them.
  - Not tested on a high-refresh LCD yet: reports welcome. If a ghost image appears at 2 frames, lower the hold seconds (one IPS tester still saw one with Immediate swaps every 30 s).
- **[Docs]** Settings guide (and PDF): the LCD section explains both modes, with a new diagram of the hold–drift–hold and the slow drift. DX reference, example ini and README updated.
- **[Tools] New: log analyzer and motion test** (`BFIPresenter-Tools-v1.0.17.zip`). The log analyzer is an offline page: drop in a session's timing logs and `ReShade.log` for per-setting cadence, repeats and skips, latency, and the setting that ran most evenly. The motion test (`BFIMotionTest.exe`) is a Direct3D 12 program with moving test patterns that ReShade and the presenter hook like a game, to compare settings on the same picture on any display. Both are in `tools/`.
- **[Docs]** Tested games: The Elder Scrolls IV: Oblivion Remastered recommended without frame generation (it shows a lot of artifacts in this game), at 2 frames per cycle with Half refresh + Smooth + Apply the cap with Reflex's frame limit, especially with DLSS 5 (just-in-time release is at its limit there: the game's frames take nearly the whole cap interval).

## [Manager 1.3.5] — 2026-10-19 (with the 1.0.16 release)

- **[Manager] Update button.** When the latest GitHub release carries a newer Manager, **Update Manager to …** appears next to **Check GitHub** (checked once after the first scan of each start, also when a folder is picked as the source). It downloads the new exe, swaps it in for the running one and restarts; the game list, settings and backups are kept. Needs the Manager in a folder you can write to.
- **[Manager] Fixed: "?" for many games' APIs after a large scan.** PCGamingWiki was asked about 25 games per query; for games without a Steam app id (looked up by name, with their edition variants) the query URL grew past the Manager's 4,096-character limit, the lookup stopped there, and every game after it fell back to its executable, often "?". Queries are now kept short, a failed one is retried once and then skipped (two in a row stop the lookup), and the cache is saved after each one, so the next scan continues where it stopped instead of asking again for every game. The detail pane says why an API is "?".
- **[Manager] Fixed: the PCGamingWiki tooltip appeared on "Ask about G-SYNC and Vertical Sync when installing"** (under Libraries...). Each option has its own tooltip now.
- **[Docs]** Tested games: Moons of Madness now recommended with the presenter at Half refresh + Lowest latency (the BFI shader still works); Blasphemous at Refresh + Lowest latency (locked at 60 fps), or with the BFI shader at 120 Hz with NVIDIA Smooth Motion.

## [1.0.16] — 2026-10-19

- **[DX] Fixed: a loading screen could switch off the Reflex frame limit for the whole session.** The collapse safety net from 1.0.14 counted a loading stall right after the automatic frame cap stepped down as a collapse (Until Dawn's startup load: 1–12 fps, with the hitch guard paused and the window not responding), so frame generation then ran uncapped at about 65 fps. Seconds below 3 fps or with the hitch guard paused now restart the check instead of counting. Real collapses keep drawing steadily with the hitch guard running and are still caught (Returnal about 7 fps, Pragmata about 8). Until Dawn with frame generation now holds 60 at the Refresh cap.
- **[Docs] Frame generation at Half refresh.** When frame generation's output is only about the strobe rate (e.g. 60 at 120 Hz: 3× at about 20 rendered fps, 2× at about 30), Half refresh + Smooth shows every generated frame and was both smoother and quicker than Reflex's limit at Refresh: Alan Wake 2 with 3×, 60 of 60 flashes at about 99 ms from input to flash against about 7% repeated at about 130 ms. README, settings guide, DX README and tested games.
- **[Docs]** Tested games: Pragmata works with frame generation at Half refresh + Smooth (change the resolution once to start BFI); Until Dawn at Half as an alternative; Kena runs smoothly at Refresh with the automatic frame cap; South of Midnight and Alan Wake 2 recommended settings.
- **[Vulkan]** No changes; version number only.

## [1.0.15] — 2026-10-19

- **[DX] New: Apply the cap with Reflex's frame limit** (`CapWithReflex`, **off by default**; checkbox under Game frame cap). With Half, Quarter or another cap below the refresh rate, in games with NVIDIA Reflex, the cap is applied by Reflex's own frame limit, like a game's own Reflex fps limit, instead of just-in-time release. **Only for games whose frames arrive irregularly at Half or Quarter**, where just-in-time release keeps switching itself off and motion stutters: The Midnight Walk at Half, 1.7% of flashes off-cadence with it against 15.6% without, with no in-game limit needed. **It adds latency compared with just-in-time release**, about 10–20 ms, and is no smoother in games where just-in-time release works: Silent Hill 2 at Half, 2.2% against 1.3% off-cadence and 34 ms against 25 ms release to flash; Alan Wake 2 at Quarter, 56 ms against 35 ms. Games without Reflex keep the presenter's cap; a game that collapses under the limit falls back to it for the session.
- **[DX]** The log no longer says "automatic frame cap: Refresh rate (frame generation active, no Reflex)" when the cap is changed by hand.
- **[Docs]** Settings guide, DX README and example ini: the new option. Tested games: The Midnight Walk recommended at Half with it.
- **[Vulkan]** No changes; version number only.

## [1.0.14] — 2026-10-19

- **[DX] Fixed: games that collapse under a Reflex frame limit.** In Returnal the automatic frame cap's Reflex limit dropped the game from 60 to about 7.5 fps, and so did the game's own 16,666 µs limit, which it switches on and off by itself. The presenter now notices the collapse within 2 seconds and, for the rest of the session (also across swap-chain changes): sets no Reflex limit, takes the game's own limit out of its Reflex calls, and caps with its own cap instead. Returnal now holds a steady 60 fps without frame generation. The log says when this happens.
- **[DX] Frame generation without Reflex reports.** When a game that gave Reflex reports stops giving them while DLSS-G presents, the automatic frame cap counts it as frame generation and stays at Refresh, instead of holding DLSS-G's frames with its own cap.
- **[Docs]** Tested games: Jusant recommended with DLSS frame generation and the automatic frame cap (it removes the engine's judder; with RenoDX, use the UE Extended mod), Returnal notes. Settings guide and DX README: the collapse fallback.
- **[Vulkan]** No changes; version number only.

## [1.0.13] — 2026-10-18

- **[DX] Fixed: with DLSS frame generation the Reflex frame limit switched on and off every second** (1.0.12): the frame-generation check compared one second's presents with the previous second's rendered frames, so the limit undid its own detection, with heavy stutter (God of War Ragnarok). Presents and rendered frames are now compared over the same second, and the state changes only after 3 seconds in a row (on) or 5 (off). The automatic frame cap doesn't step down while that may be changing.
- **[DX] Reflex games: the automatic frame cap steps down with Reflex's frame limit, with or without frame generation.** The presenter's own cap is used only in games without Reflex. When DLSS-G switched itself off near the limit, the presenter's cap had held the game at Present behind DLSS-G's queue (~57 ms from release to flash, God of War Ragnarok); the driver's limit keeps the game paced where it reads input. With frame generation, it tries Refresh only every 5 minutes (each try is 3 s without the limit, which DLSS-G shows as a stutter).
- **[DX] Reflex limit phase.** The limiter paces the game on its own clock, so its frames could settle right at the moment the presenter picks one for the lit refresh, where a little jitter repeats one frame and skips the next again and again (10–18 times a second for a whole run, against none in another). When that happens, the limit is shortened by 0.15 ms for one second to move the frames away; the log says when. Tested in Control, Silent Hill 2, Alan Wake 2 and God of War Ragnarok with frame generation: a new frame on 60 of 60 flashes.
- **[Vulkan]** No changes (the Vulkan layer has no Reflex limit); version number only.

## [1.0.12] — 2026-10-18 (Manager 1.3.4)

- **[Both] Automatic frame cap** (`AutoFrameCap`, on by default, with the Refresh rate cap). When the game can't hold the refresh rate (3 s below 95% of it, e.g. 90 fps at 120 Hz), frames between the strobe rate and the refresh rate were dropped unevenly, which judders. The cap now steps down to one game frame per strobe cycle (e.g. 60 fps at 120 Hz with 2 frames per cycle), so every flash shows the next frame (Onimusha: a new frame on 60 of 60 flashes, ~25 ms release to flash). Back to Refresh: in DX games with NVIDIA Reflex, when the game's own reports (GPU, simulation and submit time per frame) stay under 85% of a refresh for 10 s; otherwise after a ~3 s try at Refresh that holds 95% of the refresh rate, after 30 s, then 60 s, then every 120 s while it keeps failing. The overlay shows which cap is in use. Old `BFIPresenter.ini` files don't have the key, so they get it on too: untick it in the overlay and save to keep the old behaviour.
- **[DX] Frame generation: Reflex frame limit at one presented frame per strobe cycle.** With the automatic frame cap on and frame generation detected, the cap stays at Refresh and Reflex's own frame limit is set to one presented frame per strobe cycle (e.g. 16.7 ms at 120 Hz with 2 frames): the driver paces DLSS frame generation so every flash gets the next frame (God of War Ragnarok: 60 of 60, ~21.5 ms release to flash, against 57 with frames dropped at ~76 fps). The game's low-latency and Boost choices are kept, the limit is sent again every 2 s (the game or driver reset it), and the game's own setting comes back when frame generation stops. Frame generation is detected from Reflex's rendered frame numbers (presented frames well above rendered ones), or, without Reflex, an FSR or XeSS frame-generation module presenting; DLSS-G's module alone doesn't count, since it sits on the present path with frame generation off too. Needs a game that uses Reflex (every DLSS frame-generation game does). The overlay warns when frame generation runs with a cap below the refresh rate. The Vulkan layer has no Reflex frame limit: with a frame-generation module loaded, its automatic frame cap stays at Refresh (in testing, Vulkan games' Reflex calls didn't show whether frame generation was on, and the limit made presentation less steady).
- **[Vulkan] Presentation recovery backs off when rebuilds don't help.** In a game that keeps the GPU busy, images come back late for that reason, not because of the desktop compositor, and a new swap chain can't fix it: Indiana Jones and the Great Circle with heavy settings rebuilt every 20-30 seconds. When presentation slows again within a minute of a rebuild, the next one waits twice as long (20, 40, 80 s); after 3 such rebuilds in a row, automatic recovery stops for the session and the log says why. Ctrl+Shift+O still rebuilds by hand, and no longer rebuilds twice when the keys are held a moment. Late images during the automatic frame cap's short tries at Refresh don't count either: the game runs uncapped then and fills the GPU (Red Dead Redemption 2: 20-30 a second).
- **[Both] Lowest latency is the default frame pacing for new ini files.** Smooth adds about a refresh of latency at the Refresh cap, and with frame generation a full cycle (God of War Ragnarok: ~38 ms against ~21.5 ms) with no smoother motion; it only helps at Half or Quarter while just-in-time release keeps hitting its limit. Existing `BFIPresenter.ini` files keep their `FramePacing`: set it to 1 by hand, or pick Lowest latency in the overlay and save.
- **[DX] Reflex timing log:** with the timing log on, `BFIPresenter_reflex_<date>_<time>.csv` has a line a second with the cap in use, the Reflex limit applied, release-to-flash latency and the game's Reflex reports (simulation, GPU and queue times, rendered fps).
- **[Manager] Battle.net games** are found too (Blizzard's and Activision's games, from their uninstall entries), on by default under **Libraries...**.
- **[Manager] VR-only games are left out** (**Leave out VR-only games**, on by default): Steam's own "VR only" flag, elsewhere a title with "VR" as a word. BFI strobes a monitor or TV, so a headset game can't use it; games with an optional VR mode stay in the list. The scan result says how many were left out.
- **[Manager] [Vulkan] Vulkan layer for all users.** Windows' Vulkan loader ignores per-user layers in programs run as administrator, so with User Account Control turned off, or Steam or the game set to run as administrator, the layer never loaded (no `BFIPresenterVK.log`, DOOM: The Dark Ages and DOOM Eternal in a user's report). The Manager's **All users** button registers it machine-wide (with an administrator prompt), and it warns when it sees those settings; run as administrator, it does that by itself. `install_vulkan_layer.bat` run as administrator registers it for all users too, and `uninstall_vulkan_layer.bat` removes both. The Manager also warns, with **Fix order**, when ReShade's Vulkan layer is listed after BFI Presenter's.
- **[Manager] Connecting a drive rescans.** An external disk with a Steam library plugged in while the Manager was open wasn't picked up until **Rescan**; the Manager now rescans a few seconds after a drive is connected.
- **[Manager] Warning for online and anti-cheat games.** Games Steam lists as multiplayer or online (PvP, co-op, MMO) and games with Easy Anti-Cheat, BattlEye or another anti-cheat in their folder get a warning in their details, and installing BFI Presenter or ReShade in them for the first time asks first: ReShade with add-ons can count as cheating, and playing online with it can get an account banned, so installing is at your own discretion. **Leave these out** installs only in the other ticked games.
- **[Manager] Steam libraries on hover.** With more than one Steam library, hovering over "and N more libraries" under **Libraries...** lists their folders.
- **[Manager] Support on Ko-fi** button (top right), for anyone who wants to support BFI Presenter's development.

## [1.0.11] — 2026-10-16

- **[Vulkan] Exclusive fullscreen for the presenter's swap chain** (`VulkanExclusiveFullscreen`, on by default; overlay checkbox with its state). The presenter's own swap chain takes application-controlled exclusive fullscreen (`VK_EXT_full_screen_exclusive`); the game's window mode is unchanged, and a game's own exclusive-fullscreen choice is replaced on that swap chain only (the game never presents to it). Opening the RivaTuner OSD or alt-tabbing could move presentation into the desktop compositor for good: the display then handed the presenter's images back late and out of order, and the strobe flashed until the game restarted. Indiana Jones and the Great Circle now stays steady, also after alt-tab. Applies at the next game start.
- **[Vulkan] Automatic presentation recovery** (`VulkanAutoRecover`, on by default; Ctrl+Shift+O by hand). A window briefly covering the game (a launcher's splash screen) or the ReShade menu can still leave Windows in that slower mode after it's gone. When the display hands frames back late for 3 seconds, the game is asked to rebuild its swap chain, which brings the strobe back in step: a blink in most games, a few seconds in heavy ones, at most every 20 seconds. The log says when, and lists the windows over the game at that moment. Restarting exclusive fullscreen alone didn't help, so it isn't tried.
- **[Docs]** Settings guide and Vulkan README: both keys, Ctrl+Shift+O, and a tip to change settings in `BFIPresenter.ini` rather than the ReShade menu while playing a Vulkan game.

## [1.0.10] — 2026-10-16 (Manager 1.3.3)

- **[Both] Automatic frames ahead goes back to 2 on scattered hiccups.** It stepped up only after 3 seconds in a row with uneven strobe cycles, so a game that missed a cycle every few seconds stayed at 1 with a repeat-then-skip stutter (e.g. about 16 a minute at 1, none at 2). Now 6 uneven cycles within 30 seconds at 1 frame ahead (not counting loading pauses) also bring it back to 2; needed again within a minute of trying 1, it stays at 2.
- **[Both] Just-in-time release switches itself off when it doesn't help at its limit.** At the Half / Quarter cap with game frames taking nearly the whole interval, it releases as early as the cap allows (that fixed Obduction). Where frames still miss their lit refresh that way (3 in 20 seconds), it now releases at the refresh as with the option off, until game frames have fitted the interval for 30 seconds, and for the rest of the session once that has happened twice (switching straight back on brought the repeats back within a second). The log says when.
- **[DX] Overlays that hook Present later no longer get into the presenter's presents.** With `ShowOverlays=0` the presenter calls DXGI's Present past any hook found when it starts, but a function that wasn't hooked yet was called as is, so a hook written later still ran: RivaTuner's OSD switches itself on in some games mid-session, drew on the presenter's frames and crashed Alan Wake 2. The presenter now always calls a copy of Present's first instructions, so hooks added later are skipped too.
- **[DX] No false "driver is ignoring VSync" error with frame generation.** It needs 2 seconds in a row now, and presents clearly above the display's refresh rate: DXGI's refresh count can lag for a second, and DLSS-G starting up in Control: Resonant got flagged at 120 presents/s on a 120 Hz display.
- **[Both] [Shader] Rolling scan defaults: phosphor decay 0.2 (was 0), seam 0 (was 0.08).** Pixels in the seam's blended zone are lit in both bands' refreshes, so the seam blurred visibly in motion; phosphor decay hides the band edge without that. The seam is now a hard edge unless you set it, and only applies with decay 0. Existing `BFIPresenter.ini` files keep their values. The overlay's seam slider has a tooltip saying so.
- **[Tested games]** Replaced.
- **[Docs]** Settings guide: strobe-rate formula for any refresh rate and frame generation; "CRT displays" in the intro; the Gain tip assumes the ABL guard is set up.

## [1.0.9] — 2026-10-15 (Manager 1.3.2)

- **[DX] Show overlays** (`ShowOverlays`, Advanced section, off by default). The presenter's presents go through overlay hooks again, so the RivaTuner OSD and the Steam overlay draw on the strobed frames in 64-bit DirectX 12, 11 and 9 games. Ignored in 32-bit games (they crashed or still showed no overlay in testing) and OpenGL games (no effect). The move onto the presenter queue stays direct. Off by default because Steam's Present hook crashed Pragmata and Resident Evil 4 this way; it applies at the next game start, since overlays set themselves up on the first present they see. Keep RTSS's frame limiter off. Vulkan games already show overlays.
- **[DX] [Vulkan] LCD phase swap: 5 seconds by default** (was 30), counted from the end of each fade, so the strobe runs at full depth for the whole interval. An IPS user still saw image retention at 30 s; at 1 s it was almost gone, but with 0.8 s of fading per swap the strobe was then softened most of the time. Existing `BFIPresenter.ini` files keep their `LcdPhaseSwapSeconds`: set it to 5 by hand.
- **[Shader 1.0.9] LCD: prevent image retention**, the add-on's phase swap for the BFI shader (LCD category, off by default, 5 s).
- **[Shader] Fixed: the hitch guard's refresh drift never added up.** The state copy wrote the first state value into every slot, so the drift was reset each frame and only a single frame more than 0.6 refreshes off counted as a miss. Frame times are now summed as described, so a slow drift (a missed refresh hidden by jitter) pauses BFI too.
- **[DX] [Vulkan] Frame cap list:** Quarter refresh now reads "one game frame per two lit + dark pairs" instead of "30 fps at 120 Hz", which confused people on other refresh rates. The guide says "half / a quarter of the refresh rate".
- **[Docs]** Settings guide: diagrams for frames per cycle, the LCD phase swap, frame pacing and delay, as images in light and dark.
- **[Manager 1.3.2]** Carries the 1.0.9 shader.

## [1.0.8] — 2026-10-15

- **[DX] [Vulkan] LCD: prevent image retention** (`LcdPhaseSwap`, `LcdPhaseSwapSeconds`, off by default). An LCD drives each refresh with the opposite polarity of the one before; with an even number of frames per cycle every lit refresh got the same polarity, a DC bias that can leave temporary image retention on IPS/VA/TN panels after long sessions. On, every N seconds (30 by default, 1–300) the strobe fades over about 0.4 s to the steady average picture the hitch guard shows, repeats one refresh while every refresh looks the same, and fades back: the lit refreshes then sit on the other polarity. The fade keeps every cycle's light the same, so nothing flickers; motion is briefly a little softer. (A second dark refresh, then a single average-brightness refresh, were tried first and both flickered.) Not needed on OLED or with an odd number of frames per cycle.
- **[Docs]** README warning for LCD users; settings guide and DX reference entries.

## [1.0.7] — 2026-10-14 (Manager 1.3.1)

- **[DX] [Vulkan] Two new game frame caps for fast displays.**
  - **One game frame per strobe cycle**: the refresh rate ÷ Frames per cycle, for example 60 fps at 360 Hz with 6 frames, 90 fps at 360 Hz with 4, or 80 fps at 240 Hz with 3. Each game frame gets exactly one lit refresh, as Half refresh does with 2 frames at 120 Hz.
  - **Custom**: a new game frame every `CapDivisor` refreshes (1–8).
  - Both are counted against the presenter's refreshes like the other caps, so they stay in step with the strobe. Just-in-time release works with them whenever the interval is a whole number of strobe cycles.
  - The overlay shows the cap's frame rate, and warns when the interval doesn't fit the cycle (some game frames would start on a dark refresh).
  - Frame-rate limiters that only target an fps value (made for VRR) can't do this: at a rate that isn't the refresh ÷ a whole number, game frames drift across lit and dark refreshes.
- **[Docs]** README: Special K's SDR-to-HDR doesn't work with the presenter.
- **[Manager 1.3.1] Graphics API detection fixes.**
  - **The Witcher 3 (GOG) is no longer shown as OpenGL.** Its executable links `opengl32.dll` and loads Direct3D at run time. When OpenGL is the only API found, the exe is now also checked for the Direct3D 11/12 DLL and entry-point names and for a `dx12` / `dx11` folder (`bin\x64_dx12`).
  - **PCGamingWiki lookups for store titles with an edition name** (GOG's "The Witcher 3: Wild Hunt - Game of the Year Edition") also try the name without it.
  - **When several wiki pages list the same Steam AppID**, the page named like the game is used.
  - **"unknown" or "n/a" in a wiki API field** no longer counts as supported.
  - **An error or rate-limit answer from the wiki** is no longer recorded as "not on the wiki" for a week.

## [1.0.6] — 2026-10-14 (Manager 1.3.0)

- **[DX]** No changes apart from the version number.
- **[Docs]** README: "No warranty — use at your own risk" and a list of everything BFI Presenter and the manager change on a PC.
- **[Tested games]** PT-PC.
- **[Manager] ReShade for Vulkan games:** a **Turn on / Turn off** button in the game's ReShade row switches ReShade's own Vulkan layer on for the game by writing the `ReShade.ini` next to the exe that the layer loads on, so the BFI Presenter (Vulkan) tab is there in the game. It also tries to add the game to ReShade's setup list (`ReShadeApps.ini`), which usually needs administrator rights; without it ReShade still works, only ReShade's setup won't offer Uninstall for the game. The install question offers it for Vulkan games when ReShade's Vulkan layer is installed (skipped for games the tested list runs without ReShade). Before, "Install ReShade" put the DirectX ReShade into Vulkan games.
- **[Vulkan] `BFIPresenter.ini` is reloaded when it changes** while the game runs (checked once a second), so without ReShade a setting can be edited in the ini and seen at once, without Ctrl+Shift+R or a restart.
- **[Manager] Vulkan games that ship SDL are no longer taken for DirectX games** (PT-PC). SDL's DLL names every renderer it could use, so it is now left out of the API scan, and an executable that can only load Vulkan goes to the Vulkan layer even when a wiki lists Direct3D too. Before, such a game got ReShade as `dxgi.dll` and the DX add-on, and crashed.
- **[Manager] Warning for Vulkan games with a DirectX ReShade or the DX add-on in their folder**, with what to remove.

## Manager 1.2.0 — 2026-10-13

- **[Manager] Batch actions no longer freeze the window.** Update outdated, Install / update and Remove for ticked games, and installing ReShade in many games now run a few games per frame with a progress dialog (game n of N, the current game, a Stop button) instead of blocking until the last game is done.
- **[Manager] Readable, adjustable game list.** Columns keep their own width (drag a header border), can be reordered and hidden (right-click a header), and the list scrolls sideways instead of squeezing the headers; widths, order and sort are remembered (`manager_ui.ini`). A splitter between the list and the game details can be dragged to give either side more room, and is remembered too.
- **[Manager] No phantom OpenGL in the API column.** Engines link OpenGL next to Direct3D without shipping it on Windows (Unreal: Jusant, ABZÛ, Ashen, Blair Witch). When the executable also uses Direct3D, OpenGL is no longer listed for it; it still is when it is the only renderer (Doom 3 BFG, Heretic II) or when PCGamingWiki lists it.
- **[Manager] Fewer games with an unknown API.** The renderer is also looked for in DLLs in the usual sub-folders (x86, x64, lib, bin), in HashLink `.hdll` modules (Dead Cells) and, when no DLL imports a graphics API, in their strings (Quake II-style `ref_gl.dll`: Heretic II). FNA games show Direct3D 11 with a note to use `--graphics D3D11`; XNA games show Direct3D 9 with a note that ReShade doesn't load there.
- **[Manager] AnyCPU .NET games run 64-bit:** a PE32 .NET assembly without the 32-bit flags (FNA games) is shown and installed as 64-bit.

## [1.0.5] — 2026-10-13

- **[DX] BFI shader** (`BFIPresenter_Shader.fx`, in the DX zip): the presenter's strobe as a ReShade effect, clocked by the game's own frames (one game frame per sub-frame): BFI with highlight roll-off, Variable MPRT, sine and inverted cycles, rolling scan with phosphor decay and the ABL guard, output format from ReShade. A hitch guard sums each frame's time minus one refresh; a missed refresh, a game faster than the display or an average off the refresh rate shows the strobe's average brightness for 500 ms instead of a flash. Only for games that hold a rock-steady 120 fps and whose engine stutters with the presenter (Moons of Madness); everywhere else the add-on is better.
- **[Manager] 1.1.0: Install as → BFI shader.** Copies the shader into ReShade's shader folder (from `ReShade.ini`), turns it on as the last effect in the active preset and removes the add-on; installing the add-on removes the shader again (both backed up, Undo works). Suggested for games the tested list recommends it for.
- **[Manager] 1.1.0:** the app carries the shader itself, so the shader route also works with releases that don't include it. Older Unity games (engine inside the exe, `<exe>_Data\Managed`) are detected as Direct3D 11, not OpenGL (Ori and the Blind Forest: Definitive Edition).
- **[DX] [Vulkan]** No other changes apart from the version number.

## Manager 1.0.2 — 2026-10-13

- **[Manager] Check GitHub ignored release tags written `v.1.0.4`:** it read them as 0.1.0.4, older than the release already downloaded, and kept using that. Tags with any prefix (`v1.0.4`, `v.1.0.4`) are now read correctly.

## [1.0.4] — 2026-10-13

- **[DX] `BFIPresenter.ini` is read, and on the first start created, as soon as the add-on loads, in every game.** Direct3D 9 and OpenGL games never loaded it before: the file wasn't written until you pressed Save in the overlay, and an existing ini was only read after Reload ini or Ctrl+Shift+R (DOOM 3 BFG).
- **[DX] OpenGL: warning when the presenter's own presents block.** When an OpenGL game's presenter runs well below the refresh rate because its presents block (DOOM 3 BFG ran at about 30 fps, Need for Speed: Porsche Unleashed at 20–46 ms per present), the overlay and the log say to set *Vulkan/OpenGL present method* to *Prefer layered on DXGI swapchain* in the game's NVIDIA profile.
- **[Vulkan] No changes apart from the version number.** The layer runs without ReShade: settings in `BFIPresenter.ini`, Ctrl+Shift+R reloads it, Ctrl+Shift+B / M / S toggle BFI, the method and rolling scan (DOOM Eternal, which doesn't start with ReShade, runs this way).
- **[Manager] 1.0.1:** sets that NVIDIA setting for OpenGL games when installing (a tick in the install question, on by default) and shows it in the game's NVIDIA section (*OpenGL present*). The ini checkbox next to Remove is now labelled "Remove deletes the ini too": it was never an install option.

## [1.0.3] — 2026-10

First public release of this round of changes. (1.0.2 was used only for test builds, so installs from those report 1.0.2 and update tools see 1.0.3 as newer.)

- **[Both] BFI Presenter Manager** (`BFIPresenterManager.exe`, a separate download): finds games in Steam, Epic, GOG, Xbox and other folders, works out what each needs (32/64-bit add-on or Vulkan layer), installs, updates or removes BFI Presenter with backups and undo, installs or changes ReShade, and sets G-SYNC and Vertical Sync in each game's NVIDIA profile. The PowerShell Find/Update scripts are no longer in the release zips (they stay in the repository).


- **[DX] Direct3D 9 games.** The add-on now manages Direct3D 9 games (32- and 64-bit) through the same presenter as DX11 games. It asks ReShade (6.8 or newer) to create the game's device as Direct3D 9Ex, the only kind that can share its frames. Each finished frame (ReShade's effects applied) is copied into a texture shared with the add-on's own D3D11 device, then into the presenter's staging ring, and shown with the strobe through a window laid over the game's. The game's own Present is skipped. Tested: Alan Wake and Lara Croft and the Guardian of Light (both 32-bit).
  - The game stays windowed (borderless at full size). Direct3D 9Ex can't take an exclusive-fullscreen device the way older games ask for one, and the presenter needs a window anyway.
  - D3DX (`d3dx9_NN.dll`, used by most DX9 games to load textures) refuses managed-pool textures once it sees a 9Ex device. The add-on hides 9Ex from D3DX only; ReShade already handles managed textures on 9Ex.
  - Direct3D 9 has no shared fences, so the game's thread waits on the CPU for each frame's copy (about 0.5–1.5 ms at 4K in Alan Wake).
  - The Direct3D 9 runtime puts its own Present back into the device's vtable after the add-on hooks it; the hook is checked and restored before every Present.
  - `DX9Support=0` leaves DX9 games alone. `DX9Ex=0` keeps a plain Direct3D 9 device (windowed, no BFI), and `DX9Diag=1` logs failing D3D9/D3DX calls for troubleshooting.
- **[DX] OpenGL games (NVIDIA).** At ReShade's present event (after its effects and overlay), each finished OpenGL frame is copied into a Direct3D 11 texture shared through NVIDIA's `WGL_NV_DX_interop2` and shown by the same presenter as DX11 and DX9 games. The game's own SwapBuffers still runs, with its swap interval set to 0 so it doesn't wait for vblank (`OpenGLSwapInterval0=0` keeps the game's interval). `OpenGLSupport=0` leaves OpenGL games alone. Tested: Legacy of Kain: Soul Reaver 1 & 2 Remastered (with DLSS 5), Heretic II and Need for Speed: Porsche Unleashed (both 32-bit). Old exchange-swap OpenGL games (Porsche Unleashed) need the NVIDIA profile setting *Vulkan/OpenGL present method: Prefer layered on DXGI swapchain*, else the presenter's presents block for 20–46 ms.
- **[DX] Borderless fullscreen instead of a window** (`BorderlessFullscreen=1`, the default). Where the add-on holds off exclusive fullscreen (DX11 games asking for it, DX9 games on 9Ex), the game's window is made borderless and sized to its monitor, and put back when the game leaves fullscreen. Clive Barker's Undying (through dgVoodoo) stayed in a small window before.
- **[Both] Smooth pacing no longer skips a frame to catch up after a hitch** (`SmoothCatchUp=0`, new default). After a late game frame, Smooth used to drop a frame once two finished frames had waited for three cycles, to get back to the lowest latency; in games whose frames finish close to the lit refresh that meant a repeat and then a skip about once a second (Thief, The Midnight Walk). Now it stays one frame behind (about one cycle, \~17 ms at Half refresh, more latency) and shows every frame once, in order: none of those repeats or skips at 60 fps. The overlay checkbox "Smooth: catch up after a hitch" (`SmoothCatchUp=1`) brings the old behaviour back. The game frame cap tooltip now says to use the Refresh cap with frame generation.
- **[DX] `DX11GamePresent=1`: the game's own swap chain presents too** (Wreckfest). Normally the game's DX11 swap chain never presents, because the presenter shows its frames. In Wreckfest that made the game's GPU frames slower and slower (14 ms to over 100 ms within a minute, down to 5 fps, the GPU "busy" at 70 W), most likely because the driver's per-frame bookkeeping waits for the game's Present. With the setting, each game frame is also presented to the game's own window, under the presenter's, with sync interval 0 and straight to DXGI, past the overlay hooks (with Steam's overlay seeing those presents the game crashed). Off by default; turn it on for games that slow down like this.
- **[Both] `Update-BFIPresenter.ps1` keeps what it replaces and can undo an update.** Each replaced add-on or Vulkan-layer file is kept in `BFIPresenter_backups` next to the script (one copy per build) and logged with its old and new version in `BFIPresenter_update_log.csv`. `-Revert` puts back the files the last update replaced; run it again to step back one more update. Find and Update also handle `BFIPresenter.addon32`.
- **[DX] 32-bit games: `BFIPresenter.addon32`.** A 32-bit build of the DX add-on, for 32-bit Direct3D 11 games with the 32-bit ReShade (tested: Twelve Minutes, BAJA: Edge of Control HD, Burnout Paradise Remastered with the Luma HDR add-on, Dead Cells). The crash logger and watchdog walk 32-bit stacks; calls past other hooks relocate 32-bit code and read 32-bit DLLs' original vtables; per-thread flags use Windows' own TLS. For 32-bit DX11 games this is the route to use: DXVK + the 32-bit Vulkan layer flickered in Twelve Minutes and Alan Wake (known limit, see the Vulkan README).
- **[DX] Vulkan and Direct3D 10 games no longer crash when the add-on is in their folder** (first seen in Heretic II, OpenGL). The probe-swap-chain check (also new in this release) read the game's native swap-chain handle as a DXGI swap chain before checking the API; the add-on now steps aside for anything but Direct3D 12, 11, 9 and OpenGL and says why in the overlay.
- **[DX] `DX11Support=0` works again for the first swap chain.** The setting was checked before the ini was read, so the first DX11 swap chain of a session was always managed.
- **[DX] NVIDIA Smooth Motion: the DX11 path stays off while it is active.** Smooth Motion (driver frame generation) presents through its own swap chain; with the DX11 presenter on top, Blasphemous closed and relaunched itself in a loop ("Another instance is already running"). The overlay and `ReShade.log` say so; turn Smooth Motion off for the game in the NVIDIA app. It adds nothing to 2-frame BFI, which shows 60 images a second at 120 Hz. `AllowSmoothMotion=1` in the ini manages the game anyway.
- **[DX] Tiny probe swap chains are ignored** (Guacamelee 2 with Special K). Overlays and injectors create a throw-away 2×2 swap chain to find DXGI's functions; the add-on took that one, left the game's real swap chain unmanaged, and the game crashed when the probe went away. Swap chains under 64×64 are now skipped; one that is later resized to a real size is taken then.
- **[DX] Epic Online Services overlay is detected.** `EOSOVH-Win64-Shipping.dll`, which games using EOS load from a shared Epic folder (also on Steam, without the Epic launcher), hooks Direct3D 12 and crashed the DX11 presenter in Silt. `ReShade.log` now lists it among the other Present hooks and names its full path. The DX11 path stays off while it is loaded, so the game runs without BFI instead of crashing; rename the file to use BFI (`AllowEosOverlay=1` manages the game anyway).
- **[DX] No more hang or crash when a DX11 game leaves exclusive fullscreen and recreates its swap chain** (The Rogue Prince of Persia). Stopping the presenter waited on the game's window thread while the presenter's Present could need that same thread. It now serves the window's messages while it waits. If the presenter still doesn't stop within 5 s, BFI switches off for the rest of the session and the game continues on its own frames; nothing the stuck presenter uses is freed (freeing it crashed the game). Use borderless fullscreen in this game.
- **[Both] BFI off (Ctrl+Shift+B or the checkbox) now shows every game frame.** The presenter kept the strobe's cycle with BFI off: at the Refresh cap a 120 fps game had every second frame dropped and each shown twice (60 Hz motion), and a CRT shader's own BFI flickered wildly on top of it (Twelve Minutes).
- **[DX] Watchdog: "the game's window is not responding".** When Windows would call the game hung (no window messages for 5 s), `ReShade.log` gets the call stacks of the game's window thread, the presenter's window thread (DX11) and the game's present thread, to show where it is stuck.
- **[Vulkan] Games run through DXVK no longer freeze or crash when BFI starts** (Twelve Minutes, 32-bit). The presenter waited for vblank through DXGI, and in a DXVK game the process's `dxgi.dll` is DXVK's: its vblank wait is only an estimate, and creating its factory started another Vulkan instance and device from inside the presenter thread. The layer now waits on the kernel's vblank event directly and doesn't use DXGI at all. It also signals the present fences DXVK 3 attaches to each present (swapchain maintenance1); left unsignalled, DXVK waited on them for good and the game froze. And SDR games under DXVK are no longer presented as HDR10 (`SdrToHdr` is skipped there): an HDR10 swap chain makes the NVIDIA driver present through DXGI and D3D11, which in a DXVK game are DXVK's own DLLs, and the presenter's first present never returned. A new swap chain the game creates while the previous one still holds the window (no `oldSwapchain`) now retires that one instead of failing over and over (switching display modes in Twelve Minutes).

## [1.0.1] — 2026-10

- **[DX] Unity (Mono) games no longer hang or quit while loading** (Blasphemous). The crash logger ran before every other exception handler and walked the stack of each first-seen access violation. Mono raises those on purpose, and asking Windows to unwind its JIT-compiled code can deadlock inside Mono. The logger now runs after the game's own handlers (exceptions the game handles never reach it) and stops a stack walk at JIT code. The watchdog's stack dumps stop there too.
- **[DX] `CrashLog=0`** in `BFIPresenter.ini` turns the crash logger off completely.
- **[Vulkan]** No changes.

## [1.0.0] — 2026-10

First public release.

- **[Both]** One repository and one version number for both parts. Each release ships `BFIPresenter-DX-v<version>.zip` and `BFIPresenter-Vulkan-v<version>.zip`.
- **[Both]** Functionally the same as DX v0.82 and Vulkan v0.8.2.
- **[Both]** One shared `BFIPresenter.example.ini` with every key, including the DX-only and Vulkan-only sections.
- **[Vulkan]** The version is stored as `BFI Presenter (Vulkan layer) v<version>` in the DLL, the log and the overlay.
- **[Both]** `Find-BFIPresenter.ps1` and `Update-BFIPresenter.ps1` read both the old and the new version formats, so 0.x installs are still recognised and compared correctly.

## History before 1.0.0

### DX add-on (v0.2–v0.82)

#### DX v0.82

- **Overlay reorganised into sections.** No settings changed.
    - **Top:** version, status line, BFI on/off. Below them, warnings appear only when something needs attention (VRR, frame limiter, VSync override, missed refreshes, hitch guard, game below the cap, uneven frames).
    - **Strobe:** method, frames per cycle, gain, dark floor, roll-off, sine/invert, rolling scan with phosphor decay / seam.
    - **Brightness and output:** output format, display peak, ABL guard.
    - **Frame pacing and latency:** frame cap, pacing mode, just-in-time release (Half/Quarter), Later release (Refresh), margin and status, frames ahead.
    - **Status:** fps, refreshes, frame age, uneven cycles, late refreshes, presenter and queue priority.
    - **Advanced (collapsed):** Wait for vblank, queue ahead, presenter buffers, own queue, hitch guard, low-level counters.
    - **Bottom:** timing log, Save / Reload.
- The "Queue one refresh ahead" tooltip was attached to the hitch guard slider; fixed. Tooltips for the cap and pacing mode now say which combination suits which game.

#### DX v0.81

- ReShade.log now says why the realtime privilege could not be enabled: whether the game process is elevated, and the Windows error. Steam and other launchers restart the game without administrator rights even when its .exe is run as administrator.

#### DX v0.80

- **Global realtime presenter queue when the game runs as administrator.**
    - A global realtime GPU queue needs Windows' "increase scheduling priority" privilege to be enabled, not just held. An administrator's process holds it disabled, so the queue fell back to "high" even when run as administrator.
    - The add-on now enables it before creating the queue, and logs whether that worked.
    - Realtime priority may let the presenter's small per-refresh draw run sooner when the game keeps the GPU busy for a long stretch. These stretches caused ~30 ms stalls and a held refresh, a flash, in God of War Ragnarök with DLSS 5. It cannot help if the game's GPU work can't be interrupted at all.

#### DX v0.79

- **Smooth no longer costs frame rate at the Refresh cap.** Smooth released the game's frame-latency slots only when the presenter took a frame at a lit refresh (60 times a second). A game that can't hold 120 then waited for slots instead of rendering: God of War Ragnarök fell from ~100 fps in Lowest latency to ~73 fps in Smooth, same scene, same GPU load. With any frame cap on, slots are now released when the frame is handed over (as in Lowest latency), and the cap does the pacing. Smooth still differs in which frame each lit refresh shows. Smooth with the cap off keeps releasing at the pick.

#### DX v0.78

- **"Game frame rate does not match the strobe" warning fixed.**
    - It counted every game frame that wasn't lit as "never shown". At the Refresh cap at 120 fps with 2-frame BFI, every other frame is never lit by design, so the warning was on permanently (Hades 2 at a steady 120).
    - Strobe cycles are now counted as uneven only when their step differs from the expected one: 2 frames at 120 fps / Refresh cap, 1 at Half, and at Quarter only skips (its 0/1 alternation is normal).
    - The warning shows only when that persists (3+ per second). It says either that the game is below the cap's frame rate, or that frames reach the strobe unevenly.
    - The status line now reads "Uneven strobe cycles: N/s (a frame repeated / frames skipped)".
- **Automatic frames ahead uses the same count.** At the Quarter cap the normal every-other-cycle repeat used to push it to 2 frames ahead and keep it there (Obduction).

#### DX v0.77

- **Fixed brief flashes when the driver stalls the game's frame copy.**
    - The game's Present held the staging lock while submitting its frame copy to the GPU.
    - When the driver blocked that submission (Obduction: 48-68 ms, likely streaming), the presenter waited on the lock to pick a frame. It missed 6-8 refreshes, and the panel held the last one: a dark or bright flash.
    - The lock now only covers marking the slot as being rewritten and publishing the finished frame. The presenter keeps strobing through such stalls, so the game hitches but the screen no longer flashes.
- Obduction, Half cap with just-in-time release (v0.76): Smooth had 19 repeats and 19 skips in ~7000 lit refreshes (0.3%, down from 1.5%). Frames were 15 ms old when shown, down from 20 ms.

#### DX v0.76

- **Just-in-time release no longer falls back at Half / Quarter.** When a game's frame time plus margin came within 1 ms of the cap interval (Obduction in heavy scenes), v0.75 paused and went back to releasing one refresh before the lit refresh. That gave frames only ~8 ms and brought the repeat-then-skip judder back. Now every frame still targets a lit refresh and is released as early as the cap allows (1 ms after the previous lit refresh). The overlay shows "at its limit". At the Refresh cap, the limit is still the normal release at the vblank.
- **The margin recovers.** It narrows after 5 clean seconds also while at the limit (before, a margin widened in a heavy scene stayed wide and kept it paused). It is capped at 3 ms over the base.

#### DX v0.75

- **Just-in-time release at the Half and Quarter caps (`CapJustInTime=1`, on by default, both pacing modes).**
    - **The problem (Obduction):** since v0.69 the cap released the game one refresh before the lit refresh. Obduction needs ~4.3 ms CPU plus GPU time, so its frames finished right at the pick. Some made it and most were shown a cycle later. That gave repeat-then-skip pairs at Half (14% of lit refreshes with Lowest latency) and frames held 1 then 3 cycles at Quarter.
    - **The fix:** each frame now targets a lit refresh (Half: every lit, Quarter: every other lit). It is released at that refresh's vblank minus its measured build time (95th percentile) and the margin.
    - This uses the same measurement and adaptive margin as Later release: wider after misses, paused (old behaviour) when build time + margin comes within 1 ms of the cap interval.
    - Frames land on the same lit refresh every time. Light games are released later than before, so their frames are fresher; heavy games earlier, so they stop missing.
- The Later release status and margin are shared and now labelled "Just-in-time release". Refresh-cap Later release still needs its own checkbox (off by default).

#### DX v0.74

- **Rolling scan phosphor decay (`RollingPhosphorDecay`, overlay slider under Rolling scan; 0 = off).**
    - Works like the Blur Busters CRT beam simulator. The simulated beam sweeps the screen once per strobe cycle, and each row lights when the beam passes it, then fades with this time constant (in sub-frames).
    - Each refresh shows the strobe profile smeared by that fade. The band edge moves with the beam and fades out instead of switching, so there is no seam line.
    - Every pixel still gets exactly one cycle's light, so brightness and the ABL guard are unchanged.
    - Longer decay looks smoother with less visible flicker, at the cost of a little motion clarity. With decay above 0 the seam setting is hidden and unused.

#### DX v0.73

- **Later release (`LaterRelease=1` or the overlay checkbox; off by default).** Applies with Lowest latency + Refresh cap and Wait for vblank on.
    - Before, the frame cap released the game at the vblank, a full refresh before the pick that shows its frame. A light game finished its frame ~1 ms later, then waited ~7 ms with it ready. Its input was read 7 ms earlier than needed.
    - The release is now held until *pick vblank − build time − margin*.
    - Build time is measured per frame, from release to the copy finishing on the GPU (95th percentile of the last 128 frames).
    - The margin starts at `LaterReleaseMarginMs` (1.5 ms). It widens by 1 ms in any second where 2+ frames miss their refresh, and narrows by 0.5 ms after 5 clean seconds.
    - When build time + margin comes within 1 ms of a refresh, it pauses (release at the vblank as before). It resumes after 3 seconds with room again.
    - Overlay and log (`lr_state`, `lr_lead_ms`, `lr_est_ms`, `lr_margin_ms`) show the state, build time and margin.
- **Late pick removed.** It gave no measurable gain in a fast non-FG game (release→display 16.7 ms on and off) and slightly more uneven picks. `LatePick` / `LatePickMarginMs` in old ini files are ignored.

#### DX v0.72

- **The add-on shows its version**, at the top of the overlay tab and in ReShade.log ("BFI Presenter: v0.72 loaded from ..."). This makes it easy to confirm which build a game is actually loading.

#### DX v0.71

- **Late pick (experimental, `LatePick=1` or the overlay checkbox).**
  - **What it changes.** Normally the presenter picks a game frame, draws the strobe pass and presents right after a vblank. That refresh is shown at the *next* vblank, so the frame is picked about 8 ms before it appears. With late pick, the presenter sleeps until about 2 ms before the vblank and does all of that then. The refresh still makes the same vblank, and the frame it shows is up to about 6 ms fresher.
  - **Where it helps.** The gain is largest with Lowest latency at 120 fps in games with spare GPU headroom. In games that need the whole refresh to render a frame, it picks the same frame as before (no gain, no loss).
  - **It checks itself.**
    - For the first second it records how many presents are normally still waiting at each vblank. Once active, if that number rises (presents missing the vblank or the compositor's deadline) or refreshes are missed outside hitches, it widens its margin by 1 ms.
    - Beyond 6 ms it switches itself off and picks at the vblank as before.
    - It narrows back towards `LatePickMarginMs` after 5 clean seconds.
    - The overlay shows its state and the margin in use. The timing log has four new columns: `late_state`, `late_sleep_ms`, `late_margin_ms` and `pending_at_vblank`.
  - **Interplay with v0.66.** The short wait for a frame that's about to finish is limited to 1 ms while late pick is active.

#### DX v0.70

- **Smooth pacing no longer sits a cycle behind for good.** When two finished frames kept waiting cycle after cycle, Smooth was meant to catch up by dropping one. Instead it kept picking the older of the two again. With the Half cap that left frames 21 ms old when shown, against 5 ms with Lowest latency (The Midnight Walk, v0.69). After three such cycles it now shows the newer frame.
- v0.69 confirmed: with Half cap + Lowest latency, frames are now 5 ms old when shown (13 ms before), and with the Refresh cap only 2–5% of cycles are uneven.

#### DX v0.69

- **The Half/Quarter cap now releases the game one refresh before the lit refresh.** Before, the release could fall on the vblank of the lit refresh itself. The frame then missed that pick and was shown a whole cycle later: in The Midnight Walk, 13 ms old with Lowest latency and 30 ms with Smooth, which also kept the late frame in reserve. The frame now has a full refresh (8.3 ms) to finish before it is shown, so it should be about 4–6 ms old in both modes.
  - This only applies when the game runs one frame per strobe cycle (Half cap at 2 frames per cycle, Quarter at 4) and arrives on time. A game that's running late is never held back for it.

#### DX v0.68

- **Half cap + Smooth is about one strobe cycle (~15 ms) more responsive.**
  - In games that use a frame-latency waitable, Smooth mode gave the game its next slot when the strobe took a frame. Each new frame therefore started just after a lit refresh, missed it, and waited a whole extra cycle. In The Midnight Walk, frames were 21 ms old when shown with Half + Smooth, against 6 ms with Half + Lowest latency.
  - With a Half or Quarter cap the cap already paces the game, so the slot now comes back as soon as the frame is handed over, as in Lowest latency.
- **Fewer uneven steps in every mode.** v0.66's short wait for a frame that's about to finish now also covers the Half cap, where about 6% of cycles repeated a frame and then skipped one. It also applies in Smooth mode.
- **Correction to v0.67:** the game log's `time_ms` is when the game *calls* Present, so its 15–18 ms spread was the game's own work varying, not uneven releases. The releases were already even to about 0.1 ms. v0.67's change (releasing at the vblank) is still slightly tighter, but it wasn't the cause of the judder.

#### DX v0.67

- **Evenly timed frames with the frame cap.** The cap used to release the game each time the presenter's own Present returned. How long that took varied (picking, recording and submitting a refresh: 0.2–3 ms), so each release moved a little.
  - The game timed its animation to those uneven gaps. In The Midnight Walk's log, frames came 15–18 ms apart at a steady 60 fps, and 6–10 ms apart at 120. Objects then moved in uneven steps, which BFI makes plainly visible as judder in every pacing mode.
  - The game is now released at the vblank itself, so its frames start exactly one or two refreshes apart.

#### DX v0.66

- **Lowest latency pacing judders far less when the game runs faster than the strobe** (for example 120 fps on a 60 Hz strobe).
  - At each lit refresh the presenter takes the newest finished frame, normally two ahead of the last one.
  - When a frame finished a moment after that, it took one ahead and then three, giving uneven 8 and 25 ms steps that judder under BFI. ABZU and The Midnight Walk showed this in up to 40% of cycles.
  - If the expected frame has already been submitted, the presenter now waits up to 3 ms for it. The refresh is still drawn well before its vblank, so no refreshes are missed. The worst case adds 3 ms of latency on the cycles where the game is late.
  - Smooth pacing and the Half cap are unchanged. Half cap + Smooth remains the steadiest choice.

#### DX v0.65

- **Fixed: v0.64 didn't load in Unreal Engine games (Grip).**
  - ReShade unloads and reloads add-ons with every Direct3D device a game creates and destroys, and Unreal makes several short-lived ones at startup.
  - v0.64 unregistered itself on the first unload but, being pinned in memory, never registered again. ReShade then reported "No add-on was registered" and dropped it.
  - The add-on now registers again through ReShade's `AddonInit` call each time it is reloaded.

#### DX v0.64

- **Lowest latency pacing lets the game run above 60 fps again** in games that use a frame-latency waitable (Unity games such as Prince of Persia: The Lost Crown, and many DX11/DX12 games).
  - The game is allowed a fixed number of frames in flight, and each frame's slot used to come back only when the presenter took a new frame, 60 times a second.
  - With a frame still on the GPU at each of those moments, only one slot came back per strobe cycle, so the game was held at 60 fps whatever the setting.
  - In Lowest latency mode the slot now comes back as soon as the frame has been handed over, so the game runs at its own rate, up to the frame cap. Smooth mode still paces the game to the strobe (60 fps at 120 Hz), as intended.
- **No more "Add-on BFI Presenter was not unregistered!" warning** when the game closes. The add-on now unregisters through ReShade's `AddonUninit` call. (This broke loading in Unreal Engine games; fixed in v0.65.)

#### DX v0.63

- **The ABL guard no longer relaxes with rolling scan on.** Tested on the A95L: with rolling scan on and the guard off, the TV's own ABL still dimmed the brightest areas. Its limiter doesn't go by each refresh's average, so lighting only half the screen per refresh doesn't spare it. The guard now measures the same way with rolling scan on or off.
- **Rolling scan is meant for 240 Hz and faster displays.** At 120 Hz it can only alternate two halves with a fixed seam. That reduces flicker in peripheral vision, but the seam band has no BFI. At 240 Hz (Frames 4, Quarter cap) it rolls through four bands; Sine BFI on top gives each band a soft phosphor-like fade. Keep `RollingSeam` small there (0.02–0.04).
- **Missed refreshes are judged at the display's own refresh rate** instead of an assumed 120 Hz, so the hitch guard and the late-refresh count work on 144/240 Hz displays.

#### DX v0.62

- **Rolling scan (`RollingScan=1`, or Ctrl+Shift+S to toggle).** The screen is split into one horizontal band per sub-frame (two at 120 Hz with a 60 fps strobe). The bands are lit in turn from the top down, like a CRT beam.
  - **Less visible flicker.** Every pixel still flashes at 60 Hz, but some part of the screen is always lit, so the whole-screen pulsing that peripheral vision picks up is gone.
  - **Works with both methods.** With Variable MPRT, the brightness budget applies per pixel as before; rolling only changes *when* each band gets its light, not how much.
  - **The seam.** `RollingSeam` (default 0.08 of the screen height) blends the two bands so there's no hard line. Pixels in the seam share their light between both refreshes, so they get less clarity. On a fast horizontal pan, the lower band is shown 8.3 ms after the upper one, which can show as a small step at the seam. A real CRT has the same delay, but spread evenly down the screen.
  - With more frames per cycle (Quarter cap, 4 frames), there are four bands and the scan rolls more smoothly.

#### DX v0.61

- **Fixed a game freeze after a resize in games that use a frame-latency waitable (No Rest for the Wicked).**
  - These games wait for a "go" signal before each frame, and the presenter gives that signal each time it takes a game frame.
  - When the game resized its swap chain, the presenter stopped and the frames it held were dropped, so their signals never came. The game then waited forever, and the presenter only restarts on the game's next frame.
  - Stopping the presenter now hands the game a full set of signals.
- **Two DX11 paths no longer swallow a signal:** a frame whose size doesn't match yet, and frames shown directly while the presenter is unavailable.
- **The watchdog now covers DX11 games.** If the game stays blocked on that signal for 2 seconds, it is released and the log says so, instead of the game freezing silently.

#### DX v0.60

- **GPU-heavy games no longer lose finished frames.** The ring of finished frames waiting for the strobe grew from 4 to 6.
  - In God of War with RenoDX DLSS 5 neural rendering, the GPU ran about three frames behind the game. With one slot on screen and three still being copied, the finished frame waiting for its strobe cycle was overwritten before it was shown.
  - The game rendered 37 fps but only about 11 reached the screen, which looked like a slideshow.
  - Every finished frame is now shown, at the cost of about 66 MB more video memory at 4K.
- **New log warning** when finished frames are replaced before they are shown, so this can be spotted directly.
- **Fixed a false VRR warning from v0.59** ("120.0 Hz instead of 124.3 Hz"). A second that counted a few extra refreshes after a stall became the reference. The display mode's refresh rate is now always the reference.

#### DX v0.59

- **New warning: a frame limiter is holding the presenter back.** If something hooked into the presenter's own swap chain caps it a little below the display's refresh rate, the log and overlay now say so within about 3 seconds. For example, RTSS at 118 fps in God of War with OptiScaler left the display at 120 Hz but let only 118 presents a second through. The strobe then slipped a refresh twice a second, which showed as a steady flicker. Set the RTSS frame limit to 0 for the game.
- **The VRR warning also compares against the display mode's refresh rate (120 Hz),** not only the fastest rate seen so far. G-SYNC holding the display below 120 Hz from the very start is now caught too.
- **No more bogus "presenter stuck for 771150 s" watchdog warning** when the presenter starts.

#### DX v0.58

- **DX11 mode writes the per-frame game log** (`BFIPresenter_game_*.csv`, as in DX12 games) when the timing log is on. It shows where the game's frame time goes: its own work, waiting for the GPU (frames ahead), or the frame cap.

#### DX v0.57

- **DX11 mode: the stack-overflow crash in God of War (2018) is fixed.** The presenter's own swap chain shares DXGI's swap-chain class with the game's, so its calls ran through the add-on's own hooks and then Steam's overlay hooks, which called back in. Setting the colour space recursed until the stack overflowed.
  - Every call the presenter makes on its own swap chain now goes straight to DXGI's code.
  - The DX11-only hooks (fullscreen state, colour space) go straight to DXGI when they are re-entered.
- **The presenter's DXGI and D3D12 entry points come from the system folder.** ReShade is often installed as `dxgi.dll` in the game folder, and v0.56 picked that one up instead of Windows' own.

#### DX v0.56

- **DX11 mode: the presenter's own D3D12 device, factory and swap chain are created past every hook.** In God of War (2018), ReShade, the RenoDX and DLSS add-ons, and NVIDIA's present layer had all latched onto the presenter's DX12 objects:
  - ReShade set up a second effect runtime on the presenter's swap chain.
  - The DX12 half of the add-on started managing that swap chain as if it were a game's.
  - The two halves then released the same swap chain, which crashed the game in NVIDIA's present layer (`NvPresent64.dll`).
- **Clean fallback.** A DX11 setup that fails now removes its hooks again, so the game runs normally.
- **The shared staging textures are created with a render-target flag.** DX11 refused the v0.55 ones. The log now names the step that fails.

#### DX v0.55

- **Direct3D 11 games.** The add-on now manages DX11 games as well, with the same strobe, pacing, guards, logs and settings window as in DX12 games. The game keeps its real DX11 device, so its own DLSS works, and so do the DLSS 5 DX11 bridge, ReShade and the CRT shader.
  - **How:** the game's own swap chain no longer presents. Each finished frame, ReShade's effects included, is copied into the add-on's staging ring, through textures and a fence shared with a D3D12 device the add-on creates.
  - The DX12 presenter shows these frames through its own swap chain, on a small window of its own laid over the game's window. That window passes mouse input through to the game and never takes focus.
- **Use borderless or windowed fullscreen in DX11 games.** If the game asks for exclusive fullscreen, the add-on keeps it windowed (the game is told it got fullscreen) and writes a note to `ReShade.log`.
- **HDR:** the presenter's swap chain follows the colour space the game sets (HDR10 or scRGB).
- **Install ReShade for DirectX 10/11 in DX11 games**, not for Vulkan. Remove DXVK's `d3d11.dll` / `dxgi.dll` from the game folder.
- `DX11Support=0` turns DX11 management off.
- **Not run yet:** this mode has not been tested in a game. God of War (2018) is the first test.

#### DX v0.54

- **Hitch guard no longer causes brightness flashes.** While paused, every refresh is now shown at the strobe's *average* brightness rather than as the full lit image. Pausing and resuming changes motion clarity only, not brightness. In Control (v0.53 log), each pause showed the plain image for at least a second, about 40 % brighter than the strobe at gain 0.70, and that was the flicker; 9 % of the session was spent paused.
- **Ordinary gameplay hitches keep strobing.** Pausing on "no new frame" now needs `HitchGuardLoadingMs` (new key, 1000 ms) instead of `HitchGuardStallMs` (150 ms; no longer read). Control's 190 ms hitches had triggered a pause each time. During a hitch the presenter keeps strobing the frozen frame cleanly, so there's nothing to hide. Missed refreshes (3 within half a second) still pause it, as during Oblivion's loading.

#### DX v0.53

- **Budget default is now 250 nits.** Testing on the A95L: God of War Ragnarök's brightest scene was clean at 300, South of Midnight's sky still flashed at 300 and was clean at 250. Your ini keeps whatever you set.
- **Slower, steadier release.** The ceiling still drops within a frame or two when a bright scene arrives, but now rises again only after the scene has stayed under budget for 1.5 s, and then gradually (about 2.5 s). In v0.52 the ceiling bounced between ~250 and ~1,000 nits several times a second during bright pans, and every bounce could trip the panel's own limiter.
- **The log and window show what the scene asks for.** New last timing-log column `raw_avg_nits`: the lit-refresh average with no ceiling at all. The window shows it next to the average after the ceiling and the ceiling in use.

#### DX v0.52

- **ABL guard measures the panel's workload.** One setting, `AblGuardBudgetNits` (450), replaces trigger level, area and cap: for every new frame the guard finds the highest lit-refresh ceiling at which the lit refresh — every 32×18 cell capped at that ceiling — averages no more than the budget. Area and brightness both count, so one value covers what needed different caps before: a near-white screen (God of War Ragnarök, ~90 % white) gets a ceiling around 500 nits, a sky over a third of the screen around 1,000, and small highlights keep the full 1,400. The window shows the lit-refresh average and the ceiling in use; the timing log's last two columns are `ceiling_nits` and `lit_avg_nits`. The v0.51 keys are no longer used.

#### DX v0.51

- **ABL guard: trigger level and cap are separate.** `AblGuardTriggerNits` (400) decides which parts of the screen count as bright — measured as 32×18 cell averages, so a bright sky's cells sit well below its brightest pixels — and `AblGuardCapNits` (700) is the lit-refresh ceiling while the guard is engaged. In South of Midnight one value had to serve both: at 700 the guard never engaged, at 400 it engaged but also capped at 400. Now it engages as reliably as at 400 and caps at 700.

#### DX v0.50

- **ABL guard reworked: it caps bright areas instead of lowering the overall boost.** The v0.49 log showed the average-brightness version working as designed (lit-refresh average up to ~430 nits, budget lowered to 0.63) yet the sky still darkened — while capping `DisplayPeakNits` at 700 had fixed it. The trigger is a large area of very bright lit refreshes, not the average. The guard now measures how much of the screen (32×18 cells) the lit refresh would push above `AblGuardCapNits` (700); while that exceeds `AblGuardArea` (5 %), the lit-refresh ceiling drops to the cap (energy above it spills into the dark refresh), and the full ceiling returns over about a second when the area passes. Scenes with small highlights keep the full 1400 nits. The window shows the share of the screen above the cap and the ceiling in use; the timing log has `ceiling_nits` and `hot_area_pct`. New ini keys `AblGuardCapNits` and `AblGuardArea` (the old `AblGuardNits` is no longer used).

#### DX v0.49

- ABL guard diagnostics: the timing log gains `gain_in_use` (the brightness budget actually applied) and `lit_apl_nits` (the measured lit-refresh average); `ReShade.log` notes the first measurement, or why the guard is unavailable.

#### DX v0.48

- **ABL guard** (HDR, on by default). OLED panels dim the whole picture when a refresh is too bright overall (automatic brightness limiting). With BFI the lit refresh carries the boost, so a large bright sky could push it over the panel's limit — in South of Midnight a darkening "layer" over the sky, and flicker as it engaged and released, only with a brightness budget above 0.5 (capping `DisplayPeakNits` at 700 avoided it, at the cost of every highlight). The presenter now measures each new frame's lit-refresh average brightness on the GPU (a 32×18 grid, read back without waiting) and, while it would exceed `AblGuardNits` (250), lowers the brightness budget for that scene — down quickly, back up over about a second, never below 0.5. Dark and mixed scenes keep the full boost and full highlights. The window shows the live lit-refresh average and the budget in use, for tuning the limit.

#### DX v0.47

- **Fixed: highlights clipping with RenoDX (and other HDR-upgrade mods) on Output Auto.** A 16-bit float swap chain is now always treated as HDR scRGB. With an SDR game upgraded to HDR by RenoDX, the reported colour space can stay sRGB, Auto took the image for SDR, and the strobe's SDR encode clipped everything above SDR white (South of Midnight: sky highlights clipping whenever the brightness budget was above 0.5). `ReShade.log` now notes the output type in use whenever it changes.

#### DX v0.46

- **Automatic frames ahead no longer switches back and forth.** If, after returning to 1, the game needs 2 again within a minute, it stays at 2 for the rest of the session (until the game resizes or recreates its swap chain). A game near the limit therefore settles after at most one retry instead of juddering every few seconds.

#### DX v0.45

- **Automatic frames ahead** (`AutoFramesAhead=1`, default): Max frames ahead 1 gives the least latency but stops the game's CPU and GPU work from overlapping, which can cost 10–20% frame rate (South of Midnight: 55 fps and 4.8 repeated cycles/s at 1, 59.8 fps and 0.1/s at 2). The presenter now uses 1 normally, switches to 2 when the game misses 2 or more strobe cycles per second for 3 seconds, and returns to 1 once the game waits more than 6 ms per frame in the frame cap for 10 seconds. The window shows the value in use, `ReShade.log` notes each switch, and the timing log's `ahead` column records it. `MaxFramesAhead=0` (no limit) or `AutoFramesAhead=0` turn it off.

#### DX v0.44

- **VRR detection.** The window now shows the display's own refresh rate (from its frame statistics). If it stays below its peak for 3 seconds without missed refreshes, VRR (G-SYNC) is active: the panel then refreshes whenever the presenter's frame arrives, slightly late every time, so the strobe runs slow and uneven (South of Midnight: 110–112 Hz in long stretches, 120 Hz between). A red warning appears and `ReShade.log` notes it once. Fix: G-SYNC off for that game in the NVIDIA profile, or Queue one refresh ahead on (a present is always waiting, so the panel runs at its maximum).

#### DX v0.43

- **Hitch guard** (on by default): during loading the GPU is saturated, the presenter misses refreshes, and every missed refresh holds a lit or dark frame on screen twice as long — a flash (Oblivion Remastered: up to 150 missed refreshes in 5 s while loading). When the presenter misses 3 refreshes within half a second, or the game delivers no new frame for `HitchGuardLoadingMs` (1000 ms since v0.54; 150 ms `HitchGuardStallMs` before), BFI pauses: every refresh is shown at the strobe's average brightness, so a missed refresh only repeats an image (v0.54; earlier versions showed every refresh lit, which changed brightness). BFI resumes after a second with neither. `ReShade.log` notes each pause and resume, the window shows it, and the timing log has a `hitch_paused` column. Off with `HitchGuard=0`.

#### DX v0.42

- **Fixed: The Last of Us Part I crashing on its first frame (device removed, invalid call).** The debug layer showed the cause: the stand-in back buffers are typeless (`R8G8B8A8_TYPELESS`, so both sRGB and UNORM views work, as on real swap-chain buffers), and the game creates its own screen-size render targets from the back buffer's description, with a clear value in that format. A typeless clear value is invalid, every one of those textures failed to create, and clearing one removed the device. The stand-ins now report the swap chain's typed format when asked for their description (`TypedBackBufferDesc=1`, default); their views still behave as before.

#### DX v0.41

- **`Diagnostics=2`: D3D12 debug layer.** For crashes reported as "device removed, reason 0x887A0001" (an invalid D3D12 call, as in The Last of Us Part I on its first frame with BFI): the debug layer is enabled and every D3D12 error or warning is written to `BFIPresenter_debug.txt` (errors also to `ReShade.log`), naming the exact call and resource. Needs the Windows optional feature "Graphics Tools"; slows the game down, so use it only for diagnosis.
- Command lists BFI Presenter records are now checked; a frame copy that fails to record logs both buffers' size, format and flags.

#### DX v0.40

- **Presenter queue priority is configurable** (`PresenterQueuePriority`: 0 normal, 1 high, 2 global realtime when allowed — the default, as before). Resident Evil 4 hung the GPU only with BFI Presenter, stuck at the start of a compute workload; a high-priority queue makes the GPU interrupt the game's work for every refresh, and some workloads may not survive that. Lower priority is a test for that, at the cost of more late refreshes when the GPU is busy. Read at game start; the window shows the priority in use.

#### DX v0.39

- **Fixed: Resident Evil 4 crashing on its first frame (stack overflow inside Steam's Present hook).** Before BFI starts, the game's own frames are passed on to DXGI; that call still went through the overlay hooks, and Steam's Present hook recursed endlessly, as it did on the presenter's own presents in Pragmata. Those frames now use the same direct path to DXGI as the presenter (`BypassOverlayResizeHooks=1`).

#### DX v0.38

- **Fixed: BFI never starting in Resonance (and any game with RTSS or the NVIDIA overlay hooking ResizeBuffers1).** Since v0.34 the move onto the presenter's queue calls DXGI directly, past overlay hooks. Hooks that hold references to the old buffers release them only inside their own ResizeBuffers1 hook, so DXGI refused the direct call (`0x887A0001`), and the game stayed on its own presents. When DXGI refuses the direct call, it is now retried through the hooks.
- If the move fails anyway, it is attempted again at the game's next resize, instead of only once per swap chain — changing resolution now retries it.

#### DX v0.37

- **New Game frame cap: Quarter refresh** (30 fps at 120 Hz) for games that can't hold 60. Every game frame is shown lit exactly twice, evenly, instead of an uneven mix of once and twice (e.g. Alan Wake 2 at about 34 fps).
- **All frame caps are now exact:** the game is released every 1, 2 or 4 of the presenter's refreshes, counted, instead of waiting for one or two refresh signals in a row — which let a game slightly slower than the cap fall to an uneven rate. A game slower than the cap is released as soon as its frame arrives.

#### DX v0.36

- **Your BFIPresenter.ini is kept across updates.** The package no longer contains a `BFIPresenter.ini` — only `BFIPresenter.example.ini`, the commented reference. To update, replace `BFIPresenter.addon64` and leave your ini in place. At game start, settings a new version adds are appended to your ini with their defaults (`ReShade.log` lists which); nothing already in the file is changed. With no ini at all, one is created with every setting at its default.

#### DX v0.35

- **Fixed: frame generation throttled to 15–30 fps (Pragmata: 76 fps without BFI Presenter).** The frame-latency waitable a game waits on was released once per game frame *shown* — at most 60 per second with the strobe, while frame generation presents twice per game frame. The game was held back to a fraction of its rate (frames arriving in pairs every 67 ms). It is now released once for every game frame the presenter is done with, shown or skipped, as DXGI does when presents retire.
- Frame statistics read by the game report the newest frame shown as its present count (earlier ones count as retired), consistent with the waitable.

#### DX v0.34

- **Overlay hooks bypassed for BFI Presenter's own calls.** In Pragmata, v0.33 got past Steam's ResizeBuffers1 hook and BFI started — then Steam's *Present* hook recursed on the presenter's first presents until the stack overflowed. BFI Presenter now builds a small entry point for DXGI's own Present and ResizeBuffers1 that skips any inline hook (the original first instructions, read from `dxgi.dll` on disk, relocated next to the function, then a jump into the rest of it), and uses it for the presenter's presents and its move onto the presenter queue. The hooks themselves are left untouched, so the game's own calls still go through them. `ReShade.log` notes, per function, whether a hook was found and skipped. If the start of a function can't be relocated safely, calls go through the hooks as before. The previous byte-swapping approach (v0.33) is no longer used.
- Overlays (RTSS OSD, Steam) no longer draw on the strobed frames while this is on (`BypassOverlayResizeHooks=1`, default).

#### DX v0.33

- **Pragmata, second attempt:** Steam's overlay hooks DXGI's ResizeBuffers1 with an *inline* hook (a jump written over the function's first bytes), so v0.32's vtable bypass still ended up in Steam's code. For the move onto the presenter's queue, the function's original first bytes (read from `dxgi.dll` on disk) are now put back for the duration of that one call, and the hook is restored byte for byte immediately after. `ReShade.log` notes when this happens and how many bytes were patched. Off with `BypassOverlayResizeHooks=0`.

#### DX v0.32

- **Pragmata grey-screen hang:** moving the swap chain onto the presenter's queue (ResizeBuffers1 with the presenter queue) went through Steam's overlay hook, which never returned. The move now calls DXGI's own ResizeBuffers1 directly — its address is read from `dxgi.dll` on disk, so vtable hooks placed by overlays are bypassed for this one call (`BypassOverlayResizeHooks=1`). If an overlay still holds references to the old buffers and DXGI refuses, the game's own resize runs as before and BFI stays off for that swap chain. `ReShade.log` notes when the bypass is used.

#### DX v0.31

- **Fixed: stack overflow on resize in Pragmata (Steam overlay).** When BFI Presenter moves the swap chain onto its queue (ResizeBuffers1), the call passes through hooks below it; Steam's overlay hook called ResizeBuffers1 again through the swap chain's vtable — back into BFI Presenter's hook, which repeated the move, endlessly. Calls BFI Presenter makes into the swap chain itself (its presents, its resize, the game's resize it forwards) are now marked, and any re-entrant call on the same thread goes straight to the original. Overlays called from inside those also see the real buffers and real values.
- A vtable slot that already holds BFI Presenter's hook is never saved as its own "original" (an overlay unhooking by restoring the pointer it had saved could otherwise cause the same loop).

#### DX v0.30

- **Watchdog shows the stack of the thread that is actually stuck.** A game can resize its swap chain from a different thread than the one that presents (Pragmata: first resize on the present thread, a second one at 4K from another thread, which then hung). The watchdog now logs the call stack of the thread in the stuck step, then the present thread's. Stacks are logged with up to 32 frames instead of being cut short.

#### DX v0.29

- **Crash logger no longer fills up with harmless exceptions.** Mods such as REFramework (which loads as `dinput8.dll`) scan memory and handle their own access violations; in Pragmata a dozen of those at start-up used up the logger's budget before the real crash. Each faulting location is now logged only once, with room for 200 entries, so the crash itself is always recorded.
- The crash record and watchdog now also show which step of the game's swap-chain resize was running (stopping the presenter, releasing buffers, moving to the presenter's queue, the game's own resize, creating the stand-in buffers).

#### DX v0.28

- **Fixed: Pragmata (RE Engine) hanging at start-up.** The presenter started on the game's own queue before the game's first resize; when the game then called ResizeBuffers, BFI Presenter's resize hook waited for the presenter to stop, and the presenter was stuck presenting on the game's queue — a deadlock (the same family as God of War Ragnarok's). Now, for every swap chain, the game presents its own frames until it first resizes the swap chain, and BFI starts inside that resize on the presenter's own queue (`StartBFIAfterGameResize=1`, default). Nearly every game resizes at start-up; if BFI doesn't start, change resolution or display mode once.

#### DX v0.27

- **Fewer flashes when the GPU is busy with the game.** In Returnal the presenter sometimes waited 60–105 ms for the GPU to finish its own previous draw — the GPU was busy with the game's work (hitches, shader compilation) and didn't run the presenter's small draw. Those are exactly the moments the game delivers no new frames, so the presenter was redrawing an image the buffer already held. Now, when a buffer already holds the same game frame drawn with the same settings, it is presented again without drawing (`ReuseDrawnBuffers=1`), which needs no GPU work. This works best when the buffer count is a multiple of the BFI cycle: **PresenterBuffers=4** with Queue ahead (or 2 without). The window shows how many refreshes reused a buffer.
- The presenter no longer holds its frame lock while waiting for the GPU, so a GPU wait can't hold up the game's next frame either.
- In the timing log, `wait_gpu_ms` now also includes picking the frame.

#### DX v0.26

- The timing log records, for every refresh, how long each presenter step took: waiting for vblank, waiting for the GPU to finish with the buffer, picking and submitting the frame, and Present. A late refresh can then be traced to the step that held it up (a GPU still busy with the game's work, CPU scheduling, or Present itself).

#### DX v0.25

- **Frame statistics in terms of the game's own frames** (`GameFrameStatistics=1`, default): `GetFrameStatistics` and `GetLastPresentCount` called by the game now report its own frames — how many it has presented and how many have reached the screen — instead of the presenter's 120 presents per second. Games that pace themselves with these values (menus, frame limiters) could otherwise wait for presents that will never be "theirs". `ReShade.log` notes the first time the game reads them, so it's visible whether a game uses them at all. The presenter itself still reads the real values.

#### DX v0.24

- **Hooks are taken off when the game destroys its swap chain and put back on the next one.** Overlays that hook Present as well (Steam's `gameoverlayrenderer64.dll`, RTSS) clean up and re-hook when the game recreates its swap chain; with BFI Presenter's hook left on top, the Steam hook was left half-reset and called a null pointer on the new swap chain's first frame (God of War Ragnarok, on loading — v0.23's queue change alone did not fix it). A slot is only restored if it still holds BFI Presenter's hook. `ReShade.log` now notes where Present was going when hooking and after unhooking.

#### DX v0.23

- **Fixed: crash on loading in God of War Ragnarok (inside the Steam overlay's Present hook).** Overlay hooks such as Steam's `gameoverlayrenderer64.dll` remember the queue a swap chain presents on. When the game destroyed its swap chain, the presenter released its queue; on the recreated swap chain's first frame the Steam hook called into that freed queue (a call to address 0). The presenter queue is now kept alive for the life of the game and reused by later swap chains on the same device. The Steam DLL is injected even with the overlay and Steam Input disabled.

#### DX v0.22

- Crash logger: when code jumps to a null address, the caller is now recovered from the stack, so the record shows who made the call.
- The path where the game presents its own frames (no BFI yet) now records its steps, so a crash or hang there shows exactly which step it was in.

#### DX v0.21

- **Crash logger:** serious exceptions anywhere in the game process (access violations, illegal instructions, stack overflow, heap corruption) are written to `BFIPresenter_crash.txt` next to the add-on, flushed immediately: the faulting module + offset, the address involved, the call stack, which thread (BFI presenter or not) and what the presenter and the game were doing at that moment. The file is recreated at each game start. The last entry before the game dies is the crash; earlier ones may have been handled by the game.

#### DX v0.20

- **Fixed: God of War Ragnarok hanging on load.** The game destroys its swap chain when loading and creates a new one (4K, 6 buffers). Until a swap chain is resized, the presenter has to present on the game's own queue, and there the presenter's Present blocked for seconds while the game waited for frames — a deadlock. Moving to the presenter's queue from inside Present had hung the same swap chain before. Now, for a recreated swap chain, the game presents its own frames (no BFI) until it resizes the swap chain; the move to the presenter's queue happens inside that resize and BFI starts. If the game doesn't resize on its own, change resolution or display mode once (and back). The window shows a hint while BFI is waiting. `WaitForResizeOnRecreatedSwapChain=0` restores the old behaviour.

#### DX v0.19

- **Queue one refresh ahead** (new, off by default): with Wait for vblank on and 3 presenter buffers, the presenter keeps exactly one extra present queued. An occasional late slot is covered by the frame already waiting, so it doesn't flash, at +1 refresh (8 ms) of latency. Unlike turning Wait for vblank off, the queue never grows — in Resonance, a growing queue under forced VSync made the driver throttle the game to 14–28 fps.
- **VSync override guard:** if the presenter finds itself presenting far faster than the display refreshes (the driver is ignoring its VSync — NVIDIA profile Vertical Sync Force off or Fast Sync), it paces itself to vblank from then on and shows a warning. Set Vertical Sync to Force on (or Use the 3D application setting where that works).
- The window shows how many presents are queued; the timing log adds `queue_ahead` and `queued` columns.

#### DX v0.18

- **Game-side timing log:** while the timing log is on, a second file `BFIPresenter_game_<date>_<time>.csv` records one line per game frame: how long the game spent outside its Present call (its own work and waits), and inside it how long each step took — waiting for other presenting threads, a free staging slot, the presenter's lock, the copy, Max frames ahead, and the frame cap — plus whether no free slot was found. This shows whether a frame-rate drop happens inside BFI Presenter or in the game.

#### DX v0.17

- **Fixed: frame rate collapsing with 3 presenter buffers.** The game copied each frame into its staging slots strictly in turn, and its GPU queue had to wait whenever the presenter had a read of that slot still queued. With presenter buffering (3 buffers, Wait for vblank off) those reads complete a few refreshes late, so the game's GPU kept stalling — in Resonance, from about 50 fps to 15–28 fps. The game now takes the oldest slot the presenter is neither showing nor still reading, and never waits on the presenter in normal operation.
- The window shows game frames that still had to wait for the presenter (should be 0).

#### DX v0.16

- **Fewer occasional flashes:** new **Presenter buffers** (default 3). When presentation moves to the presenter's queue, the real swap chain gets at least this many buffers while the game still sees its own count. With **Wait for vblank** off, the presenter then queues frames ahead, so a slot that runs late (busy GPU, a CPU hiccup, an overlay inside Present) no longer holds a lit or dark frame on screen twice — at the cost of 1–2 refreshes of extra latency. 2 buffers with Wait for vblank on remain the lowest-latency option.
- The presenter queue is created at global realtime priority when Windows allows it (usually only when the game runs as administrator), otherwise high; the window and log show which.

#### DX v0.15

- **Timing log reworked for comparing settings:** it can be switched on and off at any time in the overlay; each time it starts it writes a new `BFIPresenter_log_<date>_<time>.csv`, and every line records the settings in force (BFI on/off, method, frames, gain, floor, roll-off, frame pacing, frame cap, max frames ahead, vblank pacing). Old logs are never overwritten.

#### DX v0.14

- **Less input lag.** New **Max frames ahead** (default 1): the game's Present waits until its frame from that many frames ago has finished on the GPU, so it can't queue several frames ahead of the screen — each queued frame is about 17 ms of lag at 60 fps. Works like a driver low-latency mode, for any game.
- **Smooth pacing catches up:** if a second finished frame keeps waiting for three strobe cycles, it is standing latency rather than jitter, and Smooth skips one frame to remove it.

#### DX v0.13

- **Smoother motion.** A new game frame is taken only at the start of each strobe cycle (the lit refresh), and the rest of the cycle shows the same frame — one image per strobe, like a CRT refresh. New **Frame pacing** setting: **Smooth** (default) shows every finished game frame exactly once, in order, holding at most one in reserve, so GPU timing jitter no longer causes repeat-then-skip stutter; **Lowest latency** shows the newest finished frame (previous behaviour). In simulation at 60 fps with 3–5 ms of GPU jitter, Smooth cut repeated and skipped frames by 4–10×.
- The window shows strobe cycles without a new game frame and game frames never shown, per second, with a hint when the game's frame rate doesn't match the strobe.

#### DX v0.12

- **Fixed: hang on a recreated swap chain** (God of War Ragnarok). The move to the presenter's own queue is now only done inside the game's own ResizeBuffers call, never from inside Present. Until the game resizes, the presenter draws and presents on the game's queue.
- **Watchdog, more detail:** when the game's present thread stalls, the log records its call stack (module + offset, which shows whose code it is waiting in) and whether the game's frame-latency waitable is signalled. When the presenter runs but stops presenting, the log says whether frames are staged and which GPU fence it is waiting for. Requests for the frame-latency waitable are logged.

#### DX v0.11

- **Fixed: games freezing at a fixed refresh (G-SYNC off)** when they pace themselves with the swap chain's frame-latency waitable (Control). DXGI's waitable was being driven by the presenter's own presents, which at a fixed refresh keep the flip queue full, so it almost never let the game through (six frames a minute). The game now gets its own waitable, released each time the presenter takes a new game frame; `SetMaximumFrameLatency` / `GetMaximumFrameLatency` act on it.

#### DX v0.10

- **Hang watchdog:** the presenter and the game-side Present path record which step they are in. If either is stuck for more than a second or two, `ReShade.log` gets a `WATCHDOG` line naming the step (for example "presenter stuck for 3.0 s in: Present (DXGI)" or "game Present thread: 5.0 s since its last step, now outside BFI Presenter"), repeated every 5 s while it lasts.

#### DX v0.9

- **RTSS no longer blocks the add-on by default** (`AllowRTSS=1`): the window shows a red warning instead. Keep RTSS's frame limiter off for the game and don't toggle its OSD while playing. `AllowRTSS=0` restores the refusal. (In v0.4–v0.8 the refusal made the add-on look dead, e.g. in Tomb Raider.)
- "Not managed" reasons are shown in red.
- Game-side presents are serialised, for frame-generation add-ons that present generated frames from their own thread; each new presenting thread is logged.

#### DX v0.8

- Fixed: when the game resizes with BufferCount = 0 ("keep the count"), the move to the presenter's queue now passes the real buffer count, so DXGI cannot ignore the queue list. Each move is logged with its result.

#### DX v0.7

- **Fixed (regression in v0.5/v0.6): no BFI in games that resize their swap chain before the first frame** (Shadow of the Tomb Raider). The resize started the presenter early; the move to the presenter's queue on the first frame then ran while the presenter still held the swap chain's buffers. A pending move now happens inside the resize itself, and the first-frame path stops the presenter before moving.

#### DX v0.6

- Diagnostics for frame generation: the log and the window name the module that implements the swap chain below ReShade (`dxgi.dll` = the real one; anything else is a proxy such as a frame-generation layer) and list frame-generation components found in the game (DLSS-G, Streamline, FSR 3 / FidelityFX, XeSS FG, DLSS-G-to-FSR3 mods).

#### DX v0.5

- **Fixed: hang when a game recreates its swap chain** (Resonance). Presentation is now moved to the presenter's queue on the game's first frame, not during swap chain setup, where ReShade holds a lock that the game's other threads were waiting on.
- **Fixed: device removed with `PresentOnOwnQueue=0`** (Resonance, and most likely the v0.1 Control crash). The presenter now draws on the same queue the swap chain presents on; drawing into the swap chain from another queue gets the device removed.
- The presenter's frame pick and submission are ordered with the game's frame copies, which is required when both share one queue.

#### DX v0.4

- RivaTuner (RTSS) refused by default (changed in v0.9). Its OSD and frame limiter hook Present, so they run inside the presenter — that crashed Control, and the limiter would throttle the strobe itself. Exclude the game in RTSS (Application detection level: None) and use this add-on's Game frame cap instead; `AllowRTSS=1` overrides.
- Other tools that hook Present (Steam, Discord, NVIDIA, Epic overlays, Special K) are listed in the log and the window.
- Fixed: the crash report was written over `BFIPresenter_log.csv` instead of `BFIPresenter_dred.txt`.
- One-time progress lines in `ReShade.log`: "first game frame received", "first frame presented".

#### DX v0.3

- **Game frame cap: Half refresh** — 60 fps at 120 Hz, so every game frame is shown exactly once lit. The smoothest motion with 2-frame BFI; leave frame generation off.
- **Present on own queue** can be switched off (`PresentOnOwnQueue=0`) for games that show a black screen.
- Step-by-step setup logging (`init 1/6` … `init 6/6`) and a presenter heartbeat ("Last present … ms ago", and a log line if presenting stalls), to pinpoint where a game gets stuck.

#### DX v0.2

- Settings window in the ReShade overlay, with live status.
- Presentation now runs on the add-on's own queue, independent of the game's GPU work (v0.1 presented on the game's queue without waiting for its own draw).
- Overlays that hook Present underneath the add-on now see the real back buffers.
- 8-bit back buffers are created typeless, so games that view them as sRGB stay valid.
- DRED crash diagnostics; explicit "not managed" reasons (Direct3D 11 games, for example).

### Vulkan layer (v0.1–v0.8.2)

#### Vulkan v0.8.2

- **Queue one refresh ahead** (`QueueAhead=1`, overlay: Advanced). Off by default.
    - **The problem (Indiana Jones):** with the GPU fully loaded, the presenter's per-refresh draw sometimes finished after its vblank. The next present call then blocked a refresh, and the display held a lit or dark refresh: a flash, 10-20 a minute even at Half.
    - **What it does:** one extra refresh is presented right after a vblank, so the display always has the next refresh queued and a late draw is covered. Costs one refresh (~8 ms) of latency.
    - The 3-image FIFO swap chain caps the depth. If a present still blocks, the queue is deeper than wanted and one vblank is skipped to drain it. After the presenter falls a refresh behind, the extra refresh is queued again.
    - The overlay shows how often each happens. The timing log has a `queue_ahead` column (1 primed, 2 the priming present, 0 not primed, -1 off).

#### Vulkan v0.8.1

- **The overlay no longer says "Waiting for the game's swap chain" while BFI is running.** Games recreate a swap chain by creating the new one first and then destroying the old one. Destroying the old one reset the status. It now resets only when no managed swap chain is left.

#### Vulkan v0.8.0

Brings the layer up to date with the Direct3D add-on (v0.66-v0.82):

- **Just-in-time release at the Half / Quarter cap** (`CapJustInTime=1`, on by default).
    - Each game frame targets a lit refresh and is released at that refresh's vblank minus its measured frame time (release to copy finished on the GPU, 95th percentile) and a margin.
    - Every frame lands on the same lit refresh, so there is no repeat-then-skip at Half and no frames held 1 then 3 cycles at Quarter.
    - At its limit it releases as early as the cap allows, instead of falling back to the old one-refresh-early release.
- **Later release at the Refresh cap** (`LaterRelease=1`, Lowest latency, off by default). Cuphead (DX12 add-on): release to display went from 16.7 to 11.3 ms.
- The two share `LaterReleaseMarginMs` (1.5 ms; widens after misses, narrows after 5 clean seconds, at most 3 ms over the base). The overlay shows the state, frame time and margin. The timing log has `lr_state`, `lr_lead_ms`, `lr_est_ms` and `lr_margin_ms`.
- **Cap alignment.** Without just-in-time release, the Half/Quarter cap releases the game one refresh before the lit refresh, so frames make it.
- **Smooth fixes:**
    - Smooth catches up to the newest frame after two have waited 3 cycles. Before, it stayed a cycle behind.
    - With a frame cap on, present waits complete when the frame is handed over (as in Lowest latency). Smooth no longer lowers the frame rate of games that can't hold the cap.
- **Lowest latency waits up to 3 ms for the expected frame** if it is already submitted, instead of repeating a frame and skipping the next.
- **Rolling scan phosphor decay** (`RollingPhosphorDecay`): rows fade like CRT phosphor, with no seam.
- **Uneven strobe cycles** are now counted against the expected step. A steady 120 fps game no longer shows "does not match the strobe", and the Quarter cap's normal repeats no longer push automatic frames ahead to 2.
- **Overlay reorganised into sections:** Strobe, Brightness and output, Frame pacing and latency, Status, Advanced. Warnings show only when something needs attention.

#### Vulkan v0.7.3

- **Evenly timed frames with the frame cap.** The game is now released at the vblank itself, not after the presenter's own present returns. That present's varying length shifted each release, and the game timed its animation to the uneven gaps, which shows as judder under BFI. This is the same fix as DX12 add-on v0.67.

#### Vulkan v0.7.2

- **Lowest latency pacing no longer holds present-wait games at 60 fps.** This affects DXVK games and anything else that paces itself with `VK_KHR_present_wait`. Their presents used to count as done only when the strobe took a new frame, 60 times a second. In Lowest latency mode they now count as done as soon as the frame is handed over, so the game runs at its own rate up to the frame cap. Smooth mode still paces the game to the strobe. This is the same fix as DX12 add-on v0.64.

#### Vulkan v0.7.1

Same changes as DX12 add-on v0.63:
- **The ABL guard no longer relaxes with rolling scan on.** The A95L's own ABL still dims bright areas with rolling scan on.
- **Rolling scan is documented for 240 Hz and faster displays.** At 240 Hz, use Frames 4, the Quarter cap and Sine BFI.
- **Missed refreshes are judged at the display's own refresh rate.**

#### Vulkan v0.7

- **Rolling scan (`RollingScan=1`, or Ctrl+Shift+S to toggle)**, the same as DX12 add-on v0.62:
  - **How it works.** One horizontal band per sub-frame, lit in turn from the top down like a CRT beam, so the screen as a whole never goes dark.
  - **Flicker.** Less flicker in peripheral vision.
  - **The seam.** `RollingSeam` (0.08 of the screen height) blends the seam between the bands.
  - **Both methods.** It works with BFI and Variable MPRT, and the MPRT brightness budget still applies per pixel.

#### Vulkan v0.6.1

- **Frame-limiter warning.** If something caps the presenter a little below the display's refresh rate, the log and the overlay say so after about 3 seconds. The display stays at 120 Hz, but the presenter only gets, say, 118 presents a second through, so the strobe slips a refresh twice a second and flickers. Usual causes are an RTSS frame limit, including RTSS's global profile, and NVIDIA's "Max Frame Rate" setting, globally or for the exe.
- **The log shows the display's refresh rate** on the `vblank pacing on ...` line.

#### Vulkan v0.6

- **GPU-heavy games no longer lose finished frames.** The ring of finished frames waiting for the strobe grew from 4 to 6. When the GPU ran about three frames behind the game, the frame waiting for its strobe cycle could be overwritten before it was shown, so only every third to fifth frame reached the screen. This may also be behind some of the stutter and flashes in GPU-bound games such as Indiana Jones.

#### Vulkan v0.5

- **32-bit games.** A 32-bit build of the layer, `BFIPresenterVK32.dll`, is now included and registered by `install_vulkan_layer.bat`. Many DirectX 9 games, and the DXVK builds that run them, are 32-bit. Each game loads the matching build automatically.

#### Vulkan v0.4

- **HDR look for SDR games (highlight expansion).** When an SDR game is presented as HDR10, the image is unchanged below `SdrHighlightKnee` (0.65 of the signal). Above it, the brightest parts stretch toward `SdrHighlightNits` (800), keeping their colour, and the curve joins without a visible seam. With SDR white at 240 nits:

  | SDR signal | Shown at |
  |---|---|
  | 50 % | 51 nits (unchanged) |
  | 85 % | 308 nits |
  | 100 % | 800 nits |

  BFI then boosts the result as in HDR games, and the ABL guard limits large bright areas. `SdrHighlightNits=0` turns the expansion off, so `SdrWhiteNits` alone sets the level. All three settings are also in the overlay.

#### Vulkan v0.3

- **SDR games (emulators) are presented as HDR10.** When Windows HDR is on, an SDR game's frames are shown in HDR10, with SDR white at `SdrWhiteNits` (200). The lit refresh can then go above SDR white, as in HDR games.
  - In plain SDR the boost has nowhere to go. With Gain above 0.5, Variable MPRT moves everything brighter than mid-grey into the "dark" refresh, so bright areas barely strobe. That's why PCSX2 and RPCS3 showed little added clarity at Gain 1.0.
  - Set `SdrToHdr=0` to keep plain SDR. In plain SDR, use Gain 0.5.
- **The settings window comes back after an emulator loads a new game.** Emulators destroy and recreate Vulkan for each game, and ReShade is reloaded with it. The layer now takes its window off the old ReShade and adds it to the new one.
- **Layer-order check fixed.** Steam's overlay layer can sit between ReShade and this layer (Indiana Jones). The check now looks for ReShade anywhere above the layer, so it no longer reports a false error.
- **The log lists the GPU's queue families and whether the driver offers queue priority.** This is groundwork for giving the presenter a higher-priority queue in GPU-heavy games.

v0.2: the hitch guard matches DX12 v0.54. While paused it shows every refresh at the strobe's average brightness (no brightness change). It pauses on missed refreshes or on loading (`HitchGuardLoadingMs`, 1000 ms), not on short gameplay hitches.

The same presenter-clocked BFI as the DX12 add-on, for Vulkan games. DirectX 9/10/11 games will follow through DXVK, which turns them into Vulkan games.

#### Vulkan v0.2

- the hitch guard matches DX12 v0.54. While paused it shows every refresh at the strobe's average brightness (no brightness change). It pauses on missed refreshes or on loading (`HitchGuardLoadingMs`, 1000 ms), not on short gameplay hitches.

#### Vulkan v0.1

- First build: the presenter-clocked strobe of the DX12 add-on as a Vulkan layer.
