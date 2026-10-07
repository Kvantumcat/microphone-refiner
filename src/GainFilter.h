#pragma once

#include "IAudioFilter.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace mrefiner {

class GainFilter : public IAudioFilter {
public:
    const char* name() const override { return "Gain"; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool e) override { enabled_.store(e, std::memory_order_relaxed); }
    void initialize(int, int) override {}

    void process(float* samples, int frames, int channels) override {
        const float db = gainDb_.load(std::memory_order_relaxed);
        if (std::abs(db) < 1e-6f) return;
        const float g = std::pow(10.0f, db / 20.0f);
        const int total = frames * channels;
        for (int i = 0; i < total; ++i) {
            float v = samples[i] * g;
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            samples[i] = v;
        }
    }

    void setGainDb(float db) { gainDb_.store(db, std::memory_order_relaxed); }
    float gainDb() const { return gainDb_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> enabled_{true};
    std::atomic<float> gainDb_{0.0f};
};

} // namespace mrefiner
