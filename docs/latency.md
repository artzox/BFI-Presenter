# BFI Presenter: estimated input-to-screen latency

Sony A95L, Game mode, fixed 120 Hz, 2-frame BFI. All values in milliseconds. Updated for v1.0.23.

- **Input wait**: the average time an input waits for the game to read it (half a game frame: 4.2 at 120 fps, 8.3 at 60, 16.7 at 30).
- **Release → on screen**: from the game being let go to start a frame to the start of the refresh that shows it. With Reflex's frame limit, the release is the end of the game's Reflex sleep (the start of its frame). Measured from timing logs (Obduction, Cuphead, Indiana Jones, God of War Ragnarök) where marked; otherwise estimated from measured frame times.
- **TV**: the A95L's 8 ms Game-mode response at 120 Hz, added as a constant.
- Not included: the game's own input handling and CPU time before rendering (the same in every setup).

| Setup | Game fps | Input wait | Release → on screen | TV | Total (approx.) | Basis |
|---|---|---|---|---|---|---|
| No presenter, VRR (reference) | ~118 | 4 | 10–14 | 8 | 22–26 | Estimate |
| Refresh + Lowest latency + Later release (light game, e.g. Cuphead) | 120 | 4 | 11.3 | 8 | ~23 | Measured |
| Refresh + Lowest latency (typical game) | 120 | 4 | 16.7 | 8 | ~29 | Measured |
| Refresh + Smooth | 120 | 4 | ~25 | 8 | ~37 | Measured (frame age) |
| Half + just-in-time, light game (frames ~2 ms) | 60 | 8 | ~12 | 8 | ~28 | Estimate |
| Half + just-in-time, heavy game (frames ~12 ms, e.g. Obduction) | 60 | 8 | 23.8 | 8 | ~40 | Measured |
| Quarter + just-in-time, light game | 30 | 17 | ~12 | 8 | ~37 | Estimate |
| Quarter + just-in-time, heavy game | 30 | 17 | 24–40 | 8 | 50–65 | Estimate |
| Half + just-in-time (Silent Hill 2) | 60 | 8 | 25 | 8 | ~41 | Measured |
| Half + Reflex's frame limit (`CapWithReflex`, Silent Hill 2) | 60 | 8 | 34 | 8 | ~50 | Measured |
| Quarter + just-in-time (Alan Wake 2) | 30 | 17 | 35 | 8 | ~60 | Measured |
| Quarter + Reflex's frame limit (`CapWithReflex`, Alan Wake 2) | 30 | 17 | 56 | 8 | ~81 | Measured |
| Refresh + Lowest latency + `ReflexAlign` (God of War Ragnarök, automatic frame cap at 60) | 60 | 8 | ~31 every launch (31–44 without) | 8 | ~47 | Measured |

## Reflex games: the automatic frame cap

When a Reflex game can't hold the refresh rate, the automatic frame cap (on by default with the Refresh rate cap) steps down with a frame limit in the game's own Reflex sleep, where the game reads input, rather than holding its Present (DX since v1.0.19).

| Game (DX) | Reflex limit at the sleep | Presenter's own cap | Measured |
|---|---|---|---|
| Onimusha: Way of the Sword | ~54 | ~58 | Frame start → flash |
| God of War Ragnarök | 54–64 | ~70 | Frame start → flash |
| God of War Ragnarök, Reflex limit lost at launch (fixed in v1.0.21) | ~15 | 45–53 | Frame start → GPU |

`ReflexAlign` (DX, off by default, needs Lowest latency, not with frame generation) moves the start of each frame so it is ready about 2 ms before its flash, instead of waiting up to a whole strobe cycle by an amount that changes from launch to launch. It helps games with even frame times (God of War Ragnarök, row above); in games with uneven frame times it gains little and can repeat a frame now and then (Spider-Man 2: 1.5 ms quicker, more repeats; Dead Space: no change).

## Frame generation

With frame generation and the Refresh rate cap, the automatic frame cap limits it to one presented frame per strobe cycle (e.g. 60 presented at 120 Hz) with a frame limit in the game's Reflex sleep. The input wait follows the rendered frame rate, not the presented one: half of 33 ms at 30 rendered fps.

| Setup | Rendered / presented fps | Value | Measured |
|---|---|---|---|
| Refresh + automatic frame cap, 2× (The Witcher 3, DX) | 30 / 60 | ~30 | Release → flash |
| Refresh + automatic frame cap, 3× (Alan Wake 2, DX) | 20 / 60 | ~42 (~140 with the presenter's own cap) | Frame start → GPU |
| Refresh + automatic frame cap, 2× (Indiana Jones, Vulkan, v1.0.23) | 30 / 60 | 6–9 (30–36 uncapped) | Frame age at the flash |
| Refresh + frame limit without Reflex, 2× XeSS FG through OptiScaler (Riven, DX, v1.0.25) | 30 / 60 | ~38 (~43 with OptiScaler's 60 fps limit) | Release → flash |
| Half + Smooth, 3× (Alan Wake 2, DX, no Reflex limit) | 20 / 60 | ~99 (~130 Reflex limit at Refresh, with ~7% repeats) | Input → flash |
| Refresh + Smooth instead of Lowest latency (God of War Ragnarök, DX) | – / 60 | ~38 against ~21.5 | Same measure for both |

- **Vulkan:** the layer limits frame generation only while that leaves about 30 rendered fps or more (2× at 120 Hz). With 3× (20 rendered) it lagged and presented unevenly, so the cap stays at Refresh. The Vulkan layer has no Reflex limit without frame generation (no `CapWithReflex` or `ReflexAlign`).
- **Smooth with frame generation** costs a full strobe cycle with no smoother motion, which is why Lowest latency is the default frame pacing since v1.0.12.

## Add-ons on top of any row

| Setting | Extra latency |
|---|---|
| Queue one refresh ahead | +8.3 |
| 4 presenter buffers (suggested by the flash hints at 2 or 4 frames per cycle) | none |
| Frames ahead 2 (automatic when the game keeps missing strobe cycles) | up to one game frame: +8 at 120 fps, +17 at 60 |
| Smooth at Half / Quarter, when it holds a late frame in reserve | +17 for that frame (otherwise the same as Lowest latency since v0.70) |

## Reading the table

- At 120 fps the presenter costs about 3–7 ms against plain VRR: the one refresh it needs to draw the strobe after picking a frame. Later release recovers most of it in light games.
- Half refresh costs latency mostly through the input wait: at 60 fps the game reads input half as often. Heavy games add up to another refresh, because just-in-time release has to let them go earlier.
- Applying the Half or Quarter cap with Reflex's frame limit adds about 10–20 ms against just-in-time release, because Reflex starts each frame on its own schedule rather than just before its flash. Use it only for games whose irregular frame delivery makes just-in-time release keep switching off.
- Smooth at the Refresh cap adds a full refresh and doesn't make 120 fps motion more even. Use it only at Half or Quarter.
- In Reflex games the frame limit at the game's Reflex sleep is about as quick as the presenter's own cap or quicker, and much quicker with frame generation, where the presenter's cap would hold the game behind DLSS-G's queue.
- With frame generation, latency follows the rendered frame rate: 2× at 30 rendered fps reads input half as often as 60 fps without it. Use it for motion, not for responsiveness.
- The reference row is the least certain: plain VRR latency varies with the game and with Reflex.
