VRR timed-present test 1.6 (standalone, no game)

Findings so far (A95L, RTX 5080, Windows 11 24H2): with NVIDIA Vertical Sync left to the application (not Force on),
steady targets plus a preferred present duration gave every refresh within 0.25 ms with no GPU load, and about 90%
with a game-like load. 1.6 raises the strobe's GPU priority so its drawing goes ahead of the load.

Setup: G-SYNC on (windowed and full screen) for VrrTimedTest.exe, Vertical Sync: Use the 3D application setting
(NOT Force on), no frame limiter. 60 s per run, each with its own log.
1_load_priority.bat                    GPU load, strobe at GPU priority 7, 3 presents ahead
2_load_priority_queue5.bat             the same, 5 presents ahead
3_no_load_no_preferred_duration.bat    no load, no preferred present duration: is the duration hint what keeps it even?
4_load_priority_110Hz.bat              GPU load at 110 Hz
run_170Hz_240Hz_monitor.bat            for 240 Hz monitors
Manual: VrrTimedTest.exe <rate> <frames> <ahead> <seconds> <load ms> <mode 0/1/2> <clock 0> <scale> <GPU priority -7..7>
