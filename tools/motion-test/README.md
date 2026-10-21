# BFI Motion Test

`BFIMotionTest.exe` is a small Direct3D 12 program that draws moving test patterns, inspired by [Blur Busters' TestUFO](https://testufo.com), but as a program that ReShade and BFI Presenter hook like any game. It lets you judge sharpness, cadence and strobe settings on the same picture on every display, without depending on a particular game.

**Setup:** put `BFIMotionTest.exe` in a folder, install ReShade (Direct3D 10/11/12, as `dxgi.dll`) and `BFIPresenter.addon64` next to it, as for a game. The BFI Presenter Manager can do both if you add the folder under **Libraries... → Other folders**. Run it, press **F** for borderless fullscreen, and open the presenter's settings in the ReShade overlay as usual.

**What it shows**
- Three lanes moving at full, half and quarter speed (bars with a marker block, a checkerboard, thin lines, or stepped blocks). Motion follows the clock, not the frame count, so a late or repeated frame shows as a jump, as it would in a game.
- At the bottom, a 10-bit frame counter and a block that moves one step per frame: in a photo or a slow-motion video, a repeated frame shows the same counter twice and a skipped one leaves a gap.

**Keys**

| Key | Does |
|---|---|
| Up / Down | Speed (240–3840 px/s) |
| Left / Right | Pattern |
| 1–4 | Without the presenter: one frame every 1st–4th refresh (sync interval). With the presenter, use its frame cap instead |
| V | VSync on / off |
| F or F11 | Borderless fullscreen / window |
| Space | Pause |
| Esc | Quit |

The window title shows the pattern, speed, frame rate and VSync state.

**How to compare**
- **Start with a reference without BFI:** BFI off (Ctrl+Shift+B), frame cap at Refresh rate, and look at the middle lane at e.g. 960 px/s. That's how blurry sample-and-hold is at your refresh rate.
- **Then the setting you play with:** e.g. BFI on at 2 frames per cycle with the Half refresh cap (60 new frames a second at 120 Hz). Compare it with BFI off at the same Half cap (60 fps sample-and-hold): that's the gain BFI gives you at that frame rate. Comparing BFI at 60 against no BFI at 120 is not a fair test, since 120 fps already halves the blur.
- **For the sharpest edges, use the BFI method or a lower Gain.** With Variable MPRT and a high Gain, bright parts spill light into the dark refresh, so white bars on a dark lane (the top patterns) can look less crisp than mid-tones. That's the price of the extra brightness, not a fault.
- **Watch the frame counter and the sweeping block at the bottom** in a photo or slow-motion video: every step should appear once. A doubled step is a repeated frame (a double image in motion), and a gap is a skipped one.
- **Change one setting at a time**, and give it a few seconds: the automatic frame cap and frames ahead settle after a moment.

Build: `build_mingw.sh` (MinGW-w64), no dependencies beyond the Windows SDK libraries `d3d12`, `dxgi` and `user32`.

Written from scratch for BFI Presenter; no Blur Busters code is used.
