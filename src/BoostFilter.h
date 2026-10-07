#pragma once

#include "IAudioFilter.h"

#include <atomic>
#include <cmath>

namespace mrefiner {

// tanh soft-clip boost. Louder average level without harsh digital clipping.
class BoostFilter : public IAudioFilter {
public:
    const char* name() const override { return "Boost / Saturation"; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool e) override { enabled_.store(e, std::memory_order_relaxed); }
    void initialize(int, int) override {}

    void process(float* samples, int frames, int channels) override {
        const float db = driveDb_.load(std::memory_order_relaxed);
        if (std::abs(db) < 0.05f) return;
        const float drive = std::pow(10.0f, db / 20.0f);
        const int total = frames * channels;
        for (int i = 0; i < total; ++i)
            samples[i] = std::tanh(samples[i] * drive);
    }

    void setDriveDb(float db) { driveDb_.store(db, std::memory_order_relaxed); }
    float driveDb() const { return driveDb_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> enabled_{true};
    std::atomic<float> driveDb_{0.0f};
};

} // namespace mrefiner
