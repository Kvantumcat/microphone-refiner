#include "IniConfig.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace mrefiner {

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string toLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string fmtFloat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

const char* fmtBool(bool b) { return b ? "true" : "false"; }

using Section = std::map<std::string, std::string>;
using IniData = std::map<std::string, Section>;

IniData parseIni(const std::string& path) {
    IniData data;
    std::ifstream in(path);
    if (!in) return data;

    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key   = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (!key.empty()) data[section][key] = value;
    }
    return data;
}

std::string get(const IniData& data, const char* section, const char* key) {
    auto it = data.find(section);
    if (it == data.end()) return {};
    auto jt = it->second.find(key);
    if (jt == it->second.end()) return {};
    return jt->second;
}

std::string samplesDir() {
    return (fs::path(exeDirectory()) / "samples").string();
}

// Paths inside the exe folder are stored relative so the folder stays portable.
std::string toStoredPath(const std::string& path) {
    if (path.empty()) return path;
    fs::path rel = fs::path(path).lexically_relative(exeDirectory());
    if (rel.empty() || *rel.begin() == "..") return path;
    return rel.generic_string();
}

std::string resolveStoredPath(const std::string& stored) {
    if (stored.empty() || fs::path(stored).is_absolute()) return stored;
    return (fs::path(exeDirectory()) / stored).lexically_normal().string();
}

class Reader {
public:
    Reader(const IniData& d, std::vector<std::string>& warnings) : d_(d), warnings_(warnings) {}

    float number(const char* section, const char* key, float fallback) {
        std::string v = get(d_, section, key);
        if (v.empty()) return fallback;
        try {
            size_t used = 0;
            float r = std::stof(v, &used);
            if (used == v.size()) return r;
        } catch (...) {}
        warn(section, key, v, fmtFloat(fallback));
        return fallback;
    }

    bool flag(const char* section, const char* key, bool fallback) {
        std::string v = get(d_, section, key);
        if (v.empty()) return fallback;
        std::string lo = toLower(v);
        if (lo == "true" || lo == "yes" || lo == "1" || lo == "on")  return true;
        if (lo == "false" || lo == "no" || lo == "0" || lo == "off") return false;
        warn(section, key, v, fmtBool(fallback));
        return fallback;
    }

    int detection(const char* section, const char* key, int fallback) {
        std::string v = get(d_, section, key);
        if (v.empty()) return fallback;
        std::string lo = toLower(v);
        if (lo == "peak") return 0;
        if (lo == "rms")  return 1;
        warn(section, key, v, fallback == 1 ? "RMS" : "Peak");
        return fallback;
    }

    std::string text(const char* section, const char* key) { return get(d_, section, key); }

private:
    void warn(const char* section, const char* key, const std::string& value, const std::string& used) {
        warnings_.push_back(std::string("[") + section + "] " + key + " = '" + value +
                            "' is invalid, using " + used);
    }

    const IniData& d_;
    std::vector<std::string>& warnings_;
};

} // namespace

std::string exeDirectory() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return ".";
    int utf8len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)len, nullptr, 0, nullptr, nullptr);
    std::string path(utf8len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, (int)len, path.data(), utf8len, nullptr, nullptr);
    return fs::path(path).parent_path().string();
}

std::string configFilePath() {
    return (fs::path(exeDirectory()) / "config.ini").string();
}

std::string sampleFilePath(const std::string& name) {
    fs::path p(samplesDir());
    if (name.empty()) p /= "sample.wav";
    else              p /= ("sample-" + name + ".wav");
    return p.string();
}

std::string lastLearnedPath() {
    return (fs::path(samplesDir()) / "last-learned.wav").string();
}

std::string presetsDir() {
    return (fs::path(exeDirectory()) / "presets").string();
}

std::string presetIniPath(const std::string& name) {
    return (fs::path(presetsDir()) / (name + ".ini")).string();
}

std::string presetNoisePath(const std::string& name) {
    return (fs::path(presetsDir()) / (name + ".wav")).string();
}

bool readIni(const std::string& path, Profile& p, std::vector<std::string>& warnings) {
    if (!fs::exists(path)) return false;

    IniData d = parseIni(path);
    Reader r(d, warnings);
    p = Profile{};
    auto& s = p.settings;

    s.spectralEnabled        = r.flag  ("Spectral", "Enabled", s.spectralEnabled);
    s.spectralAlpha          = r.number("Spectral", "Alpha",   s.spectralAlpha);
    s.spectralFloor          = r.number("Spectral", "Floor",   s.spectralFloor);

    s.gateEnabled            = r.flag  ("Gate", "Enabled",     s.gateEnabled);
    s.gateThresholdDb        = r.number("Gate", "ThresholdDb", s.gateThresholdDb);

    s.expanderEnabled        = r.flag     ("Expander", "Enabled",      s.expanderEnabled);
    s.expanderRatio          = r.number   ("Expander", "Ratio",        s.expanderRatio);
    s.expanderThresholdDb    = r.number   ("Expander", "ThresholdDb",  s.expanderThresholdDb);
    s.expanderAttackMs       = r.number   ("Expander", "AttackMs",     s.expanderAttackMs);
    s.expanderReleaseMs      = r.number   ("Expander", "ReleaseMs",    s.expanderReleaseMs);
    s.expanderOutputGainDb   = r.number   ("Expander", "OutputGainDb", s.expanderOutputGainDb);
    s.expanderDetectionMode  = r.detection("Expander", "Detection",    s.expanderDetectionMode);

    s.eqEnabled              = r.flag  ("Equalizer", "Enabled", s.eqEnabled);
    s.eqLowDb                = r.number("Equalizer", "LowDb",   s.eqLowDb);
    s.eqMidDb                = r.number("Equalizer", "MidDb",   s.eqMidDb);
    s.eqHighDb               = r.number("Equalizer", "HighDb",  s.eqHighDb);

    s.compressorEnabled      = r.flag  ("Compressor", "Enabled",      s.compressorEnabled);
    s.compressorRatio        = r.number("Compressor", "Ratio",        s.compressorRatio);
    s.compressorThresholdDb  = r.number("Compressor", "ThresholdDb",  s.compressorThresholdDb);
    s.compressorAttackMs     = r.number("Compressor", "AttackMs",     s.compressorAttackMs);
    s.compressorReleaseMs    = r.number("Compressor", "ReleaseMs",    s.compressorReleaseMs);
    s.compressorOutputGainDb = r.number("Compressor", "OutputGainDb", s.compressorOutputGainDb);

    s.gainEnabled            = r.flag  ("Gain", "Enabled", s.gainEnabled);
    s.gainDb                 = r.number("Gain", "Db",      s.gainDb);

    s.boostEnabled           = r.flag  ("Boost", "Enabled", s.boostEnabled);
    s.boostDriveDb           = r.number("Boost", "DriveDb", s.boostDriveDb);

    s.learnLengthSec         = r.number("Learn", "LengthSec", s.learnLengthSec);
    s.learnDelaySec          = r.number("Learn", "DelaySec",  s.learnDelaySec);

    p.inputDevice.id            = r.text("Devices", "InputId");
    p.inputDevice.friendlyName  = r.text("Devices", "InputName");
    p.outputDevice.id           = r.text("Devices", "OutputId");
    p.outputDevice.friendlyName = r.text("Devices", "OutputName");
    p.hasDevices = !p.inputDevice.id.empty() || !p.outputDevice.id.empty() ||
                   !p.inputDevice.friendlyName.empty() || !p.outputDevice.friendlyName.empty();

    p.noiseFile = resolveStoredPath(r.text("Noise", "File"));
    return true;
}

bool loadConfig(const std::string& path, Profile& out, std::vector<std::string>& warnings) {
    if (readIni(path, out, warnings)) return false;
    out = Profile{};
    try {
        writeIni(path, out, false);
    } catch (const std::exception& ex) {
        warnings.push_back(std::string("could not create ") + path + ": " + ex.what());
    }
    return true;
}

void writeIni(const std::string& path, const Profile& p, bool isPreset) {
    fs::path fp(path);
    if (fp.has_parent_path())
        fs::create_directories(fp.parent_path());

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);

    const auto& s = p.settings;
    const char* nl = "\r\n";

    if (isPreset) {
        out << "# Microphone Refiner preset - apply in the app with 'load <name>'" << nl << nl;
    } else {
        out << "# Microphone Refiner - config.ini" << nl
            << "# Recreated with defaults if deleted. Commands that change a setting write it back here." << nl
            << "# Hand edits while the app runs: save this file, then type 'reload-config' in the app." << nl
            << nl;
    }

    out << "[Spectral]" << nl;
    out << "Enabled = "    << fmtBool(s.spectralEnabled) << nl;
    out << "Alpha = "      << fmtFloat(s.spectralAlpha) << nl;
    out << "Floor = "      << fmtFloat(s.spectralFloor) << nl << nl;

    out << "[Gate]" << nl;
    out << "Enabled = "     << fmtBool(s.gateEnabled) << nl;
    out << "ThresholdDb = " << fmtFloat(s.gateThresholdDb) << nl << nl;

    out << "[Expander]" << nl;
    out << "Enabled = "      << fmtBool(s.expanderEnabled) << nl;
    out << "Ratio = "        << fmtFloat(s.expanderRatio) << nl;
    out << "ThresholdDb = "  << fmtFloat(s.expanderThresholdDb) << nl;
    out << "AttackMs = "     << fmtFloat(s.expanderAttackMs) << nl;
    out << "ReleaseMs = "    << fmtFloat(s.expanderReleaseMs) << nl;
    out << "OutputGainDb = " << fmtFloat(s.expanderOutputGainDb) << nl;
    out << "Detection = "    << (s.expanderDetectionMode == 1 ? "RMS" : "Peak") << nl << nl;

    out << "[Equalizer]" << nl;
    out << "Enabled = " << fmtBool(s.eqEnabled) << nl;
    out << "LowDb = "   << fmtFloat(s.eqLowDb) << nl;
    out << "MidDb = "   << fmtFloat(s.eqMidDb) << nl;
    out << "HighDb = "  << fmtFloat(s.eqHighDb) << nl << nl;

    out << "[Compressor]" << nl;
    out << "Enabled = "      << fmtBool(s.compressorEnabled) << nl;
    out << "Ratio = "        << fmtFloat(s.compressorRatio) << nl;
    out << "ThresholdDb = "  << fmtFloat(s.compressorThresholdDb) << nl;
    out << "AttackMs = "     << fmtFloat(s.compressorAttackMs) << nl;
    out << "ReleaseMs = "    << fmtFloat(s.compressorReleaseMs) << nl;
    out << "OutputGainDb = " << fmtFloat(s.compressorOutputGainDb) << nl << nl;

    out << "[Gain]" << nl;
    out << "Enabled = " << fmtBool(s.gainEnabled) << nl;
    out << "Db = "      << fmtFloat(s.gainDb) << nl << nl;

    out << "[Boost]" << nl;
    out << "Enabled = " << fmtBool(s.boostEnabled) << nl;
    out << "DriveDb = " << fmtFloat(s.boostDriveDb) << nl << nl;

    out << "[Learn]" << nl;
    out << "LengthSec = " << fmtFloat(s.learnLengthSec) << nl;
    out << "DelaySec = "  << fmtFloat(s.learnDelaySec) << nl << nl;

    if (!isPreset) {
        out << "[Devices]" << nl;
        out << "InputId = "    << p.inputDevice.id << nl;
        out << "InputName = "  << p.inputDevice.friendlyName << nl;
        out << "OutputId = "   << p.outputDevice.id << nl;
        out << "OutputName = " << p.outputDevice.friendlyName << nl << nl;
    }

    out << "[Noise]" << nl;
    out << "File = " << toStoredPath(p.noiseFile) << nl;

    if (!out) throw std::runtime_error("failed writing " + path);
}

} // namespace mrefiner
