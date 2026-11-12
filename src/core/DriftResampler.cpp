#include "DriftResampler.h"

#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ob {

namespace {
constexpr int kChunk = 64;          // frames handed to libsamplerate per callback
constexpr double kMaxCorr = 0.005;  // +-0.5 %
constexpr double kKp = 2.0e-6;      // per frame of error
constexpr double kKi = 4.0e-7;      // per frame*second of error
constexpr double kFillSmoothing = 0.02;
} // namespace

DriftResampler::~DriftResampler()
{
    if (src_) src_delete(src_);
}

void DriftResampler::prepare(Mode mode, int channels, double deviceRate, double hostRate, int maxHostBlock, int quality)
{
    if (src_) src_delete(src_);
    src_ = nullptr;
    mode_ = mode;
    channels_ = channels;
    deviceRate_ = deviceRate;
    hostRate_ = hostRate;
    quality_ = quality;
    const int type = quality <= 0 ? SRC_LINEAR : quality == 1 ? SRC_SINC_FASTEST : SRC_SINC_MEDIUM_QUALITY;
    int err = 0;
    src_ = src_new(type, channels, &err);
    const int maxDev = (int)std::ceil(maxHostBlock * deviceRate / hostRate) + 64;
    scratch_.assign((size_t)std::max(maxDev, kChunk) * 2 * (size_t)channels, 0.0f);
    pending_.assign((size_t)(maxDev * 2 + 1024) * (size_t)channels, 0.0f);
    pendingFrames_ = 0;
    reset();
}

void DriftResampler::reset()
{
    if (src_) src_reset(src_);
    fill_ = 0;
    integral_ = 0;
    corr_ = 1.0;
    primed_ = false;
    pendingFrames_ = 0;
}

int DriftResampler::latencyHostFrames() const
{
    // ring target (device frames) expressed in host frames + converter delay
    const double conv = quality_ <= 0 ? 1 : quality_ == 1 ? 32 : 64;
    return (int)std::lround((target_ + conv) * hostRate_ / deviceRate_);
}

void DriftResampler::updateControl(double fill, int hostFrames)
{
    const double dt = hostFrames / hostRate_;
    fill_ = fill_ == 0 ? fill : fill_ + kFillSmoothing * (fill - fill_);
    const double e = fill_ - target_;
    integral_ = std::clamp(integral_ + e * dt, -kMaxCorr / kKi, kMaxCorr / kKi);
    corr_ = 1.0 + std::clamp(kKp * e + kKi * integral_, -kMaxCorr, kMaxCorr);
}

bool DriftResampler::pull(FrameRing& ring, float* out, int n)
{
    const double fill = (double)ring.readable();
    if (!primed_) {
        if (fill < target_) {
            std::fill(out, out + (size_t)n * channels_, 0.0f);
            return true;
        }
        // drop the backlog accumulated before we started consuming
        if (fill > target_ * 2) ring.read(nullptr, (size_t)(fill - target_));
        src_reset(src_);
        pendingFrames_ = 0;
        primed_ = true;
        fill_ = 0;
        integral_ = 0;
    } else if (fill > target_ * 4 + 4096) {
        ring.read(nullptr, (size_t)(fill - target_)); // host stalled: resync
        fill_ = 0;
        integral_ = 0;
    }
    updateControl((double)ring.readable() + (double)pendingFrames_, n);
    // More device frames in the ring than wanted -> consume faster -> fewer output per input.
    const double ratio = hostRate_ / deviceRate_ / corr_;
    const size_t cap = pending_.size() / (size_t)channels_;
    long done = 0;
    // Bounded loop (no libsamplerate callback mode: it can spin forever when its input runs dry).
    for (int iter = 0; iter < 8 && done < n; ++iter) {
        const size_t want = (size_t)std::ceil((double)(n - done) / ratio) + 8;
        if (pendingFrames_ < want && pendingFrames_ < cap)
            pendingFrames_ += ring.read(pending_.data() + pendingFrames_ * (size_t)channels_,
                                        std::min(want - pendingFrames_, cap - pendingFrames_));
        if (pendingFrames_ == 0) break;
        SRC_DATA d{};
        d.data_in = pending_.data();
        d.input_frames = (long)pendingFrames_;
        d.data_out = out + done * channels_;
        d.output_frames = n - done;
        d.src_ratio = ratio;
        d.end_of_input = 0;
        if (src_process(src_, &d) != 0) break;
        if (d.input_frames_used > 0) {
            const size_t used = (size_t)d.input_frames_used;
            std::memmove(pending_.data(), pending_.data() + used * (size_t)channels_,
                         (pendingFrames_ - used) * (size_t)channels_ * sizeof(float));
            pendingFrames_ -= used;
        }
        done += d.output_frames_gen;
        if (d.input_frames_used == 0 && d.output_frames_gen == 0) break;
    }
    if (done < n) {
        std::fill(out + done * channels_, out + (size_t)n * channels_, 0.0f);
        primed_ = false; // underrun: re-prime
        return false;
    }
    return true;
}

bool DriftResampler::push(const float* in, int n, FrameRing& ring)
{
    updateControl((double)ring.readable(), n);
    // More frames waiting for the device than wanted -> produce fewer.
    const double ratio = deviceRate_ / hostRate_ / corr_;
    SRC_DATA d{};
    d.data_in = in;
    d.input_frames = n;
    d.src_ratio = ratio;
    d.end_of_input = 0;
    bool ok = true;
    const long cap = (long)(scratch_.size() / (size_t)channels_);
    while (d.input_frames > 0) {
        d.data_out = scratch_.data();
        d.output_frames = cap;
        if (src_process(src_, &d) != 0) return false;
        if (ring.write(scratch_.data(), (size_t)d.output_frames_gen) < (size_t)d.output_frames_gen) ok = false;
        d.data_in += d.input_frames_used * channels_;
        d.input_frames -= d.input_frames_used;
        if (d.input_frames_used == 0 && d.output_frames_gen == 0) break;
    }
    if (!primed_ && (double)ring.readable() >= target_) primed_ = true;
    return ok;
}

} // namespace ob
