#pragma once

#include "IAudioFilter.h"

#include <atomic>
#include <cmath>

namespace mrefiner {

class NoiseGateFilter : public IAudioFilter {
public:
    const char* name() const override { return "Noise Gate"; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool e) override { enabled_.store(e, std::memory_order_relaxed); }

    void initialize(int, int) override { envelope_ = 0.0f; }

    void process(float* samples, int frames, int channels) override {
        const float thresholdDb = thresholdDb_.load(std::memory_order_relaxed);
        const float threshold = std::pow(10.0f, thresholdDb / 20.0f);

        for (int f = 0; f < frames; ++f) {
            float peak = 0.0f;
            const int base = f * channels;
            for (int ch = 0; ch < channels; ++ch) {
                const float abs = std::abs(samples[base + ch]);
                if (abs > peak) peak = abs;
            }
            const float target = peak >= threshold ? 1.0f : 0.0f;
            const float coef = target > envelope_ ? kAttack : kRelease;
            envelope_ += (target - envelope_) * coef;
            for (int ch = 0; ch < channels; ++ch)
                samples[base + ch] *= envelope_;
        }
    }

    void setThresholdDb(float db) { thresholdDb_.store(db, std::memory_order_relaxed); }
    float thresholdDb() const { return thresholdDb_.load(std::memory_order_relaxed); }

private:
    static constexpr float kAttack = 0.4f;
    static constexpr float kRelease = 0.02f;

    std::atomic<bool> enabled_{true};
    std::atomic<float> thresholdDb_{-50.0f};
    float envelope_{0.0f};
};

} // namespace mrefiner
