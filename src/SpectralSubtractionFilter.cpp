#include "SpectralSubtractionFilter.h"

#include "FFT.h"
#include "WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mrefiner {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kFrameSize = SpectralSubtractionFilter::kFrameSize;
constexpr int kHopSize   = SpectralSubtractionFilter::kHopSize;

void dropConsumedSamples(std::vector<float>& queue, size_t& head) {
    // Erasing only every few frames keeps the per-sample cost constant.
    if (head > 4 * kFrameSize) {
        queue.erase(queue.begin(), queue.begin() + head);
        head = 0;
    }
}

void applyWindow(const float* frame, const float* window, std::complex<float>* out) {
    for (int i = 0; i < kFrameSize; ++i)
        out[i] = std::complex<float>(frame[i] * window[i], 0.0f);
}

void subtractNoiseSpectrum(std::complex<float>* spectrum, const float* noiseMag,
                           float alpha, float floor) {
    for (int k = 0; k <= kFrameSize / 2; ++k) {
        const float re = spectrum[k].real();
        const float im = spectrum[k].imag();
        const float mag = std::sqrt(re * re + im * im);
        const float newMag = std::max(mag - alpha * noiseMag[k], floor * mag);
        if (mag > 1e-10f) {
            const float scale = newMag / mag;
            spectrum[k] = std::complex<float>(re * scale, im * scale);
        } else {
            spectrum[k] = std::complex<float>(0.0f, 0.0f);
        }
    }
}

void mirrorNegativeFrequencies(std::complex<float>* spectrum) {
    for (int k = 1; k < kFrameSize / 2; ++k)
        spectrum[kFrameSize - k] = std::conj(spectrum[k]);
}

} // namespace

SpectralSubtractionFilter::SpectralSubtractionFilter() {
    window_.resize(kFrameSize);
    for (int i = 0; i < kFrameSize; ++i)
        window_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * i / (kFrameSize - 1));
    // Hann + 75% overlap: steady-state sum-of-squares of overlapping windows is 1.5.
    windowNorm_ = 1.0f / 1.5f;
}

SpectralSubtractionFilter::ChannelState::ChannelState() {
    history.assign(kFrameSize, 0.0f);
    outputAccum.assign(kFrameSize, 0.0f);
    inputQueue.reserve(kFrameSize * 2);
    outputQueue.reserve(kFrameSize * 2);
    // Prime with one frame of silence to absorb WASAPI callback jitter and
    // hide the natural startup transient of the algorithm.
    outputQueue.assign(kFrameSize, 0.0f);
}

float SpectralSubtractionFilter::ChannelState::popOutput() {
    if (outputHead >= outputQueue.size()) return 0.0f;
    const float v = outputQueue[outputHead++];
    dropConsumedSamples(outputQueue, outputHead);
    return v;
}

void SpectralSubtractionFilter::ChannelState::drainHops(
    const float* window, float windowNorm, const float* noiseMag,
    float alpha, float floor, std::complex<float>* fftBuf)
{
    while (inputQueue.size() - inputHead >= static_cast<size_t>(kHopSize)) {
        appendHopToHistory(&inputQueue[inputHead]);
        inputHead += kHopSize;

        applyWindow(history.data(), window, fftBuf);
        FFT::forward(fftBuf, kFrameSize);
        subtractNoiseSpectrum(fftBuf, noiseMag, alpha, floor);
        mirrorNegativeFrequencies(fftBuf);
        FFT::inverse(fftBuf, kFrameSize);

        overlapAdd(fftBuf, window, windowNorm);
        emitHop();
    }
    dropConsumedSamples(inputQueue, inputHead);
}

void SpectralSubtractionFilter::ChannelState::appendHopToHistory(const float* hop) {
    std::memmove(history.data(), history.data() + kHopSize,
                 sizeof(float) * (kFrameSize - kHopSize));
    std::memcpy(history.data() + (kFrameSize - kHopSize), hop, sizeof(float) * kHopSize);
}

void SpectralSubtractionFilter::ChannelState::overlapAdd(
    const std::complex<float>* frame, const float* window, float windowNorm)
{
    for (int i = 0; i < kFrameSize; ++i)
        outputAccum[i] += frame[i].real() * window[i] * windowNorm;
}

void SpectralSubtractionFilter::ChannelState::emitHop() {
    outputQueue.insert(outputQueue.end(), outputAccum.begin(), outputAccum.begin() + kHopSize);
    std::memmove(outputAccum.data(), outputAccum.data() + kHopSize,
                 sizeof(float) * (kFrameSize - kHopSize));
    std::memset(outputAccum.data() + (kFrameSize - kHopSize), 0, sizeof(float) * kHopSize);
}

void SpectralSubtractionFilter::initialize(int sampleRate, int channels) {
    sampleRate_ = sampleRate;
    channels_ = channels;
    channelStates_ = std::vector<ChannelState>(channels);

    std::lock_guard<std::mutex> lk(profileMutex_);
    if (!rawNoise_.empty())
        recomputeNoiseSpectrum();
}

void SpectralSubtractionFilter::rawNoiseSnapshot(std::vector<float>& outSamples, int& outRate) const {
    std::lock_guard<std::mutex> lk(profileMutex_);
    outSamples = rawNoise_;
    outRate = rawNoiseRate_;
}

void SpectralSubtractionFilter::setRawNoise(std::vector<float> mono, int sampleRate) {
    std::lock_guard<std::mutex> lk(profileMutex_);
    rawNoise_ = std::move(mono);
    rawNoiseRate_ = sampleRate;
    if (sampleRate_ > 0)
        recomputeNoiseSpectrum();
}

void SpectralSubtractionFilter::clearProfile() {
    std::lock_guard<std::mutex> lk(profileMutex_);
    rawNoise_.clear();
    rawNoiseRate_ = 0;
    std::atomic_store(&noiseMagnitudes_, std::shared_ptr<std::vector<float>>{});
}

void SpectralSubtractionFilter::beginLearning(float seconds) {
    if (sampleRate_ <= 0) return;
    const size_t total = std::max<size_t>(kFrameSize,
        static_cast<size_t>(seconds * sampleRate_));
    learningBuffer_.assign(total, 0.0f);
    learningWritten_.store(0, std::memory_order_relaxed);
    learning_.store(true, std::memory_order_relaxed);
}

float SpectralSubtractionFilter::learningProgress() const {
    const size_t total = learningBuffer_.size();
    if (total == 0) return 0.0f;
    const size_t w = learningWritten_.load(std::memory_order_relaxed);
    return std::min(1.0f, static_cast<float>(w) / total);
}

bool SpectralSubtractionFilter::hasProfile() const {
    return std::atomic_load(&noiseMagnitudes_) != nullptr;
}

void SpectralSubtractionFilter::captureLearningSamples(const float* samples, int frames, int channels) {
    const size_t total = learningBuffer_.size();
    size_t written = learningWritten_.load(std::memory_order_relaxed);
    for (int f = 0; f < frames && written < total; ++f) {
        float mix = 0.0f;
        for (int c = 0; c < channels; ++c) mix += samples[f * channels + c];
        learningBuffer_[written++] = mix / channels;
    }
    learningWritten_.store(written, std::memory_order_relaxed);
    if (written >= total) finishLearning();
}

void SpectralSubtractionFilter::finishLearning() {
    learning_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(profileMutex_);
    rawNoise_ = std::move(learningBuffer_);
    rawNoiseRate_ = sampleRate_;
    learningBuffer_.clear();
    recomputeNoiseSpectrum();
}

void SpectralSubtractionFilter::recomputeNoiseSpectrum() {
    if (rawNoise_.empty() || sampleRate_ <= 0) return;

    const std::vector<float>* src = &rawNoise_;
    std::vector<float> resampled;
    if (rawNoiseRate_ != sampleRate_) {
        resampled = linearResampleMono(rawNoise_, rawNoiseRate_, sampleRate_);
        src = &resampled;
    }
    if (src->size() < static_cast<size_t>(kFrameSize)) {
        std::atomic_store(&noiseMagnitudes_, std::shared_ptr<std::vector<float>>{});
        return;
    }

    auto accum = std::make_shared<std::vector<float>>(kSpectrum, 0.0f);
    std::vector<std::complex<float>> fft(kFrameSize);
    int frameCount = 0;

    for (size_t start = 0; start + kFrameSize <= src->size(); start += kHopSize) {
        applyWindow(src->data() + start, window_.data(), fft.data());
        FFT::forward(fft.data(), kFrameSize);
        for (int k = 0; k < kSpectrum; ++k) {
            const float re = fft[k].real();
            const float im = fft[k].imag();
            (*accum)[k] += std::sqrt(re * re + im * im);
        }
        ++frameCount;
    }
    if (frameCount == 0) {
        std::atomic_store(&noiseMagnitudes_, std::shared_ptr<std::vector<float>>{});
        return;
    }
    for (int k = 0; k < kSpectrum; ++k)
        (*accum)[k] /= frameCount;

    std::atomic_store(&noiseMagnitudes_, accum);
}

void SpectralSubtractionFilter::process(float* samples, int frames, int channels) {
    if (static_cast<int>(channelStates_.size()) != channels) return;

    if (learning_.load(std::memory_order_relaxed)) {
        captureLearningSamples(samples, frames, channels);
        return;
    }

    auto noiseMagPtr = std::atomic_load(&noiseMagnitudes_);
    if (!noiseMagPtr) return;

    const float* noiseMag = noiseMagPtr->data();
    const float alpha = alpha_.load(std::memory_order_relaxed);
    const float floor = floor_.load(std::memory_order_relaxed);

    std::vector<std::complex<float>> fftBuf(kFrameSize);
    for (int ch = 0; ch < channels; ++ch) {
        auto& st = channelStates_[ch];
        for (int f = 0; f < frames; ++f)
            st.pushInput(samples[f * channels + ch]);
        st.drainHops(window_.data(), windowNorm_, noiseMag, alpha, floor, fftBuf.data());
        for (int f = 0; f < frames; ++f)
            samples[f * channels + ch] = st.popOutput();
    }
}

} // namespace mrefiner
