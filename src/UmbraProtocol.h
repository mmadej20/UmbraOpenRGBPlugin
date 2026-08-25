/*---------------------------------------------------------*\
| UmbraProtocol.h                                           |
|                                                           |
|   Pure protocol layer for the AsiaHorse UMBRA / ROBOBLOQ  |
|   USBFAN controller. No HID, no OS dependencies - every   |
|   function operates on plain byte buffers and can be      |
|   unit tested without hardware attached.                  |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace UmbraProtocol
{
    /*-----------------------------------------------------*\
    | Frame sizes                                           |
    \*-----------------------------------------------------*/
    const size_t    PAYLOAD_SIZE        = 64;   /* bytes per report payload (excluding report ID) */
    const size_t    REPORT_SIZE         = 65;   /* report ID byte + payload                       */
    const size_t    NATIVE_OVERHEAD     = 5;    /* header(2)+length+zero+checksum                 */
    const size_t    RGB_PACKET_HEADER   = 3;    /* 0x88 + count + index                           */
    const size_t    LEDS_PER_PACKET     = 20;
    const size_t    NUM_PORTS           = 10;

    /*-----------------------------------------------------*\
    | Topology response layout (offsets from frame start)   |
    \*-----------------------------------------------------*/
    const size_t    PORT_RECORDS_OFFSET = 6;
    const size_t    PORT_RECORD_SIZE    = 5;

    /*-----------------------------------------------------*\
    | Parsed topology record for one physical port          |
    \*-----------------------------------------------------*/
    struct TopologyRecord
    {
        uint8_t led_count;      /* LEDs reported by controller    */
        uint8_t unknown1;       /* topology record field, unused  */
        uint8_t unknown2;       /* topology record field, unused  */
        uint8_t port_index;     /* physical port index, 0-based   */
        uint8_t unknown4;       /* topology record field, unused  */
    };

    /*-----------------------------------------------------*\
    | Parsed hub status                                     |
    \*-----------------------------------------------------*/
    struct StatusInfo
    {
        bool    fixed_mode;         /* boot mode: fixed effect vs memory  */
        bool    self_check_enabled; /* boot self-check state              */
    };

    /*-----------------------------------------------------*\
    | Additive 8-bit checksum: sum(bytes) & 0xFF            |
    \*-----------------------------------------------------*/
    uint8_t Checksum(const unsigned char* data, size_t length);

    /*-----------------------------------------------------*\
    | Native command framing                                |
    |                                                       |
    | Builds "52 42 [length] 00 [body...] [checksum]" where |
    | length counts every byte of the frame including the   |
    | trailing checksum.                                    |
    |                                                       |
    | out must have room for PAYLOAD_SIZE bytes. Returns    |
    | false (and writes nothing) when body does not fit.    |
    \*-----------------------------------------------------*/
    bool BuildNativeCommand(const uint8_t* body, size_t body_length,
                            unsigned char* out, size_t* out_frame_length);

    /*-----------------------------------------------------*\
    | Locates a native response frame whose command body    |
    | matches cmd. Returns offset within report or -1       |
    \*-----------------------------------------------------*/
    int FindResponse(const unsigned char* report, size_t report_size,
                     const uint8_t* cmd, size_t cmd_length);

    /*-----------------------------------------------------*\
    | Validates frame length + additive checksum at offset  |
    | start and returns the frame length byte value.        |
    | Fail-closed: returns 0 whenever anything is off.      |
    \*-----------------------------------------------------*/
    size_t ValidatedFrameLength(const unsigned char* report, size_t report_size,
                                size_t start, size_t min_frame_length);

    /*-----------------------------------------------------*\
    | Status response ("00" command).                       |
    | Fail-closed on length, checksum and value ranges.     |
    \*-----------------------------------------------------*/
    bool ParseStatus(const unsigned char* report, size_t report_size,
                     StatusInfo* out);

    /*-----------------------------------------------------*\
    | Topology response ("01 FF" command).                  |
    | Fills all NUM_PORTS records indexed by physical port. |
    | Fail-closed on length, checksum, duplicate or         |
    | out-of-range port indices.                            |
    \*-----------------------------------------------------*/
    bool ParseTopology(const unsigned char* report, size_t report_size,
                       TopologyRecord* out_ports /* [NUM_PORTS] */);

    /*-----------------------------------------------------*|
    | Direct RGB packet                                      |
    |                                                        |
    | Layout:                                                |
    |   byte 0      = 0x88                                   |
    |   byte 1      = total packet count                     |
    |   byte 2      = 1-based packet index                   |
    |   bytes 3..62 = up to 20 x (R,G,B), zero padded        |
    |   byte 63     = checksum over bytes 0..62              |
    |                                                        |
    | rgb points at this packet's slice of the packed color  |
    | stream; rgb_bytes may be smaller than 60 for the final |
    | packet - remaining slots are zero filled               |
    |-------------------------------------------------------*/
    bool BuildRgbPacket(unsigned int packet_count,
                        unsigned int packet_index_1based,
                        const unsigned char* rgb, size_t rgb_bytes,
                        unsigned char* out /* [PAYLOAD_SIZE] */);
}
