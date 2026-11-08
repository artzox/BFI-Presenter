// Game clock drift check (shared by the DX add-on and the Vulkan layer).
//
// Some games time their frames on a clock of their own instead of waiting for the display: ICO at 59.94 fps
// (NTSC timing). On a display at 120.00 Hz its frames then drift against the strobe cycle -- about 0.06 frames a
// second, so once every 15-30 s a frame lands right on the lit refresh's flip: images come back late, the hitch
// guard pauses BFI (flashes) and a swap chain rebuild can't help. At 119.88 Hz the two match and it runs clean.
//
// Fed once a second with that second's game frames, presenter refreshes and the mean arrival phase of the new
// frames (their age when first shown, as an angle over one refresh). Two things must agree over the last 60 s:
//  - the frame count: a steady rate a little off a whole fraction of the refresh rate, 0.06% to 0.6% (59.94
//    against 60 is 0.1%; every 10 s stretch agrees, no second falls 5% short);
//  - the phase: the frames' arrival moves steadily through the refresh, at the speed and in the direction that
//    rate gives (|m| x 1000 ms a second). A game held by the presenter's cap that drops a frame now and then has
//    the same count but a fixed phase -- not reported (DX logs of capped games at 59.9 of 60 fps).
#pragma once
#include <algorithm>
#include <cmath>

struct ClockDrift
{
    int    f[60] = {}, p[60] = {};
    double a[60] = {};        // mean arrival phase that second (radians over one refresh); NAN = none
    int    n = 0, head = 0, good = 0;
    bool   on = false;        // drift reported
    double mis = 0.0;         // game rate x refreshes per frame / refresh rate - 1
    double gameFps = 0.0;     // the game's own rate
    double suggestHz = 0.0;   // the display rate that matches it
    double slipS = 0.0;       // a game frame slips against the strobe cycle about this often (s)
    double phaseMsS = 0.0;    // measured phase drift, ms per second
    int    per = 0;           // refreshes per game frame

    void Reset() { n = head = good = 0; on = false; }

    // Least-squares slope (ms/s) of the unwrapped phase over seconds [from, to) in time order; false if too few.
    bool Slope(double hz, int from, int to, double &ms) const
    {
        const double kPi = 3.14159265358979323846;
        double sx = 0, sy = 0, sxx = 0, sxy = 0, prev = 0, off = 0;
        int cnt = 0;
        bool have = false;
        for (int i = from; i < to; i++)
        {
            const double v = a[(head + i) % 60];
            if (std::isnan(v)) continue;
            double u = v;
            if (have)
            {
                double dlt = v + off - prev;
                while (dlt > kPi) { off -= 2 * kPi; dlt -= 2 * kPi; }
                while (dlt < -kPi) { off += 2 * kPi; dlt += 2 * kPi; }
            }
            u = v + off; prev = u; have = true;
            sx += i; sy += u; sxx += (double)i * i; sxy += i * u; cnt++;
        }
        if (cnt < (to - from) * 3 / 4) return false;
        const double den = cnt * sxx - sx * sx;
        if (den <= 0) return false;
        const double rad = (cnt * sxy - sx * sy) / den;            // radians per second
        ms = rad / (2 * kPi) * (1000.0 / hz);
        return true;
    }

    // Returns true when 'on' changed. hz: the display mode's refresh rate. phase: NAN when no new frame was shown.
    bool Second(int frames, int presents, double phase, double hz, bool exclude)
    {
        const bool was = on;
        if (exclude || presents <= 0 || hz <= 1.0) { Reset(); return was != on; }
        f[head] = frames; p[head] = presents; a[head] = phase;
        head = (head + 1) % 60;
        if (n < 60) n++;
        if (n < 60) return false;
        long F = 0, P = 0;
        for (int i = 0; i < 60; i++) { F += f[i]; P += p[i]; }
        if (F <= 0) { good = 0; on = false; return was != on; }
        const int k = (int)std::floor((double)P / (double)F + 0.5);
        bool steady = k >= 1 && k <= 4;
        const double m = steady ? (double)F * k / (double)P - 1.0 : 0.0;
        for (int c = 0; steady && c < 6; c++)
        {
            long fc = 0, pc = 0;
            for (int i = c * 10; i < c * 10 + 10; i++) { fc += f[i]; pc += p[i]; }
            if (pc <= 0 || std::fabs((double)fc * k / (double)pc - 1.0 - m) > 0.004) steady = false;
        }
        for (int i = 0; steady && i < 60; i++)
            if ((double)f[i] * k < 0.95 * p[i]) steady = false;
        // The phase: frames of a slower game arrive later each cycle (they are older when shown: the age falls).
        double s0 = 0, s1 = 0, s2 = 0;
        const double want = m * 1000.0;                                 // ms a second
        if (steady && Slope(hz, 0, 60, s0) && Slope(hz, 0, 30, s1) && Slope(hz, 30, 60, s2))
        {
            auto close = [&](double v) { return v * want > 0 && std::fabs(v) > 0.5 * std::fabs(want) && std::fabs(v) < 2.0 * std::fabs(want); };
            steady = close(s0) && close(s1) && close(s2);
        }
        else steady = false;
        const double am = std::fabs(m);
        good = (steady && am > 0.0006 && am < 0.006) ? good + 1 : 0;
        on = on ? (steady && am > 0.0004 && am < 0.008) : good >= 10;
        if (on)
        {
            mis = m; per = k; phaseMsS = s0;
            gameFps = hz * (double)F / (double)P;
            double sug = hz * (1.0 + m);
            // the usual pair: 119.88 / 120, 59.94 / 60, 239.76 / 240 ...
            if (std::fabs(sug - hz * 1000.0 / 1001.0) < 0.0003 * hz) sug = hz * 1000.0 / 1001.0;
            else if (std::fabs(sug - hz * 1001.0 / 1000.0) < 0.0003 * hz) sug = hz * 1001.0 / 1000.0;
            suggestHz = sug;
            slipS = 1.0 / std::max(1e-6, am * hz / k);
        }
        return was != on;
    }
};
