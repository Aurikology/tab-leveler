// Tab Leveler DSP core: an ITU-R BS.1770-4 / EBU R128 loudness meter, a loudness leveler and a
// look-ahead peak limiter. Plain C++17, no dependencies, no allocation in the audio path after
// construction (except LoudnessMeter's integrated-loudness history, which is opt-in).
#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace tl {

// Direct form II transposed biquad, double precision state.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double process(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

// The BS.1770 K-weighting filter (high-shelf "pre-filter" then RLB high-pass) for any sample rate.
struct KWeighting {
    Biquad shelf, highpass;
    explicit KWeighting(double sampleRate = 48000.0);
    double process(double x) { return highpass.process(shelf.process(x)); }
    void reset() { shelf.reset(); highpass.reset(); }
};

constexpr double kSilenceLufs = -200.0;  // reported when there is no energy at all

// Loudness meter per BS.1770-4: 100 ms sub-blocks, 400 ms momentary, 3 s short-term,
// integrated loudness with the -70 LUFS absolute gate and the -10 LU relative gate.
// Up to 2 channels (L, R), both with weight 1.0.
class LoudnessMeter {
public:
    LoudnessMeter(double sampleRate, int channels, bool keepIntegratedHistory = true);

    // Planar input: ch[c][i] for i < frames.
    void process(const float* const* ch, std::size_t frames);

    double momentary() const;   // LUFS over the last 400 ms
    double shortTerm() const;   // LUFS over the last 3 s
    double integrated() const;  // gated LUFS since reset()
    bool subBlockReady();       // true once per completed 100 ms sub-block (clears the flag)
    void reset();

private:
    double energyOver(std::size_t subBlocks) const;

    double rate_;
    int channels_;
    bool keepHistory_;
    std::size_t subBlockLen_;
    std::array<KWeighting, 2> filters_;
    double acc_ = 0;            // sum over channels of squared K-weighted samples, current sub-block
    std::size_t accFrames_ = 0;
    std::array<double, 30> subBlocks_{};  // mean-square energy per finished sub-block (ring)
    std::size_t subCount_ = 0;            // total sub-blocks finished
    std::vector<double> blockEnergies_;   // 400 ms gating blocks, 75 % overlap
    bool ready_ = false;
};

double energyToLufs(double meanSquare);

struct LevelerSettings {
    double targetLufs = -18.0;     // where every tab ends up
    double maxBoostDb = 12.0;      // never lift quiet content more than this
    double maxCutDb = 24.0;        // never cut more than this
    double gateLufs = -50.0;       // below this the gain is held: silence and room noise are not lifted
    double attackSec = 0.3;        // how fast gain falls when content gets louder
    double releaseSec = 2.0;       // how fast gain rises when content gets quieter
    double jumpLu = 6.0;           // momentary above short-term by this much = sudden jump, react at once
    double ceilingDbfs = -1.0;     // limiter ceiling
    double lookaheadMs = 5.0;      // limiter look-ahead, which is also the added latency
    double limiterReleaseMs = 80.0;
};

// Loudness leveler followed by a look-ahead limiter. Stereo or mono, processes in place.
class Leveler {
public:
    Leveler(double sampleRate, int channels, LevelerSettings s = {});

    void process(float* const* ch, std::size_t frames);

    double gainDb() const { return gainDb_; }          // current smoothed leveler gain
    double targetGainDb() const { return targetDb_; }  // where the gain is heading
    double inputMomentary() const { return meter_.momentary(); }
    double inputShortTerm() const { return meter_.shortTerm(); }
    double limiterGainDb() const;                      // current limiter reduction (<= 0)
    std::size_t latencyFrames() const { return lookahead_; }
    void setTarget(double lufs) { s_.targetLufs = lufs; }
    void reset();

private:
    void updateTarget();

    double rate_;
    int channels_;
    LevelerSettings s_;
    LoudnessMeter meter_;
    double gainDb_ = 0, targetDb_ = 0;
    bool jumping_ = false;
    double attackCoef_, releaseCoef_;

    // Look-ahead limiter. For each frame t: m[t] = min required gain over the last L+1 frames
    // (sliding minimum), r[t] = min(m[t], r[t-1] + release), and the gain applied to the frame
    // delayed by L is the mean of r over the last L frames. Every r in that mean already covers
    // the delayed frame's own peak, so the output never exceeds the ceiling, and the gain ramps
    // down linearly instead of stepping (no clicks).
    double limiterStep(double required);

    std::size_t lookahead_;                  // L, in frames (>= 1)
    std::vector<std::vector<float>> delay_;  // per channel ring of L+1 samples
    std::size_t writePos_ = 0;
    std::vector<double> qVal_;               // monotonic queue (ring) for the sliding minimum
    std::vector<std::size_t> qIdx_;
    std::size_t qHead_ = 0, qSize_ = 0;
    std::vector<double> boxRing_;            // last L values of r
    std::size_t boxPos_ = 0;
    double boxSum_ = 0;
    std::size_t frameIdx_ = 0;
    double released_ = 1.0, limReleaseStep_, limGain_ = 1.0;
    double ceiling_;
};

}  // namespace tl
