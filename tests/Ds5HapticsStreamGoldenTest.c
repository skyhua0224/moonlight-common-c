#include "Ds5HapticsStream.h"

#include <stdio.h>
#include <string.h>

typedef struct _CAPTURED_FRAME {
    int count;
    LI_DS5_HAPTICS_PCM_FRAME frame;
    uint8_t pcm[32];
} CAPTURED_FRAME;

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void captureFrame(const LI_DS5_HAPTICS_PCM_FRAME* frame, void* context) {
    CAPTURED_FRAME* captured = context;
    captured->count++;
    captured->frame = *frame;
    if (frame->pcmDataLength <= sizeof(captured->pcm)) {
        memcpy(captured->pcm, frame->pcmData, frame->pcmDataLength);
        captured->frame.pcmData = captured->pcm;
    }
}

static void testValidFrame(void) {
    static const uint8_t packet[] = {
        0x01, 0x05, 0x1c, 0x00,
        0x03, 0x00, 0x02, 0x00,
        0x44, 0x33, 0x22, 0x11,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x80, 0xbb, 0x00, 0x00,
        0x02, 0x10, 0x00, 0x00,
        0x01, 0x00, 0x02, 0x00,
        0xff, 0x7f, 0x00, 0x80,
    };
    CAPTURED_FRAME captured = {0};

    CHECK(processDs5HapticsStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    CHECK(captured.count == 1);
    CHECK(captured.frame.flags == (LI_DS5_HAPTICS_PCM_FLAG_STREAM_START |
                                   LI_DS5_HAPTICS_PCM_FLAG_DISCONTINUITY));
    CHECK(captured.frame.controllerNumber == 3);
    CHECK(captured.frame.frameCount == 2);
    CHECK(captured.frame.sequenceNumber == 0x11223344);
    CHECK(captured.frame.presentationTimeUs == UINT64_C(0x0102030405060708));
    CHECK(captured.frame.sampleRate == 48000);
    CHECK(captured.frame.channelCount == 2);
    CHECK(captured.frame.bitsPerSample == 16);
    CHECK(captured.frame.pcmDataLength == 8);
    CHECK(memcmp(captured.frame.pcmData, packet + 28, 8) == 0);
}

static void testFourChannelFrame(void) {
    static const uint8_t packet[] = {
        0x01, 0x00, 0x1c, 0x00,
        0x00, 0x00, 0x02, 0x00,
        0x09, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0xbb, 0x00, 0x00,
        0x04, 0x10, 0x00, 0x00,
        0x11, 0x12, 0x13, 0x14, 0x01, 0x02, 0x03, 0x04,
        0x21, 0x22, 0x23, 0x24, 0x05, 0x06, 0x07, 0x08,
    };
    CAPTURED_FRAME captured = {0};

    CHECK(processDs5HapticsStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    CHECK(captured.count == 1);
    CHECK(captured.frame.channelCount == 4);
    CHECK(captured.frame.frameCount == 2);
    CHECK(captured.frame.pcmDataLength == 16);
    CHECK(memcmp(captured.frame.pcmData, packet + 28, 16) == 0);
    // A frame count that does not match the declared channels is refused.
    {
        uint8_t shortPacket[sizeof(packet) - 8];
        memcpy(shortPacket, packet, sizeof(shortPacket));
        captured.count = 0;
        CHECK(!processDs5HapticsStreamPacket(shortPacket, sizeof(shortPacket), captureFrame, &captured));
        CHECK(captured.count == 0);
    }
}

static void testStreamEndWithoutPcm(void) {
    static const uint8_t packet[] = {
        0x01, 0x02, 0x1c, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0xbb, 0x00, 0x00,
        0x02, 0x10, 0x00, 0x00,
    };
    CAPTURED_FRAME captured = {0};

    CHECK(processDs5HapticsStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    CHECK(captured.count == 1);
    CHECK(captured.frame.flags == LI_DS5_HAPTICS_PCM_FLAG_STREAM_END);
    CHECK(captured.frame.pcmDataLength == 0);
}

static void testMalformedFrames(void) {
    uint8_t packet[32] = {
        0x01, 0x00, 0x1c, 0x00,
        0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x80, 0xbb, 0x00, 0x00,
        0x02, 0x10, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    CAPTURED_FRAME captured = {0};

    CHECK(!processDs5HapticsStreamPacket(packet, 27, captureFrame, &captured));
    packet[0] = 2;
    CHECK(!processDs5HapticsStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    packet[0] = 1;
    packet[24] = 6;
    CHECK(!processDs5HapticsStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    packet[24] = 2;
    packet[26] = 1;
    CHECK(!processDs5HapticsStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    packet[26] = 0;
    CHECK(!processDs5HapticsStreamPacket(packet, sizeof(packet) - 1, captureFrame, &captured));
    CHECK(captured.count == 0);
}

int main(void) {
    testValidFrame();
    testFourChannelFrame();
    testStreamEndWithoutPcm();
    testMalformedFrames();

    if (failures != 0) {
        fprintf(stderr, "%d DualSense haptics protocol checks failed\n", failures);
        return 1;
    }

    puts("DualSense haptics protocol golden vectors passed");
    return 0;
}
