# UmbraOpenRGBPlugin

An **OpenRGB** plugin (Plugin API v4, OpenRGB 1.0rc3) adding support for the
**AsiaHorse UMBRA ARGB Hub / ROBOBLOQ USBFAN** (`USB VID 0x1A86`, `PID 0xFE05`).

Implementation is based on the protocol reverse-engineered in the SignalRGB
plugin: [maihcx/AsiaHorse-Umbra-ARGB-Hub-SignalRGB-Plugin](https://github.com/maihcx/AsiaHorse-Umbra-ARGB-Hub-SignalRGB-Plugin).

## What it does

* detects UMBRA over HID and registers it as a standard OpenRGB device,
* queries port topology (`01 FF`) — every populated physical port becomes an
  independent zone `ARGB Port 01`…`ARGB Port 10` (Direct mode only),
* streams colors with `88 [count] [index] [20×RGB] [checksum]` packets,
* rate limits writes to ~330/s (vendor software runs at ~378/s),
* deliberately **does not send** `FB 64` — `0xFB` is `ARGB_pwm_speed` in the
  firmware (fan PWM), so RGB initialization consists of `FD 01` only.

The device as seen by OpenRGB:

```
AsiaHorse UMBRA
├── ARGB Port 01 [14 LEDs]
├── ARGB Port 02 [14 LEDs]
├── ...
└── ARGB Port 10 [24 LEDs]
```

Each port is an independent zone — works with the Effects Plugin, Hardware
Sync, etc.

## What it does NOT do

* **PWM/RPM** — fan speed control stays with the official AsiaHorse software /
  Fan Control. The per-port encoding of command `0xFB` is not publicly
  confirmed yet.
* Re-wiring ports requires a re-scan (the *Re-scan devices* button on the
  plugin tab) or an OpenRGB restart.

## Requirements

* OpenRGB **1.0rc3** (Plugin API v4),
* OpenRGB sources of the **same version** as the binary you build/run
  (the plugin compiles against its headers and links `RGBController.cpp`),
* Qt5 (the same Qt major version as your OpenRGB build), hidapi (fetched
  automatically by CMake by default),
* the same toolchain as OpenRGB: official Windows releases are **MSYS2 MinGW64**
  — build the plugin with the same compiler family. libstdc++/libgcc/winpthreads
  are linked statically into the plugin, so it only depends on system DLLs and
  the Qt5 libraries already shipped next to OpenRGB.exe.

## Build (MSYS2 MinGW64)

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
                   mingw-w64-x86_64-ninja mingw-w64-x86_64-qt5-base git

git clone https://github.com/mmadej20/UmbraOpenRGBPlugin.git
cd UmbraOpenRGBPlugin
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DOPENRGB_SOURCE_DIR=/c/src/OpenRGB \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5   # only needed for CMake >= 4
cmake --build build -j
```

`OPENRGB_SOURCE_DIR` = directory with OpenRGB sources matching your
OpenRGB.exe version (e.g. tag `release_candidate_1.0rc3.1`).

The pure protocol layer is unit tested without hardware attached
(standalone - no OpenRGB sources, Qt or hidapi required):

```bash
cmake -B build -DUMBRA_BUILD_PLUGIN=OFF
cmake --build build && ctest --test-dir build
```

If you own the hub, please add a golden capture from real hardware -
see `tests/GOLDEN_CAPTURES.md`. That is the only way to verify the parser
against actual device bytes rather than our assumptions.

Qt major version must match the host application: official OpenRGB releases
use Qt5. Use `-DUMBRA_PLUGIN_FORCE_QT6=ON` only for OpenRGB hosts that were
themselves built against Qt6.

Runtime DLLs: official Windows OpenRGB packages ship **no** MinGW runtime
(`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`) next to
OpenRGB.exe - the host links them statically, and so does this plugin.
Only system DLLs and the Qt libraries already present in the host folder
are needed at load time. If you build your own OpenRGB with dynamic
runtimes, remove the `-static-lib*` flags in CMakeLists.txt so both modules
share one libstdc++.

## Installation

Copy `UmbraOpenRGBPlugin.dll` into either:

* `<folder with OpenRGB.exe>\plugins\`, **or**
* `%APPDATA%\OpenRGB\plugins\`

and start OpenRGB. The hub appears as an `AsiaHorse UMBRA` device.
The **UMBRA** tab (Devices) shows detection status and the port layout.

## Troubleshooting

| Problem | Solution |
|---|---|
| Device does not appear | Close the official AsiaHorse software (tray too) and press *Re-scan devices*. The plugin also retries after every OpenRGB detection run |
| Ports report 0 LEDs | Topology is re-read on every detection run. Connect devices and press *Re-scan devices*; if still 0 — run the official software once (LED auto-detection) or send command `07` with a diagnostic tool |
| Colors "jump" between ports | Do not mix this plugin with SignalRGB/official software — only one host can hold the HID endpoint open |
| Hub detected but initialization fails | First suspect: response format differs from our assumptions (frame length / checksum). Capture a real response with USBPcap and add it as a golden test — see `tests/GOLDEN_CAPTURES.md` |
| Plugin does not load | Check that your OpenRGB has API v4 (OpenRGB 1.0.x) and that the DLL was built with the same toolchain family as OpenRGB |

## Structure

```
src/UmbraProtocol.{h,cpp}        pure protocol layer (framing/parsing, unit-testable)
src/UmbraController.{h,cpp}      USB HID transport layer (IO, retries, pacing)
src/RGBController_Umbra.{h,cpp}  OpenRGB device abstraction (zones/mode)
src/UmbraPlugin.{h,cpp}          Plugin API v4 entry point (detection, registration)
src/UmbraWidget.{h,cpp}          Status panel + re-scan
tests/umbra_protocol_test.cpp    Unit tests for the protocol layer
docs/UmbraProtocol.md            Protocol notes
```

## License

GPL-2.0-or-later (same as OpenRGB).
