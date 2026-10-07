#pragma once

#include <string>

namespace mrefiner {

struct FilterSettings {
    bool  spectralEnabled = true;
    float spectralAlpha   = 2.0f;
    float spectralFloor   = 0.02f;

    bool  gateEnabled     = true;
    float gateThresholdDb = -50.0f;

    bool  gainEnabled     = true;
    float gainDb          = 0.0f;

    bool  boostEnabled    = true;
    float boostDriveDb    = 0.0f;

    bool  expanderEnabled        = true;
    float expanderRatio          = 2.0f;
    float expanderThresholdDb    = -40.0f;
    float expanderAttackMs       = 10.0f;
    float expanderReleaseMs      = 50.0f;
    float expanderOutputGainDb   = 0.0f;
    int   expanderDetectionMode  = 0;    // 0=Peak, 1=RMS

    bool  eqEnabled              = true;
    float eqLowDb                = 0.0f;
    float eqMidDb                = 0.0f;
    float eqHighDb               = 0.0f;

    bool  compressorEnabled      = true;
    float compressorRatio        = 4.0f;
    float compressorThresholdDb  = -18.0f;
    float compressorAttackMs     = 6.0f;
    float compressorReleaseMs    = 60.0f;
    float compressorOutputGainDb = 0.0f;

    float learnLengthSec  = 3.0f;
    float learnDelaySec   = 1.0f;   // lets the Enter key's sound die away before recording
};

// WASAPI endpoint reference. `id` is fast/exact (but machine-specific);
// `friendlyName` is a portable fallback when moving the app between machines.
struct DeviceRef {
    std::string id;
    std::string friendlyName;
};

struct Profile {
    FilterSettings settings;

    DeviceRef inputDevice;
    DeviceRef outputDevice;
    bool hasDevices = false;

    std::string noiseFile;  // absolute path of the active noise WAV, empty if none
};

} // namespace mrefiner
