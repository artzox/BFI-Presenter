VRR timed-present test 1.7 (standalone, no game)

Works so far (A95L, RTX 5080, Windows 11 24H2, NVIDIA Vertical Sync "Use the 3D application setting"): steady target
times plus a preferred present duration (Windows then drives the panel at that rate through VRR) kept 99-100% of
refreshes within 0.25 ms at 100 and 110 Hz. Without the preferred duration the display stays on its fixed 120 Hz.
1.7 checks how few images can be handed over ahead (latency), that the GPU load really runs, and whether G-SYNC is needed.

Setup: G-SYNC on (windowed and full screen), Vertical Sync NOT Force on, no frame limiter. 60 s per run.
1_load_ahead3.bat           GPU load, 3 images ahead (as before)
2_load_ahead2.bat           2 ahead (about 10 ms less latency)
3_load_ahead1.bat           1 ahead (about 20 ms less)
4_gsync_OFF_load_ahead3.bat run this one with G-SYNC turned OFF for VrrTimedTest.exe
run_170Hz_240Hz_monitor.bat for 240 Hz monitors
