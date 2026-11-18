// Writes the leveler's output for a fixed test programme as raw float32 (L then R per block), so
// tests/parity.mjs can run the same programme through the WASM build and compare sample by sample.
// The programme: 4 s quiet speech-band tone, 3 s loud burst (ad), 3 s noise, all deterministic.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "leveler.hpp"

// Same generator as parity.mjs: integer LCG so both sides get identical noise.
static float sample(std::uint32_t t, std::uint32_t& seed, double rate) {
    const double pi = 3.14159265358979323846;
    const double sec = t / rate;
    if (sec < 4.0) return static_cast<float>(0.03 * std::sin(2 * pi * 440.0 * t / rate));
    if (sec < 7.0) return static_cast<float>(0.7 * std::sin(2 * pi * 1000.0 * t / rate));
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>((static_cast<double>(seed >> 8) / 16777216.0 * 2.0 - 1.0) * 0.5);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const double rate = 48000.0;
    const std::size_t total = static_cast<std::size_t>(10 * rate), block = 128;
    tl::Leveler lev(rate, 2);
    std::vector<float> l(block), r(block);
    float* p[2] = {l.data(), r.data()};
    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::uint32_t seed = 12345, t = 0;
    for (std::size_t done = 0; done < total; done += block) {
        for (std::size_t i = 0; i < block; ++i) {
            const float s = sample(t++, seed, rate);
            l[i] = s;
            r[i] = -s;
        }
        lev.process(p, block);
        std::fwrite(l.data(), sizeof(float), block, f);
        std::fwrite(r.data(), sizeof(float), block, f);
    }
    std::fclose(f);
    return 0;
}
