VRR timed-present test 1.8 (standalone, no game)

Findings: steady target times plus a preferred present duration make Windows run the panel at that rate (G-SYNC
not needed; NVIDIA Vertical Sync must not be Force on). Clean without load; 1.7's GPU load ran away to 11-24 ms per
20 ms frame (a GPU that can't hold the frame rate), and the strobe slipped. 1.8 calibrates the load alone for 3 s
first, then keeps it fixed, like a game that holds its frame cap.

Setup: Vertical Sync "Use the 3D application setting", no frame limiter; G-SYNC on or off. 60 s per run.
1_load8ms_ahead3.bat   8 ms of GPU work per 20 ms frame (40%), 3 images ahead
2_load8ms_ahead2.bat   the same, 2 ahead
3_load12ms_ahead3.bat  12 ms (60%), 3 ahead
4_load12ms_ahead2.bat  12 ms, 2 ahead
