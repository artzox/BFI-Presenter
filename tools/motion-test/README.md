# BFI Motion Test

`BFIMotionTest.exe` is a small Direct3D 12 program that draws moving test patterns, in the spirit of TestUFO, but as a program that ReShade and BFI Presenter hook like any game. It lets you judge sharpness, cadence and strobe settings on the same picture on every display, without depending on a particular game.

**Setup:** put `BFIMotionTest.exe` in a folder, install ReShade (Direct3D 10/11/12, as `dxgi.dll`) and `BFIPresenter.addon64` next to it, as for a game. The BFI Presenter Manager can do both if you add the folder under **Libraries... → Other folders**. Run it, press **F** for borderless fullscreen, and open the presenter's settings in the ReShade overlay as usual.

**What it shows**
- Three lanes moving at full, half and quarter speed (bars with a "UFO", a checkerboard, thin lines, or stepped blocks). Motion follows the clock, not the frame count, so a late or repeated frame shows as a jump, as it would in a game.
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

Build: `build_mingw.sh` (MinGW-w64), no dependencies beyond the Windows SDK libraries `d3d12`, `dxgi` and `user32`.
