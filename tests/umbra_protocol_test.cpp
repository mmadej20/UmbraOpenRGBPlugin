/*---------------------------------------------------------*\
| umbra_protocol_test.cpp                                   |
|                                                           |
|   Unit tests for the pure UmbraProtocol layer.            |
|   No hardware required.                                   |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "../src/UmbraProtocol.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond))                                                         \
        {                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                   \
        }                                                                   \
    } while(0)

using namespace UmbraProtocol;

/*---------------------------------------------------------*\
| Helpers                                                   |
\*---------------------------------------------------------*/
static void PutFrame(unsigned char* report, size_t report_size,
                     size_t offset, const unsigned char* frame, size_t frame_len)
{
    memset(report, 0, report_size);
    memcpy(&report[offset], frame, frame_len);
}

/* Builds a full native frame into dst; returns its length */
static size_t MakeNativeFrame(unsigned char* dst,
                              const uint8_t* body, size_t body_len)
{
    size_t len = 0;
    BuildNativeCommand(body, body_len, dst, &len);
    return len;
}

/* Builds a valid topology frame; led_counts indexed by record order */
static size_t MakeTopologyFrame(unsigned char* dst,
                                const uint8_t led_counts[NUM_PORTS],
                                const uint8_t port_indices[NUM_PORTS])
{
    uint8_t body[2 + NUM_PORTS * PORT_RECORD_SIZE];
    size_t  body_len = 0;

    body[body_len++] = 0x01;
    body[body_len++] = 0xFF;

    for(size_t r = 0; r < NUM_PORTS; r++)
    {
        body[body_len++] = led_counts[r];
        body[body_len++] = 0xA1;                    /* unknown1     */
        body[body_len++] = 0xB2;                    /* unknown2     */
        body[body_len++] = port_indices[r];         /* port index   */
        body[body_len++] = 0xC3;                    /* unknown4     */
    }

    return MakeNativeFrame(dst, body, body_len);
}

/* Builds a valid status frame */
static size_t MakeStatusFrame(unsigned char* dst, uint8_t boot_mode, uint8_t self_chk)
{
    unsigned char frame[PAYLOAD_SIZE];
    size_t        frame_length;

    memset(frame, 0, sizeof(frame));

    frame[0] = 0x52;
    frame[1] = 0x42;
    frame[2] = 26;      /* STATUS_MIN_FRAME_LENGTH */
    frame[3] = 0x00;
    frame[4] = 0x00;    /* command echo            */
    frame[23] = boot_mode;
    frame[24] = self_chk;
    frame[25] = Checksum(frame, 25);

    memcpy(dst, frame, 26);

    return 26;
}

/*---------------------------------------------------------*\
| Checksum                                                  |
\*---------------------------------------------------------*/
static void TestChecksum()
{
    const unsigned char one[]   = { 0xFF };
    const unsigned char mixed[] = { 0x52, 0x42, 0x07, 0x00, 0x01, 0xFF };

    CHECK(Checksum(one, 1) == 0xFF);
    CHECK(Checksum(mixed, sizeof(mixed)) == ((0x52 + 0x42 + 0x07 + 0x00 + 0x01 + 0xFF) & 0xFF));
    CHECK(Checksum(nullptr, 0) == 0);
}

/*---------------------------------------------------------*\
| Native command framing                                    |
\*---------------------------------------------------------*/
static void TestBuildNativeCommand()
{
    /* PORT_QUERY: expected exact bytes                      */
    const uint8_t expected[] = { 0x52, 0x42, 0x07, 0x00, 0x01, 0xFF, 0x9B };

    unsigned char out[PAYLOAD_SIZE];
    size_t        len = 0;

    CHECK(BuildNativeCommand(expected + 4, 2, out, &len));
    CHECK(len == 7);
    CHECK(memcmp(out, expected, sizeof(expected)) == 0);

    /* Status command                                        */
    const uint8_t status_body[] = { 0x00 };

    CHECK(BuildNativeCommand(status_body, 1, out, &len));
    CHECK(len == 6);
    CHECK(out[0] == 0x52 && out[1] == 0x42 && out[2] == 0x06 && out[3] == 0x00);
    CHECK(out[4] == 0x00);
    CHECK(out[5] == Checksum(out, 5));

    /* Oversized body must fail                              */
    uint8_t big[PAYLOAD_SIZE];

    memset(big, 0xAA, sizeof(big));
    CHECK(!BuildNativeCommand(big, PAYLOAD_SIZE - NATIVE_OVERHEAD + 1, out, &len));

    /* Maximum-size body must succeed                        */
    CHECK(BuildNativeCommand(big, PAYLOAD_SIZE - NATIVE_OVERHEAD, out, &len));
    CHECK(len == PAYLOAD_SIZE);
}

/*---------------------------------------------------------*\
| Response scanning                                         |
\*---------------------------------------------------------*/
static void TestFindResponse()
{
    unsigned char report[REPORT_SIZE];

    memset(report, 0xEE, sizeof(report));

    const uint8_t cmd[] = { 0x01, 0xFF };

    unsigned char frame[PAYLOAD_SIZE];
    size_t        frame_len = MakeNativeFrame(frame, cmd, sizeof(cmd));

    PutFrame(report, sizeof(report), 3, frame, frame_len);

    CHECK(FindResponse(report, sizeof(report), cmd, sizeof(cmd)) == 3);

    /* Not present                                           */
    memset(report, 0, sizeof(report));
    CHECK(FindResponse(report, sizeof(report), cmd, sizeof(cmd)) == -1);

    /* Truncated buffer                                      */
    CHECK(FindResponse(report, 3, cmd, sizeof(cmd)) == -1);
}

/*---------------------------------------------------------*\
| Topology parsing                                          |
\*---------------------------------------------------------*/
static void TestParseTopologyValid()
{
    unsigned char frame[PAYLOAD_SIZE];
    unsigned char report[REPORT_SIZE];

    const uint8_t leds[]         = { 14, 14, 14, 0, 24, 12, 8, 30, 1, 100 };
    const uint8_t indices[]      = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    const size_t  frame_len      = MakeTopologyFrame(frame, leds, indices);

    PutFrame(report, sizeof(report), 1, frame, frame_len);

    TopologyRecord ports[NUM_PORTS];

    CHECK(ParseTopology(report, sizeof(report), ports));

    for(size_t p = 0; p < NUM_PORTS; p++)
    {
        CHECK(ports[p].port_index == p);
        CHECK(ports[p].led_count == leds[p]);
        CHECK(ports[p].unknown1 == 0xA1);
        CHECK(ports[p].unknown2 == 0xB2);
        CHECK(ports[p].unknown4 == 0xC3);
    }
}

static void TestParseTopologyRejects()
{
    unsigned char frame[PAYLOAD_SIZE];
    unsigned char report[REPORT_SIZE];

    TopologyRecord ports[NUM_PORTS];

    /* Duplicate port index                                  */
    {
        const uint8_t leds[]    = { 14, 14, 14, 14, 14, 14, 14, 14, 14, 14 };
        const uint8_t indices[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 8 };

        size_t frame_len = MakeTopologyFrame(frame, leds, indices);

        PutFrame(report, sizeof(report), 0, frame, frame_len);
        CHECK(!ParseTopology(report, sizeof(report), ports));
    }

    /* Out-of-range port index                               */
    {
        const uint8_t leds[]    = { 14, 14, 14, 14, 14, 14, 14, 14, 14, 14 };
        const uint8_t indices[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10 };

        size_t frame_len = MakeTopologyFrame(frame, leds, indices);

        PutFrame(report, sizeof(report), 0, frame, frame_len);
        CHECK(!ParseTopology(report, sizeof(report), ports));
    }

    /* Corrupted checksum                                    */
    {
        const uint8_t leds[]    = { 14, 14, 14, 14, 14, 14, 14, 14, 14, 14 };
        const uint8_t indices[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

        size_t frame_len = MakeTopologyFrame(frame, leds, indices);

        frame[frame_len - 1] ^= 0xFF;

        PutFrame(report, sizeof(report), 0, frame, frame_len);
        CHECK(!ParseTopology(report, sizeof(report), ports));
    }

    /* Truncated report (records cut off)                    */
    {
        const uint8_t leds[]    = { 14, 14, 14, 14, 14, 14, 14, 14, 14, 14 };
        const uint8_t indices[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

        size_t frame_len = MakeTopologyFrame(frame, leds, indices);

        PutFrame(report, sizeof(report), 0, frame, frame_len);
        CHECK(!ParseTopology(report, frame_len - 10, ports));
    }

    /* Declared length larger than received data             */
    {
        const uint8_t leds[]    = { 14, 14, 14, 14, 14, 14, 14, 14, 14, 14 };
        const uint8_t indices[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

        size_t frame_len = MakeTopologyFrame(frame, leds, indices);

        PutFrame(report, sizeof(report), 0, frame, frame_len);

        report[2] = 63;     /* claims more than we got          */

        CHECK(!ParseTopology(report, frame_len, ports));
    }

    /* Zero-length declaration                               */
    {
        const uint8_t leds[]    = { 14, 14, 14, 14, 14, 14, 14, 14, 14, 14 };
        const uint8_t indices[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

        size_t frame_len = MakeTopologyFrame(frame, leds, indices);

        PutFrame(report, sizeof(report), 0, frame, frame_len);

        report[2] = 0;

        CHECK(!ParseTopology(report, frame_len, ports));
    }
}

/*---------------------------------------------------------*\
| Status parsing                                            |
\*---------------------------------------------------------*/
static void TestParseStatus()
{
    unsigned char frame[PAYLOAD_SIZE];
    unsigned char report[REPORT_SIZE];
    StatusInfo    status;

    /* Valid fixed mode + self-check enabled                 */
    {
        size_t frame_len = MakeStatusFrame(frame, 0x01, 0xFF);

        PutFrame(report, sizeof(report), 2, frame, frame_len);

        CHECK(ParseStatus(report, sizeof(report), &status));
        CHECK(status.fixed_mode);
        CHECK(status.self_check_enabled);
    }

    /* Memory mode + self-check disabled                     */
    {
        size_t frame_len = MakeStatusFrame(frame, 0xFF, 0x01);

        PutFrame(report, sizeof(report), 0, frame, frame_len);

        CHECK(ParseStatus(report, sizeof(report), &status));
        CHECK(!status.fixed_mode);
        CHECK(!status.self_check_enabled);
    }

    /* Invalid value ranges                                  */
    {
        size_t frame_len = MakeStatusFrame(frame, 0x55, 0xFF);

        PutFrame(report, sizeof(report), 0, frame, frame_len);
        CHECK(!ParseStatus(report, sizeof(report), &status));
    }

    {
        size_t frame_len = MakeStatusFrame(frame, 0x01, 0x77);

        PutFrame(report, sizeof(report), 0, frame, frame_len);
        CHECK(!ParseStatus(report, sizeof(report), &status));
    }

    /* Bad checksum must be rejected (fail-closed)           */
    {
        size_t frame_len = MakeStatusFrame(frame, 0x01, 0xFF);

        frame[25] ^= 0x01;

        PutFrame(report, sizeof(report), 0, frame, frame_len);
        CHECK(!ParseStatus(report, sizeof(report), &status));
    }

    /* Undersized declared length must be rejected           */
    {
        size_t frame_len = MakeStatusFrame(frame, 0x01, 0xFF);

        frame[2] = 10;      /* claims shorter than required     */

        PutFrame(report, sizeof(report), 0, frame, frame_len);
        CHECK(!ParseStatus(report, sizeof(report), &status));
    }

    /* Truncated actual data must be rejected                */
    {
        size_t frame_len = MakeStatusFrame(frame, 0x01, 0xFF);

        PutFrame(report, sizeof(report), 0, frame, frame_len);
        CHECK(!ParseStatus(report, 20, &status));
    }
}

/*---------------------------------------------------------*\
| Direct RGB packets                                        |
\*---------------------------------------------------------*/
static bool VerifyPacket(const unsigned char* pkt, unsigned int count,
                         unsigned int index, size_t rgb_bytes,
                         const unsigned char* rgb)
{
    if(pkt[0] != 0x88 || pkt[1] != count || pkt[2] != index)
    {
        return false;
    }

    for(size_t i = 0; i < 60; i++)
    {
        unsigned char expected = (i < rgb_bytes && rgb != nullptr) ? rgb[i] : 0x00;

        if(pkt[3 + i] != expected)
        {
            return false;
        }
    }

    return pkt[63] == Checksum(pkt, 63);
}

static void TestRgbPackets()
{
    unsigned char rgb[21 * 3];
    unsigned char pkt[PAYLOAD_SIZE];

    /* Fill with a recognizable pattern                      */
    for(size_t i = 0; i < sizeof(rgb); i++)
    {
        rgb[i] = (unsigned char)(i * 7 + 1);
    }

    /* Exactly one full packet (20 LEDs)                     */
    CHECK(BuildRgbPacket(1, 1, rgb, 60, pkt));
    CHECK(VerifyPacket(pkt, 1, 1, 60, rgb));

    /* Final partial packet (19 LEDs -> 1 zero-padded slot)  */
    CHECK(BuildRgbPacket(1, 1, rgb, 57, pkt));
    CHECK(VerifyPacket(pkt, 1, 1, 57, rgb));
    CHECK(pkt[57] != 0x00);                 /* LED 19 blue byte   */
    CHECK(pkt[60] == 0x00 && pkt[61] == 0x00 && pkt[62] == 0x00);

    /* Second packet of two carrying 21 LEDs total (1 LED)   */
    CHECK(BuildRgbPacket(2, 2, rgb + 60, 3, pkt));
    CHECK(VerifyPacket(pkt, 2, 2, 3, rgb + 60));

    /* Empty tail slot                                       */
    CHECK(BuildRgbPacket(3, 3, nullptr, 0, pkt));
    CHECK(VerifyPacket(pkt, 3, 3, 0, nullptr));

    /* Argument validation                                   */
    CHECK(!BuildRgbPacket(0, 1, rgb, 60, pkt));             /* no packets       */
    CHECK(!BuildRgbPacket(2, 3, rgb, 60, pkt));             /* index > count    */
    CHECK(!BuildRgbPacket(2, 0, rgb, 60, pkt));             /* index is 1-based */
    CHECK(!BuildRgbPacket(1, 1, rgb, 61, pkt));             /* oversize slice   */
    CHECK(!BuildRgbPacket(256, 256, rgb, 60, pkt));         /* count overflow   */
}

/*---------------------------------------------------------*\
| Golden captures                                           |
|                                                           |
| The synthetic tests above verify parser behaviour against |
| the format we ASSUME. A golden capture from real hardware |
| verifies the assumption itself. See GOLDEN_CAPTURES.md    |
| for how to produce one - the file is picked up            |
| automatically when present.                               |
\*---------------------------------------------------------*/
#if __has_include("golden_topology_response.h")
#include "golden_topology_response.h"

static void TestGoldenCapture()
{
    TopologyRecord ports[NUM_PORTS];

    CHECK(ParseTopology(REAL_TOPOLOGY_RESPONSE,
                        REAL_TOPOLOGY_RESPONSE_SIZE,
                        ports));

    if(g_failures == 0)
    {
        printf("GOLDEN CAPTURE ACCEPTED - detected layout:");

        unsigned int total = 0;

        for(size_t p = 0; p < NUM_PORTS; p++)
        {
            printf(" P%02u:%u", (unsigned)(p + 1), ports[p].led_count);
            total += ports[p].led_count;
        }

        printf("  total=%u\n", total);
    }
}
#else
static void TestGoldenCapture()
{
    printf("GOLDEN CAPTURE: not present (see tests/GOLDEN_CAPTURES.md) - skipped\n");
}
#endif

int main()
{
    TestChecksum();
    TestBuildNativeCommand();
    TestFindResponse();
    TestParseTopologyValid();
    TestParseTopologyRejects();
    TestParseStatus();
    TestRgbPackets();
    TestGoldenCapture();

    if(g_failures == 0)
    {
        printf("ALL UMBRA PROTOCOL TESTS PASSED\n");
        return 0;
    }

    printf("%d FAILURE(S)\n", g_failures);

    return 1;
}
