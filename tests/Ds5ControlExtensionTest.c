#include "Ds5ControlExtension.h"

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

static uint32_t parse(const char* text) {
    return ds5ExtParseSdpValue(text, strlen(text));
}

static void testSdpNamespace(void) {
    // The offer a client sends is understood by a peer that implements the namespace.
    CHECK(parse(DS5EXT_SDP_OFFER) == (DS5EXT_MASK_OUTPUT_V1 | DS5EXT_MASK_INPUT_STATUS_V1));
    CHECK(parse(DS5EXT_SDP_OFFER_PAD_AUDIO) == (DS5EXT_MASK_OUTPUT_V1 | DS5EXT_MASK_INPUT_STATUS_V1 | DS5EXT_MASK_PAD_AUDIO_V1));
    CHECK(parse("pad-audio=2") == 0);
    CHECK(parse("output=1") == DS5EXT_MASK_OUTPUT_V1);
    CHECK(parse("input-status=1") == DS5EXT_MASK_INPUT_STATUS_V1);
    CHECK(parse("output=1;input-status=1") == 3);
    CHECK(parse("  output=1 ,  input-status=1  ") == 3);
    // An old peer sends nothing and sees nothing: an empty or absent value is an empty mask.
    CHECK(parse("") == 0);
    CHECK(ds5ExtParseSdpValue("", 0) == 0);
    // Unknown names, other versions and malformed tokens never produce a bit.
    CHECK(parse("output=2") == 0);
    CHECK(parse("output=10") == 0);
    CHECK(parse("output=") == 0);
    CHECK(parse("output") == 0);
    CHECK(parse("=1") == 0);
    CHECK(parse("outputs=1") == 0);
    CHECK(parse("output=1x") == 0);
    CHECK(parse("Output=1") == 0);
    CHECK(parse("future=1,output=1") == DS5EXT_MASK_OUTPUT_V1);
    CHECK(parse("output=1,,;;input-status=0") == DS5EXT_MASK_OUTPUT_V1);
    // Length bounds the read; trailing bytes after the length are not looked at.
    CHECK(ds5ExtParseSdpValue("output=1,input-status=1", 8) == DS5EXT_MASK_OUTPUT_V1);
}

static void testOutputGolden(void) {
    // version 1, reserved 0, controller 0x0302, valid 0x000000a5, then eight value bytes.
    static const uint8_t golden[16] = {
        0x01, 0x00, 0x02, 0x03, 0xa5, 0x00, 0x00, 0x00,
        0x1b, 0x00, 0x7f, 0x00, 0x00, 0x01, 0x00, 0x02,
    };
    DS5EXT_OUTPUT decoded;
    DS5EXT_OUTPUT source;
    uint8_t body[16];

    CHECK(ds5ExtDecodeOutput(golden, sizeof(golden), &decoded));
    CHECK(decoded.controllerNumber == 0x0302);
    CHECK(decoded.validMask == 0xa5);
    CHECK(decoded.playerLedMask == 0x1b);
    CHECK(decoded.speakerVolume == 0x7f);
    CHECK(decoded.micMute == 1);
    CHECK(decoded.lightbarBrightness == 2);
    CHECK(decoded.muteLed == 0 && decoded.headphoneVolume == 0 && decoded.micVolume == 0 &&
          decoded.audioRouting == 0);

    memset(&source, 0, sizeof(source));
    source.controllerNumber = 0x0302;
    source.validMask = 0xa5;
    source.playerLedMask = 0x1b;
    source.speakerVolume = 0x7f;
    source.micMute = 1;
    source.lightbarBrightness = 2;
    CHECK(ds5ExtEncodeOutput(&source, body, sizeof(body)) == sizeof(body));
    CHECK(memcmp(body, golden, sizeof(golden)) == 0);
}

static void testOutputEachBit(void) {
    uint32_t bit;
    for (bit = 0; bit < 8; bit++) {
        DS5EXT_OUTPUT in, out;
        uint8_t body[16];
        memset(&in, 0, sizeof(in));
        in.controllerNumber = (uint16_t)(bit + 1);
        in.validMask = 1u << bit;
        in.playerLedMask = (bit == 0) ? 0x15 : 0;
        in.muteLed = (bit == 1) ? 1 : 0;
        in.speakerVolume = (bit == 2) ? 0xff : 0;
        in.headphoneVolume = (bit == 3) ? 0x40 : 0;
        in.micVolume = (bit == 4) ? 0x33 : 0;
        in.micMute = (bit == 5) ? 1 : 0;
        in.audioRouting = (bit == 6) ? 0x30 : 0;
        in.lightbarBrightness = (bit == 7) ? 1 : 0;
        CHECK(ds5ExtEncodeOutput(&in, body, sizeof(body)) == 16);
        CHECK(ds5ExtDecodeOutput(body, sizeof(body), &out));
        CHECK(memcmp(&in, &out, sizeof(in)) == 0);
    }
}

static void testOutputRejections(void) {
    static const uint8_t good[16] = {
        0x01, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00,
        0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    DS5EXT_OUTPUT out;
    uint8_t bad[17];
    size_t index;

    CHECK(ds5ExtDecodeOutput(good, 16, &out));
    CHECK(!ds5ExtDecodeOutput(good, 15, &out));
    memcpy(bad, good, 16);
    bad[16] = 0;
    CHECK(!ds5ExtDecodeOutput(bad, 17, &out));
    CHECK(!ds5ExtDecodeOutput(NULL, 16, &out));
    CHECK(!ds5ExtDecodeOutput(good, 16, NULL));

    memcpy(bad, good, 16); bad[0] = 2; CHECK(!ds5ExtDecodeOutput(bad, 16, &out));       // version
    memcpy(bad, good, 16); bad[1] = 1; CHECK(!ds5ExtDecodeOutput(bad, 16, &out));       // reserved
    // A speaker bit with a zero value is a legal setting (volume zero) and is accepted.
    memcpy(bad, good, 16); bad[4] = 0x07; CHECK(ds5ExtDecodeOutput(bad, 16, &out));
    memcpy(bad, good, 16); bad[4] = 0x00; CHECK(!ds5ExtDecodeOutput(bad, 16, &out));    // fields without bits
    memcpy(bad, good, 16); bad[5] = 0x01; CHECK(!ds5ExtDecodeOutput(bad, 16, &out));    // unknown bit 8
    memcpy(bad, good, 16); bad[7] = 0x80; CHECK(!ds5ExtDecodeOutput(bad, 16, &out));    // unknown bit 31
    memcpy(bad, good, 16); bad[8] = 0x20; CHECK(!ds5ExtDecodeOutput(bad, 16, &out));    // player mask > 0x1f
    memcpy(bad, good, 16); bad[9] = 2; CHECK(!ds5ExtDecodeOutput(bad, 16, &out));       // mute led > 1
    memcpy(bad, good, 16); bad[4] = 0x23; bad[13] = 2;
    CHECK(!ds5ExtDecodeOutput(bad, 16, &out));                                          // mic mute > 1
    // Every non-zero value byte under a clear valid bit is rejected whole.
    for (index = 8; index < 16; index++) {
        memcpy(bad, good, 16);
        bad[4] = 0x00;
        bad[8] = bad[9] = 0;
        bad[index] = 1;
        CHECK(!ds5ExtDecodeOutput(bad, 16, &out));
    }
    // A rejected body leaves the destination untouched.
    memset(&out, 0x5a, sizeof(out));
    memcpy(bad, good, 16); bad[0] = 9;
    CHECK(!ds5ExtDecodeOutput(bad, 16, &out));
    CHECK(((uint8_t*)&out)[0] == 0x5a);
}

static void testInputStatus(void) {
    static const uint8_t golden[12] = {
        0x01, 0x00, 0x03, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    };
    DS5EXT_INPUT_STATUS in, out;
    uint8_t body[12];
    uint8_t bad[12];
    uint32_t bit;

    memset(&in, 0, sizeof(in));
    in.controllerNumber = 3;
    in.validMask = DS5EXT_IN_HEADSET | DS5EXT_IN_MICROPHONE | DS5EXT_IN_MIC_MUTED;
    in.headsetPresent = 1;
    in.microphonePresent = 1;
    in.microphoneMuted = 0;
    CHECK(ds5ExtEncodeInputStatus(&in, body, sizeof(body)) == 12);
    CHECK(memcmp(body, golden, sizeof(golden)) == 0);
    CHECK(ds5ExtDecodeInputStatus(golden, sizeof(golden), &out));
    CHECK(memcmp(&in, &out, sizeof(in)) == 0);

    for (bit = 0; bit < 3; bit++) {
        memset(&in, 0, sizeof(in));
        in.controllerNumber = 1;
        in.validMask = 1u << bit;
        in.headsetPresent = bit == 0;
        in.microphonePresent = bit == 1;
        in.microphoneMuted = bit == 2;
        CHECK(ds5ExtEncodeInputStatus(&in, body, sizeof(body)) == 12);
        CHECK(ds5ExtDecodeInputStatus(body, sizeof(body), &out));
        CHECK(memcmp(&in, &out, sizeof(in)) == 0);
    }

    CHECK(!ds5ExtDecodeInputStatus(golden, 11, &out));
    memcpy(bad, golden, 12); bad[0] = 2; CHECK(!ds5ExtDecodeInputStatus(bad, 12, &out));
    memcpy(bad, golden, 12); bad[1] = 1; CHECK(!ds5ExtDecodeInputStatus(bad, 12, &out));
    memcpy(bad, golden, 12); bad[11] = 1; CHECK(!ds5ExtDecodeInputStatus(bad, 12, &out));
    memcpy(bad, golden, 12); bad[4] = 0x08; CHECK(!ds5ExtDecodeInputStatus(bad, 12, &out));
    memcpy(bad, golden, 12); bad[4] = 0x06; CHECK(!ds5ExtDecodeInputStatus(bad, 12, &out)); // headset set, bit clear
    memcpy(bad, golden, 12); bad[8] = 2; CHECK(!ds5ExtDecodeInputStatus(bad, 12, &out));
    in.validMask = 0x08;
    CHECK(ds5ExtEncodeInputStatus(&in, body, sizeof(body)) == 0);
}

int main(void) {
    testSdpNamespace();
    testOutputGolden();
    testOutputEachBit();
    testOutputRejections();
    testInputStatus();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ds5 control extension tests passed\n");
    return 0;
}
