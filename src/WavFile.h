#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mrefiner {

struct WavData {
    int sampleRate = 0;
    int channels = 1;
    std::vector<float> samples;  // interleaved [-1, 1]
};

// Minimal WAV loader: PCM 16-bit and IEEE Float 32-bit.
// Skips unknown chunks. Ignores extended fmt chunk fields.
inline WavData loadWav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open WAV file: " + path);

    auto read4 = [&](char* out) {
        f.read(out, 4);
        return f.good();
    };
    auto readU32 = [&]() {
        uint32_t v = 0; f.read(reinterpret_cast<char*>(&v), 4); return v;
    };
    auto readU16 = [&]() {
        uint16_t v = 0; f.read(reinterpret_cast<char*>(&v), 2); return v;
    };

    char id[4];
    if (!read4(id) || std::memcmp(id, "RIFF", 4) != 0)
        throw std::runtime_error("Not a RIFF file: " + path);
    readU32(); // total size (unused)
    if (!read4(id) || std::memcmp(id, "WAVE", 4) != 0)
        throw std::runtime_error("Not a WAVE file: " + path);

    WavData d;
    uint16_t audioFormat = 0;
    uint16_t bitsPerSample = 0;
    bool haveFmt = false;

    while (read4(id)) {
        uint32_t chunkSize = readU32();
        if (!f) break;

        if (std::memcmp(id, "fmt ", 4) == 0) {
            audioFormat = readU16();
            d.channels = readU16();
            d.sampleRate = static_cast<int>(readU32());
            readU32(); // byte rate
            readU16(); // block align
            bitsPerSample = readU16();
            if (chunkSize > 16) f.seekg(chunkSize - 16, std::ios::cur);
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (!haveFmt) throw std::runtime_error("data chunk before fmt");

            std::vector<char> raw(chunkSize);
            f.read(raw.data(), chunkSize);

            if ((audioFormat == 1 || audioFormat == 0xFFFE) && bitsPerSample == 16) {
                const size_t n = chunkSize / sizeof(int16_t);
                d.samples.resize(n);
                const auto* p = reinterpret_cast<const int16_t*>(raw.data());
                for (size_t i = 0; i < n; ++i)
                    d.samples[i] = static_cast<float>(p[i]) / 32768.0f;
            } else if ((audioFormat == 3 || audioFormat == 0xFFFE) && bitsPerSample == 32) {
                const size_t n = chunkSize / sizeof(float);
                d.samples.resize(n);
                std::memcpy(d.samples.data(), raw.data(), chunkSize);
            } else {
                throw std::runtime_error(
                    "Unsupported WAV: format=" + std::to_string(audioFormat) +
                    " bits=" + std::to_string(bitsPerSample) + " (need PCM16 or Float32)");
            }
            return d;
        } else {
            f.seekg(chunkSize, std::ios::cur);
        }
    }
    throw std::runtime_error("No data chunk found in WAV: " + path);
}

inline std::vector<float> downmixToMono(const std::vector<float>& interleaved, int channels) {
    if (channels <= 1) return interleaved;
    std::vector<float> mono(interleaved.size() / channels);
    for (size_t f = 0; f < mono.size(); ++f) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) sum += interleaved[f * channels + c];
        mono[f] = sum / channels;
    }
    return mono;
}

// Creates parent directories as needed.
inline void saveWavMonoFloat(const std::string& path,
                             const std::vector<float>& samples,
                             int sampleRate) {
    if (samples.empty()) throw std::runtime_error("No samples to save");

    std::filesystem::path fp(path);
    if (fp.has_parent_path())
        std::filesystem::create_directories(fp.parent_path());

    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open for write: " + path);

    const uint32_t sampleRateU  = static_cast<uint32_t>(sampleRate);
    const uint32_t dataSize     = static_cast<uint32_t>(samples.size() * sizeof(float));
    const uint32_t fmtChunkSize = 16;
    const uint16_t audioFormat  = 3;  // IEEE Float
    const uint16_t channels     = 1;
    const uint32_t byteRate     = sampleRateU * channels * static_cast<uint32_t>(sizeof(float));
    const uint16_t blockAlign   = static_cast<uint16_t>(channels * sizeof(float));
    const uint16_t bitsPerSample = 32;
    const uint32_t riffSize     = 4 + (8 + fmtChunkSize) + (8 + dataSize);

    auto write4 = [&](const char* tag) { f.write(tag, 4); };
    auto writeU32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto writeU16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    write4("RIFF");
    writeU32(riffSize);
    write4("WAVE");
    write4("fmt ");
    writeU32(fmtChunkSize);
    writeU16(audioFormat);
    writeU16(channels);
    writeU32(sampleRateU);
    writeU32(byteRate);
    writeU16(blockAlign);
    writeU16(bitsPerSample);
    write4("data");
    writeU32(dataSize);
    f.write(reinterpret_cast<const char*>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(float)));
    if (!f) throw std::runtime_error("Failed writing WAV: " + path);
}

// Simple linear-interpolation resampler for mono float samples.
// Fine for noise profiles (not a hi-fi resampler).
inline std::vector<float> linearResampleMono(const std::vector<float>& in, int fromRate, int toRate) {
    if (fromRate == toRate || in.empty()) return in;
    const double ratio = static_cast<double>(fromRate) / toRate;
    const size_t outLen = static_cast<size_t>(in.size() / ratio);
    std::vector<float> out(outLen);
    for (size_t i = 0; i < outLen; ++i) {
        const double srcIdx = i * ratio;
        const size_t i0 = static_cast<size_t>(srcIdx);
        const size_t i1 = std::min(i0 + 1, in.size() - 1);
        const float t = static_cast<float>(srcIdx - i0);
        out[i] = in[i0] * (1.0f - t) + in[i1] * t;
    }
    return out;
}

} // namespace mrefiner
