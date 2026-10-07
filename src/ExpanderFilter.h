#pragma once

#include "IAudioFilter.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace mrefiner {

// Downward expander: below the threshold, signal is attenuated more aggressively
// than unity (gain = (level/threshold)^(ratio-1)). Above threshold, bypass.
// Detection: Peak (max |x|) or RMS (sqrt(mean(x^2))) across channels.
class ExpanderFilter : public IAudioFilter {
public:
    enum DetectionMode { DetectionPeak = 0, DetectionRMS = 1 };

    const char* name() const override { return "Expander"; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool e) override { enabled_.store(e, std::memory_order_relaxed); }

    void initialize(int sampleRate, int) override {
        sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
        envGain_ = 1.0f;
    }

    void process(float* samples, int frames, int channels) override {
        const float threshold = std::pow(10.0f, thresholdDb_.load(std::memory_order_relaxed) / 20.0f);
        const float ratio = std::max(1.0f, ratio_.load(std::memory_order_relaxed));
        const float outGain = std::pow(10.0f, outputGainDb_.load(std::memory_order_relaxed) / 20.0f);
        const int mode = detectionMode_.load(std::memory_order_relaxed);
        const float attackCoef = coefFromMs(attackMs_.load(std::memory_order_relaxed), sampleRate_);
        const float releaseCoef = coefFromMs(releaseMs_.load(std::memory_order_relaxed), sampleRate_);

        for (int f = 0; f < frames; ++f) {
            float level = 0.0f;
            if (mode == DetectionRMS) {
                for (int c = 0; c < channels; ++c) {
                    float x = samples[f * channels + c];
                    level += x * x;
                }
                level = std::sqrt(level / channels);
            } else {
                for (int c = 0; c < channels; ++c) {
                    float ax = std::abs(samples[f * channels + c]);
                    if (ax > level) level = ax;
                }
            }

            float targetGain = 1.0f;
            if (level < threshold && level > 1e-9f) {
                targetGain = std::pow(level / threshold, ratio - 1.0f);
            } else if (level <= 1e-9f) {
                targetGain = std::pow(1e-9f / threshold, ratio - 1.0f);
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
    void setDetectionMode(int m)   { detectionMode_.store(m ? 1 : 0, std::memory_order_relaxed); }

    float ratio() const         { return ratio_.load(std::memory_order_relaxed); }
    float thresholdDb() const   { return thresholdDb_.load(std::memory_order_relaxed); }
    float attackMs() const      { return attackMs_.load(std::memory_order_relaxed); }
    float releaseMs() const     { return releaseMs_.load(std::memory_order_relaxed); }
    float outputGainDb() const  { return outputGainDb_.load(std::memory_order_relaxed); }
    int   detectionMode() const { return detectionMode_.load(std::memory_order_relaxed); }

private:
    static float coefFromMs(float ms, int sampleRate) {
        if (ms <= 0.001f) return 1.0f;
        return 1.0f - std::exp(-1.0f / (ms * 0.001f * sampleRate));
    }

    std::atomic<bool>  enabled_{true};
    std::atomic<float> ratio_{2.0f};
    std::atomic<float> thresholdDb_{-40.0f};
    std::atomic<float> attackMs_{10.0f};
    std::atomic<float> releaseMs_{50.0f};
    std::atomic<float> outputGainDb_{0.0f};
    std::atomic<int>   detectionMode_{DetectionPeak};

    int sampleRate_ = 48000;
    float envGain_ = 1.0f;
};

} // namespace mrefiner
