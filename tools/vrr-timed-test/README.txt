VRR timed-present test 1.4 (standalone, no game)

1.4 gives the target times in the QPC clock, the clock the display's own timestamps come back in. 1.0-1.3 used the
interrupt time, about half a second behind it, so every target was already in the past and images went out at once.

Setup: G-SYNC on (windowed and full screen) for VrrTimedTest.exe, VSync on, no frame limiter.
Runs (60 s each, Esc quits early), each with its own log file:
1_alternating_targets_load.bat  targets 1 ms early/late in turn, GPU load: refresh lengths after odd/even presents
                                should come out about 9 and 11 ms if the display follows the targets
2_steady_load.bat               steady 10 ms targets, GPU load
3_steady_no_load.bat            steady 10 ms targets, no load
run_170Hz_240Hz_monitor.bat     for 240 Hz monitors
Manual: VrrTimedTest.exe <rate> <frames> <queued ahead> <seconds> <GPU load ms> <mode 0/1/2> <clock 0 QPC / 1 interrupt time>
