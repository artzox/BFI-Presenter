# BFI Presenter: estimated input-to-screen latency

Sony A95L, Game mode, fixed 120 Hz, 2-frame BFI. All values in milliseconds.

- **Input wait**: the average time an input waits for the game to read it (half a game frame: 4.2 at 120 fps, 8.3 at 60, 16.7 at 30).
- **Release → on screen**: from the game being let go to start a frame to the start of the refresh that shows it. Measured from timing logs (Obduction, Cuphead, Indiana Jones, God of War Ragnarök) where marked; otherwise estimated from measured frame times.
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

## Add-ons on top of any row

| Setting | Extra latency |
|---|---|
| Queue one refresh ahead | +8.3 |
| Frames ahead 2 (automatic when the game keeps missing strobe cycles) | up to one game frame: +8 at 120 fps, +17 at 60 |
| Smooth at Half / Quarter, when it holds a late frame in reserve | +17 for that frame (otherwise the same as Lowest latency since v0.70) |

## Reading the table

- At 120 fps the presenter costs about 3–7 ms against plain VRR: the one refresh it needs to draw the strobe after picking a frame. Later release recovers most of it in light games.
- Half refresh costs latency mostly through the input wait: at 60 fps the game reads input half as often. Heavy games add up to another refresh, because just-in-time release has to let them go earlier.
- Applying the Half or Quarter cap with Reflex's frame limit adds about 10–20 ms against just-in-time release, because Reflex starts each frame on its own schedule rather than just before its flash. Use it only for games whose irregular frame delivery makes just-in-time release keep switching off.
- Smooth at the Refresh cap adds a full refresh and doesn't make 120 fps motion more even. Use it only at Half or Quarter.
- The reference row is the least certain: plain VRR latency varies with the game and with Reflex.
