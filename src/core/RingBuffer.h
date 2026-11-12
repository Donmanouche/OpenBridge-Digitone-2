// Lock-free single-producer / single-consumer ring of interleaved float frames.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace ob {

class FrameRing {
public:
    void configure(int channels, size_t capacityFrames)
    {
        channels_ = channels;
        capacity_ = capacityFrames + 1;
        data_.assign(capacity_ * (size_t)channels, 0.0f);
        reset();
    }

    // Not thread-safe: only call while neither side is running.
    void reset()
    {
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

    int channels() const { return channels_; }
    size_t capacity() const { return capacity_ - 1; }

    size_t readable() const
    {
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t r = read_.load(std::memory_order_acquire);
        return w >= r ? w - r : w + capacity_ - r;
    }
    size_t writable() const { return capacity() - readable(); }

    // Writes up to n frames, returns frames written.
    size_t write(const float* src, size_t n)
    {
        n = std::min(n, writable());
        size_t w = write_.load(std::memory_order_relaxed);
        for (size_t done = 0; done < n;) {
            const size_t chunk = std::min(n - done, capacity_ - w);
            std::memcpy(&data_[w * channels_], src + done * channels_, chunk * channels_ * sizeof(float));
            done += chunk;
            w = (w + chunk) % capacity_;
        }
        write_.store(w, std::memory_order_release);
        return n;
    }

    // Reads up to n frames (dst may be null to discard), returns frames read.
    size_t read(float* dst, size_t n)
    {
        n = std::min(n, readable());
        size_t r = read_.load(std::memory_order_relaxed);
        for (size_t done = 0; done < n;) {
            const size_t chunk = std::min(n - done, capacity_ - r);
            if (dst) std::memcpy(dst + done * channels_, &data_[r * channels_], chunk * channels_ * sizeof(float));
            done += chunk;
            r = (r + chunk) % capacity_;
        }
        read_.store(r, std::memory_order_release);
        return n;
    }

private:
    int channels_ = 0;
    size_t capacity_ = 1;
    std::vector<float> data_;
    std::atomic<size_t> write_{0};
    std::atomic<size_t> read_{0};
};

} // namespace ob
