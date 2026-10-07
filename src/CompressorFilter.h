#pragma once

#include "IAudioFilter.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace mrefiner {

// Downward compressor: above the threshold, signal is attenuated so the output
// approaches the threshold (gain = (level/threshold)^(1/ratio - 1)).
// Peak detection.
class CompressorFilter : public IAudioFilter {
public:
    const char* name() const override { return "Compressor"; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool e) override { enabled_.store(e, std::memory_order_relaxed); }

    void initialize(int sampleRate, int) override {
        sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
        envGain_ = 1.0f;
    }

    void process(float* samples, int frames, int channels) override {
        const float threshold = std::pow(10.0f, thresholdDb_.load(std::memory_order_relaxed) / 20.0f);
        const float ratio = std::max(1.0f, ratio_.load(std::memory_order_relaxed));
        const float invRatioMinus1 = 1.0f / ratio - 1.0f;  // negative for ratio > 1
        const float outGain = std::pow(10.0f, outputGainDb_.load(std::memory_order_relaxed) / 20.0f);
        const float attackCoef = coefFromMs(attackMs_.load(std::memory_order_relaxed), sampleRate_);
        const float releaseCoef = coefFromMs(releaseMs_.load(std::memory_order_relaxed), sampleRate_);

        for (int f = 0; f < frames; ++f) {
            float level = 0.0f;
            for (int c = 0; c < channels; ++c) {
                float ax = std::abs(samples[f * channels + c]);
                if (ax > level) level = ax;
            }

            float targetGain = 1.0f;
            if (level > threshold) {
                targetGain = std::pow(level / threshold, invRatioMinus1);
            }

            float coef = targetGain < envGain_ ? attackCoef : releaseCoef;
            envGain_ += (targetGain - envGain_) * coef;

            float finalGain = envGain_ * outGain;
            for (int c = 0; c < channels; ++c)
                samples[f * channels + c] *= finalGain;
        }
    }

    void setRatio(float r)         { ratio_.store(std::max(1.0f, r), std::memory_order_relaxed); }
    void setThresholdDb(float db)  { thresholdDb_.store(db, std::memory_order_relaxed); }
    void setAttackMs(float ms)     { attackMs_.store(std::max(0.1f, ms), std::memory_order_relaxed); }
    void setReleaseMs(float ms)    { releaseMs_.store(std::max(1.0f, ms), std::memory_order_relaxed); }
    void setOutputGainDb(float db) { outputGainDb_.store(db, std::memory_order_relaxed); }

    float ratio() const        { return ratio_.load(std::memory_order_relaxed); }
    float thresholdDb() const  { return thresholdDb_.load(std::memory_order_relaxed); }
    float attackMs() const     { return attackMs_.load(std::memory_order_relaxed); }
    float releaseMs() const    { return releaseMs_.load(std::memory_order_relaxed); }
    float outputGainDb() const { return outputGainDb_.load(std::memory_order_relaxed); }

private:
    static float coefFromMs(float ms, int sampleRate) {
        if (ms <= 0.001f) return 1.0f;
        return 1.0f - std::exp(-1.0f / (ms * 0.001f * sampleRate));
    }

    std::atomic<bool>  enabled_{true};
    std::atomic<float> ratio_{4.0f};
    std::atomic<float> thresholdDb_{-18.0f};
    std::atomic<float> attackMs_{6.0f};
    std::atomic<float> releaseMs_{60.0f};
    std::atomic<float> outputGainDb_{0.0f};

    int sampleRate_ = 48000;
    float envGain_ = 1.0f;
};

} // namespace mrefiner
