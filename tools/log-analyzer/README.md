# BFI Presenter Log Analyzer

`BFIPresenter-LogAnalyzer.html` is a single offline page: open it in any browser and drop in the files from a session:

- `BFIPresenter_log_<date>_<time>.csv` (every refresh the presenter showed)
- `BFIPresenter_reflex_<date>_<time>.csv` (one line a second: cap, pacing, frame generation, Reflex reports; DX games)
- `BFIPresenter_game_<date>_<time>.csv` (the game's frame times)
- `ReShade.log` (presenter events: automatic frame cap, Reflex limit, collapses, hitch guard)
- Vulkan games: `BFIPresenterVK_log_<date>_<time>.csv` (every refresh, with release to flash and how long frames were ready before their flash) and `BFIPresenterVK.log` (the layer's events)

Turn the timing log on in the presenter's overlay first; the files are written next to the game's executable. Several sessions can be dropped at once: they are grouped by their date and time stamp, and the last table compares every setting across them.

For each setting used in a session it shows the presented and rendered fps, how many new frames reached the flashes, how many flashes repeated a frame or skipped one, release-to-flash and (in Reflex games) input-to-flash latency, the game's render submit and GPU times, and a verdict with the setting that ran most evenly. Since 1.0.27 it also reports the automatic frame cap at the frame start (the start time found and what it gained against the usual release, or why it wasn't needed), whether a session started from what was remembered for the game, games running a little slower than the strobe (with the display mode that matches them), and repeated frame-start searches. **Copy summary** puts the tables and findings on the clipboard as Markdown, ready for an issue.

Everything is read in the browser; no file leaves the computer.
