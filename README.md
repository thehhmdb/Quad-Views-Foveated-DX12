# Foveated Rendering via Quad Views

In layperson's terms:

This software lets you use Eye-Tracked Foveated Rendering (sometimes referred to as Dynamic Foveated Rendering) with your Pimax Crystal, Meta Quest Pro, and other headsets supporting eye tracking via OpenXR in games using the [quad views rendering](https://github.com/mbucchia/Quad-Views-Foveated/wiki/What-is-Quad-Views-rendering%3F) technique like Digital Combat Simulation (DCS) and Pavlov VR.

In technical terms:

This software enables OpenXR apps developed with [`XR_VARJO_quad_views`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#XR_VARJO_quad_views) and optionally [`XR_VARJO_foveated_rendering`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#XR_VARJO_foveated_rendering) to be used on platforms that do not typically support those extensions. It composes each quad view projection layer into a stereo projection layer, and uses the eye tracking support on the device to make the inner projection views follow the eye gaze.

DISCLAIMER: This software is distributed as-is, without any warranties or conditions of any kind. Use at your own risks.

# Details and instructions on the [wiki](https://github.com/mbucchia/Quad-Views-Foveated/wiki)!

## Setup

Download the latest version from the [Releases page](https://github.com/mbucchia/Quad-Views-Foveated/releases). Find the installer program under **Assets**, file `Quad-Views-Foveated-<version>.msi`.

More information on the [wiki](https://github.com/mbucchia/Quad-Views-Foveated/wiki)!

## Configuration

Settings can be customized by creating a `settings.cfg` file in `%LocalAppData%\Quad-Views-Foveated\`. Do not copy the bundled `settings.cfg` from the repo — start from a blank file and override only the keys you need. See the repo-root `settings.cfg` for all available options and their defaults.

Per-headset and per-app overrides are supported via `[SectionName]` headers (e.g. `[SteamVR]`, `[exe:Contractors]`). Common knobs include `peripheral_multiplier`, `focus_multiplier`, `horizontal_focus_section`, `vertical_focus_section`, `peripheral_lod_bias`, `radial_lod_max_boost`, `sharpen_focus_view`, and `turbo_mode`.

## Troubleshooting

The log file is at `%LocalAppData%\Quad-Views-Foveated\Quad-Views-Foveated.log`. Set `log_level=Debug` or `log_level=Verbose` in your `settings.cfg` for more detail.

## Building and Running Tests

This project includes an automated test suite (Google Test) covering ViewManager math, FramePipeline state management, and EyeTracker cache behavior.

### Prerequisites

- Visual Studio 2022 (v18) with MSBuild
- C++ desktop development workload

### Build

From a **Git Bash** shell, use `MSYS_NO_PATHCONV=1` to prevent path conversion issues. Use `vswhere` to locate MSBuild automatically (works for any VS 2022 edition):

```bash
MSYS_NO_PATHCONV=1 "$(vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild/**/Bin/MSBuild.exe)" XR_APILAYER_MBUCCHIA_quad_views_foveated.sln /p:Configuration=Release /p:Platform=x64 /t:QuadViewsFoveatedTests /m
```

Alternatively, open a **Developer Command Prompt for VS 2022** (or run `vcvarsall.bat x64`) and use MSBuild directly:

```cmd
MSBuild.exe XR_APILAYER_MBUCCHIA_quad_views_foveated.sln /p:Configuration=Release /p:Platform=x64 /t:QuadViewsFoveatedTests /m
```

To build the layer DLL itself (not just the tests), omit the `/t` target:

```bash
MSYS_NO_PATHCONV=1 "$(vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild/**/Bin/MSBuild.exe)" XR_APILAYER_MBUCCHIA_quad_views_foveated.sln /p:Configuration=Release /p:Platform=x64 /m
```

### Tests

```bash
./x64/Release/QuadViewsFoveatedTests.exe
```

For verbose output:

```bash
./x64/Release/QuadViewsFoveatedTests.exe --gtest_brief=0 --gtest_print_time=1
```

To run a specific test suite:

```bash
./x64/Release/QuadViewsFoveatedTests.exe --gtest_filter="ViewMathTest.*"
```

## Versioning

The project version is defined in a single file, `version.info`:

```
major=1
minor=2
patch=3
```

To bump the version, run the top-level sync script:

```bash
python update_version.py 1.2.4
```

This updates `version.info` and all five files that embed the version number:

| File | What it holds |
|---|---|
| `openxr-api-layer/version.h` | C++ `LayerVersionMajor/Minor/Patch` constants |
| `openxr-api-layer/resource.rc` | Windows `FILEVERSION` / `PRODUCTVERSION` strings |
| `CustomSetup/Properties/AssemblyInfo.cs` | .NET `AssemblyVersion` / `AssemblyFileVersion` |
| `installer/installer.vdproj` | MSI `ProductVersion` |
| `installer/README.rtf` | Installer splash text (binary RTF, updated length-safe) |

To check whether all files are in sync without modifying anything:

```bash
python update_version.py --check
```

This exits with code `0` if everything matches `version.info`, or `1` if any file is out of sync.

## Architecture

For a detailed technical overview of the layer's internals — pipeline stages, CRTP compositor design, descriptor heap layout, shader passes, and testing infrastructure — see [ARCHITECTURE.md](ARCHITECTURE.md).

## Donate

Donations are welcome and totally optional. Please use [my GitHub sponsorship page](https://github.com/sponsors/mbucchia) to make one-time or recurring donations!

Thank you!

## Special thanks

Thanks to my beta testers for helping throughout development and release (in alphabetical order):

- BARRACUDAS
- edmuss
- MastahFR
- mfrisby
- Omniwhatever
- xMcCARYx
