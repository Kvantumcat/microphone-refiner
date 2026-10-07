#pragma once

#include <cmath>

namespace mrefiner {

// Direct-form I biquad. Coefficients from the Audio EQ Cookbook (R. Bristow-Johnson).
// Normalized so a0 = 1 (divides numerator and feedback coefficients by raw a0).
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

    inline float process(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }

    void reset() { x1 = x2 = y1 = y2 = 0.0f; }

    void setLowShelf(float sampleRate, float freq, float gainDb, float S = 1.0f) {
        constexpr float kPi = 3.14159265358979323846f;
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * kPi * freq / sampleRate;
        float cs = std::cos(w0);
        float sn = std::sin(w0);
        float alpha = sn / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
        float sqrtA = std::sqrt(A);

        float a0 = (A + 1) + (A - 1) * cs + 2 * sqrtA * alpha;
        b0 = A * ((A + 1) - (A - 1) * cs + 2 * sqrtA * alpha) / a0;
        b1 = 2 * A * ((A - 1) - (A + 1) * cs) / a0;
        b2 = A * ((A + 1) - (A - 1) * cs - 2 * sqrtA * alpha) / a0;
        a1 = -2 * ((A - 1) + (A + 1) * cs) / a0;
        a2 = ((A + 1) + (A - 1) * cs - 2 * sqrtA * alpha) / a0;
    }

    void setHighShelf(float sampleRate, float freq, float gainDb, float S = 1.0f) {
        constexpr float kPi = 3.14159265358979323846f;
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * kPi * freq / sampleRate;
        float cs = std::cos(w0);
        float sn = std::sin(w0);
        float alpha = sn / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
        float sqrtA = std::sqrt(A);

        float a0 = (A + 1) - (A - 1) * cs + 2 * sqrtA * alpha;
        b0 = A * ((A + 1) + (A - 1) * cs + 2 * sqrtA * alpha) / a0;
        b1 = -2 * A * ((A - 1) + (A + 1) * cs) / a0;
        b2 = A * ((A + 1) + (A - 1) * cs - 2 * sqrtA * alpha) / a0;
        a1 = 2 * ((A - 1) - (A + 1) * cs) / a0;
        a2 = ((A + 1) - (A - 1) * cs - 2 * sqrtA * alpha) / a0;
    }

    void setPeaking(float sampleRate, float freq, float gainDb, float Q = 1.0f) {
        constexpr float kPi = 3.14159265358979323846f;
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * kPi * freq / sampleRate;
        float cs = std::cos(w0);
        float sn = std::sin(w0);
        float alpha = sn / (2.0f * Q);

        float a0 = 1 + alpha / A;
        b0 = (1 + alpha * A) / a0;
        b1 = -2 * cs / a0;
        b2 = (1 - alpha * A) / a0;
        a1 = -2 * cs / a0;
        a2 = (1 - alpha / A) / a0;
    }
};

} // namespace mrefiner
