#include "../../common/bfi_warpclock.h"
#include <cstdio>
#include <random>
#include <vector>
// Simulate a game at `fps` against a strobe of 60 cycles/s, lowest-latency pick (newest finished frame).
static void Run(bool mode, double fps, double jitterMs, double maxAhead, double minRate, bool hitch = false)
{
    std::mt19937 rng(7);
    std::normal_distribution<double> nd(0.0, jitterMs / 1000.0);
    const double cyc = 1.0 / 60.0;
    std::vector<double> ft; double t = 0.0;
    for (int i = 0; i < 2000; i++) { double d = 1.0 / fps + nd(rng); if (hitch && i == 400) d += 0.12; t += d; ft.push_back(t); }
    WarpClock c;
    size_t k = 0; double prevD = 0, s1 = 0, s2 = 0, worst = 0, sumA = 0; int n = 0, back = 0, rep = 0; uint64_t lastId = 0;
    for (int j = 60; j < 1500; j++)
    {
        const double now = j * cyc;
        while (k + 1 < ft.size() && ft[k + 1] <= now) k++;
        const uint64_t id = k + 1;
        const double a = c.Cycle(mode, id, ft[k], now, cyc, maxAhead, minRate);
        // content position in seconds of game time: the frame's time plus the part moved ahead (in frame times)
        const double D = ft[k] + a / fps;
        if (j > 200 && !(hitch && j > 395 * 60 / fps && j < 395 * 60 / fps + 40))
        {
            const double st = (D - prevD) / cyc;          // 1.0 = perfectly even
            s1 += st; s2 += st * st; n++;
            if (std::fabs(st - 1.0) > worst) worst = std::fabs(st - 1.0);
            if (st < -1e-9) back++;
            if (id == lastId) rep++;
            sumA += a;
        }
        prevD = D; lastId = id;
    }
    const double m = s1 / n, sd = std::sqrt(s2 / n - m * m);
    printf("mode %d  %5.1f fps jitter %.1f ms%s: step mean %.3f sd %.3f worst dev %.2f  backward %d  repeats/s %.1f  avg ahead %.2f  rate %.3f\n",
           (int)mode, fps, jitterMs, hitch ? " +hitch" : "", m, sd, worst, back, rep * 60.0 / n, sumA / n, c.rate);
}
int main()
{
    for (double fps : { 54.0, 57.0, 48.0, 59.5, 60.0, 40.0, 30.0, 70.0 })
        for (int mode = 0; mode <= 1; mode++) Run(mode != 0, fps, 0.8, 1.0, 0.45);
    Run(true, 54, 0.8, 1.0, 0.75, true);
}
