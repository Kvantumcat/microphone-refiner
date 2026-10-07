#pragma once

#include "IAudioFilter.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mrefiner {

struct DeviceInfo {
    std::wstring id;
    std::wstring friendlyName;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    static std::vector<DeviceInfo> listInputDevices();
    static std::vector<DeviceInfo> listOutputDevices();

    void addFilter(std::shared_ptr<IAudioFilter> f);

    // Throws std::runtime_error on failure.
    void start(const std::wstring& inputId, const std::wstring& outputId);
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_relaxed); }

    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }

private:
    void captureThreadMain();
    void renderThreadMain();
    void teardown();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::vector<std::shared_ptr<IAudioFilter>> filters_;
    std::mutex filtersMutex_;

    int sampleRate_ = 0;
    int channels_ = 0;

    std::atomic<bool> running_{false};
    std::thread captureThread_;
    std::thread renderThread_;
};

} // namespace mrefiner
