# Microphone Refiner

Windows console app that cleans up a microphone signal and offers it to every
other app as a virtual microphone.

It was built to remove periodic USB interference: you record a few seconds of
the noise, and the app subtracts its spectrum from the live signal. The rest of
the chain mirrors the usual OBS mic filters, so a mic tuned once sounds the same
in Discord, Zoom, games or anything else. One-way latency is about 25-50 ms.

**Spectral Subtraction → Noise Gate → Expander → 3-Band EQ → Compressor → Gain → Boost**

## Prerequisites

1. **VB-Audio Virtual Cable**: download from https://vb-audio.com/Cable/, run
   `VBCABLE_Setup_x64.exe` as administrator, then reboot. Windows now has a
   `CABLE Input` playback and a `CABLE Output` recording device.
2. **Matching sample rates**: set your mic, `CABLE Input` and `CABLE Output` to
   48000 Hz (Control Panel → Sound → device → Properties → Advanced).
3. **C++ toolchain**, from an admin PowerShell:

   ```powershell
   winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended"
   winget install --id Kitware.CMake
   ```

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The exe is `build\Release\microphone-refiner.exe` (CMake creates other `Release`
folders too). After code changes only the second command is needed.

## Usage

```powershell
.\build\Release\microphone-refiner.exe
```

1. On first run, pick your mic as input and `CABLE Input` as output.
2. Type `l` and stay silent for a few seconds while it learns the noise.
3. In other apps, choose `CABLE Output` as the microphone.

Optional arguments: `--noise-wav <file>` uses a noise sample, `--preset <name>`
starts with a saved preset.

Type `?` in the app for all commands:

| Command | Effect |
|---|---|
| `l` | learn noise (waits `learn-delay`, records `learn-length`) |
| `learn-length <s>` / `learn-delay <s>` | learn timing |
| `a <val>` / `b <val>` | spectral aggressiveness α / floor β |
| `g <dB>` | noise gate threshold |
| `exp-ratio`, `exp-th`, `exp-attack`, `exp-release`, `exp-gain`, `exp-detect peak\|rms` | expander |
| `eq-low`, `eq-mid`, `eq-high <dB>` | EQ (100 Hz shelf, 1 kHz peak, 10 kHz shelf) |
| `comp-ratio`, `comp-th`, `comp-attack`, `comp-release`, `comp-gain` | compressor |
| `v <dB>` / `d <dB>` | clean gain / soft-clip boost drive |
| `s <filter> on\|off` | toggle `spectral`, `gate`, `expander`, `eq`, `compressor`, `gain`, `boost` |
| `device`, `device input`, `device output` | re-pick endpoints (restarts the engine) |
| `sample-save [name]` / `sample-load [name]` | archive / activate a noise sample |
| `save <name>` / `load <name>` | save / apply a preset (settings + noise, keeps the devices) |
| `reload-config` | re-read `config.ini` after editing it by hand |
| `status` | show everything |
| `q` | quit |

Ratios are entered as the first number of `N:1` (`comp-ratio 4` → `4.00:1`), as in OBS.

## Configuration

Everything is stored next to the exe, so the `build\Release\` folder can be
copied to another PC as is.

- `config.ini`: all settings, the chosen devices and the active noise sample.
  Created with defaults when missing and rewritten after every command that
  changes a setting.
- To edit `config.ini` while the app runs, save the file and type
  `reload-config`. Until then the app won't overwrite your edits. Invalid values
  are reported and fall back to their defaults.
- `samples\`: `last-learned.wav` after every `l`, plus the `sample-save` files.
- `presets\`: `save <name>` writes `<name>.ini` and `<name>.wav`.

Problems? See [docs/troubleshooting.md](docs/troubleshooting.md).

## License

Copyright (C) 2026 Intelin. Licensed under the GNU General Public License v3.0
or later, see [LICENSE](LICENSE).
