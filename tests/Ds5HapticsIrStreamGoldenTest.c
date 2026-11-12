#include "Ds5HapticsIrStream.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void writeLe16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void writeLe32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void writeLe64(uint8_t* data, uint64_t value) {
    writeLe32(data, (uint32_t)value);
    writeLe32(data + 4, (uint32_t)(value >> 32));
}

static void writeLeFloat(uint8_t* data, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    writeLe32(data, bits);
}

static void makeValidPacket(uint8_t packet[DS5_HAPTICS_IR_STREAM_WIRE_SIZE]) {
    int lane;
    memset(packet, 0, DS5_HAPTICS_IR_STREAM_WIRE_SIZE);
    packet[0] = DS5_HAPTICS_IR_STREAM_PROTOCOL_VERSION;
    packet[1] = LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY;
    writeLe16(packet + 2, DS5_HAPTICS_IR_STREAM_WIRE_SIZE);
    writeLe16(packet + 4, 3);
    writeLe32(packet + 8, UINT32_C(0x11223344));
    writeLe64(packet + 12, UINT64_C(0x0102030405060708));
    writeLe32(packet + 20, 240);
    for (lane = 0; lane < 2; lane++) {
        uint8_t* out = packet + 24 + lane * 20;
        writeLeFloat(out, 0.25f + lane * 0.1f);
        writeLeFloat(out + 4, 0.5f + lane * 0.1f);
        writeLeFloat(out + 8, 0.75f);
        writeLeFloat(out + 12, 0.2f);
        writeLeFloat(out + 16, 1200.0f + lane * 100.0f);
    }
    writeLeFloat(packet + 64, -0.5f);
}

static void captureFrame(const LI_DS5_HAPTICS_IR_FRAME_V2* frame, void* context) {
    LI_DS5_HAPTICS_IR_FRAME_V2* captured = context;
    *captured = *frame;
}

static void testValidFrame(void) {
    uint8_t packet[DS5_HAPTICS_IR_STREAM_WIRE_SIZE];
    LI_DS5_HAPTICS_IR_FRAME_V2 captured = {0};
    makeValidPacket(packet);

    CHECK(processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    CHECK(captured.flags == LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY);
    CHECK(captured.controllerNumber == 3);
    CHECK(captured.sourceSequenceNumber == UINT32_C(0x11223344));
    CHECK(captured.timestampUs == UINT64_C(0x0102030405060708));
    CHECK(captured.sourceFrameCount == 240);
    CHECK(fabsf(captured.lanes[0].rmsAmplitude - 0.25f) < 0.0001f);
    CHECK(fabsf(captured.lanes[1].zeroCrossingRateHz - 1300.0f) < 0.0001f);
    CHECK(fabsf(captured.laneCorrelation + 0.5f) < 0.0001f);
}

static void testMalformedFrames(void) {
    uint8_t packet[DS5_HAPTICS_IR_STREAM_WIRE_SIZE];
    LI_DS5_HAPTICS_IR_FRAME_V2 captured = {0};
    makeValidPacket(packet);

    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet) - 1, captureFrame, &captured));
    packet[0] = 1;
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    packet[0] = DS5_HAPTICS_IR_STREAM_PROTOCOL_VERSION;
    packet[1] = 0x80;
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    packet[1] = 0;
    writeLe16(packet + 6, 1);
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    writeLe16(packet + 6, 0);
    writeLe32(packet + 24, UINT32_C(0x7fc00000));
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    makeValidPacket(packet);
    writeLeFloat(packet + 24, 0.75f);
    writeLeFloat(packet + 28, 0.5f);
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    makeValidPacket(packet);
    writeLeFloat(packet + 64, 1.1f);
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
}


static void makeValidExtendedPacket(uint8_t* packet, unsigned events) {
    unsigned i;
    memset(packet, 0, DS5_HAPTICS_IR_EXTENDED_STREAM_MAX_SIZE);
    makeValidPacket(packet);
    writeLe16(packet + 4, 2);
    writeLe32(packet + 8, UINT32_C(0xfedcba98));
    packet[72] = 1;
    packet[73] = (uint8_t)events;
    writeLe16(packet + 74, (uint16_t)(32 + events * 20));
    writeLe32(packet + 76, UINT32_C(0xfffffffe));
    writeLe64(packet + 80, UINT64_C(0x0102030405060708));
    for (i = 0; i < 2; i++) {
        writeLeFloat(packet + 88 + i * 8, 0.8f);
        writeLeFloat(packet + 92 + i * 8, 0.6f);
    }
    for (i = 0; i < events; i++) {
        uint8_t* event = packet + 104 + i * 20;
        writeLe32(event, UINT32_C(0x12345678) + i);
        writeLe64(event + 4, UINT64_C(0x0102030405060708) - i * 5000);
        writeLeFloat(event + 12, 0.25f);
        writeLeFloat(event + 16, 0.75f);
    }
}

static void captureExtendedFrame(const LI_DS5_HAPTICS_IR_EXTENDED_FRAME* frame, void* context) {
    *(LI_DS5_HAPTICS_IR_EXTENDED_FRAME*)context = *frame;
}

static void testExtendedFrames(void) {
    uint8_t packet[DS5_HAPTICS_IR_EXTENDED_STREAM_MAX_SIZE + 1];
    LI_DS5_HAPTICS_IR_EXTENDED_FRAME captured = {0};
    LI_DS5_HAPTICS_IR_FRAME_V2 base = {0};
    unsigned i, count;
    for (count = 0; count <= LI_DS5_HAPTICS_IR_EXTENDED_MAX_EVENTS; count++) {
        int length = 104 + (int)count * 20;
        makeValidExtendedPacket(packet, count);
        CHECK(processDs5HapticsIrExtendedStreamPacket(packet, length, captureExtendedFrame, &captured));
        CHECK(processDs5HapticsIrStreamPacket(packet, 72, captureFrame, &base));
        CHECK(memcmp(&base, &captured.base, sizeof(base)) == 0);
        CHECK(!processDs5HapticsIrStreamPacket(packet, length, captureFrame, &base));
        CHECK(captured.base.controllerNumber == 2);
        CHECK(captured.base.sourceSequenceNumber == UINT32_C(0xfedcba98));
        CHECK(captured.epoch == UINT32_C(0xfffffffe));
        CHECK(captured.windowEndUs == UINT64_C(0x0102030405060708));
        CHECK(captured.base.sourceFrameCount == 240 && captured.eventCount == count);
        CHECK(captured.lanes[0].continuousLow > captured.base.lanes[0].peakAmplitude);
        for (i = 0; i < count; i++) {
            CHECK(captured.events[i].eventId == UINT32_C(0x12345678) + i);
            CHECK(captured.events[i].sourceTimeUs == captured.windowEndUs - i * 5000);
            CHECK(captured.events[i].leftStrength == 0.25f && captured.events[i].rightStrength == 0.75f);
        }
        for (i = 0; i < (unsigned)length; i++)
            CHECK(!processDs5HapticsIrExtendedStreamPacket(packet, (int)i, captureExtendedFrame, &captured));
    }
    makeValidExtendedPacket(packet, 0);
    packet[1] = LI_DS5_HAPTICS_IR_FLAG_STREAM_END;
    writeLe32(packet + 20, 0);
    writeLe32(packet + 76, 0);
    CHECK(processDs5HapticsIrExtendedStreamPacket(packet, 104, captureExtendedFrame, &captured));
    CHECK(captured.epoch == 0 && captured.base.sourceFrameCount == 0);

    makeValidExtendedPacket(packet, 1);
    CHECK(!processDs5HapticsIrExtendedStreamPacket(NULL, 124, captureExtendedFrame, &captured));
    CHECK(!processDs5HapticsIrExtendedStreamPacket(packet, 124, NULL, &captured));
    CHECK(!processDs5HapticsIrExtendedStreamPacket(packet, 125, captureExtendedFrame, &captured));
    CHECK(!processDs5HapticsIrExtendedStreamPacket(packet, 265, captureExtendedFrame, &captured));
    {
        static const unsigned offsets[] = {72,73,74};
        static const uint8_t invalid[] = {2,9,71};
        for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            makeValidExtendedPacket(packet, 1);
            packet[offsets[i]] = invalid[i];
            CHECK(!processDs5HapticsIrExtendedStreamPacket(packet, 124, captureExtendedFrame, &captured));
        }
    }
    {
        static const uint32_t counts[] = {241,UINT32_MAX};
        for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
            makeValidExtendedPacket(packet, 1);
            writeLe32(packet + 20, counts[i]);
            CHECK(processDs5HapticsIrExtendedStreamPacket(packet, 124, captureExtendedFrame, &captured));
            CHECK(captured.base.sourceFrameCount == counts[i]);
        }
    }
    writeLe32(packet + 20, 0);
    CHECK(!processDs5HapticsIrExtendedStreamPacket(packet, 124, captureExtendedFrame, &captured));
    makeValidExtendedPacket(packet, 1);
    writeLe64(packet + 108, UINT64_C(0x0102030405060709));
    CHECK(!processDs5HapticsIrExtendedStreamPacket(packet, 124, captureExtendedFrame, &captured));
    makeValidExtendedPacket(packet, 1);
    writeLe64(packet + 80, UINT64_C(0x0102030405060707));
    CHECK(!processDs5HapticsIrExtendedStreamPacket(packet, 124, captureExtendedFrame, &captured));
    {
        static const unsigned offsets[] = {88,92,96,100,116,120};
        static const float invalid[] = {NAN,INFINITY,-INFINITY,-0.01f,1.01f};
        unsigned j;
        for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            for (j = 0; j < sizeof(invalid) / sizeof(invalid[0]); j++) {
                makeValidExtendedPacket(packet, 1);
                writeLeFloat(packet + offsets[i], invalid[j]);
                CHECK(!processDs5HapticsIrExtendedStreamPacket(packet, 124, captureExtendedFrame, &captured));
            }
        }
    }
    // One invalid base proves that enhancement decoding retains the v2 guards.
    makeValidExtendedPacket(packet, 1);
    writeLeFloat(packet + 24, 0.8f);
    CHECK(!processDs5HapticsIrExtendedStreamPacket(packet, 124, captureExtendedFrame, &captured));
}

int main(void) {
    testValidFrame();
    testMalformedFrames();
    testExtendedFrames();
    if (failures != 0) {
        fprintf(stderr, "%d DualSense IR protocol checks failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
