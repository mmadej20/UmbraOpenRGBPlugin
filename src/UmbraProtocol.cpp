/*---------------------------------------------------------*\
| UmbraProtocol.cpp                                         |
|                                                           |
|   Pure protocol layer - implementation                    |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "UmbraProtocol.h"

#include <cstring>

namespace UmbraProtocol
{
    /*-----------------------------------------------------*\
    | Frame markers and command bodies                      |
    \*-----------------------------------------------------*/
    static const uint8_t    NATIVE_HEADER_0             = 0x52;
    static const uint8_t    NATIVE_HEADER_1             = 0x42;

    static const uint8_t    DIRECT_RGB_HEADER           = 0x88;

    static const uint8_t    CMD_STATUS_BODY[]           = { 0x00 };
    static const uint8_t    CMD_PORT_QUERY_BODY[]       = { 0x01, 0xFF };

    /* Status response layout (offsets relative to frame)    */
    static const size_t     STATUS_BOOT_MODE_OFFSET     = 23;
    static const size_t     STATUS_SELF_CHECK_OFFSET    = 24;
    static const size_t     STATUS_MIN_FRAME_LENGTH     = 26;

    uint8_t Checksum(const unsigned char* data, size_t length)
    {
        uint8_t sum = 0;

        for(size_t i = 0; i < length; i++)
        {
            sum = (uint8_t)(sum + data[i]);
        }

        return sum;
    }

    bool BuildNativeCommand(const uint8_t* body, size_t body_length,
                            unsigned char* out, size_t* out_frame_length)
    {
        if(out == nullptr || out_frame_length == nullptr)
        {
            return false;
        }

        if(body == nullptr && body_length > 0)
        {
            return false;
        }

        /*-----------------------------------------------------*\
        | The frame must fit in one HID report payload          |
        \*-----------------------------------------------------*/
        if(body_length > PAYLOAD_SIZE - NATIVE_OVERHEAD)
        {
            return false;
        }

        size_t frame_length = 4 + body_length + 1;

        memset(out, 0, PAYLOAD_SIZE);

        out[0] = NATIVE_HEADER_0;
        out[1] = NATIVE_HEADER_1;
        out[2] = (uint8_t)frame_length;
        out[3] = 0x00;

        if(body_length > 0)
        {
            memcpy(&out[4], body, body_length);
        }

        out[frame_length - 1] = Checksum(out, frame_length - 1);

        *out_frame_length = frame_length;

        return true;
    }

    int FindResponse(const unsigned char* report, size_t report_size,
                     const uint8_t* cmd, size_t cmd_length)
    {
        if(report == nullptr || cmd == nullptr)
        {
            return -1;
        }

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

    size_t ValidatedFrameLength(const unsigned char* report, size_t report_size,
                                size_t start, size_t min_frame_length)
    {
        if(start >= report_size)
        {
            return 0;
        }

        size_t remaining = report_size - start;

        /*-----------------------------------------------------*\
        | Fail-closed: the declared frame must be large enough  |
        | for this response type AND fit inside what was        |
        | actually received                                     |
        \*-----------------------------------------------------*/
        if(remaining < min_frame_length)
        {
            return 0;
        }

        size_t frame_len = report[start + 2];

        if(frame_len < min_frame_length || frame_len > remaining)
        {
            return 0;
        }

        /*-----------------------------------------------------*\
        | Additive checksum over all bytes except the last      |
        \*-----------------------------------------------------*/
        uint8_t checksum = Checksum(&report[start], frame_len - 1);

        if(checksum != report[start + frame_len - 1])
        {
            return 0;
        }

        return frame_len;
    }

    bool ParseStatus(const unsigned char* report, size_t report_size,
                     StatusInfo* out)
    {
        if(report == nullptr || out == nullptr)
        {
            return false;
        }

        int start = FindResponse(report, report_size,
                                 CMD_STATUS_BODY, sizeof(CMD_STATUS_BODY));

        if(start < 0)
        {
            return false;
        }

        size_t off       = (size_t)start;
        size_t frame_len = ValidatedFrameLength(report, report_size,
                                                off, STATUS_MIN_FRAME_LENGTH);

        if(frame_len == 0)
        {
            return false;
        }

        uint8_t boot_mode = report[off + STATUS_BOOT_MODE_OFFSET];
        uint8_t self_chk  = report[off + STATUS_SELF_CHECK_OFFSET];

        if(boot_mode != 0xFF && boot_mode != 0x01)
        {
            return false;
        }

        if(self_chk != 0xFF && self_chk != 0x01)
        {
            return false;
        }

        out->fixed_mode         = (boot_mode == 0x01);
        out->self_check_enabled = (self_chk == 0xFF);

        return true;
    }

    bool ParseTopology(const unsigned char* report, size_t report_size,
                       TopologyRecord* out_ports)
    {
        if(report == nullptr || out_ports == nullptr)
        {
            return false;
        }

        const size_t min_frame_length = PORT_RECORDS_OFFSET
                                      + NUM_PORTS * PORT_RECORD_SIZE
                                      + 1;

        int start = FindResponse(report, report_size,
                                 CMD_PORT_QUERY_BODY, sizeof(CMD_PORT_QUERY_BODY));

        if(start < 0)
        {
            return false;
        }

        size_t off       = (size_t)start;
        size_t frame_len = ValidatedFrameLength(report, report_size,
                                                off, min_frame_length);

        if(frame_len == 0)
        {
            return false;
        }

        TopologyRecord parsed[NUM_PORTS];
        bool seen[NUM_PORTS];
        memset(seen, 0, sizeof(seen));

        for(size_t r = 0; r < NUM_PORTS; r++)
        {
            size_t rec_off = off + PORT_RECORDS_OFFSET + r * PORT_RECORD_SIZE;

            uint8_t idx = report[rec_off + 3];

            /*-------------------------------------------------*\
            | Port index must be in range and unique - reject   |
            | the whole response otherwise                      |
            \*-------------------------------------------------*/
            if(idx >= NUM_PORTS || seen[idx])
            {
                return false;
            }

            seen[idx]               = true;
            parsed[idx].led_count   = report[rec_off + 0];
            parsed[idx].unknown1    = report[rec_off + 1];
            parsed[idx].unknown2    = report[rec_off + 2];
            parsed[idx].port_index  = idx;
            parsed[idx].unknown4    = report[rec_off + 4];
        }

        memcpy(out_ports, parsed, sizeof(parsed));

        return true;
    }

    bool BuildRgbPacket(unsigned int packet_count,
                        unsigned int packet_index_1based,
                        const unsigned char* rgb, size_t rgb_bytes,
                        unsigned char* out)
    {
        if(out == nullptr)
        {
            return false;
        }

        if(packet_count == 0 || packet_count > 0xFF)
        {
            return false;
        }

        if(packet_index_1based == 0 || packet_index_1based > packet_count)
        {
            return false;
        }

        if(rgb == nullptr && rgb_bytes > 0)
        {
            return false;
        }

        const size_t max_rgb_bytes = (PAYLOAD_SIZE - RGB_PACKET_HEADER - 1);

        if(rgb_bytes > max_rgb_bytes)
        {
            return false;
        }

        memset(out, 0, PAYLOAD_SIZE);

        out[0] = DIRECT_RGB_HEADER;
        out[1] = (uint8_t)packet_count;
        out[2] = (uint8_t)(packet_index_1based & 0xFF);

        if(rgb_bytes > 0)
        {
            memcpy(&out[RGB_PACKET_HEADER], rgb, rgb_bytes);
        }

        out[PAYLOAD_SIZE - 1] = Checksum(out, PAYLOAD_SIZE - 1);

        return true;
    }
}
