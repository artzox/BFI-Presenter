# BFI Presenter Settings Guide

What each option in the BFI Presenter window (and in `BFIPresenter.ini`) does, in plain words, for people who just want sharper motion and don't care how a GPU works. Each entry says what it is, what you'd notice, and when it's worth touching. Most people only ever need the first two sections.

**Contents:** [The idea in one minute](#the-idea-in-one-minute) · [The strobe](#the-strobe) · [Rolling scan](#rolling-scan) · [Your display](#your-display) · [Smoothness and latency](#smoothness-and-latency) · [Safety nets](#safety-nets) · [Compatibility switches](#compatibility-switches) · [Vulkan only](#vulkan-only) · [Logs](#logs) · [Hotkeys](#hotkeys) · [Good starting points](#good-starting-points)

Each entry shows the setting's name in the overlay, its key in `BFIPresenter.ini` and its default.

## The idea in one minute

An OLED or LCD keeps each picture lit for the whole time until the next one arrives. Your eyes follow moving things smoothly, but the picture on screen jumps in steps, so everything in motion smears. Old CRT TVs didn't have this problem because each picture only flashed briefly.

BFI Presenter brings the flash back. At 120 Hz your screen shows 120 pictures a second; BFI Presenter makes one of them bright and the next one dark, over and over. The game still shows 60 new images a second, but each one is only visible for half the time, so motion looks about twice as sharp. The price is brightness (half the light) and a 60 Hz flicker some people notice.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/bfi-refreshes-dark.png">
  <img src="img/bfi-refreshes-light.png" alt="Refreshes at 120 Hz: normal (all lit), BFI with 2 frames (lit, dark), Variable MPRT (dark refreshes slightly lit) and a hitch guard pause (steady half-bright refreshes)." width="760">
</picture>

Everything below is a way of tuning that flash: how bright it is, how it handles highlights, and how BFI Presenter keeps the game in step with it.

## The strobe

**BFI enabled** · `Enabled` · default: on\
The main switch. Off, BFI Presenter still shows the game but without any dark frames, so you can compare.\
*When to change it:* use the hotkey Ctrl+Shift+B to flip it during play and see the difference.

**Method** · `Method` · default: Variable MPRT
- **BFI** makes the dark refresh truly black. Sharpest motion, but the picture loses half its light, and very bright areas have nowhere to go, so they get gently squashed.
- **Variable MPRT** works like a brightness budget: dark and mid-tones flash exactly like BFI, but anything too bright to fit in one flash borrows a bit of the dark refresh. You keep bright highlights at the cost of slightly less sharpness in only those bright parts. Think of it as "BFI where it's free, a little less where it would cost you brightness."

*When to change it:* Variable MPRT is the better default, especially in HDR. Try BFI if you want the crispest possible motion and don't mind a dimmer picture. Hotkey: Ctrl+Shift+M.

**Frames per cycle** · `Frames` · default: 2\
How many refreshes make up one flash cycle: 2 means "one lit, one dark". 3 or 4 means one lit and two or three dark, which is even sharper but darker, and flickers at a lower rate.\
*When to change it:* keep 2 at 120 Hz. Higher values are for 240 Hz+ displays; pair them with the **One game frame per strobe cycle** frame cap, so each game image gets exactly one flash.

> **The formula:** strobe rate = your refresh rate ÷ frames per cycle. For a new image on every flash, the game needs at least that many fps. With frame generation, count the **final** fps, generated frames included: the presenter sees every presented frame the same way.
> E.g. 120 Hz ÷ 2 = 60 fps · 240 Hz ÷ 2 = 120 fps (e.g. 60 real ×2 frame generation, or 40 real ×3) · 360 Hz ÷ 6 = 60 fps.
> Prefer a higher frame-generation factor over more frames per cycle: e.g. at 240 Hz, 40 real ×3 at 2 frames (a 120 Hz strobe) beats 40 real ×2 at 3 frames (an 80 Hz strobe, more flicker).

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/frames-per-cycle-dark.png">
  <img src="img/frames-per-cycle-light.png" alt="The same 1/15 s at 120 Hz with 2 frames, 240 Hz with 4, 360 Hz with 6, 360 Hz with 6 inverted, and 120 Hz with 2 frames at 30 fps: more frames per cycle give a shorter flash but less light; inverted is bright but barely sharper; at 30 fps each image is flashed twice." width="760">
</picture>

**Gain vs blur** · `Gain` · default: 0.5 (recommended 1.0)\
How much of the lost brightness to win back. At 0.5 the lit frame looks exactly like the normal picture, so the overall image is half as bright. At 1.0 the lit frame is boosted to make up the difference, so the average brightness is close to normal.\
*When to change it:* use 1.0 on bright displays (most OLEDs in HDR). Lower it if highlights look washed out or the screen dims in bright scenes.

**Dark frame floor** · `DarkFloor` · default: 0\
How dark the dark refresh is. 0 is pure black. Raising it leaves a faint copy of the picture in the dark refresh: less flicker, a bit more blur.\
*When to change it:* raise to 0.05–0.15 only if the flicker bothers you.

**Highlight roll-off** and **knee** · `HighlightRolloff`, `RolloffKnee` · default: on, 0.75\
BFI method only. When the boosted lit frame can't get any brighter, the brightest areas would clip into flat white blobs and shift colour. Roll-off eases them in smoothly instead, keeping their colour. The knee is where the easing starts: lower means it starts earlier and is gentler.\
*When to change it:* leave it on. Lower the knee a little if bright skies or lamps look flat.

**Sine BFI** · `SineBFI` · default: off\
Instead of a hard on/off, the brightness rises and falls in a soft wave across the cycle. Softer flicker, slightly more blur. Mostly useful with 3 or more frames per cycle.

**Invert cycle** · `InvertCycle` · default: off\
With 3+ frames per cycle, flips the pattern to "lit, lit, dark" instead of "lit, dark, dark". Brighter and less flicker, less sharpness gain. Does nothing at 2 frames.

## Rolling scan

A CRT didn't flash the whole screen at once: a beam drew it line by line from top to bottom. Rolling scan imitates that by lighting the screen in horizontal bands, one after another.

**Rolling scan** · `RollingScan` · default: off\
Lights the screen band by band instead of all at once. Because part of the screen is always lit, the flicker feels much calmer.\
*When to change it:* really meant for 240 Hz+ displays (4 bands). At 120 Hz there are only two halves, with a visible seam in the middle. Hotkey: Ctrl+Shift+S.

**Seam** · `RollingSeam` · default: 0.08\
How wide the blended zone is where two bands meet, as a share of the screen height. Wider hides the edge better.

**Phosphor decay** · `RollingPhosphorDecay` · default: 0 (off)\
Makes each line fade out after the "beam" passes, like the glow of a real CRT, instead of switching off sharply. No seam at all, smoother and less flickery, slightly more blur.\
*When to change it:* 0.2–0.4 is a good start if you use rolling scan.

## Your display

**LCD: prevent image retention** · `LcdPhaseSwap`, `LcdPhaseSwapSeconds` · default: off, 5 s\
**Turn this on if you have an LCD monitor (IPS, VA or TN).** An LCD drives every refresh with the opposite voltage of the one before, so over time it all evens out. With BFI's "lit, dark, lit, dark" every lit picture lands on the same voltage, and after a long session the screen can keep a faint ghost of the image (HUDs, menus). It's usually temporary, but annoying. This option, every few seconds, briefly fades the flashing out to a steady picture (about half a second), shifts it by one refresh where nobody can see it, and fades it back in, so the lit pictures switch to the other voltage and it evens out again. At most you'll notice motion getting a little softer for a moment.\
*When to change it:* on for every LCD; leave off for OLED, which doesn't work this way. Not needed with an odd number of frames per cycle (3), where the lit refresh alternates anyway.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/lcd-phase-swap-dark.png">
  <img src="img/lcd-phase-swap-light.png" alt="Without the LCD option every lit refresh gets the same + voltage; with it the strobe fades to a steady picture, one extra refresh is inserted, and afterwards every lit refresh gets the − voltage." width="760">
</picture>

**Output** · `Output` · default: auto\
Whether the game's picture is SDR or HDR (and which HDR format). Auto reads it from the game.\
*When to change it:* leave on auto. Only force it if colours look obviously wrong.

**Display peak** · `DisplayPeakNits` · default: 1400\
The brightest your screen can get, in nits. In HDR, the boosted lit frame never asks for more than this.\
*When to change it:* set it to your display's real peak brightness (check a review such as RTINGS). Too high and highlights clip; too low and you waste brightness.

**ABL guard** and **budget** · `AblGuard`, `AblGuardBudgetNits` · default: on, 250\
OLED screens have a power limit: when a lot of the screen is bright at once, the TV dims everything (that's "ABL"). With BFI the lit frame is extra bright, so that limit can kick in and make bright scenes pump or flash. The guard watches how much bright area is on screen and gently lowers the boost in very bright scenes, so the TV never hits its limit.\
*When to change it:* set the budget to your screen's full-white brightness: about 250 for the Sony A95L and similar QD-OLEDs, 150–200 for many LG-panel OLEDs.

## Smoothness and latency

BFI only looks right if every new game image lands exactly on a lit refresh. These settings decide how BFI Presenter paces the game to make that happen, and how much input delay you trade for smoothness.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/frame-pipeline-dark.png">
  <img src="img/frame-pipeline-light.png" alt="Delay from input to the lit image at 120 Hz: Refresh cap with Later release is quickest (about 8 ms), then Lowest latency without it; at the Half cap just-in-time release is quickest; Smooth after a late image, a second frame ahead and Queue one refresh ahead each add waiting." width="760">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/frame-pacing-dark.png">
  <img src="img/frame-pacing-light.png" alt="Example at 120 Hz and 60 fps, flash by flash: every other image finishes just after its flash. Lowest latency shows the newest image, so it repeats one and then jumps over the late one (C, E and G are never shown); Smooth shows the oldest not yet shown, so after one repeat every image appears once, in order, one flash later." width="760">
</picture>

**Game frame cap** · `CapGameToRefresh` · default: Refresh rate\
Limits how fast the game runs so it stays in step with the flashing.
- **Refresh rate** (the full refresh rate, e.g. 120 fps at 120 Hz): for games that easily hold 120.
- **Half refresh** (half the refresh rate, e.g. 60 fps at 120 Hz): for games that can't hold 120. Every game image gets exactly one flash, which is the smoothest look.
- **Quarter** (a quarter of the refresh rate, e.g. 30 fps at 120 Hz): for very heavy games; each image is flashed twice, evenly.
- **One game frame per strobe cycle**: the refresh rate divided by Frames per cycle. Made for fast displays with longer cycles: 60 fps at 360 Hz with 6 frames, 90 fps at 360 Hz with 4, 80 fps at 240 Hz with 3. Each game image gets exactly one flash.
- **Custom** (`CapDivisor`): a new game image every N refreshes (1–8). Pick a multiple of Frames per cycle, or some images start on a dark refresh and motion judders; the overlay warns when it doesn't fit.
- **Off**: no limit (not recommended).

*When to change it:* if the game can hold 120, use Refresh. If it dips below, use Half. With frame generation (DLSS FG, FSR FG, Lossless Scaling) always use Refresh, never Half.

**Frame pacing** · `FramePacing` · default: Smooth
- **Smooth** shows every game image once, in order, even if one arrives a little late: no stutter, but up to one frame more delay.
- **Lowest latency** always shows the newest image: quickest response, but a late frame can cause a small hitch.

*When to change it:* fast or competitive games: Lowest latency. Slow, cinematic games that dip in frame rate: Smooth.

**Smooth: catch up after a hitch** · `SmoothCatchUp` · default: off\
After a stutter, Smooth mode can skip one frame to get back to the lowest delay. That makes a second small stutter right after the first. Off means it stays one frame behind instead and keeps motion even.\
*When to change it:* leave off. Turn on only if you prefer lower delay over perfectly even motion.

**Just-in-time release** · `CapJustInTime` · default: on\
With any cap below the refresh rate (Half, Quarter, one frame per strobe cycle, Custom), BFI Presenter learns how long the game takes to draw a frame and tells it to start at exactly the right moment, so each frame finishes just before its flash. Even motion, no waiting around.\
*When to change it:* leave on.

**Later release** and **margin** · `LaterRelease`, `LaterReleaseMarginMs` · default: off, 1.5 ms\
The same trick for the Refresh cap with Lowest latency: the game starts each frame as late as safely possible, which cuts up to about 5 ms of input delay in light games. The margin is the safety gap it keeps (it grows by itself if frames start arriving late).\
*When to change it:* turn on for light games that hold 120 fps comfortably (Cuphead, indies). Not with frame generation.

**Frames ahead** and **automatic** · `MaxFramesAhead`, `AutoFramesAhead` · default: 1, automatic on\
How many finished frames the game may line up before they're shown. Like a queue at a counter: 1 means you're always served next (least delay), 2 gives a buffer if the game is struggling. Automatic picks 1 normally and switches to 2 only while the game keeps missing its slot.\
*When to change it:* leave automatic on.

**Wait for vblank** · `WaitForVBlank` · default: on\
"Vblank" is the moment the screen starts a new refresh. BFI Presenter waits for it before sending each flash, which keeps the flashes perfectly in time with the screen.\
*When to change it:* leave on.

**Presenter buffers** · `PresenterBuffers` · default: 3\
How many picture slots BFI Presenter uses to hand flashes to the screen. More slots give more slack, fewer give slightly less delay.\
*When to change it:* leave at 3.

**Queue one refresh ahead** · `QueueAhead` · default: off\
Keeps one finished flash ready in advance, so if the graphics card is briefly too busy, the screen still gets its flash on time. Costs one refresh (about 8 ms at 120 Hz) of delay.\
*When to change it:* turn on only if you see single bright or dark flashes in very GPU-heavy games, or if G-SYNC keeps sneaking in.

## Safety nets

**Hitch guard** · `HitchGuard`, `HitchGuardLoadingMs` · default: on, 1000 ms\
When the game freezes for a moment (loading screens, big stutters), a lit or dark frame would be held too long and you'd see a flash. The hitch guard notices and shows a steady, evenly bright picture until things settle, then resumes BFI after a second. Motion is less sharp during the pause, but nothing flashes.\
*When to change it:* leave on. The loading time (1000 ms) is how long the game may stop sending new pictures before it counts as a loading screen.

**Allow RivaTuner** · `AllowRTSS` · default: on\
Lets BFI Presenter run when RivaTuner (the frame counter / limiter tool) is active. Its frame limiter must be off for BFI games, or it causes flashes.

## Compatibility switches

These exist to make specific games work. The defaults are right for nearly everyone; the [tested-games list](tested-games.md) says when a game needs one changed.

**Own GPU queue** · `PresentOnOwnQueue` · default: on\
BFI Presenter sends its flashes on a separate lane of the graphics card, so a busy game can't hold them up.

**Queue priority** · `PresenterQueuePriority` · default: 2 (realtime)\
How urgently the graphics card treats the flashes. Realtime needs the game (or Steam) started as administrator; otherwise it falls back to high.\
*When to change it:* lower to 1 or 0 only if a game freezes with BFI Presenter.

**Start BFI after the game resizes** · `StartBFIAfterGameResize`, `WaitForResizeOnRecreatedSwapChain` · default: on\
BFI starts once the game has finished setting up its window (almost every game does this at launch). This avoids freezes in a few games.\
*When to change it:* if BFI doesn't start, change resolution or display mode once in the game.

**Report the game's own frames** · `GameFrameStatistics` · default: on\
Games that measure their own frame rate see their real frames, not BFI Presenter's 120 flashes, so their in-game FPS counters and pacing stay correct.

**Reuse drawn buffers** · `ReuseDrawnBuffers` · default: on\
If the next flash would be identical to one already prepared, it's reused instead of drawn again. Less work for the graphics card, steadier timing.

**Bypass overlay hooks** · `BypassOverlayResizeHooks` · default: on\
Overlays like Steam's or RivaTuner's don't draw on the flashed frames. This prevented hangs in some games.

**Show overlays** · `ShowOverlays` · default: off\
Lets RivaTuner's on-screen display (fps counter) and the Steam overlay draw on the picture you see, in 64-bit DirectX games. Normally they're skipped, because Steam's overlay crashed a few games when the flashed frames went through it. It does nothing in 32-bit games (they crashed or still showed nothing) and OpenGL games.\
*When to change it:* turn it on if you want the RTSS OSD in a DirectX game, and off again if the game then crashes or hangs. Keep RivaTuner's frame limiter off. It takes effect the next time you start the game. Vulkan games show the OSD without it.

**DirectX 11 / 9 / OpenGL support** · `DX11Support`, `DX9Support`, `OpenGLSupport` · default: on\
Lets the add-on handle older games. They're shown through BFI Presenter's own window over the game, so these games need borderless or windowed mode (it switches them automatically).

**Borderless fullscreen** · `BorderlessFullscreen` · default: on\
For those older games: turns the game's window into a borderless full-screen one, since true exclusive fullscreen doesn't work with BFI Presenter's window.

**Let the DX11 game present too** · `DX11GamePresent` · default: off\
A fix for a few DX11 games that slow down over time while BFI runs (Wreckfest).

**OpenGL: don't wait for vblank** · `OpenGLSwapInterval0` · default: on\
OpenGL games are told not to wait for the screen themselves, since BFI Presenter does the waiting.

**Typed buffer description** · `TypedBackBufferDesc` · default: on\
A technical detail some games check (The Last of Us Part I crashed without it). Leave on.

## Vulkan only

**Vulkan layer** · `VulkanLayer` · default: on\
For Vulkan games and emulators: having `BFIPresenter.ini` next to the game turns BFI on. This setting switches it off for that game without deleting the file.

**Own queue** · `VulkanOwnQueue` · default: on\
Same idea as the DirectX "own GPU queue": the flashes get their own lane.

**SDR to HDR**, **SDR white**, **SDR highlights** · `SdrToHdr`, `SdrWhiteNits`, `SdrHighlightNits`, `SdrHighlightKnee` · default: on, 200, 800, 0.65\
For SDR games and emulators with Windows HDR on: the picture is sent as HDR so the lit flash has room to get brighter and BFI doesn't make the image dim. SDR white is how bright normal white looks (match Windows' "SDR content brightness"). Highlights optionally stretches the brightest parts toward a real HDR look.\
*When to change it:* set SDR white to taste; set highlights to 0 for a faithful SDR look.

Without ReShade, edit `BFIPresenter.ini` and save: Vulkan games pick up the change within a second.

## Logs

**Timing log** · `Log` · default: off\
Writes a detailed timing file while you play. Only useful when reporting a problem.

**Diagnostics** and **crash log** · `Diagnostics`, `CrashLog` · default: 1, on\
If a game crashes, these write a short report of what went wrong (`BFIPresenter_crash.txt` and friends). Tiny overhead; leave on, and send the files along with a bug report.

## Hotkeys

| Keys | Does |
|---|---|
| Ctrl+Shift+B | BFI on / off |
| Ctrl+Shift+M | Switch BFI ↔ Variable MPRT |
| Ctrl+Shift+S | Rolling scan on / off |
| Ctrl+Shift+R | Reload `BFIPresenter.ini` after editing it |

## Good starting points

| Situation | Settings |
|---|---|
| The game holds 120 fps | Frame cap: Refresh rate · Pacing: Lowest latency · Light game: Later release on · Gain 1.0, Variable MPRT |
| The game can't hold 120 | Frame cap: Half refresh · Pacing: Smooth (or Lowest latency for fast games) · Just-in-time release on |
| Frame generation | Frame cap: Refresh rate, never Half · Later release off · Final fps (generated frames included) at least refresh ÷ frames per cycle |
| Before anything else | G-SYNC / VRR off for the game, VSync on or "app setting" · No frame limiter (RivaTuner, NVIDIA) on the game · CRT shader: set its own decay off |

---

Defaults shown are the ones in `BFIPresenter.ini`. Gain ships at 0.5 (no change in the lit frame); 1.0 is the recommended setting on bright displays. Strobing flickers at 60 Hz by design; if you are sensitive to flicker or flashing images, take care and use Ctrl+Shift+B to turn it off.
