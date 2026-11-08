VRR timed-present test 1.3 (standalone, no game)

Shows a BFI strobe (grey field with a moving white bar on the lit refresh, black refreshes between) on a
borderless fullscreen window, with each image handed over with a target time (Windows' presentation API), and
logs how evenly the display really refreshes. Each run writes its own VrrTimedTest_log_<rate>_load<ms>_mode<n>.txt.

Setup: G-SYNC on (windowed and full screen) for VrrTimedTest.exe, VSync on, no frame limiter.

Runs (60 s each, Esc quits early):
1_load_preferred_duration.bat     game-like GPU load, targets + preferred present duration (as 1.2's load run)
2_load_no_preferred_duration.bat  the same without the preferred present duration
3_load_alternating_targets.bat    targets alternately 1 ms early/late: shows whether the display follows the targets
4_no_load_max_performance.bat     no load; first set NVIDIA "Power management mode: Prefer maximum performance"
                                  for VrrTimedTest.exe (checks whether the idle GPU's power saving caused the bad 4K run)
run_170Hz_240Hz_monitor.bat       for 240 Hz monitors

Manual: VrrTimedTest.exe <rate Hz> <frames per cycle> <queued ahead> <seconds> <GPU load ms> <mode 0/1/2>
