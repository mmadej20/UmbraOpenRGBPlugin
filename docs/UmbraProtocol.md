# AsiaHorse UMBRA / ROBOBLOQ USBFAN — USB HID protocol notes

Reverse-engineered transport used by the [SignalRGB plugin](https://github.com/maihcx/AsiaHorse-Umbra-ARGB-Hub-SignalRGB-Plugin)
(maihcx) and reused by this OpenRGB plugin.

## Device

| Property    | Value      |
|-------------|------------|
| USB VID     | `0x1A86`   |
| USB PID     | `0xFE05`   |
| Interface   | `0`        |
| Usage Page  | `0xFF00`   |
| Usage       | `0x0001`   |
| ARGB ports  | 10         |

Communication is 64-byte HID reports with report ID `0`
(65 bytes on the wire including the report ID byte).

## Command framing

Native commands:

```
52 42 [length] 00 [body...] [checksum]
```

* `[length]` counts every byte of the frame **including** the trailing checksum
  (i.e. `4 + body_length + 1`)
* checksum is an additive 8-bit sum: `sum(all bytes before the checksum) & 0xFF`

Responses echo `52 42`, a length byte, `00`, the original command body, the
response payload and a checksum. Responses are located by scanning the input
report for `52 42` followed by the expected command body at offset +4.

## Known commands

| Body          | Meaning                                                                 | Verified |
|---------------|-------------------------------------------------------------------------|----------|
| `00`          | Hub status query. Boot mode at frame+23 (`0xFF` memory / `0x01` fixed), self-check at frame+24 (`0xFF` on / `0x01` off), checksum at frame+25 | yes |
| `01 FF`       | Port topology query                                                     | yes      |
| `07`          | `argb_auto_rgb_num` — re-detect LED counts (device may briefly disconnect). Earlier revisions of the SignalRGB plugin mistakenly called this "auto detect" via `0xFC` — **`0xFC` is actually `argb_fan_save`: save to flash + reboot, do not send it casually** | yes |
| `0B <val>`    | Boot mode: `0xFF` = memory (last streamed state), `0x01` = fixed effect | partial  |
| `0C <val>`    | Boot self-check: `0xFF` enabled / `0x01` disabled                       | partial  |
| `FA`          | Reset device (may temporarily disconnect)                               | unverified |
| `FB <speed>`  | `ARGB_pwm_speed` — fan PWM duty. Global format not fully confirmed. **Do NOT send during RGB init or you override the user's fan curve** (the old `FB 64` startup sequence was removed from the SignalRGB plugin for exactly this reason) | command id confirmed, per-port encoding unknown |
| `FD 01`       | Enter software-controlled RGB streaming mode                            | yes      |
| `88 ...`      | Direct RGB streaming packet (see below)                                 | yes      |

## Port topology

Request body: `01 FF`.

The response contains 10 records, one per physical port:

```
[LED count] [unknown] [unknown] [port index] [unknown]
```

Records start at `frame_start + 6`, each record is 5 bytes. The `port index`
byte places the record at its physical port; LED counts are authoritative for
building the Direct RGB stream. Ports reporting `0` LEDs contribute no entries
to the stream.

## Direct RGB streaming

```
Byte 0     = 0x88
Byte 1     = total packet count
Byte 2     = 1-based packet index
Byte 3..62 = 20 x (R, G, B)
Byte 63    = additive checksum over bytes 0..62
```

* one packet carries exactly 20 LEDs
* `packet count = ceil(total_leds / 20)`
* colors are packed in physical port order; ports with 0 LEDs reserve nothing
* the final packet is zero-padded when the total is not a multiple of 20

### Example

Ports `14, 14, 14, 0, 14` → 56 physical LEDs → `ceil(56/20) = 3` packets,
60 stream slots (4 black padding entries).

## Timing

The vendor software sustains roughly 378 HID writes/sec. To stay safely below
that ceiling this plugin paces frames so that

```
frames/sec <= TARGET_WRITES_PER_SEC / packets_per_frame   (330 writes/sec ceiling)
```

i.e. a 200-LED setup (10 packets/frame) is limited to ~33 fps.

## Initialization sequence used by this plugin

```
QueryStatus()            00                      (3 attempts, 100 ms read timeout)
QueryTopology()          01 FF                   (3 attempts)
EnableSoftwareControl()  FD 01
```

Deliberately **not** sent: `FB 64` (fan PWM override), `FC` (save-to-flash +
reboot), `FA` (reset).
