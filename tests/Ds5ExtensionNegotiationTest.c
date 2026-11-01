#include "Limelight-internal.h"
#include "Ds5ControlExtension.h"

// The internal header maps these names onto the global connection context.
#undef RemoteAddr
#undef AddrLen
#undef StreamConfig
#undef NegotiatedVideoFormat
#undef ListenerCallbacks
#undef RtspPortNumber
#undef VideoPortNumber

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Negotiation behaviour of the DualSense control extensions against the real
// SDP generator and the real send gate: an old peer must see no change.

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void ignorePcm(const LI_DS5_HAPTICS_PCM_FRAME* frame) {
    (void)frame;
}

static void ignoreOutput(const LI_DS5_OUTPUT_EXTENSION* output) {
    (void)output;
}

static PML_CONNECTION_CONTEXT newContext(bool sunshine) {
    // The generator also reads the global context, so the test fills that one.
    PML_CONNECTION_CONTEXT ctx = &gConnectionContext;
    struct sockaddr_in* addr;

    memset(ctx, 0, sizeof(*ctx));
    ctx->AppVersionQuad[0] = 7;
    ctx->AppVersionQuad[1] = 1;
    ctx->AppVersionQuad[2] = 431;
    ctx->AppVersionQuad[3] = sunshine ? -1 : 0;
    ctx->StreamConfig.width = 1920;
    ctx->StreamConfig.height = 1080;
    ctx->StreamConfig.fps = 60;
    ctx->StreamConfig.bitrate = 20000;
    ctx->StreamConfig.packetSize = 1392;
    ctx->StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    ctx->StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    ctx->StreamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    ctx->NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    ctx->RtspPortNumber = 48010;
    ctx->VideoPortNumber = 47998;
    addr = (struct sockaddr_in*)&ctx->RemoteAddr;
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(0x7f000001u);
    ctx->AddrLen = sizeof(*addr);
    return ctx;
}

static void ignoreIr(const LI_DS5_HAPTICS_IR_FRAME_V2* frame) { (void)frame; }

static char* sdpFor(PML_CONNECTION_CONTEXT ctx) {
    int length = 0;
    char* payload = getSdpPayloadForStreamConfigCtx(ctx, 14, &length);
    if (payload == NULL) {
        return NULL;
    }
    char* copy = malloc((size_t)length + 1);
    memcpy(copy, payload, (size_t)length);
    copy[length] = '\0';
    free(payload);
    return copy;
}

static void testClientOffer(void) {
    PML_CONNECTION_CONTEXT ctx = newContext(true);
    char* withoutCallback = sdpFor(ctx);
    char* withCallback;

    CHECK(withoutCallback != NULL);
    // No callback registered: the offer is absent and the SDP carries no trace of the extension.
    CHECK(withoutCallback != NULL && strstr(withoutCallback, "ds5Extensions") == NULL);

    LiSetDs5OutputExtensionCallbackCtx(ctx, ignoreOutput);
    withCallback = sdpFor(ctx);
    CHECK(withCallback != NULL);
    CHECK(withCallback != NULL &&
          strstr(withCallback, "a=x-ml-general.ds5Extensions:output=1,input-status=1") != NULL);

    // A client that can play the pad's speaker (a haptics PCM callback) adds the pad-audio token.
    ctx->ListenerCallbacks.ds5HapticsPcm = ignorePcm;
    {
        // The PCM callback alone offers no speaker token: the offer mode says what the client does with the pair.
        char* plain = sdpFor(ctx);
        const char* plainLine = plain != NULL ? strstr(plain, "ds5Extensions:output=1,input-status=1") : NULL;
        CHECK(plainLine != NULL && strncmp(plainLine, "ds5Extensions:output=1,input-status=1", 37) == 0 && plainLine[37] != ',');
        free(plain);
    }
    LiSetDs5PadAudioOffer(1);
    {
        char* withPadAudio = sdpFor(ctx);
        CHECK(withPadAudio != NULL &&
              strstr(withPadAudio, "a=x-ml-general.ds5Extensions:output=1,input-status=1,pad-audio=1") != NULL);
        free(withPadAudio);
    }
    // "pad-speaker": IR haptics plus the speaker pair. The PCM feature flag (0x04) must not be offered, or a host that does not know the
    // token sees PCM and IR together and takes the raw PCM route, which sends no IR.
    ctx->ListenerCallbacks.ds5HapticsIrV2 = ignoreIr;
    LiSetDs5PadAudioOffer(2);
    {
        char* withPadSpeaker = sdpFor(ctx);
        CHECK(withPadSpeaker != NULL &&
              strstr(withPadSpeaker, "a=x-ml-general.ds5Extensions:output=1,input-status=1,pad-speaker=1") != NULL);
        const char* flags = withPadSpeaker != NULL ? strstr(withPadSpeaker, "x-ml-general.featureFlags:") : NULL;
        CHECK(flags != NULL);
        if (flags != NULL) {
            const unsigned long value = strtoul(flags + strlen("x-ml-general.featureFlags:"), NULL, 10);
            CHECK((value & ML_FF_DS5_HAPTICS_IR_V2) != 0);
            CHECK((value & ML_FF_DS5_HAPTICS_PCM) == 0);
        }
        free(withPadSpeaker);
    }
    LiSetDs5PadAudioOffer(0);
    ctx->ListenerCallbacks.ds5HapticsIrV2 = NULL;
    ctx->ListenerCallbacks.ds5HapticsPcm = NULL;

    // The only difference between the two payloads is the one attribute line.
    if (withoutCallback != NULL && withCallback != NULL) {
        const char* line = strstr(withCallback, "a=x-ml-general.ds5Extensions:");
        size_t before = (size_t)(line - withCallback);
        size_t lineLength = strcspn(line, "\n") + 1;
        CHECK(strlen(withCallback) == strlen(withoutCallback) + lineLength);
        CHECK(strncmp(withCallback, withoutCallback, before) == 0);
        CHECK(strcmp(withCallback + before + lineLength, withoutCallback + before) == 0);
    }
    free(withoutCallback);
    free(withCallback);

    // A non-Sunshine host never gets the attribute, even with the callback set.
    ctx = newContext(false);
    LiSetDs5OutputExtensionCallbackCtx(ctx, ignoreOutput);
    withCallback = sdpFor(ctx);
    CHECK(withCallback == NULL || strstr(withCallback, "ds5Extensions") == NULL);
    free(withCallback);
}

static void testSendGate(void) {
    PML_CONNECTION_CONTEXT ctx = newContext(true);
    static short packetTypes[32];
    int index;

    for (index = 0; index < 32; index++) {
        packetTypes[index] = -1;
    }
    packetTypes[26] = 0x5516;
    ctx->controlContext.connectionContext = ctx;
    ctx->controlContext.packetTypes = packetTypes;

    // Old host: it declared nothing. Nothing is sent, and the call does not touch the peer.
    ctx->controlContext.ds5ExtensionMask = 0;
    CHECK(LiSendDs5InputStatusCtx(&ctx->controlContext, 0, DS5EXT_IN_HEADSET, 1, 0, 0) == LI_ERR_UNSUPPORTED);
    CHECK(LiGetHostDs5ExtensionsCtx(ctx) == 0);

    // A host that declared only "output" does not get 0x5516.
    ctx->controlContext.ds5ExtensionMask = DS5EXT_MASK_OUTPUT_V1;
    CHECK(LiSendDs5InputStatusCtx(&ctx->controlContext, 0, DS5EXT_IN_HEADSET, 1, 0, 0) == LI_ERR_UNSUPPORTED);

    // Declared, but the control stream is not connected: still nothing.
    ctx->controlContext.ds5ExtensionMask = DS5EXT_MASK_INPUT_STATUS_V1;
    CHECK(LiSendDs5InputStatusCtx(&ctx->controlContext, 0, DS5EXT_IN_HEADSET, 1, 0, 0) == LI_ERR_UNSUPPORTED);
    CHECK(LiGetHostDs5ExtensionsCtx(ctx) == DS5EXT_MASK_INPUT_STATUS_V1);

    // A non-Sunshine host is never a target.
    ctx->AppVersionQuad[3] = 0;
    CHECK(LiSendDs5InputStatusCtx(&ctx->controlContext, 0, DS5EXT_IN_HEADSET, 1, 0, 0) == LI_ERR_UNSUPPORTED);
    CHECK(LiSendDs5InputStatusCtx(NULL, 0, DS5EXT_IN_HEADSET, 1, 0, 0) == LI_ERR_UNSUPPORTED);
}

int main(void) {
    testClientOffer();
    testSendGate();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ds5 extension negotiation tests passed\n");
    return 0;
}
