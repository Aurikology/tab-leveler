// Native test suite for the DSP core. No framework: each check prints and counts failures.
// Loudness cases 1-5 are EBU Tech 3341 (2016) "minimum requirements" signals, synthesized from
// their definitions (stereo 1 kHz sine, both channels at the stated dBFS peak level).
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "leveler.hpp"

namespace {
int g_fail = 0, g_pass = 0;

void check(bool ok, const std::string& what) {
    (ok ? g_pass : g_fail)++;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
}
void near(double got, double want, double tol, const std::string& what) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s: got %.4f, want %.4f +/- %.4g", what.c_str(), got, want, tol);
    check(std::fabs(got - want) <= tol, buf);
}

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kBlock = 128;  // an AudioWorklet render quantum

using Gen = std::function<double(std::size_t frame)>;  // mono sample source, copied to both channels

// Feeds `seconds` of a generator through `fn(ptrs, n)` in 128-frame stereo blocks.
template <class Fn>
void stream(double rate, double seconds, const Gen& gen, Fn fn, std::size_t& t) {
    std::vector<float> l(kBlock), r(kBlock);
    float* ptrs[2] = {l.data(), r.data()};
    auto total = static_cast<std::size_t>(std::lround(seconds * rate));
    while (total > 0) {
        const std::size_t n = std::min(total, kBlock);
        for (std::size_t i = 0; i < n; ++i) l[i] = r[i] = static_cast<float>(gen(t++));
        fn(ptrs, n);
        total -= n;
    }
}

Gen sine(double rate, double dbfs, double hz = 1000.0) {
    const double a = std::pow(10.0, dbfs / 20.0);
    return [=](std::size_t t) { return a * std::sin(2.0 * kPi * hz * static_cast<double>(t) / rate); };
}

double meterSegments(double rate, const std::vector<std::pair<double, double>>& segs, double* mom = nullptr,
                     double* st = nullptr) {
    tl::LoudnessMeter m(rate, 2);
    std::size_t t = 0;
    for (auto [dbfs, sec] : segs)
        stream(rate, sec, sine(rate, dbfs), [&](float* const* p, std::size_t n) { m.process(p, n); }, t);
    if (mom) *mom = m.momentary();
    if (st) *st = m.shortTerm();
    return m.integrated();
}

void ebuCases(double rate) {
    const std::string at = " @" + std::to_string(static_cast<int>(rate)) + " Hz";
    double mo, st;
    double i = meterSegments(rate, {{-23, 20}}, &mo, &st);
    near(i, -23.0, 0.1, "EBU 3341 case 1 integrated" + at);
    near(mo, -23.0, 0.1, "EBU 3341 case 1 momentary" + at);
    near(st, -23.0, 0.1, "EBU 3341 case 1 short-term" + at);
    i = meterSegments(rate, {{-33, 20}}, &mo, &st);
    near(i, -33.0, 0.1, "EBU 3341 case 2 integrated" + at);
    near(mo, -33.0, 0.1, "EBU 3341 case 2 momentary" + at);
    near(st, -33.0, 0.1, "EBU 3341 case 2 short-term" + at);
    near(meterSegments(rate, {{-36, 10}, {-23, 60}, {-36, 10}}), -23.0, 0.1, "EBU 3341 case 3 (relative gate)" + at);
    near(meterSegments(rate, {{-72, 10}, {-36, 10}, {-23, 60}, {-36, 10}, {-72, 10}}), -23.0, 0.1,
         "EBU 3341 case 4 (absolute + relative gate)" + at);
    near(meterSegments(rate, {{-26, 20}, {-20, 20.1}, {-26, 20}}), -23.0, 0.1, "EBU 3341 case 5" + at);
}

void kWeightingCoefficients() {
    // BS.1770-4 table 1 and 2 (48 kHz)
    tl::KWeighting k(48000.0);
    near(k.shelf.b0, 1.53512485958697, 1e-6, "K-weighting shelf b0 matches BS.1770-4");
    near(k.shelf.b1, -2.69169618940638, 1e-6, "K-weighting shelf b1 matches BS.1770-4");
    near(k.shelf.b2, 1.19839281085285, 1e-6, "K-weighting shelf b2 matches BS.1770-4");
    near(k.shelf.a1, -1.69065929318241, 1e-6, "K-weighting shelf a1 matches BS.1770-4");
    near(k.shelf.a2, 0.73248077421585, 1e-6, "K-weighting shelf a2 matches BS.1770-4");
    near(k.highpass.a1, -1.99004745483398, 1e-6, "K-weighting high-pass a1 matches BS.1770-4");
    near(k.highpass.a2, 0.99007225036621, 1e-6, "K-weighting high-pass a2 matches BS.1770-4");
}

// Runs a leveler over segments of generators; tracks the output meter and the output peak.
struct Run {
    double outShortTerm, outMomentary, peak, gainDb;
};
Run level(double rate, const std::vector<std::pair<Gen, double>>& segs, tl::LevelerSettings s = {}) {
    tl::Leveler lev(rate, 2, s);
    tl::LoudnessMeter out(rate, 2, false);
    double peak = 0;
    std::size_t t = 0;
    for (auto& [gen, sec] : segs)
        stream(rate, sec, gen, [&](float* const* p, std::size_t n) {
            lev.process(p, n);
            for (std::size_t i = 0; i < n; ++i) peak = std::max({peak, (double)std::fabs(p[0][i]), (double)std::fabs(p[1][i])});
            out.process(p, n);
        }, t);
    return {out.shortTerm(), out.momentary(), peak, lev.gainDb()};
}

void levelerBehaviour() {
    const double R = 48000.0;
    const double ceiling = std::pow(10.0, -1.0 / 20.0);

    auto quiet = level(R, {{sine(R, -30), 20}});
    near(quiet.outShortTerm, -18.0, 0.5, "quiet tab (-30 LUFS) is lifted to the -18 LUFS target");

    auto veryQuiet = level(R, {{sine(R, -40), 20}});
    near(veryQuiet.outShortTerm, -28.0, 0.5, "boost is capped at +12 dB (-40 LUFS ends at -28)");

    auto loud = level(R, {{sine(R, -6), 20}});
    near(loud.outShortTerm, -18.0, 0.5, "loud tab (-6 LUFS) is cut to the target");
    check(loud.peak <= ceiling + 1e-6, "loud tab output never exceeds the -1 dBFS ceiling");

    // An ad: a quiet show, then a sudden blast 20 dB louder.
    auto ad = level(R, {{sine(R, -30), 10}, {sine(R, -10), 1.5}});
    near(ad.outMomentary, -18.0, 1.5, "sudden +20 dB jump is back near target within 1.5 s");
    check(ad.peak <= ceiling + 1e-6, "sudden jump: output never exceeds the ceiling, even before the gain falls");

    // Full-scale noise burst after silence: the limiter alone has to catch it.
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    Gen noise = [&](std::size_t) { return u(rng); };
    tl::LevelerSettings hot;
    hot.maxBoostDb = 0;  // leveler cannot help: only the limiter stands between noise and clipping
    auto burst = level(R, {{[](std::size_t) { return 0.0; }, 1}, {noise, 2}}, hot);
    char buf[128];
    std::snprintf(buf, sizeof buf, "full-scale noise burst: peak %.4f <= ceiling %.4f", burst.peak, ceiling);
    check(burst.peak <= ceiling + 1e-6, buf);

    // Room noise at -60 dBFS: below the gate, so it must not be pumped up.
    std::normal_distribution<double> gauss(0.0, std::pow(10.0, -60.0 / 20.0));
    Gen hiss = [&](std::size_t) { return gauss(rng); };
    auto floor = level(R, {{hiss, 20}});
    near(floor.gainDb, 0.0, 1e-9, "noise floor below the -50 LUFS gate is never boosted");

    tl::Leveler lat(R, 2);
    check(lat.latencyFrames() == 240, "latency is the 5 ms look-ahead: 240 frames at 48 kHz");
}
}  // namespace

int main() {
    kWeightingCoefficients();
    ebuCases(48000.0);
    ebuCases(44100.0);
    levelerBehaviour();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
