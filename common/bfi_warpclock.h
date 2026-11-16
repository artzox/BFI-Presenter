// ============================================================================
// BFI Presenter -- frame warp clock (shared by the DX add-on; no graphics API here)
//
// Copyright (C) 2026 artzox -- https://github.com/artzox/BFI-Presenter
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A game that can't hold the strobe rate (54 fps against a 60 Hz strobe) leaves strobe cycles without a new frame:
// the presenter lights the last frame again, and the picture stands still for one flash and then jumps -- stutter.
// Frame warp fills those cycles: the last frame is shown moved ahead along its own motion (measured on the GPU
// between the last two frames), so nothing is held back and no latency is added -- unlike frame generation, which
// waits for the next frame to interpolate.
//
// This clock decides, once per strobe cycle, HOW FAR ahead of the newest frame the picture is shown, in game frames
// ("ahead": 0 = the frame as rendered, 0.5 = half way to where the next frame is expected).
//
// Frames are shown as rendered; only a cycle without a new frame shows the last one moved ahead, half way for a
// single missing frame (steps 1, 1, 0.5, 0.5, 1 ... instead of 1, 1, 0, 1, 1). (Test builds t1-t2 also had an "even
// pacing" mode that moved every frame a little; it moved frames that were fine, so it was dropped.)
// ============================================================================
#pragma once
#include <cmath>
#include <cstdint>

struct WarpClock
{
    // --- state
    static const int kHist = 96;
    uint64_t histId[kHist] = {};
    double   histT[kHist] = {};
    int      histN = 0, histAt = 0;
    uint64_t lastId = 0;
    double   lastNewT = 0.0;
    double   ahead = 0.0;        // position shown, in game frames past the newest frame
    double   rate = 0.0;         // game frames per strobe cycle (0 = not known yet)
    int      repeats = 0;        // cycles in a row without a new frame
    bool     engaged = false;

    void Reset()
    {
        histN = histAt = 0;
        lastId = 0; lastNewT = 0.0; ahead = 0.0; rate = 0.0; repeats = 0; engaged = false;
    }

    // The game's frames per strobe cycle, from the frames presented in the last second (ids count the frames that
    // were never shown too, so a game faster than the strobe reads above 1).
    void Measure(double now, double cycleSec)
    {
        rate = 0.0;
        if (histN < 8 || cycleSec <= 0.0) return;
        const int newest = (histAt + kHist - 1) % kHist;
        int oldest = newest;
        for (int i = 1; i < histN; i++)
        {
            const int k = (histAt + kHist - 1 - i) % kHist;
            if (histT[newest] - histT[k] > 1.0) break;
            oldest = k;
        }
        const double span = histT[newest] - histT[oldest];
        const double n = (double)(histId[newest] - histId[oldest]);
        if (n < 6.0 || span < 0.2 || now - histT[newest] > 0.5) return;
        rate = cycleSec * n / span;
    }

    // Once per strobe cycle, at its start. id: the frame the cycle shows; frameT: when the game presented it;
    // now: the pick; cycleSec: one strobe cycle. Returns how far ahead of that frame to show it (game frames).
    double Cycle(bool on, uint64_t id, double frameT, double now, double cycleSec, double maxAhead, double minRate)
    {
        const bool isNew = id != lastId;
        if (isNew)
        {
            const bool jump = lastId == 0 || id < lastId || id - lastId > 16 || frameT - lastNewT > 0.5;
            if (jump) { histN = histAt = 0; }
            histId[histAt] = id; histT[histAt] = frameT;
            histAt = (histAt + 1) % kHist;
            if (histN < kHist) histN++;
            lastId = id; lastNewT = frameT;
            repeats = 0;
            Measure(now, cycleSec);
        }
        else repeats++;

        engaged = on && rate >= minRate && rate <= 2.5 && maxAhead > 0.0;
        if (!engaged)
        {
            if (isNew) ahead = 0.0;                        // never backward: the offset goes as the next frame arrives
            return ahead;
        }
        if (isNew) return ahead = 0.0;                     // frames are shown as rendered
        // n-th cycle in a row without a frame, of the N the frame rate predicts: n / (N + 1) of the way.
        int N = (int)std::floor(1.0 / rate + 0.5) - 1;
        if (N < 1) N = 1;
        const int n = repeats < N ? repeats : N;           // a stall: stay where the last expected fill was
        double a = (double)n / (double)(N + 1);
        if (a > maxAhead) a = maxAhead;
        if (a > ahead) ahead = a;
        return ahead;
    }
};
