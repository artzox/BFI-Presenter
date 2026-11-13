# Presenter on a device of its own (DX12 games) — design notes

Status: test (`PresenterDevice=1`, 1.0.40-t4 onwards). Default off.

## Why

The 1.0.40-t2/t3 stall probe (Control, 3x frame generation) timed empty submissions on two extra queues every
millisecond: one on the game's D3D12 device, one on an independent device on the same GPU. Whenever the presenter's own
submission stalled (2-15 ms, with ~0.1 ms of CPU time: waiting, not working), the queue on the game's device stalled
too (up to 21.5 ms); the independent device never went above 0.3 ms. The stalls are tied to the game's device (a driver
lock held while the game's own work is submitted, paged or compiled), not to the GPU or Windows' scheduler.

## Design

Modelled on the DX11 front end, which already runs the presenter on a D3D12 device of the add-on's own:

- `g_device` is the presenter's device; `g_gameDev` (`GameDev()`) the game's. Normal DX12 mode: both the game's.
- The independent device comes from the D3D12 device factory (`D3D12GetInterface(CLSID_D3D12DeviceFactory)`,
  `DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON`); `D3D12CreateDevice` would return the game's own device.
  It is kept across swap chains (`g_presDevOwn`), like the presenter queue.
- Output: a child window over the game window's client area (the DX11 front end's) with a flip-model swap chain on
  the presenter queue (`g_scOut`, created past ReShade's and overlays' hooks). The `Sc*` wrappers and `OwnPresent` use
  `OutSc()`.
- The game keeps its swap chain and the stand-in back buffers (`g_virt`, game device). Its swap chain never presents
  while the presenter runs (as in DX11 mode); before that, the pass-through presents it (child window hidden).
- Staging ring: committed textures with `D3D12_HEAP_FLAG_SHARED` on the presenter's device, opened on the game's
  (`g_stageG`). Fences: created shared on the presenter's device, opened on the game's (`g_gameFenceG`, `g_presFenceG`);
  the game queue signals / waits on those, the presenter queue on the originals.
- No queue rebind and no wait for the game's first resize: the game's swap chain keeps its own queue, so BFI can start
  on the first frame.
- Game resize: presenter stopped, stand-ins and shared staging released, the game's swap chain resized as asked, the
  presenter's swap chain resized to match, everything recreated, presenter restarted.
- Colour space: the game's `SetColorSpace1` is applied to the presenter's swap chain too.

## Not yet (t4)

- SDR to HDR (the presenter's swap chain could be HDR10 while the game's stays 8-bit: simpler than today's path).
- Exclusive fullscreen (the child window is hidden by it); borderless/windowed only.
- HDR metadata pass-through.
