VRR timed-present test 1.5 (standalone, no game)

1.1 at 1280x720 (scaled up to the 4K screen by the display hardware) kept every refresh within 0.25 ms; native 4K
did not. 1.5 draws the images at a fraction of the screen size and lets the display hardware scale them, to see
whether that path (an overlay plane, as video playback uses) is the one that honours the target times.

Setup: G-SYNC on (windowed and full screen) for VrrTimedTest.exe, VSync on, no frame limiter. 60 s per run.
1_720p_scaled_load.bat                 as 1.1 (1280x720), with GPU load
2_720p_scaled_alternating_targets.bat  the same with targets 1 ms early/late: odd/even refresh lengths about 9 / 11 ms
                                       mean the display follows the targets
3_1620p_scaled_load.bat                2880x1620 scaled, GPU load
4_1944p_scaled_load.bat                3456x1944 scaled, GPU load
Manual: VrrTimedTest.exe <rate> <frames> <queued ahead> <seconds> <GPU load ms> <mode 0/1/2> <clock 0> <scale 0.2-1>
