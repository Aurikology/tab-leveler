// Flat C interface for the WebAssembly build. The AudioWorklet owns one Leveler per tab and passes
// planar stereo buffers that live in WASM memory (tl_buffer), so no copies cross the boundary.
#include <cstdlib>

#include "leveler.hpp"

namespace {
struct Handle {
    tl::Leveler lev;
    float* buf[2];
    std::size_t cap;
};
}  // namespace

extern "C" {

// Returns an opaque handle, or 0 on failure. maxFrames is the largest block process() will see.
void* tl_create(double sampleRate, int channels, int maxFrames, double targetLufs) {
    tl::LevelerSettings s;
    s.targetLufs = targetLufs;
    auto* h = new Handle{tl::Leveler(sampleRate, channels, s), {nullptr, nullptr}, static_cast<std::size_t>(maxFrames)};
    h->buf[0] = static_cast<float*>(std::calloc(h->cap, sizeof(float)));
    h->buf[1] = static_cast<float*>(std::calloc(h->cap, sizeof(float)));
    return h;
}

void tl_destroy(void* p) {
    auto* h = static_cast<Handle*>(p);
    if (!h) return;
    std::free(h->buf[0]);
    std::free(h->buf[1]);
    delete h;
}

// Pointer to channel c's input/output buffer inside WASM memory.
float* tl_buffer(void* p, int c) { return static_cast<Handle*>(p)->buf[c ? 1 : 0]; }

void tl_process(void* p, int frames) {
    auto* h = static_cast<Handle*>(p);
    if (frames > 0 && static_cast<std::size_t>(frames) <= h->cap) h->lev.process(h->buf, static_cast<std::size_t>(frames));
}

void tl_set_target(void* p, double lufs) { static_cast<Handle*>(p)->lev.setTarget(lufs); }
double tl_gain_db(void* p) { return static_cast<Handle*>(p)->lev.gainDb(); }
double tl_limiter_db(void* p) { return static_cast<Handle*>(p)->lev.limiterGainDb(); }
double tl_input_momentary(void* p) { return static_cast<Handle*>(p)->lev.inputMomentary(); }
double tl_input_short_term(void* p) { return static_cast<Handle*>(p)->lev.inputShortTerm(); }
int tl_latency_frames(void* p) { return static_cast<int>(static_cast<Handle*>(p)->lev.latencyFrames()); }
void tl_reset(void* p) { static_cast<Handle*>(p)->lev.reset(); }

}  // extern "C"
