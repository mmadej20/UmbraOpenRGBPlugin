# Golden captures from real hardware

Unit tests in `umbra_protocol_test.cpp` verify that the parser accepts the
format we *believe* the UMBRA uses. They cannot prove that belief - only a
capture from a real controller can. This is exactly what happened with the
SignalRGB plugin: two commands (`0xFC`, `FB 64`) turned out to mean something
completely different than first assumed.

If you own an UMBRA hub, please contribute a golden capture.

## How to capture

1. Install [USBPcap](https://desowin.org/usbpcap/) and Wireshark.
2. Close the official AsiaHorse software, SignalRGB and OpenRGB.
3. Start capturing on the USB host controller the hub is plugged into
   (filter later by `usb.device_address == X` or simply `usb.idVendor == 0x1a86`).
4. Plug the hub in (or trigger a re-enumeration) and let it settle for ~5 s.
5. Stop the capture, export as `umbra.pcapng`.

## What to extract

We need **raw HID input reports** (device -> host), 65 bytes each
(report ID `0x00` + 64-byte payload).

### Topology response

Find an IN transfer whose payload starts with:

```
52 42 xx 00 01 FF ...
```

(`52 42` = "RB", length byte, `00`, echo of the `01 FF` topology query).
Copy the full 65-byte report.

### Status response (optional but useful)

Payload starting with:

```
52 42 1A 00 00 ...
```

(length byte `0x1A` = 26, command echo `00`).

## How to add it to the tests

Create `tests/golden_topology_response.h`:

```cpp
#pragma once

#include <cstddef>

static const unsigned char REAL_TOPOLOGY_RESPONSE[] =
{
    // paste the exact 65 bytes here, e.g.
    // 0x00, 0x52, 0x42, 0x39, 0x00, 0x01, 0xFF, ...
};

static const size_t REAL_TOPOLOGY_RESPONSE_SIZE =
    sizeof(REAL_TOPOLOGY_RESPONSE);
```

The test binary picks this file up automatically via `__has_include`
and validates `ParseTopology()` against the real bytes - layout is printed
on success. Same pattern works for a status frame if you add it yourself.

The file must never contain invented data; it is only generated from a real
USBPcap dump.
