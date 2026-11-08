VRR timed-present test (standalone, no game)

Shows a BFI strobe (a grey field with a moving white bar on the lit refresh, black refreshes between) on a
borderless fullscreen window, at a refresh rate set by exact target times with Windows' presentation API, and
logs how evenly the display really refreshes to VrrTimedTest_log.txt next to the exe.

Setup
- NVIDIA: G-SYNC on for windowed and full screen mode (globally, or a profile for VrrTimedTest.exe),
  Vertical Sync on (Force on, or Use the 3D application setting). No frame limiter (RTSS limit 0).
- The rate must be inside the display's VRR range and below its mode's refresh rate.

Run
- run_100Hz.bat (A95L / 120 Hz displays: 100 Hz, 50 Hz strobe), or run_170Hz.bat (240 Hz monitors)
- run_100Hz_gpu_load.bat: the same with a game-like GPU load (about 14 ms of GPU work per 20 ms cycle on a second device)
- or: VrrTimedTest.exe <rate Hz> <frames per cycle> <presents queued ahead> <seconds> <GPU load ms>
  e.g. VrrTimedTest.exe 110 2 3 120 10
- Esc quits. It ends by itself after the set time (default 120 s).

What the log says
- "flipped" vs "composed by DWM": images should be flipped straight to the display; if most are composed,
  the timing follows DWM and the test says so.
- "within 0.25 ms" and the 99% figure: how evenly refreshes are spaced. For BFI, most should be within 0.25 ms.
- "shown after target": how late images appear against the time they were given.
