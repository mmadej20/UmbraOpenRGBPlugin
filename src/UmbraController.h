/*---------------------------------------------------------*\
| UmbraController.h                                         |
|                                                           |
|   USB HID transport layer for the AsiaHorse UMBRA         |
|   ARGB Hub / ROBOBLOQ USBFAN controller (VID 0x1A86,      |
|   PID 0xFE05).                                            |
|                                                           |
|   Protocol framing/parsing lives in UmbraProtocol; this   |
|   class owns only HID IO, retries and frame pacing.       |
|                                                           |
|   Protocol reverse-engineered by the SignalRGB plugin     |
|   project:                                                |
|   https://github.com/maihcx/AsiaHorse-Umbra-ARGB-Hub-SignalRGB-Plugin  |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "UmbraProtocol.h"

struct hid_device_;
typedef struct hid_device_ hid_device;

class UmbraController
{
public:
    /*-----------------------------------------------------*\
    | HID device information returned by enumeration        |
    \*-----------------------------------------------------*/
    struct DeviceInfo
    {
        std::string path;
        std::string serial;
    };

    /*-----------------------------------------------------*\
    | Topology information for a single physical ARGB port  |
    \*-----------------------------------------------------*/
    using PortInfo = UmbraProtocol::TopologyRecord;

    static const unsigned int NUM_PORTS =
        (unsigned int)UmbraProtocol::NUM_PORTS;

    /*-----------------------------------------------------*\
    | Enumerate all connected UMBRA hubs                    |
    \*-----------------------------------------------------*/
    static std::vector<DeviceInfo> EnumerateDevices();

    /*-----------------------------------------------------*\
    | Initialize hidapi once for the process                |
    \*-----------------------------------------------------*/
    static bool InitHidapi();

    UmbraController(const std::string& device_path, const std::string& serial);
    ~UmbraController();

    /* Non-copyable (owns a HID handle) */
    UmbraController(const UmbraController&)             = delete;
    UmbraController& operator=(const UmbraController&)  = delete;

    /*-----------------------------------------------------*\
    | Connection management                                 |
    \*-----------------------------------------------------*/
    bool Initialize();
    void Disconnect();
    bool IsConnected();

    /*-----------------------------------------------------*\
    | Re-reads the port topology on an already-initialized  |
    | transport. Used when a hub first came up with zero    |
    | populated ports and devices were connected later.     |
    | Frame pacing is recomputed, which matters when the    |
    | hub goes 0 -> N LEDs (packet count changes).          |
    | On IO failure the transport is marked disconnected so |
    | the next attempt performs a full reconnect.           |
    \*-----------------------------------------------------*/
    bool RefreshTopology();

    /*-----------------------------------------------------*\
    | Device information                                    |
    \*-----------------------------------------------------*/
    const std::string& GetPath() const      { return path_; }
    const std::string& GetSerial() const    { return serial_; }
    std::string GetLocation() const;

    /*-----------------------------------------------------*\
    | Topology                                              |
    \*-----------------------------------------------------*/
    unsigned int GetPortLedCount(unsigned int port_index);
    unsigned int GetTotalLedCount();
    std::vector<PortInfo> GetPopulatedPorts();

    /*-----------------------------------------------------*\
    | Direct RGB streaming                                  |
    |                                                       |
    | rgb is a packed RGB byte buffer with exactly          |
    | GetTotalLedCount() * 3 bytes, in populated-port       |
    | order (the same order as OpenRGB's flat color array). |
    | Frames are rate limited to stay below the ~330        |
    | writes/sec the hub's HID stack is known to sustain.   |
    \*-----------------------------------------------------*/
    bool SendFrame(const unsigned char* rgb, unsigned int led_count);

private:
    /*-----------------------------------------------------*\
    | Timing constants                                      |
    \*-----------------------------------------------------*/
    static constexpr unsigned int READ_TIMEOUT_MS      = 100;
    static constexpr unsigned int READ_ATTEMPTS        = 3;
    static constexpr unsigned int READ_RETRY_DELAY_MS  = 20;
    static constexpr unsigned int DRAIN_MAX_REPORTS    = 16;

    /* Sustained write-rate ceiling (writes/sec)             */
    static constexpr unsigned int TARGET_WRITES_PER_SEC = 330;

    /*-----------------------------------------------------*\
    | IO helpers                                            |
    \*-----------------------------------------------------*/
    bool WritePayload(const unsigned char* payload, size_t length);

    /* Blocking read; returns bytes read or -1               */
    int ReadReport(unsigned char* report, unsigned int timeout_ms);

    /* Non-blocking drain helper                             */
    void DrainInputReports();

    bool SendNativeCommand(const uint8_t* body, size_t body_length);

    bool QueryStatus();
    bool QueryTopology();
    bool EnableSoftwareControl();
    void CloseInternal();

    /* Recomputes frame pacing from total_led_count_        */
    void UpdateFramePacingLocked();

    /*-----------------------------------------------------*\
    | Members                                               |
    \*-----------------------------------------------------*/
    std::string         path_;
    std::string         serial_;

    hid_device*         dev_;

    PortInfo            ports_[NUM_PORTS];
    unsigned int        total_led_count_;
    bool                initialized_;

    /* Frame pacing                                          */
    std::chrono::steady_clock::time_point last_frame_time_;
    double              frame_interval_ms_;
    bool                frame_paced_;

    /* Serializes all HID IO and protocol state              */
    std::mutex          io_mutex_;
};
