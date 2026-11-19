/* The encrypted RTSP ANNOUNCE a Sunshine host accepts is small: rtsp.cpp reads it into a 2048-byte buffer and refuses a
 * message whose 24-byte header plus payload does not fit ("Exceeded maximum rtsp packet size: 2048"), dropping the
 * connection. This builds the SDP with the real generator for the worst case (every option on) and checks the
 * serialized request against that limit. */
#include "../src/SdpGenerator.c"
#include "../src/Rtsp.h"
/* SdpGenerator.c redirects state through a local ctx; the test addresses the context explicitly. */
#undef AppVersionQuad
#undef StreamConfig
#undef RemoteAddr
#undef VideoPortNumber
#undef RtspPortNumber
#undef EncryptionFeaturesEnabled
#undef EncryptionFeaturesSupported
#undef EncryptionFeaturesRequested
#undef NegotiatedVideoFormat
#undef VideoCallbacks
#undef AudioCallbacks
#undef HighQualitySurroundSupported
#undef HighQualitySurroundEnabled
#undef AudioPacketDuration
#undef ListenerCallbacks
#undef RemoteTextContextCallback
#undef Ds5OutputExtensionCallback
#undef Ds5HapticsStateCallback
#undef HostSessionStateCallback
#undef MatLiveProfile
#undef EndpointRefreshRequested
#undef NegotiatedAudioCodec
#undef NegotiatedAudioBitrate
#undef VideoFecControlSupported
#include <stdio.h>

/* What the generator needs from the rest of the library. */
ML_CONNECTION_CONTEXT gConnectionContext;
PML_CONNECTION_CONTEXT LiGetThreadConnectionContext(void) { return &gConnectionContext; }
bool PltSafeStrcpy(char* dest, size_t dest_size, const char* src) {
    size_t n = strlen(src);
    if (n + 1 > dest_size) return false;
    memcpy(dest, src, n + 1);
    return true;
}
void addrToUrlSafeString(struct sockaddr_storage* addr, char* string, size_t stringLength) {
    (void)addr;
    snprintf(string, stringLength, "%s", "fd4b:c064:5bfa:48a2:6de8:8e5f:6a78:4c4d");   /* an IPv6 literal: the longest address */
}
bool isReferenceFrameInvalidationSupportedByDecoder(void) { return true; }

#define HOST_BUFFER 2048
#define HOST_HEADER 24      /* encrypted_rtsp_header_t: type+length, sequence, 16-byte tag */
#define MAX_PLAINTEXT (HOST_BUFFER - HOST_HEADER - 1)

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void noop(void) {}

static bool addOption(PRTSP_MESSAGE msg, char* option, char* content) {
    POPTION_ITEM item = malloc(sizeof(*item));
    if (item == NULL) return false;
    item->option = strdup(option);
    item->content = strdup(content);
    item->next = NULL;
    item->flags = FLAG_ALLOCATED_OPTION_FIELDS;
    insertOption(&msg->options, item);
    msg->flags |= FLAG_ALLOCATED_OPTION_ITEMS;
    return true;
}

/* Every option the client can switch on at once. */
static void fillWorstCase(PML_CONNECTION_CONTEXT ctx, bool sunshine) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->AppVersionQuad[0] = 7; ctx->AppVersionQuad[1] = 1; ctx->AppVersionQuad[2] = 431; ctx->AppVersionQuad[3] = sunshine ? -1 : 0;
    ctx->StreamConfig.width = 3840; ctx->StreamConfig.height = 2160; ctx->StreamConfig.fps = 120;
    ctx->StreamConfig.bitrate = 150000; ctx->StreamConfig.packetSize = 1392;
    ctx->StreamConfig.streamingRemotely = STREAM_CFG_REMOTE;
    ctx->StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
    ctx->StreamConfig.clientRefreshRateX100 = 11997;
    ctx->StreamConfig.colorSpace = COLORSPACE_REC_2020; ctx->StreamConfig.colorRange = COLOR_RANGE_FULL;
    ctx->StreamConfig.encryptionFlags = ENCFLG_ALL;
    ctx->StreamConfig.enableMic = true;
    ctx->StreamConfig.dynamicRangeMode = DYNAMIC_RANGE_MODE_HDR10_PQ;
    ctx->StreamConfig.dynamicHdrCaps = 7; ctx->StreamConfig.dynamicHdrPreference = 2; ctx->StreamConfig.dolbyVisionDirectSurface = true;
    ctx->StreamConfig.videoFecPercentage = 30;                 /* "automatic FEC" */
    ctx->NegotiatedVideoFormat = VIDEO_FORMAT_H265_MAIN10;
    ctx->VideoPortNumber = 47998; ctx->RtspPortNumber = 48010;
    ctx->RemoteAddr.ss_family = AF_INET6;
    ctx->EncryptionFeaturesSupported = SS_ENC_CONTROL_V2 | SS_ENC_VIDEO | SS_ENC_AUDIO | SS_ENC_MICROPHONE;
    ctx->EncryptionFeaturesRequested = SS_ENC_VIDEO | SS_ENC_AUDIO | SS_ENC_MICROPHONE;
    ctx->HighQualitySurroundSupported = true;
    ctx->controlContext.videoFecControlSupported = true;
    ctx->EndpointRefreshRequested = true;
    ctx->NegotiatedAudioCodec = AUDIO_CODEC_MAT_LIVE; ctx->NegotiatedAudioBitrate = 1536000;
    ctx->MatLiveProfile.epoch = 18446744073709551615ULL; ctx->MatLiveProfile.domain = 18446744073709551615ULL;
    ctx->ListenerCallbacks.ds5HapticsPcm = (void*)noop; ctx->ListenerCallbacks.ds5HapticsIrV2 = (void*)noop;
    ctx->ListenerCallbacks.ds5HapticsIrExtended = (void*)noop;
    ctx->Ds5HapticsStateCallback = (void*)noop; ctx->Ds5OutputExtensionCallback = (void*)noop;
    ctx->RemoteTextContextCallback = (void*)noop; ctx->HostSessionStateCallback = (void*)noop;
}

/* The whole plaintext the client seals: request line, headers, SDP. */
static int announceSize(PML_CONNECTION_CONTEXT ctx, const char* hostHeader) {
    int payloadLength = 0;
    char* payload = getSdpPayloadForStreamConfigCtx(ctx, 14, &payloadLength);
    if (payload == NULL) return -1;
    RTSP_MESSAGE request;
    char lengthStr[16];
    createRtspRequest(&request, NULL, 0, "ANNOUNCE", "streamid=control/13/0", "RTSP/1.0", 0, NULL, NULL, 0);
    addOption(&request, "CSeq", "6");
    addOption(&request, "X-GS-ClientVersion", "14");
    addOption(&request, "Host", (char*)hostHeader);
    addOption(&request, "Session", "DEADBEEF");
    addOption(&request, "Content-type", "application/sdp");
    snprintf(lengthStr, sizeof(lengthStr), "%d", payloadLength);
    addOption(&request, "Content-length", lengthStr);
    request.payload = payload; request.payloadLength = payloadLength;
    int total = 0;
    char* serialized = serializeRtspMessage(&request, &total);
    free(serialized);
    request.payload = NULL;
    freeMessage(&request);
    free(payload);
    return total;
}

int main(void) {
    static ML_CONNECTION_CONTEXT ctx;
    /* The longest Host header: an IPv6 literal with a port (the client sends host:port). */
    const char* longHost = "[fd4b:c064:5bfa:48a2:6de8:8e5f:6a78:4c4d]:48010";
    const char* shortHost = "home.example.com:48010";
    int size;

    fillWorstCase(&ctx, true);
    size = announceSize(&ctx, longHost);
    printf("Sunshine host, every option on, IPv6 host: ANNOUNCE %d bytes (limit %d)\n", size, MAX_PLAINTEXT);
    CHECK(size > 0 && size <= MAX_PLAINTEXT);

    fillWorstCase(&ctx, true);
    size = announceSize(&ctx, shortHost);
    printf("Sunshine host, every option on, host name: ANNOUNCE %d bytes\n", size);
    CHECK(size > 0 && size <= MAX_PLAINTEXT);

    ctx.StreamConfig.videoFecPercentage = -2;
    size = announceSize(&ctx, longHost);
    printf("Sunshine host, automatic FEC and bitrate off: ANNOUNCE %d bytes\n", size);
    CHECK(size > 0 && size <= MAX_PLAINTEXT);

    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("rtsp-announce-size: all checks passed\n");
    return 0;
}
