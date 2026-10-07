#include "AudioEngine.h"
#include "BoostFilter.h"
#include "CompressorFilter.h"
#include "EqualizerFilter.h"
#include "ExpanderFilter.h"
#include "GainFilter.h"
#include "IniConfig.h"
#include "NoiseGateFilter.h"
#include "Profile.h"
#include "SpectralSubtractionFilter.h"
#include "WavFile.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mrefiner;

namespace {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
    return w;
}

struct SessionState {
    std::wstring inputId;
    std::wstring inputName;
    std::wstring outputId;
    std::wstring outputName;

    float learnLengthSec = 3.0f;
    float learnDelaySec  = 1.0f;

    // Absolute path of the active noise WAV (saved under [Noise] File in config.ini).
    std::string noiseFile;
};

struct FilterSet {
    SpectralSubtractionFilter& spectral;
    NoiseGateFilter&           gate;
    ExpanderFilter&            expander;
    EqualizerFilter&           eq;
    CompressorFilter&          compressor;
    GainFilter&                gain;
    BoostFilter&               boost;
};

std::string formatRatio(float v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f:1", static_cast<double>(v));
    return buf;
}

void printWarnings(const std::vector<std::string>& warnings) {
    for (const auto& w : warnings) std::cout << "  [!] " << w << "\n";
}

std::wstring promptDevice(const std::vector<DeviceInfo>& devs, const char* label, const char* preferSubstring) {
    std::cout << "\n" << label << ":\n";
    int preferredIndex = -1;
    for (size_t i = 0; i < devs.size(); ++i) {
        std::string name = toUtf8(devs[i].friendlyName);
        std::cout << "  [" << i << "] " << name << "\n";
        if (preferSubstring && preferredIndex < 0 && name.find(preferSubstring) != std::string::npos)
            preferredIndex = static_cast<int>(i);
    }
    std::cout << "Select index";
    if (preferredIndex >= 0) std::cout << " [default " << preferredIndex << "]";
    std::cout << ": " << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) return {};
    if (line.empty() && preferredIndex >= 0) return devs[preferredIndex].id;
    try {
        int idx = std::stoi(line);
        if (idx < 0 || idx >= static_cast<int>(devs.size())) {
            std::cout << "Out of range\n";
            return {};
        }
        return devs[idx].id;
    } catch (...) {
        std::cout << "Not a number\n";
        return {};
    }
}

std::pair<std::wstring, std::wstring> matchDevice(const std::vector<DeviceInfo>& devs,
                                                  const std::string& savedId,
                                                  const std::string& savedName) {
    if (!savedId.empty()) {
        std::wstring target = fromUtf8(savedId);
        for (const auto& d : devs)
            if (d.id == target) return {d.id, d.friendlyName};
    }
    if (!savedName.empty()) {
        std::wstring target = fromUtf8(savedName);
        for (const auto& d : devs)
            if (d.friendlyName == target) return {d.id, d.friendlyName};
    }
    return {};
}

std::wstring lookupFriendlyName(const std::vector<DeviceInfo>& devs, const std::wstring& id) {
    for (const auto& d : devs) if (d.id == id) return d.friendlyName;
    return {};
}

SessionState resolveDevices(const Profile& p) {
    auto inputs = AudioEngine::listInputDevices();
    auto outputs = AudioEngine::listOutputDevices();

    SessionState st;

    if (p.hasDevices) {
        auto [id, name] = matchDevice(inputs, p.inputDevice.id, p.inputDevice.friendlyName);
        st.inputId = id;
        st.inputName = name;
        if (st.inputId.empty() && (!p.inputDevice.id.empty() || !p.inputDevice.friendlyName.empty())) {
            std::cout << "  [!] Saved input '" << p.inputDevice.friendlyName
                      << "' not found on this machine.\n";
        }
    }
    if (st.inputId.empty()) {
        st.inputId = promptDevice(inputs, "Input devices", nullptr);
        st.inputName = lookupFriendlyName(inputs, st.inputId);
    }

    if (p.hasDevices) {
        auto [id, name] = matchDevice(outputs, p.outputDevice.id, p.outputDevice.friendlyName);
        st.outputId = id;
        st.outputName = name;
        if (st.outputId.empty() && (!p.outputDevice.id.empty() || !p.outputDevice.friendlyName.empty())) {
            std::cout << "  [!] Saved output '" << p.outputDevice.friendlyName
                      << "' not found on this machine.\n";
        }
    }
    if (st.outputId.empty()) {
        st.outputId = promptDevice(outputs, "Output devices", "CABLE Input");
        st.outputName = lookupFriendlyName(outputs, st.outputId);
    }

    return st;
}

void printHelp() {
    std::cout << "\nCommands:\n"
              << "  q                        quit (saves config.ini)\n"
              << "  l                        learn noise (auto-saves samples\\last-learned.wav)\n"
              << "  learn-length <sec>       sample duration\n"
              << "  learn-delay <sec>        pre-record warmup\n"
              << "\n"
              << "  --- spectral subtraction ---\n"
              << "  a <val>                  aggressiveness alpha\n"
              << "  b <val>                  spectral floor beta\n"
              << "\n"
              << "  --- noise gate ---\n"
              << "  g <dB>                   threshold\n"
              << "\n"
              << "  --- expander ---\n"
              << "  exp-ratio <r>            ratio N:1 (e.g. 'exp-ratio 2.5' => 2.50:1)\n"
              << "  exp-th <dB>              threshold\n"
              << "  exp-attack <ms>          attack time\n"
              << "  exp-release <ms>         release time\n"
              << "  exp-gain <dB>            output gain\n"
              << "  exp-detect peak|rms      detection mode\n"
              << "\n"
              << "  --- 3-band equalizer ---\n"
              << "  eq-low <dB>              low shelf @ 100 Hz\n"
              << "  eq-mid <dB>              peaking @ 1 kHz\n"
              << "  eq-high <dB>             high shelf @ 10 kHz\n"
              << "\n"
              << "  --- compressor ---\n"
              << "  comp-ratio <r>           ratio N:1\n"
              << "  comp-th <dB>             threshold\n"
              << "  comp-attack <ms>         attack time\n"
              << "  comp-release <ms>        release time\n"
              << "  comp-gain <dB>           output gain\n"
              << "\n"
              << "  --- level stages ---\n"
              << "  v <dB>                   clean gain\n"
              << "  d <dB>                   boost / soft-clip drive\n"
              << "\n"
              << "  s <name> <on|off>        toggle: spectral | gate | expander | eq | compressor | gain | boost\n"
              << "  device                   re-select both input and output (restarts engine)\n"
              << "  device input|output      re-select one endpoint\n"
              << "  sample-save [name]       save the current noise sample (samples\\sample[-name].wav)\n"
              << "  sample-load [name]       activate a saved noise sample\n"
              << "  save <name>              save settings + noise as a preset (presets\\<name>.ini)\n"
              << "  load <name>              apply a preset (keeps the current devices)\n"
              << "  reload-config            re-read config.ini after editing it by hand\n"
              << "  status                   print current settings\n"
              << "  ?                        this help\n\n";
}

Profile snapshot(const SessionState& session, const FilterSet& fs) {
    Profile p;
    auto& s = p.settings;

    s.spectralEnabled           = fs.spectral.enabled();
    s.spectralAlpha             = fs.spectral.overSubtraction();
    s.spectralFloor             = fs.spectral.spectralFloor();

    s.gateEnabled               = fs.gate.enabled();
    s.gateThresholdDb           = fs.gate.thresholdDb();

    s.expanderEnabled           = fs.expander.enabled();
    s.expanderRatio             = fs.expander.ratio();
    s.expanderThresholdDb       = fs.expander.thresholdDb();
    s.expanderAttackMs          = fs.expander.attackMs();
    s.expanderReleaseMs         = fs.expander.releaseMs();
    s.expanderOutputGainDb      = fs.expander.outputGainDb();
    s.expanderDetectionMode     = fs.expander.detectionMode();

    s.eqEnabled                 = fs.eq.enabled();
    s.eqLowDb                   = fs.eq.lowDb();
    s.eqMidDb                   = fs.eq.midDb();
    s.eqHighDb                  = fs.eq.highDb();

    s.compressorEnabled         = fs.compressor.enabled();
    s.compressorRatio           = fs.compressor.ratio();
    s.compressorThresholdDb     = fs.compressor.thresholdDb();
    s.compressorAttackMs        = fs.compressor.attackMs();
    s.compressorReleaseMs       = fs.compressor.releaseMs();
    s.compressorOutputGainDb    = fs.compressor.outputGainDb();

    s.gainEnabled               = fs.gain.enabled();
    s.gainDb                    = fs.gain.gainDb();

    s.boostEnabled              = fs.boost.enabled();
    s.boostDriveDb              = fs.boost.driveDb();

    s.learnLengthSec            = session.learnLengthSec;
    s.learnDelaySec             = session.learnDelaySec;

    p.inputDevice.id            = toUtf8(session.inputId);
    p.inputDevice.friendlyName  = toUtf8(session.inputName);
    p.outputDevice.id           = toUtf8(session.outputId);
    p.outputDevice.friendlyName = toUtf8(session.outputName);
    p.hasDevices = !p.inputDevice.id.empty() || !p.outputDevice.id.empty();

    p.noiseFile = session.noiseFile;
    return p;
}

bool loadNoiseFile(const std::string& path, SpectralSubtractionFilter& sp) {
    try {
        auto wav = loadWav(path);
        sp.setRawNoise(downmixToMono(wav.samples, wav.channels), wav.sampleRate);
        return true;
    } catch (const std::exception& ex) {
        std::cout << "  [!] Could not load noise sample " << path << ": " << ex.what() << "\n";
        return false;
    }
}

// Applies settings and the noise sample. Devices are left to the caller,
// because switching them means restarting the engine.
void applyProfile(const Profile& p, SessionState& session, const FilterSet& fs) {
    const auto& s = p.settings;

    fs.spectral.setEnabled(s.spectralEnabled);
    fs.spectral.setOverSubtraction(s.spectralAlpha);
    fs.spectral.setSpectralFloor(s.spectralFloor);

    fs.gate.setEnabled(s.gateEnabled);
    fs.gate.setThresholdDb(s.gateThresholdDb);

    fs.expander.setEnabled(s.expanderEnabled);
    fs.expander.setRatio(s.expanderRatio);
    fs.expander.setThresholdDb(s.expanderThresholdDb);
    fs.expander.setAttackMs(s.expanderAttackMs);
    fs.expander.setReleaseMs(s.expanderReleaseMs);
    fs.expander.setOutputGainDb(s.expanderOutputGainDb);
    fs.expander.setDetectionMode(s.expanderDetectionMode);

    fs.eq.setEnabled(s.eqEnabled);
    fs.eq.setLowDb(s.eqLowDb);
    fs.eq.setMidDb(s.eqMidDb);
    fs.eq.setHighDb(s.eqHighDb);

    fs.compressor.setEnabled(s.compressorEnabled);
    fs.compressor.setRatio(s.compressorRatio);
    fs.compressor.setThresholdDb(s.compressorThresholdDb);
    fs.compressor.setAttackMs(s.compressorAttackMs);
    fs.compressor.setReleaseMs(s.compressorReleaseMs);
    fs.compressor.setOutputGainDb(s.compressorOutputGainDb);

    fs.gain.setEnabled(s.gainEnabled);
    fs.gain.setGainDb(s.gainDb);

    fs.boost.setEnabled(s.boostEnabled);
    fs.boost.setDriveDb(s.boostDriveDb);

    session.learnLengthSec = s.learnLengthSec;
    session.learnDelaySec  = s.learnDelaySec;

    if (p.noiseFile.empty()) {
        fs.spectral.clearProfile();
        session.noiseFile.clear();
    } else if (loadNoiseFile(p.noiseFile, fs.spectral)) {
        session.noiseFile = p.noiseFile;
    }
}

// Returns false if presets\<name>.ini doesn't exist.
bool applyPreset(const std::string& name, SessionState& session, const FilterSet& fs) {
    Profile p;
    std::vector<std::string> warnings;
    if (!readIni(presetIniPath(name), p, warnings)) return false;
    printWarnings(warnings);
    applyProfile(p, session, fs);
    return true;
}

std::string listPresets() {
    std::string names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(presetsDir(), ec)) {
        if (entry.path().extension() != ".ini") continue;
        if (!names.empty()) names += ", ";
        names += entry.path().stem().string();
    }
    return names.empty() ? "(none)" : names;
}

std::string restAfter(const std::string& line, const std::string& cmd) {
    if (line.size() <= cmd.size()) return {};
    size_t pos = cmd.size();
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    return line.substr(pos);
}

void countdown(float seconds, const char* label) {
    int whole = static_cast<int>(seconds);
    float frac = seconds - whole;
    for (int i = whole; i > 0; --i) {
        std::cout << "\r  " << label << " " << i << "s... " << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (frac > 0.001f)
        std::this_thread::sleep_for(std::chrono::milliseconds(int(frac * 1000)));
    std::cout << "\r  " << label << " done.        \n";
}

bool parseFloatArg(const std::string& line, const std::string& cmd,
                   float& out, float lo, float hi) {
    std::string arg = restAfter(line, cmd);
    if (arg.empty()) { std::cout << "  usage: " << cmd << " <val>\n"; return false; }
    try {
        float v = std::stof(arg);
        if (v < lo || v > hi) {
            std::cout << "  out of range (" << lo << " .. " << hi << ")\n";
            return false;
        }
        out = v;
        return true;
    } catch (...) { std::cout << "  not a number\n"; return false; }
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::string noiseWavPath;
    std::string presetName;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--noise-wav" && i + 1 < argc) noiseWavPath = argv[++i];
        else if (a == "--preset"    && i + 1 < argc) presetName   = argv[++i];
    }

    std::cout << "Microphone Refiner\n"
              << "==================\n";

    try {
        auto spectral   = std::make_shared<SpectralSubtractionFilter>();
        auto gate       = std::make_shared<NoiseGateFilter>();
        auto expander   = std::make_shared<ExpanderFilter>();
        auto eq         = std::make_shared<EqualizerFilter>();
        auto compressor = std::make_shared<CompressorFilter>();
        auto gain       = std::make_shared<GainFilter>();
        auto boost      = std::make_shared<BoostFilter>();

        FilterSet fs{*spectral, *gate, *expander, *eq, *compressor, *gain, *boost};

        const std::string configPath = configFilePath();
        SessionState session;

        // Timestamp of config.ini as we last read or wrote it. A mismatch means the
        // file was edited by hand, and saving would silently throw those edits away.
        std::filesystem::file_time_type configStamp{};
        auto refreshStamp = [&] {
            std::error_code ec;
            configStamp = std::filesystem::last_write_time(configPath, ec);
        };

        Profile startup;
        {
            std::vector<std::string> warnings;
            bool created = loadConfig(configPath, startup, warnings);
            refreshStamp();
            std::cout << (created ? "\nFirst run - created defaults: " : "\nLoaded config.ini: ")
                      << configPath << "\n";
            printWarnings(warnings);
        }
        applyProfile(startup, session, fs);
        if (!session.noiseFile.empty())
            std::cout << "  noise sample: " << session.noiseFile << "\n";

        if (!presetName.empty()) {
            if (!applyPreset(presetName, session, fs)) {
                std::cerr << "No preset named '" << presetName << "' (" << presetIniPath(presetName) << ")\n";
                return 2;
            }
            std::cout << "Applied preset: " << presetName << "\n";
        }

        if (!noiseWavPath.empty()) {
            std::string path = std::filesystem::absolute(noiseWavPath).string();
            if (!loadNoiseFile(path, *spectral)) return 2;
            session.noiseFile = path;
            std::cout << "  noise sample: " << path << "\n";
        }

        {
            SessionState resolved = resolveDevices(startup);
            session.inputId    = resolved.inputId;
            session.inputName  = resolved.inputName;
            session.outputId   = resolved.outputId;
            session.outputName = resolved.outputName;
        }
        if (session.inputId.empty() || session.outputId.empty()) {
            std::cerr << "No device selected. Exiting.\n";
            return 1;
        }

        auto saveConfig = [&]() -> bool {
            std::error_code ec;
            auto onDisk = std::filesystem::last_write_time(configPath, ec);
            if (ec) {
                std::cout << "  [!] config.ini is missing - not recreating it.\n"
                             "      Type 'reload-config' to start again from defaults.\n";
                return false;
            }
            if (onDisk != configStamp) {
                std::cout << "  [!] config.ini was edited outside the app - not overwriting it.\n"
                             "      Type 'reload-config' to apply the file (this change is not saved).\n";
                return false;
            }
            try {
                writeIni(configPath, snapshot(session, fs), false);
                refreshStamp();
                return true;
            } catch (const std::exception& ex) {
                std::cerr << "  [!] config.ini save failed: " << ex.what() << "\n";
                return false;
            }
        };
        saveConfig();

        AudioEngine engine;
        engine.addFilter(spectral);
        engine.addFilter(gate);
        engine.addFilter(expander);
        engine.addFilter(eq);
        engine.addFilter(compressor);
        engine.addFilter(gain);
        engine.addFilter(boost);

        auto startEngine = [&]() -> bool {
            std::cout << "\nStarting engine...\n"
                      << "  input:  " << toUtf8(session.inputName)  << "\n"
                      << "  output: " << toUtf8(session.outputName) << "\n";
            try {
                engine.start(session.inputId, session.outputId);
                std::cout << "Running at " << engine.sampleRate() << " Hz, "
                          << engine.channels() << " ch\n";
                return true;
            } catch (const std::exception& ex) {
                std::cout << "  [!] Failed to start engine: " << ex.what() << "\n";
                return false;
            }
        };

        if (!startEngine()) return 2;
        printHelp();

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            if (line == "q" || line == "quit" || line == "exit") break;
            if (line == "?" || line == "help") { printHelp(); continue; }

            if (line == "status") {
                auto onOff = [](bool b) { return b ? "on " : "off"; };
                std::cout << "  config.ini:   " << configPath << "\n"
                          << "  input:        " << toUtf8(session.inputName)  << "\n"
                          << "  output:       " << toUtf8(session.outputName) << "\n"
                          << "  noise file:   " << (session.noiseFile.empty() ? "(none)" : session.noiseFile) << "\n"
                          << "  learn-length: " << session.learnLengthSec << " s\n"
                          << "  learn-delay:  " << session.learnDelaySec  << " s\n"
                          << "  spectral:   " << onOff(spectral->enabled())
                          << "  alpha=" << spectral->overSubtraction()
                          << "  beta="  << spectral->spectralFloor()
                          << "  profile=" << (spectral->hasProfile() ? "yes" : "no") << "\n"
                          << "  gate:       " << onOff(gate->enabled())
                          << "  threshold=" << gate->thresholdDb() << " dB\n"
                          << "  expander:   " << onOff(expander->enabled())
                          << "  ratio=" << formatRatio(expander->ratio())
                          << "  threshold=" << expander->thresholdDb() << " dB"
                          << "  attack=" << expander->attackMs() << "ms"
                          << "  release=" << expander->releaseMs() << "ms"
                          << "  gain=" << expander->outputGainDb() << "dB"
                          << "  detect=" << (expander->detectionMode() == 0 ? "peak" : "rms") << "\n"
                          << "  eq:         " << onOff(eq->enabled())
                          << "  low=" << eq->lowDb() << "dB"
                          << "  mid=" << eq->midDb() << "dB"
                          << "  high=" << eq->highDb() << "dB\n"
                          << "  compressor: " << onOff(compressor->enabled())
                          << "  ratio=" << formatRatio(compressor->ratio())
                          << "  threshold=" << compressor->thresholdDb() << " dB"
                          << "  attack=" << compressor->attackMs() << "ms"
                          << "  release=" << compressor->releaseMs() << "ms"
                          << "  gain=" << compressor->outputGainDb() << "dB\n"
                          << "  gain:       " << onOff(gain->enabled())
                          << "  " << gain->gainDb() << " dB\n"
                          << "  boost:      " << onOff(boost->enabled())
                          << "  drive=" << boost->driveDb() << " dB\n";
                continue;
            }

            if (line == "reload-config") {
                Profile p;
                std::vector<std::string> warnings;
                bool recreated = loadConfig(configPath, p, warnings);
                refreshStamp();
                printWarnings(warnings);
                applyProfile(p, session, fs);

                // Switch devices only when the file names a different endpoint that exists.
                if (p.hasDevices) {
                    std::wstring inId = session.inputId,  inName = session.inputName;
                    std::wstring outId = session.outputId, outName = session.outputName;

                    auto [foundIn, foundInName] = matchDevice(AudioEngine::listInputDevices(),
                                                              p.inputDevice.id, p.inputDevice.friendlyName);
                    if (!foundIn.empty()) { inId = foundIn; inName = foundInName; }
                    else std::cout << "  [!] Input '" << p.inputDevice.friendlyName << "' not found - keeping the current one.\n";

                    auto [foundOut, foundOutName] = matchDevice(AudioEngine::listOutputDevices(),
                                                                p.outputDevice.id, p.outputDevice.friendlyName);
                    if (!foundOut.empty()) { outId = foundOut; outName = foundOutName; }
                    else std::cout << "  [!] Output '" << p.outputDevice.friendlyName << "' not found - keeping the current one.\n";

                    if (inId != session.inputId || outId != session.outputId) {
                        SessionState previous = session;
                        engine.stop();
                        session.inputId = inId;   session.inputName = inName;
                        session.outputId = outId; session.outputName = outName;
                        if (!startEngine()) {
                            std::cout << "  Going back to the previous devices.\n";
                            session.inputId = previous.inputId;   session.inputName = previous.inputName;
                            session.outputId = previous.outputId; session.outputName = previous.outputName;
                            startEngine();
                        }
                    }
                }

                std::cout << (recreated ? "  config.ini was missing - recreated with defaults.\n"
                                        : "  config.ini reloaded.\n");
                continue;
            }

            if (line == "l" || line == "learn") {
                if (session.learnDelaySec > 0.001f) {
                    std::cout << "  Warmup " << session.learnDelaySec << "s (let the enter-key noise settle)...\n";
                    countdown(session.learnDelaySec, "starting in");
                }
                std::cout << "  Learning " << session.learnLengthSec << "s... (keep input silent)\n";
                spectral->beginLearning(session.learnLengthSec);
                while (spectral->isLearning()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    std::cout << "\r  progress: " << int(spectral->learningProgress() * 100) << "%   " << std::flush;
                }
                if (spectral->hasProfile()) {
                    std::cout << "\r  learned. Profile ready.        \n";
                    // Persist to samples\last-learned.wav so the noise survives restarts.
                    try {
                        std::vector<float> samples;
                        int rate = 0;
                        spectral->rawNoiseSnapshot(samples, rate);
                        if (!samples.empty()) {
                            std::string path = lastLearnedPath();
                            saveWavMonoFloat(path, samples, rate);
                            session.noiseFile = path;
                            std::cout << "  saved to: " << path << "\n";
                        }
                    } catch (const std::exception& ex) {
                        std::cout << "  [!] auto-save failed: " << ex.what() << "\n";
                    }
                    saveConfig();
                } else {
                    std::cout << "\r  learning failed.               \n";
                }
                continue;
            }

            if (line.rfind("learn-length", 0) == 0) {
                float v; if (parseFloatArg(line, "learn-length", v, 0.1f, 60.0f)) {
                    session.learnLengthSec = v;
                    std::cout << "  learn-length=" << v << " s\n";
                    saveConfig();
                }
                continue;
            }
            if (line.rfind("learn-delay", 0) == 0) {
                float v; if (parseFloatArg(line, "learn-delay", v, 0.0f, 30.0f)) {
                    session.learnDelaySec = v;
                    std::cout << "  learn-delay=" << v << " s\n";
                    saveConfig();
                }
                continue;
            }

            if (line.rfind("exp-ratio",   0) == 0) { float v; if (parseFloatArg(line, "exp-ratio",   v, 1.0f, 20.0f))   { expander->setRatio(v);         std::cout << "  exp-ratio=" << formatRatio(v) << "\n"; saveConfig(); } continue; }
            if (line.rfind("exp-th",      0) == 0) { float v; if (parseFloatArg(line, "exp-th",      v, -80.0f, 0.0f))  { expander->setThresholdDb(v);   std::cout << "  exp-th=" << v << " dB\n"; saveConfig(); } continue; }
            if (line.rfind("exp-attack",  0) == 0) { float v; if (parseFloatArg(line, "exp-attack",  v, 0.1f, 500.0f))  { expander->setAttackMs(v);      std::cout << "  exp-attack=" << v << " ms\n"; saveConfig(); } continue; }
            if (line.rfind("exp-release", 0) == 0) { float v; if (parseFloatArg(line, "exp-release", v, 1.0f, 2000.0f)) { expander->setReleaseMs(v);     std::cout << "  exp-release=" << v << " ms\n"; saveConfig(); } continue; }
            if (line.rfind("exp-gain",    0) == 0) { float v; if (parseFloatArg(line, "exp-gain",    v, -24.0f, 24.0f)) { expander->setOutputGainDb(v);  std::cout << "  exp-gain=" << v << " dB\n"; saveConfig(); } continue; }
            if (line.rfind("exp-detect",  0) == 0) {
                std::string arg = restAfter(line, "exp-detect");
                if      (arg == "peak") { expander->setDetectionMode(ExpanderFilter::DetectionPeak); std::cout << "  exp-detect=peak\n"; saveConfig(); }
                else if (arg == "rms")  { expander->setDetectionMode(ExpanderFilter::DetectionRMS);  std::cout << "  exp-detect=rms\n";  saveConfig(); }
                else std::cout << "  usage: exp-detect peak|rms\n";
                continue;
            }

            if (line.rfind("eq-low",  0) == 0) { float v; if (parseFloatArg(line, "eq-low",  v, -24.0f, 24.0f)) { eq->setLowDb(v);  std::cout << "  eq-low=" << v << " dB\n"; saveConfig(); } continue; }
            if (line.rfind("eq-mid",  0) == 0) { float v; if (parseFloatArg(line, "eq-mid",  v, -24.0f, 24.0f)) { eq->setMidDb(v);  std::cout << "  eq-mid=" << v << " dB\n"; saveConfig(); } continue; }
            if (line.rfind("eq-high", 0) == 0) { float v; if (parseFloatArg(line, "eq-high", v, -24.0f, 24.0f)) { eq->setHighDb(v); std::cout << "  eq-high=" << v << " dB\n"; saveConfig(); } continue; }

            if (line.rfind("comp-ratio",  0) == 0) { float v; if (parseFloatArg(line, "comp-ratio",  v, 1.0f, 20.0f))  { compressor->setRatio(v);        std::cout << "  comp-ratio=" << formatRatio(v) << "\n"; saveConfig(); } continue; }
            if (line.rfind("comp-th",     0) == 0) { float v; if (parseFloatArg(line, "comp-th",     v, -60.0f, 0.0f)) { compressor->setThresholdDb(v);  std::cout << "  comp-th=" << v << " dB\n"; saveConfig(); } continue; }
            if (line.rfind("comp-attack", 0) == 0) { float v; if (parseFloatArg(line, "comp-attack", v, 0.1f, 500.0f)) { compressor->setAttackMs(v);     std::cout << "  comp-attack=" << v << " ms\n"; saveConfig(); } continue; }
            if (line.rfind("comp-release",0) == 0) { float v; if (parseFloatArg(line, "comp-release",v, 1.0f, 2000.0f)){ compressor->setReleaseMs(v);    std::cout << "  comp-release=" << v << " ms\n"; saveConfig(); } continue; }
            if (line.rfind("comp-gain",   0) == 0) { float v; if (parseFloatArg(line, "comp-gain",   v, -24.0f, 24.0f)){ compressor->setOutputGainDb(v); std::cout << "  comp-gain=" << v << " dB\n"; saveConfig(); } continue; }

            if (line == "sample-save" || line.rfind("sample-save ", 0) == 0) {
                std::string name = restAfter(line, "sample-save");
                std::string path = sampleFilePath(name);
                try {
                    std::vector<float> samples;
                    int rate = 0;
                    spectral->rawNoiseSnapshot(samples, rate);
                    if (samples.empty()) {
                        std::cout << "  No noise sample to save. Learn one first with 'l'.\n";
                        continue;
                    }
                    saveWavMonoFloat(path, samples, rate);
                    std::cout << "  Saved sample: " << path
                              << "  (" << samples.size() << " samples @ " << rate << " Hz)\n";
                } catch (const std::exception& ex) {
                    std::cout << "  Sample save failed: " << ex.what() << "\n";
                }
                continue;
            }

            if (line == "sample-load" || line.rfind("sample-load ", 0) == 0) {
                std::string path = sampleFilePath(restAfter(line, "sample-load"));
                if (loadNoiseFile(path, *spectral)) {
                    session.noiseFile = path;
                    std::cout << "  Loaded sample: " << path << "\n";
                    saveConfig();
                }
                continue;
            }

            if (line == "save" || line.rfind("save ", 0) == 0) {
                std::string name = restAfter(line, "save");
                if (name.empty()) { std::cout << "  usage: save <name>   (writes presets\\<name>.ini)\n"; continue; }
                try {
                    Profile p = snapshot(session, fs);
                    std::vector<float> samples;
                    int rate = 0;
                    spectral->rawNoiseSnapshot(samples, rate);
                    if (samples.empty()) {
                        p.noiseFile.clear();
                    } else {
                        // The preset gets its own copy, so re-learning later doesn't change it.
                        p.noiseFile = presetNoisePath(name);
                        saveWavMonoFloat(p.noiseFile, samples, rate);
                    }
                    writeIni(presetIniPath(name), p, true);
                    std::cout << "  Saved preset '" << name << "': " << presetIniPath(name)
                              << (samples.empty() ? "  (no noise sample)" : "  + noise sample") << "\n";
                } catch (const std::exception& ex) {
                    std::cout << "  Save failed: " << ex.what() << "\n";
                }
                continue;
            }

            if (line == "load" || line.rfind("load ", 0) == 0) {
                std::string name = restAfter(line, "load");
                if (name.empty()) {
                    std::cout << "  usage: load <name>   presets: " << listPresets() << "\n";
                    continue;
                }
                if (!applyPreset(name, session, fs)) {
                    std::cout << "  No preset named '" << name << "'. Presets: " << listPresets() << "\n";
                    continue;
                }
                std::cout << "  Loaded preset '" << name << "'\n";
                saveConfig();
                continue;
            }

            if (line == "device" || line == "devices" || line.rfind("device ", 0) == 0) {
                std::string arg = restAfter(line, line == "devices" ? "devices" : "device");
                const bool wantInput  = arg.empty() || arg == "input";
                const bool wantOutput = arg.empty() || arg == "output";
                if (!wantInput && !wantOutput) {
                    std::cout << "  usage: device | device input | device output\n";
                    continue;
                }

                std::cout << "  Stopping engine to switch device...\n";
                engine.stop();

                if (wantInput) {
                    auto inputs = AudioEngine::listInputDevices();
                    auto newId = promptDevice(inputs, "Input devices", nullptr);
                    if (!newId.empty()) {
                        session.inputId = newId;
                        session.inputName = lookupFriendlyName(inputs, newId);
                    }
                }
                if (wantOutput) {
                    auto outputs = AudioEngine::listOutputDevices();
                    auto newId = promptDevice(outputs, "Output devices", "CABLE Input");
                    if (!newId.empty()) {
                        session.outputId = newId;
                        session.outputName = lookupFriendlyName(outputs, newId);
                    }
                }

                if (startEngine()) saveConfig();
                else std::cout << "  Use 'device' to pick working devices.\n";
                continue;
            }

            std::istringstream iss(line);
            std::string cmd; iss >> cmd;
            if (cmd == "a") { float v; if (iss >> v) { spectral->setOverSubtraction(v); std::cout << "  alpha=" << v << "\n"; saveConfig(); } continue; }
            if (cmd == "b") { float v; if (iss >> v) { spectral->setSpectralFloor(v);   std::cout << "  beta=" << v << "\n";  saveConfig(); } continue; }
            if (cmd == "g") { float v; if (iss >> v) { gate->setThresholdDb(v);         std::cout << "  gate=" << v << " dB\n"; saveConfig(); } continue; }
            if (cmd == "v") { float v; if (iss >> v) { gain->setGainDb(v);              std::cout << "  gain=" << v << " dB\n"; saveConfig(); } continue; }
            if (cmd == "d") { float v; if (iss >> v) { boost->setDriveDb(v);            std::cout << "  drive=" << v << " dB\n"; saveConfig(); } continue; }
            if (cmd == "s") {
                std::string which, state; iss >> which >> state;
                const bool on = (state == "on" || state == "1" || state == "true");
                if      (which == "spectral")   spectral->setEnabled(on);
                else if (which == "gate")       gate->setEnabled(on);
                else if (which == "expander")   expander->setEnabled(on);
                else if (which == "eq")         eq->setEnabled(on);
                else if (which == "compressor") compressor->setEnabled(on);
                else if (which == "gain")       gain->setEnabled(on);
                else if (which == "boost")      boost->setEnabled(on);
                else { std::cout << "  unknown filter: " << which << "\n"; continue; }
                std::cout << "  " << which << " = " << (on ? "on" : "off") << "\n";
                saveConfig();
                continue;
            }
            std::cout << "  unknown command; type ? for help\n";
        }

        std::cout << "Stopping...\n";
        engine.stop();
        if (saveConfig())
            std::cout << "Config saved to: " << configPath << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "\nError: " << ex.what() << "\n";
        return 2;
    }
    return 0;
}
