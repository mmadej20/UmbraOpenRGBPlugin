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

#if __has_include(<hidapi/hidapi.h>)
#include <hidapi/hidapi.h>
#else
#include <hidapi.h>
#endif

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

/* Command bodies (protocol framing lives in UmbraProtocol)*/
static const uint8_t CMD_STATUS_BODY[]            = { 0x00 };
static const uint8_t CMD_PORT_QUERY_BODY[]        = { 0x01, 0xFF };
static const uint8_t CMD_SOFTWARE_CONTROL_BODY[]  = { 0xFD, 0x01 };

/*---------------------------------------------------------*\
| hidapi reports strings as UTF-16 wchar buffers            |
\*---------------------------------------------------------*/
static void AppendUtf8(std::string& out, unsigned int cp)
{
    if(cp <= 0x7F)
    {
        out.push_back((char)cp);
    }
    else if(cp <= 0x7FF)
    {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    else if(cp <= 0xFFFF)
    {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static std::string WideToUtf8(const wchar_t* wide)
{
    std::string out;

    if(wide == nullptr)
    {
        return out;
    }

    for(const wchar_t* p = wide; *p != 0; p++)
    {
        /*-------------------------------------------------*\
        | On Windows wchar_t is 16-bit UTF-16 and surrogate |
        | pairs must be decoded. On Linux/macOS wchar_t is  |
        | 32-bit and already holds the full codepoint       |
        \*-------------------------------------------------*/
        if constexpr(sizeof(wchar_t) == 2)
        {
            unsigned int cp = (unsigned int)*p & 0xFFFFu;

            if(cp >= 0xD800 && cp <= 0xDBFF && p[1] != 0)
            {
                unsigned int low = (unsigned int)p[1] & 0xFFFFu;

                if(low >= 0xDC00 && low <= 0xDFFF)
                {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    p++;
                }
            }

            AppendUtf8(out, cp);
        }
        else
        {
            AppendUtf8(out, (unsigned int)*p);
        }
    }

    return out;
}

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
        device.serial = WideToUtf8(info->serial_number);
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
    UpdateFramePacingLocked();

    frame_paced_        = false;
    initialized_        = true;

    return true;
}

void UmbraController::UpdateFramePacingLocked()
{
    /*-----------------------------------------------------*\
    | mutex_ (io_mutex_) must be held by the caller         |
    \*-----------------------------------------------------*/
    unsigned int packets = (total_led_count_ > 0)
                         ? ((total_led_count_ +
                             (unsigned int)UmbraProtocol::LEDS_PER_PACKET - 1u) /
                            (unsigned int)UmbraProtocol::LEDS_PER_PACKET)
                         : 0;

    frame_interval_ms_ = (packets > 0)
                       ? (double)((packets * 1000u + TARGET_WRITES_PER_SEC - 1u) / TARGET_WRITES_PER_SEC)
                       : 0.0;
}

bool UmbraController::RefreshTopology()
{
    std::lock_guard<std::mutex> guard(io_mutex_);

    if(dev_ == nullptr || !initialized_)
    {
        return false;
    }

    if(!QueryTopology())
    {
        /*-----------------------------------------------------*\
        | IO failure means the device is gone or wedged - mark  |
        | the transport broken so the next attempt performs a   |
        | full reconnect instead of talking into a dead handle  |
        \*-----------------------------------------------------*/
        CloseInternal();
        return false;
    }

    /*-----------------------------------------------------*\
    | Pacing depends on packet count and must be recomputed |
    | when the topology changes (e.g. 0 -> 100 LEDs)        |
    \*-----------------------------------------------------*/
    UpdateFramePacingLocked();

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
        hid_close(dev_);
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
bool UmbraController::WritePayload(const unsigned char* payload, size_t length)
{
    if(dev_ == nullptr || length > UmbraProtocol::PAYLOAD_SIZE)
    {
        return false;
    }

    unsigned char report[UmbraProtocol::REPORT_SIZE];

    memset(report, 0, sizeof(report));
    memcpy(&report[1], payload, length);

    int written = hid_write(dev_, report, sizeof(report));

    return (written == (int)sizeof(report));
}

int UmbraController::ReadReport(unsigned char* report, unsigned int timeout_ms)
{
    if(dev_ == nullptr)
    {
        return -1;
    }

    int bytes = hid_read_timeout(dev_, report, UmbraProtocol::REPORT_SIZE,
                                 (int)timeout_ms);

    return (bytes > 0) ? bytes : -1;
}

void UmbraController::DrainInputReports()
{
    /*-----------------------------------------------------*\
    | Non-blocking drain: timeout 0 returns immediately     |
    | when no input report is queued                        |
    \*-----------------------------------------------------*/
    unsigned char scratch[UmbraProtocol::REPORT_SIZE];

    for(unsigned int drained = 0; drained < DRAIN_MAX_REPORTS; drained++)
    {
        if(dev_ == nullptr)
        {
            break;
        }

        int bytes = hid_read_timeout(dev_, scratch,
                                     UmbraProtocol::REPORT_SIZE, 0);

        if(bytes <= 0)
        {
            break;
        }
    }
}

bool UmbraController::SendNativeCommand(const uint8_t* body, size_t body_length)
{
    unsigned char payload[UmbraProtocol::PAYLOAD_SIZE];
    size_t        frame_length = 0;

    if(!UmbraProtocol::BuildNativeCommand(body, body_length,
                                          payload, &frame_length))
    {
        return false;
    }

    return WritePayload(payload, frame_length);
}

/*---------------------------------------------------------*\
| Protocol steps                                            |
\*---------------------------------------------------------*/
bool UmbraController::QueryStatus()
{
    for(unsigned int attempt = 0; attempt < READ_ATTEMPTS; attempt++)
    {
        DrainInputReports();

        if(!SendNativeCommand(CMD_STATUS_BODY, sizeof(CMD_STATUS_BODY)))
        {
            return false;
        }

        unsigned char report[UmbraProtocol::REPORT_SIZE] = { 0 };
        int           bytes = ReadReport(report, READ_TIMEOUT_MS);

        if(bytes > 0)
        {
            UmbraProtocol::StatusInfo status;

            /*-------------------------------------------------*\
            | Fail-closed parsing: length and checksum are      |
            | mandatory, garbage never passes as valid state    |
            \*-------------------------------------------------*/
            if(UmbraProtocol::ParseStatus(report, (size_t)bytes, &status))
            {
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
        DrainInputReports();

        if(!SendNativeCommand(CMD_PORT_QUERY_BODY, sizeof(CMD_PORT_QUERY_BODY)))
        {
            return false;
        }

        unsigned char report[UmbraProtocol::REPORT_SIZE] = { 0 };
        int           bytes = ReadReport(report, READ_TIMEOUT_MS);

        if(bytes > 0)
        {
            UmbraProtocol::TopologyRecord parsed[UmbraProtocol::NUM_PORTS];

            /*-------------------------------------------------*\
            | Fail-closed parsing: bad length, bad checksum,    |
            | duplicate or out-of-range port index reject the   |
            | whole response                                    |
            \*-------------------------------------------------*/
            if(UmbraProtocol::ParseTopology(report, (size_t)bytes, parsed))
            {
                unsigned int total = 0;

                memcpy(ports_, parsed, sizeof(ports_));

                for(unsigned int p = 0; p < NUM_PORTS; p++)
                {
                    total += ports_[p].led_count;
                }

                total_led_count_ = total;

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

bool UmbraController::EnableSoftwareControl()
{
    return SendNativeCommand(CMD_SOFTWARE_CONTROL_BODY,
                             sizeof(CMD_SOFTWARE_CONTROL_BODY));
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
    const size_t leds_per_packet = UmbraProtocol::LEDS_PER_PACKET;

    unsigned int packet_count =
        (unsigned int)(((size_t)led_count + leds_per_packet - 1) / leds_per_packet);

    unsigned char packet[UmbraProtocol::PAYLOAD_SIZE];

    for(unsigned int pkt = 0; pkt < packet_count; pkt++)
    {
        size_t offset = (size_t)pkt * leds_per_packet * 3;
        size_t bytes  = leds_per_packet * 3;

        if(offset + bytes > (size_t)led_count * 3)
        {
            bytes = (size_t)led_count * 3 - offset;
        }

        if(!UmbraProtocol::BuildRgbPacket(packet_count, pkt + 1,
                                          rgb + offset, bytes, packet))
        {
            return false;
        }

        if(!WritePayload(packet, sizeof(packet)))
        {
            /*-------------------------------------------------*\
            | A failed write means the handle is no longer      |
            | usable (unplug/replug, driver reset). Mark the    |
            | transport broken so IsConnected() reports reality |
            | and detection can perform a full reconnect.       |
            \*-------------------------------------------------*/
            CloseInternal();
            return false;
        }
    }

    return true;
}
