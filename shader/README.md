# BFI Presenter — shader version (prototype)

`BFIPresenter_Shader.fx` is a ReShade effect with the same strobe as the add-on: BFI with highlight roll-off, the Variable MPRT brightness budget, sine and inverted cycles, rolling scan with phosphor decay, and the ABL guard. The difference is the clock. The add-on presents on the display's vblank; the shader strobes with the game's own frames, one game frame per sub-frame of the cycle.

It is meant for the few games that hold a locked frame rate equal to the refresh rate but whose engine doesn't get along with the presenter (Moons of Madness: Refresh + Smooth stuttered). Everywhere else the add-on is better: here a late game frame shows a lit or a dark refresh twice. The hitch guard can soften that, but not prevent it.

## Use

1. Remove the add-on from the game (`BFIPresenter.addon64` / `.addon32`), or set `Enabled=0` in its `BFIPresenter.ini`. Never run both.
2. Copy `BFIPresenter_Shader.fx` into the game's ReShade shader folder (`reshade-shaders\Shaders`).
3. In the ReShade overlay turn on **BFI Presenter (shader)** and drag it to the **bottom** of the list, after CRT-Standalone (set `ENABLE_DECAY=0` there).
4. Display at a fixed refresh rate (G-SYNC off for the game), VSync on, and a game that holds exactly that frame rate. Set **Display refresh rate** to it (120 by default).

## Hitch guard

The shader can't see the display's refreshes, only the time between the game's frames. It sums each frame's time minus one refresh: CPU-measured times jitter in long/short pairs that cancel out, but a missed refresh moves the sum by a whole refresh. When that happens, or the game runs faster than the display, or its average frame time is more than 10 % off the refresh, every frame is shown at the strobe's average brightness for **Pause after a hitch** (500 ms). The first 2 seconds after ReShade starts are paused too.

**Show status square** puts a small square in the top-left corner: green = strobing, amber = paused by the hitch guard, red = the game isn't running at the refresh rate.

## Differences from the add-on

- No frame cap or pacing: the game's own VSync does that.
- The output format comes from ReShade (`BUFFER_COLOR_SPACE`: SDR, scRGB or HDR10), or set it by hand.
- Direct3D 9: no ABL guard (the shader model can't run its loops); everything else untested there.
- Settings live in the ReShade preset, not in `BFIPresenter.ini`.
