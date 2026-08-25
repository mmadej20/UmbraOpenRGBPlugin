/*---------------------------------------------------------*\
| UmbraController.cpp                                       |
|                                                           |
|   USB HID transport layer for the AsiaHorse UMBRA         |
|   ARGB Hub / ROBOBLOQ USBFAN controller.                  |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "UmbraController.h"

#include <hidapi/hidapi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

/*---------------------------------------------------------*\
| UMBRA / ROBOBLOQ USBFAN USB identifiers                  |
\*---------------------------------------------------------*/
static const unsigned int UMBRA_VID = 0x1A86;
static const unsigned int UMBRA_PID = 0xFE05;

static const unsigned int UMBRA_INTERFACE  = 0;
static const unsigned int UMBRA_USAGE_PAGE = 0xFF00;
static const unsigned int UMBRA_USAGE      = 0x0001;

/*---------------------------------------------------------*\
| Process-wide hidapi initialization                        |
\*---------------------------------------------------------*/
bool UmbraController::InitHidapi()
{
    static std::once_flag init_once;
    static bool initialized = false;

    std::call_once(init_once, []()
    {
        initialized = (hid_init() == 0);
    });

    return initialized;
}

std::vector<UmbraController::DeviceInfo> UmbraController::EnumerateDevices()
{
    std::vector<DeviceInfo> devices;

    if(!InitHidapi())
    {
        return devices;
    }

    hid_device_info* infos = hid_enumerate(UMBRA_VID, UMBRA_PID);

    for(hid_device_info* info = infos; info != nullptr; info = info->next)
    {
        /*-------------------------------------------------*\
        | Without a device path the hub cannot be opened    |
        | later, so skip such entries entirely              |
        \*-------------------------------------------------*/
        if(info->path == nullptr)
        {
            continue;
        }

        /*-------------------------------------------------*\
        | The RGB endpoint is vendor-defined collection on  |
        | interface 0 (usage page 0xFF00, usage 0x0001).    |
        | Some backends do not populate usage fields, so an |
        | unset usage page is tolerated as long as the      |
        | interface matches.                                |
        \*-------------------------------------------------*/
        if(info->interface_number != (int)UMBRA_INTERFACE)
        {
            continue;
        }

        bool usage_matches = false;

        if(info->usage_page == UMBRA_USAGE_PAGE)
        {
            usage_matches = ((unsigned short)info->usage == UMBRA_USAGE)
                         || (info->usage == 0);
        }
        else if(info->usage_page == 0)
        {
            usage_matches = true;
        }

        if(!usage_matches)
        {
            continue;
        }

        /*-------------------------------------------------*\
        | hidapi may report the same collection multiple    |
        | times - deduplicate by path                       |
        \*-------------------------------------------------*/
        bool already_listed = false;

        for(const DeviceInfo& existing : devices)
        {
            if(existing.path == info->path)
            {
                already_listed = true;
                break;
            }
        }

        if(already_listed)
        {
            continue;
        }

        DeviceInfo device;
        device.path   = info->path;
        device.serial = (info->serial != nullptr) ? info->serial : "";
        devices.push_back(device);
    }

    hid_free_enumeration(infos);

    return devices;
}

/*---------------------------------------------------------*\
| Construction / destruction                                |
\*---------------------------------------------------------*/
UmbraController::UmbraController(const std::string& device_path, const std::string& serial)
    : path_(device_path), serial_(serial), dev_(nullptr),
      total_led_count_(0), initialized_(false),
      last_frame_time_(), frame_interval_ms_(0.0), frame_paced_(false)
{
    memset(ports_, 0, sizeof(ports_));
}

UmbraController::~UmbraController()
{
    Disconnect();
}

std::string UmbraController::GetLocation() const
{
    char buf[32];
    snprintf(buf, sizeof(buf), "HID %04X:%04X", UMBRA_VID, UMBRA_PID);

    std::string location = buf;

    if(!path_.empty())
    {
        location += " ";
        location += path_;
    }

    return location;
}

/*---------------------------------------------------------*\
| Connection management                                     |
\*---------------------------------------------------------*/
bool UmbraController::Initialize()
{
    std::lock_guard<std::mutex> guard(io_mutex_);

    if(dev_ != nullptr && initialized_)
    {
        return true;
    }

    CloseInternal();

    if(!InitHidapi())
    {
        return false;
    }

    dev_ = hid_open_path(path_.c_str());

    if(dev_ == nullptr)
    {
        return false;
    }

    hid_set_nonblocking(dev_, 0);

    /*-----------------------------------------------------*\
    | Handshake: confirm hub responds, read port topology,  |
    | then take over software RGB control.                  |
    |                                                       |
    | NOTE: deliberately NOT sending FB 64 here. In the     |
    | vendor firmware 0xFB is ARGB_pwm_speed (fan PWM       |
    | control); sending it would override the user's fan    |
    | settings. Software streaming only requires FD 01.     |
    \*-----------------------------------------------------*/
    if(!QueryStatus())
    {
        CloseInternal();
        return false;
    }

    if(!QueryTopology())
    {
        CloseInternal();
        return false;
    }

    if(!EnableSoftwareControl())
    {
        CloseInternal();
        return false;
    }

    /*-----------------------------------------------------*\
    | Frame pacing interval: each frame needs one write     |
    | per packet; stay below TARGET_WRITES_PER_SEC total    |
    \*-----------------------------------------------------*/
    unsigned int packets = (total_led_count_ > 0)
                         ? ((total_led_count_ + LEDS_PER_PACKET - 1) / LEDS_PER_PACKET)
                         : 0;

    frame_interval_ms_ = (packets > 0)
                       ? (double)((packets * 1000u + TARGET_WRITES_PER_SEC - 1u) / TARGET_WRITES_PER_SEC)
                       : 0.0;

    frame_paced_        = false;
    initialized_        = true;

    return true;
}

void UmbraController::Disconnect()
{
    std::lock_guard<std::mutex> guard(io_mutex_);
    CloseInternal();
}

bool UmbraController::IsConnected()
{
    std::lock_guard<std::mutex> guard(io_mutex_);
    return (dev_ != nullptr) && initialized_;
}

void UmbraController::CloseInternal()
{
    if(dev_ != nullptr)
    {
        hid_close((hid_device*)dev_);
        dev_ = nullptr;
    }

    initialized_     = false;
    total_led_count_ = 0;
    memset(ports_, 0, sizeof(ports_));
}

/*---------------------------------------------------------*\
| Topology accessors                                        |
\*---------------------------------------------------------*/
unsigned int UmbraController::GetPortLedCount(unsigned int port_index)
{
    if(port_index >= NUM_PORTS)
    {
        return 0;
    }

    std::lock_guard<std::mutex> guard(io_mutex_);
    return ports_[port_index].led_count;
}

unsigned int UmbraController::GetTotalLedCount()
{
    std::lock_guard<std::mutex> guard(io_mutex_);
    return total_led_count_;
}

std::vector<UmbraController::PortInfo> UmbraController::GetPopulatedPorts()
{
    std::lock_guard<std::mutex> guard(io_mutex_);

    std::vector<PortInfo> populated;

    for(unsigned int i = 0; i < NUM_PORTS; i++)
    {
        if(ports_[i].led_count > 0)
        {
            populated.push_back(ports_[i]);
        }
    }

    return populated;
}

/*---------------------------------------------------------*\
| Low-level IO                                              |
\*---------------------------------------------------------*/
uint8_t UmbraController::Checksum(const unsigned char* data, size_t length)
{
    uint8_t sum = 0;

    for(size_t i = 0; i < length; i++)
    {
        sum = (uint8_t)(sum + data[i]);
    }

    return sum;
}

bool UmbraController::WritePayload(const unsigned char* payload, size_t length)
{
    if(dev_ == nullptr || length > HID_PAYLOAD_SIZE)
    {
        return false;
    }

    unsigned char report[HID_REPORT_SIZE];
    memset(report, 0, sizeof(report));
    memcpy(&report[1], payload, length);

    int written = hid_write((hid_device*)dev_, report, sizeof(report));

    return (written == (int)sizeof(report));
}

bool UmbraController::ReadReport(unsigned char* report)
{
    if(dev_ == nullptr)
    {
        return false;
    }

    int bytes = hid_read_timeout((hid_device*)dev_, report, HID_REPORT_SIZE, READ_TIMEOUT_MS);

    return (bytes > 0);
}

bool UmbraController::SendNativeCommand(const uint8_t* body, size_t body_length)
{
    unsigned char payload[HID_PAYLOAD_SIZE];

    memset(payload, 0, sizeof(payload));

    /*-----------------------------------------------------*\
    | Frame: 52 42 [length] 00 [body...] [checksum]         |
    | length counts every byte of the frame including the   |
    | trailing checksum byte                                |
    \*-----------------------------------------------------*/
    size_t frame_length = 4 + body_length + 1;

    payload[0] = NATIVE_HEADER_0;
    payload[1] = NATIVE_HEADER_1;
    payload[2] = (uint8_t)frame_length;
    payload[3] = 0x00;

    if(body_length > 0)
    {
        memcpy(&payload[4], body, body_length);
    }

    payload[frame_length - 1] = Checksum(payload, frame_length - 1);

    return WritePayload(payload, frame_length);
}

int UmbraController::FindNativeResponse(const unsigned char* report, size_t report_size,
                                        const uint8_t* cmd, size_t cmd_length) const
{
    if(report_size < 4 + cmd_length + 1)
    {
        return -1;
    }

    for(size_t start = 0; start + 4 + cmd_length + 1 <= report_size; start++)
    {
        if(report[start] != NATIVE_HEADER_0 || report[start + 1] != NATIVE_HEADER_1)
        {
            continue;
        }

        bool match = true;

        for(size_t i = 0; i < cmd_length; i++)
        {
            if(report[start + 4 + i] != cmd[i])
            {
                match = false;
                break;
            }
        }

        if(match)
        {
            return (int)start;
        }
    }

    return -1;
}

/*---------------------------------------------------------*\
| Protocol steps                                            |
\*---------------------------------------------------------*/
bool UmbraController::QueryStatus()
{
    for(unsigned int attempt = 0; attempt < READ_ATTEMPTS; attempt++)
    {
        /*-------------------------------------------------*\
        | Drain stale input reports before querying        |
        \*-------------------------------------------------*/
        unsigned char scratch[HID_REPORT_SIZE];

        for(unsigned int drained = 0; drained < 16 && ReadReport(scratch); drained++)
        {
        }

        if(!SendNativeCommand(CMD_STATUS_BODY, sizeof(CMD_STATUS_BODY)))
        {
            return false;
        }

        unsigned char report[HID_REPORT_SIZE] = { 0 };

        if(ReadReport(report))
        {
            int start = FindNativeResponse(report, sizeof(report),
                                           CMD_STATUS_BODY, sizeof(CMD_STATUS_BODY));

            if(start >= 0)
            {
                size_t off = (size_t)start;

                if(off + STATUS_CHECKSUM_OFFSET >= sizeof(report))
                {
                    continue;
                }

                /*-----------------------------------------*\
                | Validate frame checksum                   |
                \*-----------------------------------------*/
                size_t frame_len = report[off + 2];

                if(frame_len >= STATUS_CHECKSUM_OFFSET + 1
                   && off + frame_len <= sizeof(report))
                {
                    uint8_t checksum = Checksum(&report[off], frame_len - 1);

                    if(checksum != report[off + frame_len - 1])
                    {
                        continue;
                    }
                }

                /*-----------------------------------------*\
                | Boot mode and self-check values           |
                \*-----------------------------------------*/
                uint8_t boot_mode = report[off + STATUS_BOOT_MODE_OFFSET];
                uint8_t self_chk  = report[off + STATUS_SELF_CHECK_OFFSET];

                if(boot_mode != 0xFF && boot_mode != 0x01)
                {
                    continue;
                }

                if(self_chk != 0xFF && self_chk != 0x01)
                {
                    continue;
                }

                return true;
            }
        }

        if(attempt + 1 < READ_ATTEMPTS)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(READ_RETRY_DELAY_MS));
        }
    }

    return false;
}

bool UmbraController::QueryTopology()
{
    for(unsigned int attempt = 0; attempt < READ_ATTEMPTS; attempt++)
    {
        /*-------------------------------------------------*\
        | Drain stale input reports before querying        |
        \*-------------------------------------------------*/
        unsigned char scratch[HID_REPORT_SIZE];

        for(unsigned int drained = 0; drained < 16 && ReadReport(scratch); drained++)
        {
        }

        if(!SendNativeCommand(CMD_PORT_QUERY_BODY, sizeof(CMD_PORT_QUERY_BODY)))
        {
            return false;
        }

        unsigned char report[HID_REPORT_SIZE] = { 0 };

        if(ReadReport(report))
        {
            int start = FindNativeResponse(report, sizeof(report),
                                           CMD_PORT_QUERY_BODY, sizeof(CMD_PORT_QUERY_BODY));

            if(start >= 0)
            {
                size_t off          = (size_t)start;
                size_t records_end  = off + PORT_RECORDS_OFFSET
                                          + NUM_PORTS * PORT_RECORD_SIZE;

                /*-----------------------------------------*\
                | Validate frame checksum using the frame   |
                | length byte                               |
                \*-----------------------------------------*/
                size_t frame_len = report[off + 2];

                if(frame_len >= (PORT_RECORDS_OFFSET + NUM_PORTS * PORT_RECORD_SIZE + 1)
                   && off + frame_len <= sizeof(report))
                {
                    uint8_t checksum = Checksum(&report[off], frame_len - 1);

                    if(checksum != report[off + frame_len - 1])
                    {
                        continue;
                    }
                }

                if(records_end > sizeof(report))
                {
                    continue;
                }

                /*-----------------------------------------*\
                | Parse 10 x [leds][u][u][port][u] records  |
                \*-----------------------------------------*/
                PortInfo parsed[NUM_PORTS];
                bool seen[NUM_PORTS] = { false };
                bool valid           = true;

                for(unsigned int r = 0; r < NUM_PORTS && valid; r++)
                {
                    size_t rec_off = off + PORT_RECORDS_OFFSET + r * PORT_RECORD_SIZE;

                    uint8_t leds  = report[rec_off + 0];
                    uint8_t idx   = report[rec_off + 3];

                    if(idx >= NUM_PORTS || seen[idx])
                    {
                        valid = false;
                        break;
                    }

                    seen[idx]             = true;
                    parsed[idx].index     = idx;
                    parsed[idx].led_count = leds;
                    parsed[idx].unknown1  = report[rec_off + 1];
                    parsed[idx].unknown2  = report[rec_off + 2];
                    parsed[idx].unknown4  = report[rec_off + 4];
                }

                if(valid)
                {
                    unsigned int total = 0;

                    for(unsigned int p = 0; p < NUM_PORTS; p++)
                    {
                        ports_[p] = parsed[p];
                        total    += ports_[p].led_count;
                    }

                    total_led_count_ = total;

                    return true;
                }
            }
        }

        if(attempt + 1 < READ_ATTEMPTS)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(READ_RETRY_DELAY_MS));
        }
    }

    return false;
}

bool UmbraController::EnableSoftwareControl()
{
    return SendNativeCommand(CMD_SOFTWARE_CONTROL_BODY, sizeof(CMD_SOFTWARE_CONTROL_BODY));
}

/*---------------------------------------------------------*\
| Direct RGB streaming                                      |
|                                                           |
| Frame layout:                                             |
|   88 [packet count] [packet index] [20 x R G B] [checksum]|
|                                                           |
| Packet count/index are 1-based; the checksum covers all   |
| preceding 63 bytes of the packet                          |
\*---------------------------------------------------------*/
bool UmbraController::SendFrame(const unsigned char* rgb, unsigned int led_count)
{
    std::lock_guard<std::mutex> guard(io_mutex_);

    if(dev_ == nullptr || !initialized_)
    {
        return false;
    }

    if(total_led_count_ == 0 || led_count != total_led_count_)
    {
        return false;
    }

    /*-----------------------------------------------------*\
    | Rate limiting                                         |
    \*-----------------------------------------------------*/
    if(frame_paced_ && frame_interval_ms_ > 0.0)
    {
        auto elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - last_frame_time_).count();

        if(elapsed < frame_interval_ms_)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds((long long)std::ceil(frame_interval_ms_ - elapsed)));
        }
    }

    last_frame_time_ = std::chrono::steady_clock::now();
    frame_paced_     = true;

    /*-----------------------------------------------------*\
    | Split into packets of 20 LEDs, black-padded tail      |
    \*-----------------------------------------------------*/
    unsigned int packet_count = (led_count + LEDS_PER_PACKET - 1) / LEDS_PER_PACKET;

    unsigned char packet[HID_PAYLOAD_SIZE];

    for(unsigned int pkt = 0; pkt < packet_count; pkt++)
    {
        memset(packet, 0, sizeof(packet));

        packet[0] = DIRECT_RGB_HEADER;
        packet[1] = (uint8_t)packet_count;
        packet[2] = (uint8_t)(pkt + 1);

        unsigned int leds_in_packet = LEDS_PER_PACKET;

        if((pkt + 1) * LEDS_PER_PACKET > led_count)
        {
            leds_in_packet = led_count - (pkt * LEDS_PER_PACKET);
        }

        memcpy(&packet[3], rgb + (size_t)pkt * LEDS_PER_PACKET * 3,
               (size_t)leds_in_packet * 3);

        packet[HID_PAYLOAD_SIZE - 1] = Checksum(packet, HID_PAYLOAD_SIZE - 1);

        if(!WritePayload(packet, sizeof(packet)))
        {
            return false;
        }
    }

    return true;
}
