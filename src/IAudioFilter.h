#pragma once

namespace mrefiner {

class IAudioFilter {
public:
    virtual ~IAudioFilter() = default;

    virtual const char* name() const = 0;
    virtual bool enabled() const = 0;
    virtual void setEnabled(bool e) = 0;

    // Called once each time the audio engine starts or the working format changes.
    virtual void initialize(int sampleRate, int channels) = 0;

    // Process `frames` audio frames of `channels` interleaved float samples in place.
    virtual void process(float* samples, int frames, int channels) = 0;
};

} // namespace mrefiner
