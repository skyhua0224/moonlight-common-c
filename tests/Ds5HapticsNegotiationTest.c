#include "Limelight-internal.h"
#include <stdio.h>

static unsigned failures;
#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            failures++;                                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value);                            \
        }                                                                                          \
    } while (0)

static void onV2(const LI_DS5_HAPTICS_IR_FRAME_V2 *frame) { (void)frame; }
static void onExtended(const LI_DS5_HAPTICS_IR_EXTENDED_FRAME *frame) { (void)frame; }
static void onPcm(const LI_DS5_HAPTICS_PCM_FRAME *frame) { (void)frame; }

static void checkFeatures(bool v2, bool enhanced, bool pcm, bool sunshine) {
    int length = 0;
    char *sdp;
    const char *field;
    uint32_t flags;
    gConnectionContext.AppVersionQuad[3] = sunshine ? -1 : 0;
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    ListenerCallbacks.ds5HapticsIrV2 = v2 ? onV2 : NULL;
    ListenerCallbacks.ds5HapticsIrExtended = enhanced ? onExtended : NULL;
    ListenerCallbacks.ds5HapticsPcm = pcm ? onPcm : NULL;
    sdp = getSdpPayloadForStreamConfig(14, &length);
    CHECK(sdp != NULL && length > 0);
    if (sdp == NULL)
        return;
    field = strstr(sdp, "a=x-ml-general.featureFlags:");
    if (!sunshine) {
        CHECK(field == NULL);
        free(sdp);
        return;
    }
    CHECK(field != NULL);
    if (field != NULL) {
        flags = (uint32_t)strtoul(field + strlen("a=x-ml-general.featureFlags:"), NULL, 10);
        CHECK((flags & ML_FF_DS5_HAPTICS_IR_V2) == (v2 ? ML_FF_DS5_HAPTICS_IR_V2 : 0));
        CHECK((flags & ML_FF_DS5_HAPTICS_IR_EXTENDED) == (enhanced ? ML_FF_DS5_HAPTICS_IR_EXTENDED : 0));
        CHECK((flags & ML_FF_DS5_HAPTICS_PCM) == (pcm ? ML_FF_DS5_HAPTICS_PCM : 0));
        CHECK((flags & (ML_FF_FEC_STATUS | ML_FF_SESSION_ID_V1)) ==
              (ML_FF_FEC_STATUS | ML_FF_SESSION_ID_V1));
    }
    free(sdp);
}

int main(void) {
    CHECK(ML_FF_DS5_HAPTICS_IR_EXTENDED == 0x80);
    CHECK(
        (ML_FF_DS5_HAPTICS_IR_EXTENDED & (ML_FF_FEC_STATUS | ML_FF_SESSION_ID_V1 | ML_FF_DS5_HAPTICS_PCM |
                                    ML_FF_DS5_HAPTICS_IR_V2 | ML_FF_REMOTE_TEXT_CONTEXT)) == 0);
    CHECK(initializePlatform() == 0);
    gConnectionContext.AppVersionQuad[0] = 7;
    gConnectionContext.AppVersionQuad[1] = 1;
    gConnectionContext.AppVersionQuad[2] = 431;
    gConnectionContext.AppVersionQuad[3] = -1;
    NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    RtspPortNumber = 53010;
    VideoPortNumber = 52998;
    AudioPacketDuration = 5;
    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    ((struct sockaddr_in *)&RemoteAddr)->sin_family = AF_INET;
    ((struct sockaddr_in *)&RemoteAddr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    LiInitializeStreamConfiguration(&StreamConfig);
    StreamConfig.width = 1280;
    StreamConfig.height = 720;
    StreamConfig.fps = 30;
    StreamConfig.bitrate = 10000;
    StreamConfig.packetSize = 1392;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    EncryptionFeaturesSupported = SS_ENC_CONTROL_V2;
    EncryptionFeaturesRequested = 0;
    EncryptionFeaturesEnabled = 0;
    CHECK(initializeControlStream() == 0);
    checkFeatures(false, false, false, true);
    checkFeatures(true, false, false, true);
    // Enhanced sessions retain the base bit so old hosts can still send v2.
    checkFeatures(true, true, false, true);
    checkFeatures(false, false, true, true);
    checkFeatures(true, true, false, false);
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    // destroyControlStream() expects the stream to have been stopped.
    gConnectionContext.controlContext.stopping = true;
    destroyControlStream();
    cleanupPlatform();
    printf("DualSense IR negotiation: %u failures\n", failures);
    return failures == 0 ? 0 : 1;
}
