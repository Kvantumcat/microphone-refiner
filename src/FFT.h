#pragma once

#include <cmath>
#include <complex>
#include <utility>

namespace mrefiner {

// Small radix-2 Cooley-Tukey FFT. Not the fastest possible, but zero-dep
// and plenty fast enough for N up to a few thousand at 48 kHz.
// Forward: no scaling. Inverse: scales by 1/N so a round trip preserves the signal.
class FFT {
public:
    static void forward(std::complex<float>* data, int n) { transform(data, n, false); }
    static void inverse(std::complex<float>* data, int n) {
        transform(data, n, true);
        const float inv = 1.0f / static_cast<float>(n);
        for (int i = 0; i < n; ++i) data[i] *= inv;
    }

private:
    static void transform(std::complex<float>* data, int n, bool inverse) {
        bitReversePermute(data, n);
        butterflies(data, n, inverse);
    }

    static void bitReversePermute(std::complex<float>* data, int n) {
        int j = 0;
        for (int i = 1; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(data[i], data[j]);
        }
    }

    static void butterflies(std::complex<float>* data, int n, bool inverse) {
        constexpr float kPi = 3.14159265358979323846f;
        for (int len = 2; len <= n; len <<= 1) {
            const float angle = (inverse ? 2.0f : -2.0f) * kPi / static_cast<float>(len);
            const std::complex<float> wlen(std::cos(angle), std::sin(angle));
            for (int i = 0; i < n; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                const int half = len >> 1;
                for (int k = 0; k < half; ++k) {
                    const auto u = data[i + k];
                    const auto v = data[i + k + half] * w;
                    data[i + k] = u + v;
                    data[i + k + half] = u - v;
                    w *= wlen;
                }
            }
        }
    }
};

} // namespace mrefiner
