/*---------------------------------------------------------*\
| UmbraController.h                                         |
|                                                           |
|   USB HID transport layer for the AsiaHorse UMBRA         |
|   ARGB Hub / ROBOBLOQ USBFAN controller (VID 0x1A86,      |
|   PID 0xFE05).                                            |
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

struct hid_device;

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
    struct PortInfo
    {
        uint8_t     index;          /* physical port index, 0-based   */
        uint8_t     led_count;      /* LEDs reported by controller    */
        uint8_t     unknown1;       /* topology record field, unused  */
        uint8_t     unknown2;       /* topology record field, unused  */
        uint8_t     unknown4;       /* topology record field, unused  */
    };

    static constexpr unsigned int NUM_PORTS = 10;

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
    | Protocol constants                                    |
    \*-----------------------------------------------------*/
    static constexpr size_t     HID_PAYLOAD_SIZE        = 64;   /* bytes per HID output report payload               */
    static constexpr size_t     HID_REPORT_SIZE         = 65;   /* report ID byte + payload                          */

    static constexpr uint8_t    NATIVE_HEADER_0         = 0x52;
    static constexpr uint8_t    NATIVE_HEADER_1         = 0x42;

    static constexpr uint8_t    DIRECT_RGB_HEADER       = 0x88;
    static constexpr unsigned int LEDS_PER_PACKET      = 20;

    /* Native command bodies                                 */
    static constexpr uint8_t CMD_STATUS_BODY[]          = { 0x00 };       /* query hub status            */
    static constexpr uint8_t CMD_PORT_QUERY_BODY[]      = { 0x01, 0xFF }; /* query port topology         */
    static constexpr uint8_t CMD_SOFTWARE_CONTROL_BODY[]= { 0xFD, 0x01 }; /* enter software RGB mode     */

    /* Status response layout (offsets relative to frame)    */
    static constexpr size_t     STATUS_BOOT_MODE_OFFSET = 23;
    static constexpr size_t     STATUS_SELF_CHECK_OFFSET= 24;
    static constexpr size_t     STATUS_CHECKSUM_OFFSET  = 25;

    /* Topology response layout                              */
    static constexpr size_t     PORT_RECORDS_OFFSET     = 6;    /* from frame start to first 5-byte record           */
    static constexpr size_t     PORT_RECORD_SIZE        = 5;

    static constexpr unsigned int READ_TIMEOUT_MS      = 100;
    static constexpr unsigned int READ_ATTEMPTS        = 3;
    static constexpr unsigned int READ_RETRY_DELAY_MS  = 20;

    /* Sustained write-rate ceiling (writes/sec)             */
    static constexpr unsigned int TARGET_WRITES_PER_SEC = 330;

    /*-----------------------------------------------------*\
    | Protocol helpers                                      |
    \*-----------------------------------------------------*/
    static uint8_t Checksum(const unsigned char* data, size_t length);

    bool WritePayload(const unsigned char* payload, size_t length);
    bool ReadReport(unsigned char* report);

    bool SendNativeCommand(const uint8_t* body, size_t body_length);

    /* Searches a raw input report for a native response     */
    /* frame whose command body matches cmd                  */
    int FindNativeResponse(const unsigned char* report, size_t report_size,
                           const uint8_t* cmd, size_t cmd_length) const;

    bool QueryStatus();
    bool QueryTopology();
    bool EnableSoftwareControl();
    void CloseInternal();

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
