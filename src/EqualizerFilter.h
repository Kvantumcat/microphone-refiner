#pragma once

#include "Biquad.h"
#include "IAudioFilter.h"

#include <atomic>
#include <vector>

namespace mrefiner {

// 3-Band Equalizer: low shelf @ 100 Hz, peaking @ 1 kHz (Q=0.8), high shelf @ 10 kHz.
// Coefficients recompute lazily on the audio thread when a slider moves,
// so the UI thread's slider changes are lock-free.
class EqualizerFilter : public IAudioFilter {
public:
    const char* name() const override { return "3-Band Equalizer"; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool e) override { enabled_.store(e, std::memory_order_relaxed); }

    void initialize(int sampleRate, int channels) override {
        sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
        channels_ = channels > 0 ? channels : 1;
        low_.assign(channels_, Biquad{});
        mid_.assign(channels_, Biquad{});
        high_.assign(channels_, Biquad{});
        dirty_.store(true, std::memory_order_relaxed);
    }

    void process(float* samples, int frames, int channels) override {
        if (dirty_.exchange(false, std::memory_order_relaxed)) {
            recomputeCoeffs();
        }
        if (static_cast<int>(low_.size()) != channels) return;

        for (int c = 0; c < channels; ++c) {
            auto& bl = low_[c];
            auto& bm = mid_[c];
            auto& bh = high_[c];
            for (int f = 0; f < frames; ++f) {
                int idx = f * channels + c;
                float x = samples[idx];
                x = bl.process(x);
                x = bm.process(x);
                x = bh.process(x);
                samples[idx] = x;
            }
        }
    }

    void setLowDb(float db)  { lowDb_.store(db,  std::memory_order_relaxed); dirty_.store(true, std::memory_order_relaxed); }
    void setMidDb(float db)  { midDb_.store(db,  std::memory_order_relaxed); dirty_.store(true, std::memory_order_relaxed); }
    void setHighDb(float db) { highDb_.store(db, std::memory_order_relaxed); dirty_.store(true, std::memory_order_relaxed); }

    float lowDb() const  { return lowDb_.load(std::memory_order_relaxed); }
    float midDb() const  { return midDb_.load(std::memory_order_relaxed); }
    float highDb() const { return highDb_.load(std::memory_order_relaxed); }

private:
    void recomputeCoeffs() {
        float sr = static_cast<float>(sampleRate_);
        float lDb = lowDb_.load(std::memory_order_relaxed);
        float mDb = midDb_.load(std::memory_order_relaxed);
        float hDb = highDb_.load(std::memory_order_relaxed);
        for (int c = 0; c < channels_; ++c) {
            low_[c].setLowShelf(sr, 100.0f, lDb);
            mid_[c].setPeaking(sr, 1000.0f, mDb, 0.8f);
            high_[c].setHighShelf(sr, 10000.0f, hDb);
        }
    }

    std::atomic<bool>  enabled_{true};
    std::atomic<float> lowDb_{0.0f};
    std::atomic<float> midDb_{0.0f};
    std::atomic<float> highDb_{0.0f};
    std::atomic<bool>  dirty_{true};

    int sampleRate_ = 48000;
    int channels_ = 1;
    std::vector<Biquad> low_, mid_, high_;
};

} // namespace mrefiner
