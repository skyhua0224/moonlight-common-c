#include "Limelight-internal.h"
#include "DynamicHdr.h"
#include "Ds5ControlExtension.h"
#include <inttypes.h>

// "pad-audio" (four-channel haptics PCM with the pad's speaker pair) is offered only when the application asks for it:
// the wired path stays on the two haptic channels unless it is switched on.
// 0: no pad audio. 1: "pad-audio" (the haptics PCM packet carries four channels, haptics on 3 and 4). 2: "pad-speaker" (IR v2 haptics, and a
// speaker-only PCM packet next to them). 3: actuator PCM and IR, with no speaker token.
volatile int Ds5PadAudioOfferMode = 0;

void LiSetDs5PadAudioOffer(int mode) {
    Ds5PadAudioOfferMode = mode < 0 ? 0 : (mode > 3 ? 3 : mode);
}

// Macros to redirect global access to context
#define AppVersionQuad (ctx->AppVersionQuad)
#ifdef StreamConfig
#undef StreamConfig
#endif
#ifdef RemoteAddr
#undef RemoteAddr
#endif
#ifdef VideoPortNumber
#undef VideoPortNumber
#endif
#ifdef RtspPortNumber
#undef RtspPortNumber
#endif
#ifdef EncryptionFeaturesEnabled
#undef EncryptionFeaturesEnabled
#endif
#ifdef EncryptionFeaturesSupported
#undef EncryptionFeaturesSupported
#endif
#ifdef EncryptionFeaturesRequested
#undef EncryptionFeaturesRequested
#endif
#ifdef NegotiatedVideoFormat
#undef NegotiatedVideoFormat
#endif
#ifdef VideoCallbacks
#undef VideoCallbacks
#endif
#ifdef AudioCallbacks
#undef AudioCallbacks
#endif
#ifdef HighQualitySurroundSupported
#undef HighQualitySurroundSupported
#endif
#ifdef HighQualitySurroundEnabled
#undef HighQualitySurroundEnabled
#endif
#ifdef AudioPacketDuration
#undef AudioPacketDuration
#endif
#ifdef ListenerCallbacks
#undef ListenerCallbacks
#endif
#ifdef RemoteTextContextCallback
#undef RemoteTextContextCallback
#endif
#ifdef Ds5OutputExtensionCallback
#undef Ds5OutputExtensionCallback
#endif
#ifdef Ds5HapticsStateCallback
#undef Ds5HapticsStateCallback
#endif
#ifdef HostSessionStateCallback
#undef HostSessionStateCallback
#endif
#ifdef MatLiveProfile
#undef MatLiveProfile
#endif
#ifdef EndpointRefreshRequested
#undef EndpointRefreshRequested
#endif
#ifdef NegotiatedAudioCodec
#undef NegotiatedAudioCodec
#endif
#ifdef NegotiatedAudioBitrate
#undef NegotiatedAudioBitrate
#endif
#ifdef VideoFecControlSupported
#undef VideoFecControlSupported
#endif

#define StreamConfig (ctx->StreamConfig)
#define RemoteAddr (ctx->RemoteAddr)
#define VideoPortNumber (ctx->VideoPortNumber)
#define RtspPortNumber (ctx->RtspPortNumber)
#define EncryptionFeaturesEnabled (ctx->EncryptionFeaturesEnabled)
#define EncryptionFeaturesSupported (ctx->EncryptionFeaturesSupported)
#define EncryptionFeaturesRequested (ctx->EncryptionFeaturesRequested)
#define NegotiatedVideoFormat (ctx->NegotiatedVideoFormat)
#define VideoCallbacks (ctx->VideoCallbacks)
#define AudioCallbacks (ctx->AudioCallbacks)
#define HighQualitySurroundSupported (ctx->HighQualitySurroundSupported)
#define HighQualitySurroundEnabled (ctx->HighQualitySurroundEnabled)
#define AudioPacketDuration (ctx->AudioPacketDuration)
#define ListenerCallbacks (ctx->ListenerCallbacks)
#define RemoteTextContextCallback (ctx->RemoteTextContextCallback)
#define Ds5OutputExtensionCallback (ctx->Ds5OutputExtensionCallback)
#define Ds5HapticsStateCallback (ctx->Ds5HapticsStateCallback)
#define HostSessionStateCallback (ctx->HostSessionStateCallback)
#define MatLiveProfile (ctx->MatLiveProfile)
#define EndpointRefreshRequested (ctx->EndpointRefreshRequested)
#define NegotiatedAudioCodec (ctx->NegotiatedAudioCodec)
#define NegotiatedAudioBitrate (ctx->NegotiatedAudioBitrate)
#define VideoFecControlSupported (ctx->controlContext.videoFecControlSupported)

#define MAX_OPTION_NAME_LEN 128

#define MAX_SDP_HEADER_LEN 128
#define MAX_SDP_TAIL_LEN 128

typedef struct _SDP_OPTION {
    char name[MAX_OPTION_NAME_LEN + 1];
    void* payload;
    int payloadLen;
    struct _SDP_OPTION* next;
} SDP_OPTION, *PSDP_OPTION;

// Cleanup the attribute list
static void freeAttributeList(PSDP_OPTION head) {
    PSDP_OPTION next;
    while (head != NULL) {
        next = head->next;
        free(head);
        head = next;
    }
}

// Get the size of the attribute list
static int getSerializedAttributeListSize(PSDP_OPTION head) {
    PSDP_OPTION currentEntry = head;
    size_t size = 0;
    while (currentEntry != NULL) {
        size += strlen("a=");
        size += strlen(currentEntry->name);
        size += strlen(":");
        size += currentEntry->payloadLen;
        size += strlen(" \r\n");

        currentEntry = currentEntry->next;
    }
    // Add one for the null terminator
    return (int)size + 1;
}

// Populate the serialized attribute list into a string
static int fillSerializedAttributeList(char* buffer, size_t length, PSDP_OPTION head) {
    PSDP_OPTION currentEntry = head;
    int offset = 0;
    while (currentEntry != NULL) {
        int ret = snprintf(&buffer[offset], length, "a=%s:", currentEntry->name);
        if (ret > 0 && (size_t)ret < length) {
            offset += ret;
            length -= ret;
        }
        else {
            LC_ASSERT(false);
            break;
        }

        if ((size_t)currentEntry->payloadLen < length) {
            memcpy(&buffer[offset], currentEntry->payload, currentEntry->payloadLen);
            offset += currentEntry->payloadLen;
            length -= currentEntry->payloadLen;
        }
        else {
            LC_ASSERT(false);
            break;
        }

        ret = snprintf(&buffer[offset], length, " \r\n");
        if (ret > 0 && (size_t)ret < length) {
            offset += ret;
            length -= ret;
        }
        else {
            LC_ASSERT(false);
            break;
        }

        currentEntry = currentEntry->next;
    }

    // We should have only space for the null terminator left over
    LC_ASSERT(length == 1);
    return offset;
}

// Add an attribute
static int addAttributeBinary(PSDP_OPTION* head, char* name, const void* payload, int payloadLen) {
    PSDP_OPTION option, currentOption;

    option = malloc(sizeof(*option) + payloadLen);
    if (option == NULL) {
        return -1;
    }

    if (!PltSafeStrcpy(option->name, sizeof(option->name), name)) {
        free(option);
        return -1;
    }

    option->next = NULL;
    option->payloadLen = payloadLen;
    option->payload = (void*)(option + 1);
    memcpy(option->payload, payload, payloadLen);

    if (*head == NULL) {
        *head = option;
    }
    else {
        currentOption = *head;
        while (currentOption->next != NULL) {
            currentOption = currentOption->next;
        }
        currentOption->next = option;
    }

    return 0;
}

// Add an attribute string
static int addAttributeString(PSDP_OPTION* head, char* name, const char* payload) {
    // We purposefully omit the null terminating character
    return addAttributeBinary(head, name, payload, (int)strlen(payload));
}

static int addGen3Options(PSDP_OPTION* head, char* addrStr) {
    int payloadInt;
    int err = 0;

    err |= addAttributeString(head, "x-nv-general.serverAddress", addrStr);

    payloadInt = htonl(0x42774141);
    err |= addAttributeBinary(head,
        "x-nv-general.featureFlags", &payloadInt, sizeof(payloadInt));

    payloadInt = htonl(0x41514141);
    err |= addAttributeBinary(head,
        "x-nv-video[0].transferProtocol", &payloadInt, sizeof(payloadInt));
    err |= addAttributeBinary(head,
        "x-nv-video[1].transferProtocol", &payloadInt, sizeof(payloadInt));
    err |= addAttributeBinary(head,
        "x-nv-video[2].transferProtocol", &payloadInt, sizeof(payloadInt));
    err |= addAttributeBinary(head,
        "x-nv-video[3].transferProtocol", &payloadInt, sizeof(payloadInt));

    payloadInt = htonl(0x42414141);
    err |= addAttributeBinary(head,
        "x-nv-video[0].rateControlMode", &payloadInt, sizeof(payloadInt));
    payloadInt = htonl(0x42514141);
    err |= addAttributeBinary(head,
        "x-nv-video[1].rateControlMode", &payloadInt, sizeof(payloadInt));
    err |= addAttributeBinary(head,
        "x-nv-video[2].rateControlMode", &payloadInt, sizeof(payloadInt));
    err |= addAttributeBinary(head,
        "x-nv-video[3].rateControlMode", &payloadInt, sizeof(payloadInt));

    err |= addAttributeString(head, "x-nv-vqos[0].bw.flags", "14083");

    err |= addAttributeString(head, "x-nv-vqos[0].videoQosMaxConsecutiveDrops", "0");
    err |= addAttributeString(head, "x-nv-vqos[1].videoQosMaxConsecutiveDrops", "0");
    err |= addAttributeString(head, "x-nv-vqos[2].videoQosMaxConsecutiveDrops", "0");
    err |= addAttributeString(head, "x-nv-vqos[3].videoQosMaxConsecutiveDrops", "0");

    return err;
}

static int addGen4Options(PML_CONNECTION_CONTEXT ctx, PSDP_OPTION* head, char* addrStr) {
    char payloadStr[92];
    int err = 0;

    LC_ASSERT(RtspPortNumber != 0);
    snprintf(payloadStr, sizeof(payloadStr), "rtsp://%s:%u", addrStr, RtspPortNumber);
    err |= addAttributeString(head, "x-nv-general.serverAddress", payloadStr);

    return err;
}

#define NVFF_BASE             0x07
#define NVFF_AUDIO_ENCRYPTION 0x20
#define NVFF_RI_ENCRYPTION    0x80

static int addGen5Options(PML_CONNECTION_CONTEXT ctx, PSDP_OPTION* head) {
    int err = 0;
    char payloadStr[32];

    // This must be initialized to false already
    LC_ASSERT(!AudioEncryptionEnabled);

    if (APP_VERSION_AT_LEAST(7, 1, 431)) {
        unsigned int featureFlags;

        // RI encryption is always enabled
        featureFlags = NVFF_BASE | NVFF_RI_ENCRYPTION;

        // Enable audio encryption if the client opted in or the host required it
        if ((StreamConfig.encryptionFlags & ENCFLG_AUDIO) || (EncryptionFeaturesEnabled & SS_ENC_AUDIO)) {
            featureFlags |= NVFF_AUDIO_ENCRYPTION;
            AudioEncryptionEnabled = true;
        }

        snprintf(payloadStr, sizeof(payloadStr), "%u", featureFlags);
        err |= addAttributeString(head, "x-nv-general.featureFlags", payloadStr);

        // Ask for the encrypted control protocol to ensure remote input will be encrypted.
        // This used to be done via separate RI encryption, but now it is all or nothing.
        err |= addAttributeString(head, "x-nv-general.useReliableUdp", "13");

        // Require at least 2 FEC packets for small frames. If a frame has fewer data shards
        // than would generate 2 FEC shards, it will increase the FEC percentage for that frame
        // above the set value (even going as high as 200% FEC to generate 2 FEC shards from a
        // 1 data shard frame).
        err |= addAttributeString(head, "x-nv-vqos[0].fec.minRequiredFecPackets", "2");

        // BLL-FEC appears to adjust dynamically based on the loss rate and instantaneous bitrate
        // of each frame, however we can't dynamically control it from our side yet. As a result,
        // the effective FEC amount is significantly lower (single digit percentages for many
        // large frames) and the result is worse performance during packet loss. Disabling BLL-FEC
        // results in GFE 3.26 falling back to the legacy FEC method as we would like.
        err |= addAttributeString(head, "x-nv-vqos[0].bllFec.enable", "0");
    }
    else {
        // We want to use the new ENet connections for control and input
        err |= addAttributeString(head, "x-nv-general.useReliableUdp", "1");
        err |= addAttributeString(head, "x-nv-ri.useControlChannel", "1");

        // When streaming 4K, lower FEC levels to reduce stream overhead.
        // For remote connections, use higher FEC to compensate for
        // increased packet loss on WAN/Internet paths.
        if (StreamConfig.width >= 3840 && StreamConfig.height >= 2160) {
            err |= addAttributeString(head, "x-nv-vqos[0].fec.repairPercent",
                StreamConfig.streamingRemotely == STREAM_CFG_REMOTE ? "10" : "5");
        }
        else {
            err |= addAttributeString(head, "x-nv-vqos[0].fec.repairPercent",
                StreamConfig.streamingRemotely == STREAM_CFG_REMOTE ? "25" : "20");
        }
    }

    if (APP_VERSION_AT_LEAST(7, 1, 446) && (StreamConfig.width < 720 || StreamConfig.height < 540)) {
        // We enable DRC with a static DRC table for very low resoutions on GFE 3.26 to work around
        // a bug that causes nvstreamer.exe to crash due to failing to populate a list of valid resolutions.
        //
        // Despite the fact that the DRC table doesn't include our target streaming resolution, we still
        // seem to stream at the target resolution, presumably because we don't send control data to tell
        // the host otherwise.
        err |= addAttributeString(head, "x-nv-vqos[0].drc.enable", "1");
        err |= addAttributeString(head, "x-nv-vqos[0].drc.tableType", "2");
    }
    else {
        // Disable dynamic resolution switching
        err |= addAttributeString(head, "x-nv-vqos[0].drc.enable", "0");
    }

    // Recovery mode can cause the FEC percentage to change mid-frame, which
    // breaks many assumptions in RTP FEC queue.
    err |= addAttributeString(head, "x-nv-general.enableRecoveryMode", "0");

    return err;
}

static PSDP_OPTION getAttributesList(PML_CONNECTION_CONTEXT ctx, char*urlSafeAddr) {
    PSDP_OPTION optionHead;
    char payloadStr[92];
    int audioChannelCount;
    int audioChannelMask;
    int err;
    int adjustedBitrate;

    // This must have been resolved to either local or remote by now
    LC_ASSERT(StreamConfig.streamingRemotely != STREAM_CFG_AUTO);

    optionHead = NULL;
    err = 0;

    if (IS_SUNSHINE()) {
        // Send client feature flags to Sunshine hosts
        uint32_t moonlightFeatureFlags = ML_FF_FEC_STATUS |
                                         ML_FF_SESSION_ID_V1;
        if (EndpointRefreshRequested) {
            moonlightFeatureFlags |= ML_FF_ENDPOINT_AUTO_REFRESH;
            err |= addAttributeString(&optionHead, "x-ml-general.endpointAutoRefreshVersion", "1");
        }
        // With "pad-speaker" the PCM callback only receives the speaker pair: the PCM feature flag stays off, so that a host without the
        // token keeps sending the IR frames (a host that sees both flags takes the raw PCM route and sends no IR).
        if (ListenerCallbacks.ds5HapticsPcm != NULL && Ds5PadAudioOfferMode != 2) {
            moonlightFeatureFlags |= ML_FF_DS5_HAPTICS_PCM;
            if (Ds5HapticsStateCallback != NULL) {
                moonlightFeatureFlags |= ML_FF_DS5_HAPTICS_STATE;
            }
        }
        if (ListenerCallbacks.ds5HapticsIrV2 != NULL) {
            moonlightFeatureFlags |= ML_FF_DS5_HAPTICS_IR_V2;
        }
        if (ListenerCallbacks.ds5HapticsIrV2 != NULL && ListenerCallbacks.ds5HapticsIrExtended != NULL) {
            // Enhancement is optional and requires the existing v2 base capability.
            moonlightFeatureFlags |= ML_FF_DS5_HAPTICS_IR_EXTENDED;
        }
        if (RemoteTextContextCallback != NULL) {
            moonlightFeatureFlags |= ML_FF_REMOTE_TEXT_CONTEXT;
        }
        if (HostSessionStateCallback != NULL) {
            moonlightFeatureFlags |= ML_FF_HOST_SESSION_STATE;
        }
        // ML_FF_PYROWAVE and the x-ml-pyrowave.* attributes are intentionally
        // never advertised: PyroWave is not enabled in this client.
        snprintf(payloadStr, sizeof(payloadStr), "%" PRIu32, moonlightFeatureFlags);
        err |= addAttributeString(&optionHead, "x-ml-general.featureFlags", payloadStr);

        // DualSense control extensions (0x5515/0x5516) use a versioned string
        // namespace instead of a capability bit. A host that does not know the
        // attribute ignores it and never sees either message.
        if (Ds5OutputExtensionCallback != NULL) {
            err |= addAttributeString(&optionHead, DS5EXT_SDP_CLIENT_ATTRIBUTE,
                                      (ListenerCallbacks.ds5HapticsPcm != NULL && Ds5PadAudioOfferMode == 1) ? DS5EXT_SDP_OFFER_PAD_AUDIO :
                                      (ListenerCallbacks.ds5HapticsPcm != NULL && Ds5PadAudioOfferMode == 2) ? DS5EXT_SDP_OFFER_PAD_SPEAKER : DS5EXT_SDP_OFFER);
        }

        // New-style control stream encryption is low overhead, so we enable it any time it is supported
        if (EncryptionFeaturesSupported & SS_ENC_CONTROL_V2) {
            EncryptionFeaturesEnabled |= SS_ENC_CONTROL_V2;
        }

        // If video encryption is supported by the host and desired by the client, use it
        if ((EncryptionFeaturesSupported & SS_ENC_VIDEO) && (StreamConfig.encryptionFlags & ENCFLG_VIDEO)) {
            EncryptionFeaturesEnabled |= SS_ENC_VIDEO;
        }
        else if ((EncryptionFeaturesRequested & SS_ENC_VIDEO) && !(StreamConfig.encryptionFlags & ENCFLG_VIDEO)) {
            // If video encryption is explicitly requested by the host but *not* by the client,
            // we'll encrypt anyway (since we are capable of doing so) and print a warning.
            Limelog("Enabling video encryption by host request despite client opt-out. Performance may suffer!");
            EncryptionFeaturesEnabled |= SS_ENC_VIDEO;
        }

        // If audio encryption is supported by the host and desired by the client, use it
        if ((EncryptionFeaturesSupported & SS_ENC_AUDIO) && (StreamConfig.encryptionFlags & ENCFLG_AUDIO)) {
            EncryptionFeaturesEnabled |= SS_ENC_AUDIO;
        }
        else if ((EncryptionFeaturesRequested & SS_ENC_AUDIO) && !(StreamConfig.encryptionFlags & ENCFLG_AUDIO)) {
            // If audio encryption is explicitly requested by the host but *not* by the client,
            // we'll encrypt anyway (since we are capable of doing so) and print a warning.
            Limelog("Enabling audio encryption by host request despite client opt-out. Audio quality may suffer!");
            EncryptionFeaturesEnabled |= SS_ENC_AUDIO;
        }

        // If microphone encryption is supported by the host and desired by the client, use it.
        // Only negotiate this when microphone uplink itself is enabled for the session.
        if (StreamConfig.enableMic &&
            (EncryptionFeaturesSupported & SS_ENC_MICROPHONE) &&
            (StreamConfig.encryptionFlags & ENCFLG_MICROPHONE)) {
            EncryptionFeaturesEnabled |= SS_ENC_MICROPHONE;
        }
        else if (StreamConfig.enableMic &&
                 (EncryptionFeaturesRequested & SS_ENC_MICROPHONE) &&
                 !(StreamConfig.encryptionFlags & ENCFLG_MICROPHONE)) {
            Limelog("Enabling microphone encryption by host request despite client opt-out. Microphone uplink latency may increase!");
            EncryptionFeaturesEnabled |= SS_ENC_MICROPHONE;
        }

        snprintf(payloadStr, sizeof(payloadStr), "%u", EncryptionFeaturesEnabled);
        err |= addAttributeString(&optionHead, "x-ss-general.encryptionEnabled", payloadStr);

        // Enable YUV444 if requested
        if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_YUV444) {
            err |= addAttributeString(&optionHead, "x-ss-video[0].chromaSamplingType", "1");
        }
        else {
            err |= addAttributeString(&optionHead, "x-ss-video[0].chromaSamplingType", "0");
        }

        // Opt-in Sunshine dynamic HDR negotiation. Legacy clients leave all
        // fields zero, preserving the host's historical HDR10+ behavior.
        if (StreamConfig.dynamicHdrCaps != 0 || StreamConfig.dynamicHdrPreference != 0) {
            snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.dynamicHdrCaps);
            err |= addAttributeString(&optionHead, "x-ss-video[0].dynamicHdrCaps", payloadStr);
            snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.dolbyVisionDirectSurface ? 1 : 0);
            err |= addAttributeString(&optionHead, "x-ss-video[0].dolbyVisionDirectSurface", payloadStr);
            snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.dynamicHdrPreference);
            err |= addAttributeString(&optionHead, "x-ss-video[0].dynamicHdrPreference", payloadStr);
        }
    }

    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.width);
    err |= addAttributeString(&optionHead, "x-nv-video[0].clientViewportWd", payloadStr);
    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.height);
    err |= addAttributeString(&optionHead, "x-nv-video[0].clientViewportHt", payloadStr);

    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.fps);
    err |= addAttributeString(&optionHead, "x-nv-video[0].maxFPS", payloadStr);

    // Adjust the video packet size to account for encryption overhead
    if (EncryptionFeaturesEnabled & SS_ENC_VIDEO) {
        LC_ASSERT(StreamConfig.packetSize % 16 == 0);
        StreamConfig.packetSize -= sizeof(ENC_VIDEO_HEADER);
        LC_ASSERT(StreamConfig.packetSize % 16 == 0);
    }
    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.packetSize);
    err |= addAttributeString(&optionHead, "x-nv-video[0].packetSize", payloadStr);

    // Only sent when the client opted into an FEC preference; -2 keeps the host default.
    if (VideoFecControlSupported && StreamConfig.videoFecPercentage != -2) {
        snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.videoFecPercentage);
        err |= addAttributeString(&optionHead, "x-ml-video.fecPercentage", payloadStr);
        err |= addAttributeString(&optionHead, "x-ml-video.fecSummaryVersion", "1");
    }

    err |= addAttributeString(&optionHead, "x-nv-video[0].rateControlMode", "4");

    err |= addAttributeString(&optionHead, "x-nv-video[0].timeoutLengthMs", "7000");
    err |= addAttributeString(&optionHead, "x-nv-video[0].framesWithInvalidRefThreshold", "0");

    // 20% of the video bitrate will added to the user-specified bitrate for FEC
    adjustedBitrate = (int)(StreamConfig.bitrate * 0.80);

    // Use more strict bitrate logic when streaming remotely. The theory here is that remote
    // streaming is much more bandwidth sensitive. Someone might select 5 Mbps because that's
    // really all they have, so we need to be careful not to exceed the cap, even counting
    // things like audio and control data.
    if (StreamConfig.streamingRemotely == STREAM_CFG_REMOTE) {
        // Subtract 500 Kbps to leave room for audio and control. On remote streams,
        // GFE will use 96Kbps stereo audio. For local streams, it will choose 512Kbps.
        if (adjustedBitrate > 500) {
            adjustedBitrate -= 500;
        }
    }

    // GFE currently imposes a limit of 100 Mbps for the video bitrate. It will automatically
    // impose that on maximumBitrateKbps but not on initialBitrateKbps. We will impose the cap
    // ourselves so initialBitrateKbps does not exceed maximumBitrateKbps.
    adjustedBitrate = adjustedBitrate > 100000 ? 100000 : adjustedBitrate;

    // We don't support dynamic bitrate scaling properly (it tends to bounce between min and max and never
    // settle on the optimal bitrate if it's somewhere in the middle), so we'll just latch the bitrate
    // to the requested value.
    if (AppVersionQuad[0] >= 5) {
        snprintf(payloadStr, sizeof(payloadStr), "%d", adjustedBitrate);

        err |= addAttributeString(&optionHead, "x-nv-video[0].initialBitrateKbps", payloadStr);
        err |= addAttributeString(&optionHead, "x-nv-video[0].initialPeakBitrateKbps", payloadStr);

        err |= addAttributeString(&optionHead, "x-nv-vqos[0].bw.minimumBitrateKbps", payloadStr);
        err |= addAttributeString(&optionHead, "x-nv-vqos[0].bw.maximumBitrateKbps", payloadStr);

        // Send the configured bitrate to Sunshine hosts, so they can adjust for dynamic FEC percentage
        if (IS_SUNSHINE()) {
            snprintf(payloadStr, sizeof(payloadStr), "%u", StreamConfig.bitrate);
            err |= addAttributeString(&optionHead, "x-ml-video.configuredBitrateKbps", payloadStr);
        }
    }
    else {
        if (StreamConfig.streamingRemotely == STREAM_CFG_REMOTE) {
            err |= addAttributeString(&optionHead, "x-nv-video[0].averageBitrate", "4");
            err |= addAttributeString(&optionHead, "x-nv-video[0].peakBitrate", "4");
        }

        snprintf(payloadStr, sizeof(payloadStr), "%d", adjustedBitrate);
        err |= addAttributeString(&optionHead, "x-nv-vqos[0].bw.minimumBitrate", payloadStr);
        err |= addAttributeString(&optionHead, "x-nv-vqos[0].bw.maximumBitrate", payloadStr);
    }

    // FEC must be enabled for proper packet sequencing to be done by RTP FEC queue
    err |= addAttributeString(&optionHead, "x-nv-vqos[0].fec.enable", "1");

    err |= addAttributeString(&optionHead, "x-nv-vqos[0].videoQualityScoreUpdateTime", "5000");

    // If the remote host is local (RFC 1918), enable QoS tagging for our traffic. Windows qWave
    // will disable it if the host is off-link, *however* Windows may get it wrong in cases where
    // the host is directly connected to the Internet without a NAT. In this case, it may send DSCP
    // marked traffic off-link and it could lead to black holes due to misconfigured ISP hardware
    // or CPE. For this reason, we only enable it in cases where it looks like it will work.
    //
    // Even though IPv6 hardware should be much less likely to have this issue, we can't tell
    // if our address is a NAT64 synthesized IPv6 address or true end-to-end IPv6. If it's the
    // former, it may have the same problem as other IPv4 traffic.
    if (StreamConfig.streamingRemotely == STREAM_CFG_LOCAL) {
        err |= addAttributeString(&optionHead, "x-nv-vqos[0].qosTrafficType", "5");
        err |= addAttributeString(&optionHead, "x-nv-aqos.qosTrafficType", "4");
    }
    else {
        err |= addAttributeString(&optionHead, "x-nv-vqos[0].qosTrafficType", "0");
        err |= addAttributeString(&optionHead, "x-nv-aqos.qosTrafficType", "0");
    }

    if (AppVersionQuad[0] == 3) {
        err |= addGen3Options(&optionHead, urlSafeAddr);
    }
    else if (AppVersionQuad[0] == 4) {
        err |= addGen4Options(ctx, &optionHead, urlSafeAddr);
    }
    else {
        err |= addGen5Options(ctx, &optionHead);
    }

    audioChannelCount = CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration);
    audioChannelMask = CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration);

    if (AppVersionQuad[0] >= 4) {
        unsigned char slicesPerFrame;

        // Use slicing for increased performance on some decoders
        slicesPerFrame = (unsigned char)(VideoCallbacks.capabilities >> 24);
        if (slicesPerFrame == 0) {
            // If not using slicing, we request 1 slice per frame
            slicesPerFrame = 1;
        }
        snprintf(payloadStr, sizeof(payloadStr), "%d", slicesPerFrame);
        err |= addAttributeString(&optionHead, "x-nv-video[0].videoEncoderSlicesPerFrame", payloadStr);

        if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_AV1) {
            err |= addAttributeString(&optionHead, "x-nv-vqos[0].bitStreamFormat", "2");
        }
        else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
            err |= addAttributeString(&optionHead, "x-nv-clientSupportHevc", "1");
            err |= addAttributeString(&optionHead, "x-nv-vqos[0].bitStreamFormat", "1");

            if (!APP_VERSION_AT_LEAST(7, 1, 408)) {
                // This disables split frame encode on GFE 3.10 which seems to produce broken
                // HEVC output at 1080p60 (full of artifacts even on the SHIELD itself, go figure).
                // It now appears to work fine on GFE 3.14.1.
                Limelog("Disabling split encode for HEVC on older GFE version");
                err |= addAttributeString(&optionHead, "x-nv-video[0].encoderFeatureSetting", "0");
            }
        }
        else {
            err |= addAttributeString(&optionHead, "x-nv-clientSupportHevc", "0");
            err |= addAttributeString(&optionHead, "x-nv-vqos[0].bitStreamFormat", "0");
        }

        if (AppVersionQuad[0] >= 7) {
            // Advertise the selected dynamic range mode for 10-bit HDR streams.
            if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_10BIT) {
                int dynamicRangeMode = StreamConfig.dynamicRangeMode;
                if (dynamicRangeMode != DYNAMIC_RANGE_MODE_SDR &&
                    dynamicRangeMode != DYNAMIC_RANGE_MODE_HDR10_PQ &&
                    dynamicRangeMode != DYNAMIC_RANGE_MODE_HLG) {
                    dynamicRangeMode = DYNAMIC_RANGE_MODE_HDR10_PQ;
                }
                snprintf(payloadStr, sizeof(payloadStr), "%d", dynamicRangeMode);
                err |= addAttributeString(&optionHead, "x-nv-video[0].dynamicRangeMode", payloadStr);
            }
            else {
                err |= addAttributeString(&optionHead, "x-nv-video[0].dynamicRangeMode", "0");
            }

            // If the decoder supports reference frame invalidation, that indicates it also supports
            // the maximum number of reference frames allowed by the codec. Even if we can't use RFI
            // due to lack of host support, we can still allow the host to pick a number of reference
            // frames greater than 1 to improve encoding efficiency.
            if (isReferenceFrameInvalidationSupportedByDecoder()) {
                err |= addAttributeString(&optionHead, "x-nv-video[0].maxNumReferenceFrames", "0");
            }
            else {
                // Restrict the video stream to 1 reference frame if we're not using
                // reference frame invalidation. This helps to improve compatibility with
                // some decoders that don't like the default of having 16 reference frames.
                err |= addAttributeString(&optionHead, "x-nv-video[0].maxNumReferenceFrames", "1");
            }

            snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.clientRefreshRateX100);
            err |= addAttributeString(&optionHead, "x-nv-video[0].clientRefreshRateX100", payloadStr);
        }

        snprintf(payloadStr, sizeof(payloadStr), "%d", audioChannelCount);
        err |= addAttributeString(&optionHead, "x-nv-audio.surround.numChannels", payloadStr);
        snprintf(payloadStr, sizeof(payloadStr), "%d", audioChannelMask);
        err |= addAttributeString(&optionHead, "x-nv-audio.surround.channelMask", payloadStr);
        if (audioChannelCount > 2) {
            err |= addAttributeString(&optionHead, "x-nv-audio.surround.enable", "1");
        }
        else {
            err |= addAttributeString(&optionHead, "x-nv-audio.surround.enable", "0");
        }

        // Sunshine-only: request a non-Opus surround codec for HDMI/SPDIF
        // passthrough. Older hosts ignore unknown attributes.
        if (NegotiatedAudioCodec != AUDIO_CODEC_OPUS) {
            const char* codecStr = (NegotiatedAudioCodec == AUDIO_CODEC_AC3) ? "ac3" :
                                   (NegotiatedAudioCodec == AUDIO_CODEC_EAC3) ? "eac3" :
                                   (NegotiatedAudioCodec == AUDIO_CODEC_PCM_S16) ? "pcm" :
                                   (NegotiatedAudioCodec == AUDIO_CODEC_MAT_LIVE) ? MAT_LIVE_NAME : "opus";
            err |= addAttributeString(&optionHead, "x-ml-audio.codec", codecStr);
            if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) {
                err |= addAttributeString(&optionHead,"x-ml-audio.mat-live","1");
                err |= addAttributeString(&optionHead,"x-ml-audio.mat-live-carrier",MAT_CARRIER);
                err |= addAttributeString(&optionHead,"x-ml-audio.mat-live-clock",MAT_CLOCK);
                err |= addAttributeString(&optionHead,"x-ml-audio.mat-live-crypto",MAT_LIVE_CRYPTO);
                snprintf(payloadStr,sizeof(payloadStr),"%llu",(unsigned long long)MatLiveProfile.epoch);
                err |= addAttributeString(&optionHead,"x-ml-audio.mat-live-epoch",payloadStr);
                snprintf(payloadStr,sizeof(payloadStr),"%llu",(unsigned long long)MatLiveProfile.domain);
                err |= addAttributeString(&optionHead,"x-ml-audio.mat-live-domain",payloadStr);
            }
            if (NegotiatedAudioBitrate > 0) {
                snprintf(payloadStr, sizeof(payloadStr), "%d", NegotiatedAudioBitrate);
                err |= addAttributeString(&optionHead, "x-ml-audio.bitrate", payloadStr);
            }
        }
    }

    if (AppVersionQuad[0] >= 7) {
        if (StreamConfig.bitrate >= HIGH_AUDIO_BITRATE_THRESHOLD && audioChannelCount > 2 &&
                HighQualitySurroundSupported &&
                !StreamConfig.disableHighQualitySurround &&
                (AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) == 0) {
            // Enable high quality mode for surround sound
            err |= addAttributeString(&optionHead, "x-nv-audio.surround.AudioQuality", "1");

            // Let the audio stream code know that it needs to disable coupled streams when
            // decoding this audio stream.
            HighQualitySurroundEnabled = true;

            // Use 5 ms frames since we don't have a slow decoder
            AudioPacketDuration = 5;
            Limelog("Audio SDP surround request: channels=%d mask=%d bitrate=%d highQualityRequested=1 highQualitySupported=%d packetDuration=%d\n",
                    audioChannelCount, audioChannelMask, StreamConfig.bitrate,
                    HighQualitySurroundSupported ? 1 : 0, AudioPacketDuration);
        }
        else {
            err |= addAttributeString(&optionHead, "x-nv-audio.surround.AudioQuality", "0");
            HighQualitySurroundEnabled = false;

            if ((AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) ||
                     ((AudioCallbacks.capabilities & CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION) != 0 &&
                       StreamConfig.bitrate < LOW_AUDIO_BITRATE_TRESHOLD)) {
                // Use 10 ms packets for slow devices and networks to balance latency and bandwidth usage
                AudioPacketDuration = 10;
            }
            else {
                // Use 5 ms packets by default for lowest latency
                AudioPacketDuration = 5;
            }

            Limelog("Audio SDP surround request: channels=%d mask=%d bitrate=%d highQualityRequested=0 highQualitySupported=%d packetDuration=%d\n",
                    audioChannelCount, audioChannelMask, StreamConfig.bitrate,
                    HighQualitySurroundSupported ? 1 : 0, AudioPacketDuration);
            if (StreamConfig.disableHighQualitySurround && audioChannelCount > 2) {
                Limelog("Audio SDP surround HQ disabled by client preference: channels=%d\n",
                        audioChannelCount);
            }
        }

        // AC3 / E-AC3 use a fixed 1536-sample frame at 48 kHz (32 ms). Override
        // the negotiated packet duration so RTP queue / FEC arithmetic matches
        // what Sunshine is actually emitting. E-AC3 may use 32/16/8 ms frames
        // depending on host config; we currently assume 32 ms ("normal" mode).
        if (NegotiatedAudioCodec == AUDIO_CODEC_AC3 || NegotiatedAudioCodec == AUDIO_CODEC_EAC3) {
            AudioPacketDuration = 32;
        }
        // PCM passthrough: keep frames as small as possible. 5 ms @ 48 kHz =
        // 240 samples, 5.1ch = 2880 B which fits the 4 KB receive buffer.
        else if (AUDIO_CODEC_IS_MAT(NegotiatedAudioCodec)) {
            AudioPacketDuration = 1; // FEC transport tick; carrier PTS is in SMAT.
        }
        else if (NegotiatedAudioCodec == AUDIO_CODEC_PCM_S16) {
            AudioPacketDuration = 5;
        }

        snprintf(payloadStr, sizeof(payloadStr), "%d", AudioPacketDuration);
        err |= addAttributeString(&optionHead, "x-nv-aqos.packetDuration", payloadStr);
    }
    else {
        // 5 ms duration for legacy servers
        AudioPacketDuration = 5;

        // High quality audio mode not supported on legacy servers
        HighQualitySurroundEnabled = false;
    }

    if (AppVersionQuad[0] >= 7) {
        const int encoderCscMode = (StreamConfig.colorSpace << 1) | StreamConfig.colorRange;
        snprintf(payloadStr, sizeof(payloadStr), "%d", encoderCscMode);
        err |= addAttributeString(&optionHead, "x-nv-video[0].encoderCscMode", payloadStr);
    }

    if (err == 0) {
        return optionHead;
    }

    freeAttributeList(optionHead);
    return NULL;
}

// Populate the SDP header with required information
static int fillSdpHeader(PML_CONNECTION_CONTEXT ctx, char* buffer, size_t length, int rtspClientVersion, char*urlSafeAddr) {
    return snprintf(buffer, length,
        "v=0\r\n"
        "o=android 0 %d IN %s %s\r\n"
        "s=NVIDIA Streaming Client\r\n",
        rtspClientVersion,
        RemoteAddr.ss_family == AF_INET ? "IPv4" : "IPv6",
        urlSafeAddr);
}

// Populate the SDP tail with required information
static int fillSdpTail(PML_CONNECTION_CONTEXT ctx, char* buffer, size_t length) {
    LC_ASSERT(VideoPortNumber != 0);
    return snprintf(buffer, length,
        "t=0 0\r\n"
        "m=video %d  \r\n",
        AppVersionQuad[0] < 4 ? 47996 : VideoPortNumber);
}

// Get the SDP attributes for the stream config
char* getSdpPayloadForStreamConfigCtx(PML_CONNECTION_CONTEXT ctx, int rtspClientVersion, int* length) {
    PSDP_OPTION attributeList;
    int attributeListSize;
    int offset, written;
    char* payload;
    char urlSafeAddr[URLSAFESTRING_LEN];

    addrToUrlSafeString(&RemoteAddr, urlSafeAddr, sizeof(urlSafeAddr));

    attributeList = getAttributesList(ctx, urlSafeAddr);
    if (attributeList == NULL) {
        return NULL;
    }

    attributeListSize = getSerializedAttributeListSize(attributeList);
    payload = malloc(MAX_SDP_HEADER_LEN + MAX_SDP_TAIL_LEN + attributeListSize);
    if (payload == NULL) {
        freeAttributeList(attributeList);
        return NULL;
    }

    offset = 0;
    written = fillSdpHeader(ctx, payload, MAX_SDP_HEADER_LEN, rtspClientVersion, urlSafeAddr);
    if (written < 0 || written >= MAX_SDP_HEADER_LEN) {
        LC_ASSERT(false);
        free(payload);
        freeAttributeList(attributeList);
        return NULL;
    }
    else {
        offset += written;
    }
    written = fillSerializedAttributeList(&payload[offset], attributeListSize, attributeList);
    if (written < 0 || written >= attributeListSize) {
        LC_ASSERT(false);
        free(payload);
        freeAttributeList(attributeList);
        return NULL;
    }
    else {
        offset += written;
    }
    written = fillSdpTail(ctx, &payload[offset], MAX_SDP_TAIL_LEN);
    if (written < 0 || written >= MAX_SDP_TAIL_LEN) {
        LC_ASSERT(false);
        free(payload);
        freeAttributeList(attributeList);
        return NULL;
    }
    else {
        offset += written;
    }

    freeAttributeList(attributeList);
    *length = offset;
    return payload;
}

char* getSdpPayloadForStreamConfig(int rtspClientVersion, int* length) {
    PML_CONNECTION_CONTEXT ctx = LiGetEffectiveConnectionContext();
    return getSdpPayloadForStreamConfigCtx(ctx, rtspClientVersion, length);
}
