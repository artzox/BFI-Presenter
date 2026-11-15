# Frame warp test

Checks frame warp without a game:

- `clock_test.cpp` runs the pacing clock (`common/bfi_warpclock.h`) against simulated games at 30–70 fps on a 60 Hz
  strobe and prints how even the steps on screen are in each mode (step sd; "backward" must be 0).
- `warp_test.cpp` draws with the add-on's own shaders (taken from `dx/bfi_presenter.cpp`) and the passes of
  `dx/bfi_warp.h` through Direct3D 12: two 1920x1080 frames of a panning scene with a static HUD, a crosshair and an object moving
  against the pan (`make_frames.py`), then the second frame shown half a frame and one frame ahead. `check.py`
  compares the result with the true picture at that moment, and the measured motion with the real one.

`./run.sh` builds and runs all of it (MinGW-w64, python3 with numpy and scipy; Wine with a Vulkan driver, or run
`warp_test.exe` on Windows). `d3dcompiler_47.dll` must be next to `warp_test.exe`.

What to expect (1.0.41-t1, 8x8 blocks): background motion within a quarter of a pixel of the truth in the median,
HUD and crosshair pixels changed by the warp: 0, pixels off by more than 16/255 half a frame ahead: about 5% (33% when
the frame is repeated instead); with 4x4 blocks (`./run.sh 0`) about 4%; bend (`./run.sh 3`) about 6%; shift, the default (`./run.sh 4`), about 2%, with the pan measured to a tenth of a pixel.
