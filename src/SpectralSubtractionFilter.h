#pragma once

#include "IAudioFilter.h"

#include <atomic>
#include <complex>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mrefiner {

// STFT-based spectral subtraction. Small frame size keeps algorithmic latency low.
class SpectralSubtractionFilter : public IAudioFilter {
public:
    static constexpr int kFrameSize = 512;   // ~11 ms at 48 kHz
    static constexpr int kHopSize   = 128;   // 75% overlap
    static constexpr int kSpectrum  = kFrameSize / 2 + 1;

    SpectralSubtractionFilter();

    const char* name() const override { return "Spectral Subtraction"; }
    bool enabled() const override { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool e) override { enabled_.store(e, std::memory_order_relaxed); }

    void initialize(int sampleRate, int channels) override;
    void process(float* samples, int frames, int channels) override;

    void  setOverSubtraction(float alpha) { alpha_.store(alpha, std::memory_order_relaxed); }
    float overSubtraction() const { return alpha_.load(std::memory_order_relaxed); }
    void  setSpectralFloor(float beta) { floor_.store(beta, std::memory_order_relaxed); }
    float spectralFloor() const { return floor_.load(std::memory_order_relaxed); }

    // Thread-safe.
    void setRawNoise(std::vector<float> mono, int sampleRate);
    void clearProfile();

    // Live learning: capture the next `seconds` of input as the noise profile.
    void beginLearning(float seconds);
    bool isLearning() const { return learning_.load(std::memory_order_relaxed); }
    float learningProgress() const;

    bool hasProfile() const;
    int  sampleRate() const { return sampleRate_; }

    // Thread-safe. outSamples is empty when there is no noise sample.
    void rawNoiseSnapshot(std::vector<float>& outSamples, int& outRate) const;

private:
    void recomputeNoiseSpectrum();
    void captureLearningSamples(const float* samples, int frames, int channels);
    void finishLearning();

    struct ChannelState {
        std::vector<float> history;      // last kFrameSize input samples
        std::vector<float> outputAccum;  // overlap-add accumulator
        std::vector<float> inputQueue;
        std::vector<float> outputQueue;
        size_t inputHead = 0;
        size_t outputHead = 0;

        ChannelState();
        void pushInput(float s) { inputQueue.push_back(s); }
        float popOutput();
        void drainHops(
            const float* window,
            float windowNorm,
            const float* noiseMag,
            float alpha,
            float floor,
            std::complex<float>* fftBuf);

    private:
        void appendHopToHistory(const float* hop);
        void overlapAdd(const std::complex<float>* frame, const float* window, float windowNorm);
        void emitHop();
    };

    std::atomic<bool>  enabled_{true};
    std::atomic<float> alpha_{2.0f};
    std::atomic<float> floor_{0.02f};

    std::vector<float> window_;
    float windowNorm_ = 1.0f;

    int sampleRate_ = 0;
    int channels_ = 0;

    // Protected by profileMutex_: modified from any thread that sets a profile.
    // The pointer is read atomically by the audio thread.
    mutable std::mutex profileMutex_;
    std::shared_ptr<std::vector<float>> noiseMagnitudes_;
    std::vector<float> rawNoise_;
    int rawNoiseRate_ = 0;

    std::vector<ChannelState> channelStates_;

    // Learning state (audio thread writes, UI thread reads)
    std::atomic<bool>   learning_{false};
    std::vector<float>  learningBuffer_;
    std::atomic<size_t> learningWritten_{0};
};

} // namespace mrefiner
