// Clock-domain bridge between the device (48 kHz, device crystal) and the host (any rate,
// sound card crystal). A libsamplerate converter whose ratio is trimmed by a PI controller
// that keeps the ring buffer between both domains at a target fill level.
#pragma once

#include "RingBuffer.h"

#include <vector>

typedef struct SRC_STATE_tag SRC_STATE;

namespace ob {

class DriftResampler {
public:
    enum class Mode { Pull, Push };
    // Pull: device ring -> host frames (device outputs into the DAW).
    // Push: host frames -> device ring (DAW into the device inputs).

    DriftResampler() = default;
    ~DriftResampler();
    DriftResampler(const DriftResampler&) = delete;
    DriftResampler& operator=(const DriftResampler&) = delete;

    // quality: 0 = linear, 1 = sinc fastest, 2 = sinc medium
    void prepare(Mode mode, int channels, double deviceRate, double hostRate, int maxHostBlock, int quality);
    void reset();

    void setTargetFill(double frames) { target_ = frames; }
    double targetFill() const { return target_; }

    // Pull mode: produce n interleaved host frames from ring. Returns false on underrun (zeros written).
    bool pull(FrameRing& ring, float* out, int n);
    // Push mode: consume n interleaved host frames into ring. Returns false on overflow.
    bool push(const float* in, int n, FrameRing& ring);

    double ratioCorrection() const { return corr_; }
    double filteredFill() const { return fill_; }
    bool primed() const { return primed_; }
    int latencyHostFrames() const;

private:
    void updateControl(double fill, int hostFrames);

    Mode mode_ = Mode::Pull;
    int channels_ = 0;
    double deviceRate_ = 48000, hostRate_ = 48000;
    SRC_STATE* src_ = nullptr;
    int quality_ = 1;
    std::vector<float> scratch_;
    std::vector<float> pending_; // pull mode: device frames read from the ring, not yet converted
    size_t pendingFrames_ = 0;

    double target_ = 512;
    double fill_ = 0;
    double integral_ = 0;
    double corr_ = 1.0;
    bool primed_ = false;
};

} // namespace ob
