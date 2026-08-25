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
* Qt5 (Qt6 also works), hidapi (fetched automatically by CMake by default),
* the same toolchain as OpenRGB: official Windows builds are **MSYS2 MinGW64**
  — build the plugin with the same compiler (STL objects cross the DLL
  boundary; do not link libstdc++ statically).

## Build (MSYS2 MinGW64)

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
                   mingw-w64-x86_64-qt5-base git

git clone https://github.com/<you>/UmbraOpenRGBPlugin.git
cd UmbraOpenRGBPlugin
cmake -B build -G "MinGW Makefiles" \
      -DOPENRGB_SOURCE_DIR=/c/src/OpenRGB \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`OPENRGB_SOURCE_DIR` = directory with OpenRGB sources matching your
OpenRGB.exe version (e.g. tag `release_candidate_1.0rc3.1`).

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
| Ports report 0 LEDs | Topology is read once during initialization. Connect devices and re-scan; if still 0 — run the official software once (LED auto-detection) or send command `07` with a diagnostic tool |
| Colors "jump" between ports | Do not mix this plugin with SignalRGB/official software — only one host can hold the HID endpoint open |
| Plugin does not load | Check that your OpenRGB has API v4 (OpenRGB 1.0.x) and that the DLL was built with the same toolchain as OpenRGB |

## Structure

```
src/UmbraController.{h,cpp}      USB HID transport layer (protocol)
src/RGBController_Umbra.{h,cpp}  OpenRGB device abstraction (zones/mode)
src/UmbraPlugin.{h,cpp}          Plugin API v4 entry point (detection, registration)
src/UmbraWidget.{h,cpp}          Status panel + re-scan
docs/UmbraProtocol.md            Protocol notes
```

## License

GPL-2.0-or-later (same as OpenRGB).
