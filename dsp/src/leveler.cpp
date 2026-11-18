#include "leveler.hpp"

#include <algorithm>
#include <cmath>

namespace tl {

namespace {
constexpr double kPi = 3.14159265358979323846;
double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
}  // namespace

// Coefficients from BS.1770-4 annex 1, re-derived for any rate via the bilinear transform
// (filter parameters as published with libebur128). At 48 kHz they match the table in the spec.
KWeighting::KWeighting(double rate) {
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / rate);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf.b1 = 2.0 * (K * K - Vh) / a0;
        shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / rate);
        const double a0 = 1.0 + K / Q + K * K;
        highpass.b0 = 1.0;
        highpass.b1 = -2.0;
        highpass.b2 = 1.0;
        highpass.a1 = 2.0 * (K * K - 1.0) / a0;
        highpass.a2 = (1.0 - K / Q + K * K) / a0;
    }
}

double energyToLufs(double ms) { return ms <= 0.0 ? kSilenceLufs : -0.691 + 10.0 * std::log10(ms); }

// ------------------------------------------------------------------------------------ meter

LoudnessMeter::LoudnessMeter(double rate, int channels, bool keepHistory)
    : rate_(rate),
      channels_(std::clamp(channels, 1, 2)),
      keepHistory_(keepHistory),
      subBlockLen_(static_cast<std::size_t>(std::lround(rate / 10.0))),
      filters_{KWeighting(rate), KWeighting(rate)} {}

void LoudnessMeter::process(const float* const* ch, std::size_t frames) {
    for (std::size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels_; ++c) {
            const double y = filters_[c].process(ch[c][i]);
            acc_ += y * y;
        }
        if (++accFrames_ == subBlockLen_) {
            subBlocks_[subCount_ % subBlocks_.size()] = acc_ / static_cast<double>(subBlockLen_);
            ++subCount_;
            acc_ = 0;
            accFrames_ = 0;
            ready_ = true;
            if (keepHistory_ && subCount_ >= 4) blockEnergies_.push_back(energyOver(4));
        }
    }
}

double LoudnessMeter::energyOver(std::size_t n) const {
    if (subCount_ < n) return 0.0;  // not enough audio yet
    double sum = 0;
    for (std::size_t k = 0; k < n; ++k) sum += subBlocks_[(subCount_ - 1 - k) % subBlocks_.size()];
    return sum / static_cast<double>(n);
}

double LoudnessMeter::momentary() const { return energyToLufs(energyOver(4)); }
double LoudnessMeter::shortTerm() const { return energyToLufs(energyOver(30)); }

double LoudnessMeter::integrated() const {
    double sum = 0;
    std::size_t n = 0;
    for (double e : blockEnergies_)
        if (energyToLufs(e) > -70.0) { sum += e; ++n; }
    if (n == 0) return kSilenceLufs;
    const double relGate = energyToLufs(sum / static_cast<double>(n)) - 10.0;
    sum = 0;
    n = 0;
    for (double e : blockEnergies_) {
        const double l = energyToLufs(e);
        if (l > -70.0 && l > relGate) { sum += e; ++n; }
    }
    return n == 0 ? kSilenceLufs : energyToLufs(sum / static_cast<double>(n));
}

bool LoudnessMeter::subBlockReady() {
    const bool r = ready_;
    ready_ = false;
    return r;
}

void LoudnessMeter::reset() {
    for (auto& f : filters_) f.reset();
    acc_ = 0;
    accFrames_ = 0;
    subBlocks_.fill(0);
    subCount_ = 0;
    blockEnergies_.clear();
    ready_ = false;
}

// ------------------------------------------------------------------------------------ leveler

Leveler::Leveler(double rate, int channels, LevelerSettings s)
    : rate_(rate),
      channels_(std::clamp(channels, 1, 2)),
      s_(s),
      meter_(rate, channels, /*keepIntegratedHistory=*/false),
      attackCoef_(std::exp(-1.0 / (s.attackSec * rate))),
      releaseCoef_(std::exp(-1.0 / (s.releaseSec * rate))),
      lookahead_(std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(s.lookaheadMs * rate / 1000.0)))),
      delay_(channels_, std::vector<float>(lookahead_ + 1, 0.0f)),
      qVal_(lookahead_ + 2, 1.0),
      qIdx_(lookahead_ + 2, 0),
      boxRing_(lookahead_, 1.0),
      boxSum_(static_cast<double>(lookahead_)),
      limReleaseStep_(1.0 / (s.limiterReleaseMs * rate / 1000.0)),
      ceiling_(dbToLin(s.ceilingDbfs)) {}

void Leveler::updateTarget() {
    const double st = meter_.shortTerm();
    const double mo = meter_.momentary();
    if (st <= s_.gateLufs && mo <= s_.gateLufs) return;  // silence or noise floor: hold the gain
    // A sudden jump (an ad, a jump scare) is followed on the 400 ms reading until the 3 s reading
    // has caught up; otherwise the 3 s reading drives the gain so speech does not make it pump.
    // Without the latch the leveler hands back to the 3 s reading half-way up and stalls loud.
    if (mo > st + s_.jumpLu) jumping_ = true;
    else if (mo <= st + 1.0) jumping_ = false;
    const double measured = jumping_ ? mo : std::max(st, s_.gateLufs);
    targetDb_ = std::clamp(s_.targetLufs - measured, -s_.maxCutDb, s_.maxBoostDb);
}

double Leveler::limiterStep(double req) {
    const std::size_t cap = qVal_.size();
    // push back, keeping the queue increasing so the front is the window minimum
    while (qSize_ > 0 && qVal_[(qHead_ + qSize_ - 1) % cap] >= req) --qSize_;
    qVal_[(qHead_ + qSize_) % cap] = req;
    qIdx_[(qHead_ + qSize_) % cap] = frameIdx_;
    ++qSize_;
    // drop entries older than L frames (window is frames t-L .. t)
    while (frameIdx_ - qIdx_[qHead_] > lookahead_) { qHead_ = (qHead_ + 1) % cap; --qSize_; }
    const double m = qVal_[qHead_];
    released_ = std::min(m, released_ + limReleaseStep_);
    boxSum_ += released_ - boxRing_[boxPos_];
    boxRing_[boxPos_] = released_;
    boxPos_ = (boxPos_ + 1) % lookahead_;
    ++frameIdx_;
    return boxSum_ / static_cast<double>(lookahead_);
}

void Leveler::process(float* const* ch, std::size_t frames) {
    meter_.process(ch, frames);  // measure the input as it arrives
    // The meter only changes its reading per 100 ms sub-block; a block of up to a few hundred
    // frames sees at most one update, so updating the target once per call is enough.
    if (meter_.subBlockReady()) updateTarget();

    const std::size_t ring = lookahead_ + 1;
    for (std::size_t i = 0; i < frames; ++i) {
        const double coef = targetDb_ < gainDb_ ? attackCoef_ : releaseCoef_;
        gainDb_ = targetDb_ + (gainDb_ - targetDb_) * coef;
        const double g = dbToLin(gainDb_);

        double peak = 0;
        for (int c = 0; c < channels_; ++c) {
            const float x = static_cast<float>(ch[c][i] * g);
            delay_[c][writePos_] = x;
            peak = std::max(peak, static_cast<double>(std::fabs(x)));
        }
        const double req = peak > ceiling_ ? ceiling_ / peak : 1.0;
        limGain_ = limiterStep(req);

        const std::size_t readPos = (writePos_ + 1) % ring;  // the frame from L frames ago
        for (int c = 0; c < channels_; ++c) {
            double y = delay_[c][readPos] * limGain_;
            y = std::clamp(y, -ceiling_, ceiling_);  // float rounding guard only
            ch[c][i] = static_cast<float>(y);
        }
        writePos_ = readPos;
    }
}

double Leveler::limiterGainDb() const { return 20.0 * std::log10(std::max(limGain_, 1e-9)); }

void Leveler::reset() {
    meter_.reset();
    gainDb_ = targetDb_ = 0;
    jumping_ = false;
    for (auto& d : delay_) std::fill(d.begin(), d.end(), 0.0f);
    writePos_ = 0;
    qHead_ = qSize_ = 0;
    std::fill(boxRing_.begin(), boxRing_.end(), 1.0);
    boxPos_ = 0;
    boxSum_ = static_cast<double>(lookahead_);
    frameIdx_ = 0;
    released_ = limGain_ = 1.0;
}

}  // namespace tl
