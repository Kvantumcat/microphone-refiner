#pragma once

#include "Profile.h"

#include <string>
#include <vector>

namespace mrefiner {

// config.ini, samples/ and presets/ all live next to the executable.
std::string exeDirectory();
std::string configFilePath();
std::string sampleFilePath(const std::string& name);
std::string lastLearnedPath();
std::string presetsDir();
std::string presetIniPath(const std::string& name);
std::string presetNoisePath(const std::string& name);

// Reads an INI written by writeIni. Returns false if the file doesn't exist.
// Invalid values keep their defaults and are described in `warnings`.
bool readIni(const std::string& path, Profile& out, std::vector<std::string>& warnings);

// Reads config.ini, first writing defaults if it's missing. Returns true if it was created.
bool loadConfig(const std::string& path, Profile& out, std::vector<std::string>& warnings);

// Throws on write failure. Presets omit [Devices]: devices belong to the machine, not the sound.
void writeIni(const std::string& path, const Profile& p, bool isPreset);

} // namespace mrefiner
