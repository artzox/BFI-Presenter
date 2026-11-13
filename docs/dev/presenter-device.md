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

## t4 result (Control, no frame generation, 3.8 min)

The presenter on its own device still had its submission held up together with the game's device (22 of 27 missed
refreshes; submit 6-22 ms with ~0.1 ms CPU), while the probe's independent device never was (max 0.5 ms). The
difference between them: the presenter's submission signals a fence shared with the game's device and draws from
staging textures shared with it. t5 times ExecuteCommandLists and Signal separately (`exec_ms`, `signal_ms`) and adds
`PresenterDevice=2`: the presenter signals a fence of its own, and a forwarder thread copies its progress to the shared
fence the game waits on. If `exec_ms` carries the stalls instead, the next step is an importer: the shared staging
frame copied into a private texture on another queue/thread before the presenter samples it.

## t6

In t4 the game's own swap chain never presented, so NVIDIA's driver returned no Reflex latency reports: frame
generation wasn't detected and the frame limit at the game's Reflex sleep never switched on. `PresenterGamePresent=1`
(default with PresenterDevice) lets the game's swap chain present each frame too (DXGI's own Present, past the overlay
hooks, sync interval 0, do-not-wait), hidden under the presenter's window, as `DX11GamePresent=1` does in DX11 mode.

## t6 result → t7

With the game's swap chain presenting again, Reflex reports and frame generation came back. The split timing showed
every remaining submission stall in ExecuteCommandLists (4-32 ms), never in Signal (max 0.08 ms), in both modes: the
presenter's draws reading a texture shared with the game's device tie it to that device's stalls. t7 adds the
importer (`PresenterImport=1`, default with PresenterDevice): a thread with its own queue on the presenter's device
copies each finished shared staging frame into a private texture of the same slot and signals a shared import fence;
the presenter sees a slot as ready only once imported, and draws only from the private copies. The game's copy into a
slot also waits (on the GPU) for that slot's import, and its slot choice skips slots still being imported.

## t7 result → t8

Control, 2.4 min: missed refreshes 2.5 a minute (t6: 5.7-6.1); presenter submit stalls over 2 ms 4 (t6: 27 in 3.7 min).
The importer logged imports held up 7.5, 19.4 and 21.0 ms; the two longer ones line up with the remaining presenter
stalls (a 17.9 ms submit and a 21.9 ms present): the importer and the presenter share the presenter's device, and a
stalled import holds it. t8 (`PresenterImport=2`, default): the importer gets a third device. The shared staging frames
and the game/import fences belong to it (shared with the game's device); the imported copies belong to the presenter's
device and are shared only with the importer's. The presenter's fence is no longer shared at all: the game never waits
on the presenter (it reads only its own copies; the importer waits for the presenter's read of a copy on the CPU).
Pass-through copies (presenter not running) use a fence of the game's own device.
