#include "Limelight-internal.h"
#include "ControlStreamPacket.h"
#include "Ds5HapticsStream.h"
#include "Ds5HapticsStateStream.h"
#include "Ds5HapticsIrStream.h"
#include "HostSessionStateStream.h"
#include "RemoteTextContextStream.h"
#include "Ds5ControlExtension.h"

#include <math.h>

#ifdef StreamConfig
#undef StreamConfig
#endif
#ifdef ListenerCallbacks
#undef ListenerCallbacks
#endif
#ifdef EncryptionFeaturesEnabled
#undef EncryptionFeaturesEnabled
#endif
#ifdef ControlPortNumber
#undef ControlPortNumber
#endif
#ifdef LocalAddr
#undef LocalAddr
#endif
#ifdef AddrLen
#undef AddrLen
#endif
#ifdef RemoteAddr
#undef RemoteAddr
#endif
#ifdef ControlConnectData
#undef ControlConnectData
#endif
#ifdef ConnectionInterrupted
#undef ConnectionInterrupted
#endif
#ifdef SunshineFeatureFlags
#undef SunshineFeatureFlags
#endif

// This is a private header, but it just contains some time macros
#include <enet/time.h>
#include <inttypes.h>

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

// Macros to redirect global access to context
#define ControlStreamAppVersionQuad (ctx->connectionContext->AppVersionQuad)
#define StreamConfig (ctx->connectionContext->StreamConfig)
#define ListenerCallbacks (ctx->connectionContext->ListenerCallbacks)
#define EncryptionFeaturesEnabled (ctx->connectionContext->EncryptionFeaturesEnabled)
#define ControlPortNumber (ctx->connectionContext->ControlPortNumber)
#define LocalAddr (ctx->connectionContext->LocalAddr)
#define AddrLen (ctx->connectionContext->AddrLen)
#define RemoteAddr (ctx->connectionContext->RemoteAddr)
#define ControlConnectData (ctx->connectionContext->ControlConnectData)
#define ConnectionInterrupted (ctx->connectionContext->ConnectionInterrupted)
#define SunshineFeatureFlags (ctx->connectionContext->SunshineFeatureFlags)

typedef struct _QUEUED_REFERENCE_FRAME_CONTROL {
    uint32_t startFrame;
    uint32_t endFrame;
    bool invalidate; // true: RFI(startFrame, endFrame); false: LTR_ACK(startFrame)
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_REFERENCE_FRAME_CONTROL, *PQUEUED_REFERENCE_FRAME_CONTROL;

// Sunshine unified dynamic parameter change payload types (see Sunshine src/video.h)
#define SS_DYNAMIC_PARAM_TYPE_RESOLUTION 0
#define SS_DYNAMIC_PARAM_TYPE_FPS 1
#define SS_DYNAMIC_PARAM_TYPE_BITRATE 2
#define SS_DYNAMIC_PARAM_TYPE_CLIENT_SDR_WHITE_NITS 9

typedef struct _QUEUED_FRAME_FEC_STATUS {
    SS_FRAME_FEC_STATUS fecStatus;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_FRAME_FEC_STATUS, *PQUEUED_FRAME_FEC_STATUS;

typedef struct _QUEUED_ASYNC_CALLBACK {
    int typeIndex;
    union {
        struct {
            uint16_t controllerNumber;
            uint16_t lowFreqRumble;
            uint16_t highFreqRumble;
        } rumble;
        struct {
            uint16_t controllerNumber;
            uint16_t leftTriggerMotor;
            uint16_t rightTriggerMotor;
        } rumbleTriggers;
        struct {
            uint16_t controllerNumber;
            uint16_t reportRateHz;
            uint8_t motionType;
        } setMotionEventState;
        struct {
            uint16_t controllerNumber;
            uint8_t r;
            uint8_t g;
            uint8_t b;
        } setControllerLed;
        struct {
            uint16_t controllerNumber;
            /**
             * 0x04 - Right trigger
             * 0x08 - Left trigger
             */
            uint8_t eventFlags;
            uint8_t typeLeft;
            uint8_t typeRight;
            // arrays of size DS_EFFECT_PAYLOAD_SIZE
            // this is an opaque payload that will be read directly from the joypad and set as is to the client controller
            // if you are curious about the actual data, there's some rationale in
            // https://gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db
            uint8_t left[DS_EFFECT_PAYLOAD_SIZE];
            uint8_t right[DS_EFFECT_PAYLOAD_SIZE];
        } dsAdaptiveTrigger;
    } data;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_ASYNC_CALLBACK, *PQUEUED_ASYNC_CALLBACK;

#if defined(_MSC_VER)
static __declspec(thread) PML_CONTROL_STREAM_CONTEXT tls_CurrentControlStreamCtx = NULL;
#else
static __thread PML_CONTROL_STREAM_CONTEXT tls_CurrentControlStreamCtx = NULL;
#endif

static atomic_uint_fast64_t sClipboardItemCounter = 1;

static void dispatchDs5HapticsPcm(const LI_DS5_HAPTICS_PCM_FRAME* frame,
                                  void* context) {
    ConnListenerDs5HapticsPcm callback = (ConnListenerDs5HapticsPcm)context;
    if (callback != NULL) {
        callback(frame);
    }
}

static void dispatchDs5HapticsIrV2(const LI_DS5_HAPTICS_IR_FRAME_V2* frame,
                                   void* context) {
    ConnListenerDs5HapticsIrV2 callback = (ConnListenerDs5HapticsIrV2)context;
    if (callback != NULL) {
        callback(frame);
    }
}

static void dispatchDs5HapticsIrExtended(const LI_DS5_HAPTICS_IR_EXTENDED_FRAME* frame,
                                         void* context) {
    ConnListenerDs5HapticsIrExtended callback = (ConnListenerDs5HapticsIrExtended)context;
    if (callback != NULL) {
        callback(frame);
    }
}

static void processRemoteTextContext(PML_CONTROL_STREAM_CONTEXT ctx,
                                     const uint8_t* payload, int payloadLength) {
    LI_REMOTE_TEXT_CONTEXT context;
    ConnListenerRemoteTextContext callback = ctx->connectionContext->RemoteTextContextCallback;

    if (callback == NULL ||
            !decodeRemoteTextContextPacket(payload, (size_t)payloadLength, &context)) {
        return;
    }
    callback(&context);
}

static bool sendMessageAndForget(PML_CONTROL_STREAM_CONTEXT ctx, short ptype,
                                 int paylen, const void* payload,
                                 uint8_t channelId, uint32_t flags,
                                 bool moreData);

// 0x5515: the whole body is validated before any field reaches the client; a
// malformed body is dropped without a partial application.
static void processDs5OutputExtension(PML_CONTROL_STREAM_CONTEXT ctx,
                                      const uint8_t* payload, int payloadLength) {
    DS5EXT_OUTPUT decoded;
    LI_DS5_OUTPUT_EXTENSION output;
    ConnListenerDs5OutputExtension callback = ctx->connectionContext->Ds5OutputExtensionCallback;

    if (callback == NULL || payloadLength <= 0 ||
            !ds5ExtDecodeOutput(payload, (size_t)payloadLength, &decoded)) {
        return;
    }
    output.controllerNumber = decoded.controllerNumber;
    output.validMask = decoded.validMask;
    output.playerLedMask = decoded.playerLedMask;
    output.muteLed = decoded.muteLed;
    output.speakerVolume = decoded.speakerVolume;
    output.headphoneVolume = decoded.headphoneVolume;
    output.micVolume = decoded.micVolume;
    output.micMute = decoded.micMute;
    output.audioRouting = decoded.audioRouting;
    output.lightbarBrightness = decoded.lightbarBrightness;
    callback(&output);
}

// In control-only mode, endpoint refresh is the only media-side signal that
// can distinguish a transport timeout from a generic control failure.
static int controlTransportError(PML_CONTROL_STREAM_CONTEXT ctx, int fallback) {
    if (StreamConfig.controlOnly && LiIsEndpointAutoRefreshEnabled()) {
        return ML_ERROR_CONTROL_TRANSPORT_TIMEOUT;
    }
    return fallback;
}

#define CONN_IMMEDIATE_POOR_LOSS_RATE 30
#define CONN_CONSECUTIVE_POOR_LOSS_RATE 15
#define CONN_OKAY_LOSS_RATE 5
#define CONN_STATUS_SAMPLE_PERIOD 3000

//
// IMPORTANT: These indices MUST match the ordering of packetTypes*[] tables.
// Newer encrypted control streams (Gen7Enc) include additional packet types
// (periodic ping, fully encrypted marker, mic, dynamic params, etc).
//
#define IDX_START_A 0
#define IDX_START_B 1
#define IDX_INVALIDATE_REF_FRAMES 2
#define IDX_LOSS_STATS 3
#define IDX_FRAME_STATS 4
#define IDX_INPUT_DATA 5
#define IDX_RUMBLE_DATA 6
#define IDX_TERMINATION 7
#define IDX_PERIODIC_PING 8
#define IDX_REQUEST_IDR_FRAME 9
#define IDX_FULLY_ENCRYPTED 10
#define IDX_HDR_INFO 11
#define IDX_RUMBLE_TRIGGER_DATA 12
#define IDX_SET_MOTION_EVENT 13
#define IDX_SET_RGB_LED 14
#define IDX_SET_ADAPTIVE_TRIGGERS 15
#define IDX_DS_ADAPTIVE_TRIGGERS IDX_SET_ADAPTIVE_TRIGGERS  // Alias for compatibility
#define IDX_MIC_DATA 16
#define IDX_MIC_CONFIG 17
#define IDX_DYNAMIC_PARAM_CHANGE 18
#define IDX_RESOLUTION_CHANGE 19
#define IDX_CLIPBOARD 20
#define IDX_CURSOR 21
#define IDX_DS5_HAPTICS_PCM 22
#define IDX_DS5_HAPTICS_IR_V2 23
#define IDX_REMOTE_TEXT_CONTEXT 24
#define IDX_DS5_OUTPUT_EXTENSION 25
#define IDX_DS5_INPUT_STATUS 26


#define CONTROL_STREAM_TIMEOUT_SEC 10
#define CONTROL_STREAM_LINGER_TIMEOUT_SEC 2
#define CONTROL_STREAM_GENERIC_ERROR (-1)

static const short packetTypesGen3[] = {
    0x1407, // Start A (Gen3 uses IDR request here)
    0x1410, // Start B
    0x1404, // Invalidate reference frames
    0x140c, // Loss Stats
    0x1417, // Frame Stats (unused)
    -1,     // Input data (unused)
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
    -1,     // Periodic Ping (unused)
    0x1407, // Request IDR frame
    -1,     // fully encrypted (unused)
    -1,     // HDR mode (unused)
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
    -1,     // Set Adaptive Triggers (unused)
    -1,     // Microphone data (unused)
    -1,     // Microphone config (unused)
    -1,     // Dynamic parameter change (unused)
    -1,     // Resolution change (unused)
    -1,     // Clipboard (unused)
    -1,     // Cursor (unused)
    -1,     // DualSense haptics PCM (unused)
    -1,     // DualSense haptics IR v2 (unused)
    -1,     // Remote text context (unused)
    -1,     // DualSense output extension (unused)
    -1,     // DualSense input status (unused)
};
static const short packetTypesGen4[] = {
    0x0606, // Start A (Gen4 uses IDR request here)
    0x0609, // Start B
    0x0604, // Invalidate reference frames
    0x060a, // Loss Stats
    0x0611, // Frame Stats (unused)
    -1,     // Input data (unused)
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
    -1,     // Periodic Ping (unused)
    0x0606, // Request IDR frame
    -1,     // fully encrypted (unused)
    -1,     // HDR mode (unused)
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
    -1,     // Set Adaptive Triggers (unused)
    -1,     // Microphone data (unused)
    -1,     // Microphone config (unused)
    -1,     // Dynamic parameter change (unused)
    -1,     // Resolution change (unused)
    -1,     // Clipboard (unused)
    -1,     // Cursor (unused)
    -1,     // DualSense haptics PCM (unused)
    -1,     // DualSense haptics IR v2 (unused)
    -1,     // Remote text context (unused)
    -1,     // DualSense output extension (unused)
    -1,     // DualSense input status (unused)
};
static const short packetTypesGen5[] = {
    0x0305, // Start A
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0207, // Input data
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
    -1,     // Periodic Ping (unused)
    -1,     // Request IDR frame (unused - uses RFI fallback)
    -1,     // fully encrypted (unused)
    -1,     // HDR mode (unknown)
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
    -1,     // Set Adaptive Triggers (unused)
    -1,     // Microphone data (unused)
    -1,     // Microphone config (unused)
    -1,     // Dynamic parameter change (unused)
    -1,     // Resolution change (unused)
    -1,     // Clipboard (unused)
    -1,     // Cursor (unused)
    -1,     // DualSense haptics PCM (unused)
    -1,     // DualSense haptics IR v2 (unused)
    -1,     // Remote text context (unused)
    -1,     // DualSense output extension (unused)
    -1,     // DualSense input status (unused)
};
static const short packetTypesGen7[] = {
    0x0305, // Start A
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0206, // Input data
    0x010b, // Rumble data
    0x0100, // Termination
    -1,     // Periodic Ping (unused - hardcoded 0x0200)
    -1,     // Request IDR frame (unused - uses RFI fallback)
    -1,     // fully encrypted (unused)
    0x010e, // HDR mode
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
    -1,     // Set Adaptive Triggers (unused)
    -1,     // Microphone data (unused)
    -1,     // Microphone config (unused)
    -1,     // Dynamic parameter change (unused)
    -1,     // Resolution change (unused)
    -1,     // Clipboard (unused)
    -1,     // Cursor (unused)
    -1,     // DualSense haptics PCM (unused)
    -1,     // DualSense haptics IR v2 (unused)
    -1,     // Remote text context (unused)
    -1,     // DualSense output extension (unused)
    -1,     // DualSense input status (unused)
};
static const short packetTypesGen7Enc[] = {
    0x0305, // Start A (index 0)
    0x0307, // Start B (index 1)
    0x0301, // Invalidate reference frames (index 2)
    0x0201, // Loss Stats (index 3)
    0x0204, // Frame Stats (unused) (index 4)
    0x0206, // Input data (index 5)
    0x010b, // Rumble data (index 6)
    0x0109, // Termination (extended) (index 7)
    0x0200, // Periodic Ping (index 8)
    0x0302, // IDR frame (index 9)
    0x0001, // fully encrypted (index 10)
    0x010e, // HDR mode (index 11)
    0x5500, // Rumble triggers (Sunshine protocol extension) (index 12)
    0x5501, // Set motion event (Sunshine protocol extension) (index 13)
    0x5502, // Set RGB LED (Sunshine protocol extension) (index 14)
    0x5503, // Set Adaptive Triggers (Sunshine protocol extension) (index 15)
    0x5504, // Microphone data (Sunshine protocol extension) (index 16)
    0x5505, // Microphone config (Sunshine protocol extension) (index 17)
    0x5506, // Dynamic parameter change (Sunshine protocol extension) (index 18)
    0x5507, // Resolution change (Sunshine protocol extension) (index 19)
    0x5508, // Clipboard sync (Sunshine protocol extension) (index 20) - opaque payload forwarded to user-session GUI agent
    0x5509, // Local cursor mode/update (Sunshine protocol extension) (index 21)
    0x550A, // Authored DualSense haptics PCM (Sunshine protocol extension) (index 22)
    0x550B, // Device-independent DualSense haptics IR v2 (Sunshine protocol extension) (index 23)
    0x550C, // Remote text context (Sunshine protocol extension) (index 24)
    0x5515, // DualSense output extension, host to client (index 25)
    0x5516, // DualSense input status, client to host (index 26)
};

static const char requestIdrFrameGen3[] = { 0, 0 };
static const int startBGen3[] = { 0, 0, 0, 0xa };

static const char requestIdrFrameGen4[] = { 0, 0 };
static const char startBGen4[] = { 0 };

static const char startAGen5[] = { 0, 0 };
static const char startBGen5[] = { 0 };

static const char requestIdrFrameGen7Enc[] = { 0, 0 };

static const short payloadLengthsGen3[] = {
    sizeof(requestIdrFrameGen3), // Start A (Gen3 uses IDR request here)
    sizeof(startBGen3),          // Start B
    24,                          // Invalidate reference frames
    32,                          // Loss Stats
    64,                          // Frame Stats
    -1,                          // Input data
    -1,                          // Rumble data
    -1,                          // Termination
    -1,                          // Periodic Ping
    sizeof(requestIdrFrameGen3), // Request IDR frame
    -1,                          // fully encrypted
    -1,                          // HDR mode
    -1,                          // Rumble triggers
    -1,                          // Set motion event
    -1,                          // Set RGB LED
    -1,                          // Set Adaptive Triggers
    -1,                          // Microphone data
    -1,                          // Microphone config
    -1,                          // Dynamic parameter change
    -1,                          // Resolution change
    -1,                          // Clipboard (variable-length)
    -1,                          // Cursor (variable-length)
    -1,                          // DualSense haptics PCM (variable-length)
    -1,                          // DualSense haptics IR v2 (fixed payload, validated by parser)
    -1,                          // Remote text context (fixed payload, validated by parser)
    -1,                          // DualSense output extension (fixed payload, validated by parser)
    -1,                          // DualSense input status (fixed payload, validated by parser)
};
static const short payloadLengthsGen4[] = {
    sizeof(requestIdrFrameGen4), // Start A (Gen4 uses IDR request here)
    sizeof(startBGen4),          // Start B
    24,                          // Invalidate reference frames
    32,                          // Loss Stats
    64,                          // Frame Stats
    -1,                          // Input data
    -1,                          // Rumble data
    -1,                          // Termination
    -1,                          // Periodic Ping
    sizeof(requestIdrFrameGen4), // Request IDR frame
    -1,                          // fully encrypted
    -1,                          // HDR mode
    -1,                          // Rumble triggers
    -1,                          // Set motion event
    -1,                          // Set RGB LED
    -1,                          // Set Adaptive Triggers
    -1,                          // Microphone data
    -1,                          // Microphone config
    -1,                          // Dynamic parameter change
    -1,                          // Resolution change
    -1,                          // Clipboard (variable-length)
    -1,                          // Cursor (variable-length)
    -1,                          // DualSense haptics PCM (variable-length)
    -1,                          // DualSense haptics IR v2 (fixed payload, validated by parser)
    -1,                          // Remote text context (fixed payload, validated by parser)
    -1,                          // DualSense output extension (fixed payload, validated by parser)
    -1,                          // DualSense input status (fixed payload, validated by parser)
};
static const short payloadLengthsGen5[] = {
    sizeof(startAGen5), // Start A
    sizeof(startBGen5), // Start B
    24,                 // Invalidate reference frames
    32,                 // Loss Stats
    80,                 // Frame Stats
    -1,                 // Input data
    -1,                 // Rumble data
    -1,                 // Termination
    -1,                 // Periodic Ping
    -1,                 // Request IDR frame (unused)
    -1,                 // fully encrypted
    -1,                 // HDR mode
    -1,                 // Rumble triggers
    -1,                 // Set motion event
    -1,                 // Set RGB LED
    -1,                 // Set Adaptive Triggers
    -1,                 // Microphone data
    -1,                 // Microphone config
    -1,                 // Dynamic parameter change
    -1,                 // Resolution change
    -1,                 // Clipboard (variable-length)
    -1,                 // Cursor (variable-length)
    -1,                 // DualSense haptics PCM (variable-length)
    -1,                 // DualSense haptics IR v2 (fixed payload, validated by parser)
    -1,                 // Remote text context (fixed payload, validated by parser)
    -1,                 // DualSense output extension (fixed payload, validated by parser)
    -1,                 // DualSense input status (fixed payload, validated by parser)
};
static const short payloadLengthsGen7[] = {
    sizeof(startAGen5), // Start A
    sizeof(startBGen5), // Start B
    24,                 // Invalidate reference frames
    32,                 // Loss Stats
    80,                 // Frame Stats
    -1,                 // Input data
    -1,                 // Rumble data
    -1,                 // Termination
    -1,                 // Periodic Ping
    -1,                 // Request IDR frame (unused)
    -1,                 // fully encrypted
    -1,                 // HDR mode
    -1,                 // Rumble triggers
    -1,                 // Set motion event
    -1,                 // Set RGB LED
    -1,                 // Set Adaptive Triggers
    -1,                 // Microphone data
    -1,                 // Microphone config
    -1,                 // Dynamic parameter change
    -1,                 // Resolution change
    -1,                 // Clipboard (variable-length)
    -1,                 // Cursor (variable-length)
    -1,                 // DualSense haptics PCM (variable-length)
    -1,                 // DualSense haptics IR v2 (fixed payload, validated by parser)
    -1,                 // Remote text context (fixed payload, validated by parser)
    -1,                 // DualSense output extension (fixed payload, validated by parser)
    -1,                 // DualSense input status (fixed payload, validated by parser)
};
static const short payloadLengthsGen7Enc[] = {
    sizeof(startAGen5),             // Start A
    sizeof(startBGen5),             // Start B
    24,                             // Invalidate reference frames
    32,                             // Loss Stats
    80,                             // Frame Stats
    -1,                             // Input data
    -1,                             // Rumble data
    -1,                             // Termination
    -1,                             // Periodic Ping
    sizeof(requestIdrFrameGen7Enc), // Request IDR frame
    -1,                             // fully encrypted
    -1,                             // HDR mode
    -1,                             // Rumble triggers
    -1,                             // Set motion event
    -1,                             // Set RGB LED
    -1,                             // Set Adaptive Triggers
    -1,                             // Microphone data
    -1,                             // Microphone config
    -1,                             // Dynamic parameter change
    -1,                             // Resolution change
    -1,                             // Clipboard (variable-length)
    -1,                             // Cursor (variable-length)
    -1,                             // DualSense haptics PCM (variable-length)
    -1,                             // DualSense haptics IR v2 (fixed payload, validated by parser)
    -1,                             // Remote text context (fixed payload, validated by parser)
    -1,                             // DualSense output extension (fixed payload, validated by parser)
    -1,                             // DualSense input status (fixed payload, validated by parser)
};

static const char* preconstructedPayloadsGen3[] = {
    requestIdrFrameGen3, // IDX_START_A
    (char*)startBGen3,   // IDX_START_B
    NULL,                // IDX_INVALIDATE_REF_FRAMES
    NULL,                // IDX_LOSS_STATS
    NULL,                // IDX_FRAME_STATS
    NULL,                // IDX_INPUT_DATA
    NULL,                // IDX_RUMBLE_DATA
    NULL,                // IDX_TERMINATION
    NULL,                // IDX_PERIODIC_PING
    requestIdrFrameGen3, // IDX_REQUEST_IDR_FRAME
    NULL,                // IDX_FULLY_ENCRYPTED
    NULL,                // IDX_HDR_INFO
    NULL,                // IDX_RUMBLE_TRIGGER_DATA
    NULL,                // IDX_SET_MOTION_EVENT
    NULL,                // IDX_SET_RGB_LED
    NULL,                // IDX_SET_ADAPTIVE_TRIGGERS
    NULL,                // IDX_MIC_DATA
    NULL,                // IDX_MIC_CONFIG
    NULL,                // IDX_DYNAMIC_PARAM_CHANGE
    NULL,                // IDX_RESOLUTION_CHANGE
    NULL,                // IDX_CLIPBOARD
    NULL,                // IDX_CURSOR
    NULL,                // IDX_DS5_HAPTICS_PCM
    NULL,                // IDX_DS5_HAPTICS_IR_V2
    NULL,                // IDX_REMOTE_TEXT_CONTEXT
    NULL,                // IDX_DS5_OUTPUT_EXTENSION
    NULL,                // IDX_DS5_INPUT_STATUS
};
static const char* preconstructedPayloadsGen4[] = {
    requestIdrFrameGen4, // IDX_START_A
    startBGen4,          // IDX_START_B
    NULL,                // IDX_INVALIDATE_REF_FRAMES
    NULL,                // IDX_LOSS_STATS
    NULL,                // IDX_FRAME_STATS
    NULL,                // IDX_INPUT_DATA
    NULL,                // IDX_RUMBLE_DATA
    NULL,                // IDX_TERMINATION
    NULL,                // IDX_PERIODIC_PING
    requestIdrFrameGen4, // IDX_REQUEST_IDR_FRAME
    NULL,                // IDX_FULLY_ENCRYPTED
    NULL,                // IDX_HDR_INFO
    NULL,                // IDX_RUMBLE_TRIGGER_DATA
    NULL,                // IDX_SET_MOTION_EVENT
    NULL,                // IDX_SET_RGB_LED
    NULL,                // IDX_SET_ADAPTIVE_TRIGGERS
    NULL,                // IDX_MIC_DATA
    NULL,                // IDX_MIC_CONFIG
    NULL,                // IDX_DYNAMIC_PARAM_CHANGE
    NULL,                // IDX_RESOLUTION_CHANGE
    NULL,                // IDX_CLIPBOARD
    NULL,                // IDX_CURSOR
    NULL,                // IDX_DS5_HAPTICS_PCM
    NULL,                // IDX_DS5_HAPTICS_IR_V2
    NULL,                // IDX_REMOTE_TEXT_CONTEXT
    NULL,                // IDX_DS5_OUTPUT_EXTENSION
    NULL,                // IDX_DS5_INPUT_STATUS
};
static const char* preconstructedPayloadsGen5[] = {
    startAGen5, // IDX_START_A
    startBGen5, // IDX_START_B
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, // IDX_CLIPBOARD
    NULL, // IDX_CURSOR
    NULL, // IDX_DS5_HAPTICS_PCM
    NULL, // IDX_DS5_HAPTICS_IR_V2
    NULL, // IDX_REMOTE_TEXT_CONTEXT
    NULL, // IDX_DS5_OUTPUT_EXTENSION
    NULL, // IDX_DS5_INPUT_STATUS
};
static const char* preconstructedPayloadsGen7[] = {
    startAGen5, // IDX_START_A
    startBGen5, // IDX_START_B
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, // IDX_CLIPBOARD
    NULL, // IDX_CURSOR
    NULL, // IDX_DS5_HAPTICS_PCM
    NULL, // IDX_DS5_HAPTICS_IR_V2
    NULL, // IDX_REMOTE_TEXT_CONTEXT
    NULL, // IDX_DS5_OUTPUT_EXTENSION
    NULL, // IDX_DS5_INPUT_STATUS
};
static const char* preconstructedPayloadsGen7Enc[] = {
    startAGen5,             // IDX_START_A
    startBGen5,             // IDX_START_B
    NULL,                   // IDX_INVALIDATE_REF_FRAMES
    NULL,                   // IDX_LOSS_STATS
    NULL,                   // IDX_FRAME_STATS
    NULL,                   // IDX_INPUT_DATA
    NULL,                   // IDX_RUMBLE_DATA
    NULL,                   // IDX_TERMINATION
    NULL,                   // IDX_PERIODIC_PING
    requestIdrFrameGen7Enc, // IDX_REQUEST_IDR_FRAME
    NULL,                   // IDX_FULLY_ENCRYPTED
    NULL,                   // IDX_HDR_INFO
    NULL,                   // IDX_RUMBLE_TRIGGER_DATA
    NULL,                   // IDX_SET_MOTION_EVENT
    NULL,                   // IDX_SET_RGB_LED
    NULL,                   // IDX_SET_ADAPTIVE_TRIGGERS
    NULL,                   // IDX_MIC_DATA
    NULL,                   // IDX_MIC_CONFIG
    NULL,                   // IDX_DYNAMIC_PARAM_CHANGE
    NULL,                   // IDX_RESOLUTION_CHANGE
    NULL,                   // IDX_CLIPBOARD
    NULL,                   // IDX_CURSOR
    NULL,                   // IDX_DS5_HAPTICS_PCM
    NULL,                   // IDX_DS5_HAPTICS_IR_V2
    NULL,                   // IDX_REMOTE_TEXT_CONTEXT
    NULL,                   // IDX_DS5_OUTPUT_EXTENSION
    NULL,                   // IDX_DS5_INPUT_STATUS
};


#define LOSS_REPORT_INTERVAL_MS 50
#define PERIODIC_PING_INTERVAL_MS 100

// Initializes the control stream
int initializeControlStreamCtx(PML_CONTROL_STREAM_CONTEXT ctx, PML_CONNECTION_CONTEXT connectionContext) {
    ctx->connectionContext = connectionContext;
    ctx->stopping = false;
    ctx->ctlSock = INVALID_SOCKET;
    ctx->client = NULL;
    ctx->peer = NULL;
    PltCreateEvent(&ctx->idrFrameRequiredEvent);
    LbqInitializeLinkedBlockingQueue(&ctx->invalidReferenceFrameTuples, 20);
    LbqInitializeLinkedBlockingQueue(&ctx->frameFecStatusQueue, 8); // Limits number of frame status reports per periodic ping interval
    LbqInitializeLinkedBlockingQueue(&ctx->asyncCallbackQueue, 30);
    PltCreateMutex(&ctx->enetMutex);
    PltCreateMutex(&ctx->fecSummaryMutex);
    ctx->fecDataPackets = ctx->fecMissingPackets = ctx->fecSequence = 0;
    ctx->fecHasBlock = false;
    ctx->fecLastSummaryMs = PltGetMillis();
    atomic_store_explicit(&ctx->fecLastSentSequence, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx->fecLastSentDataPackets, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx->fecLastSentMissingPackets, 0, memory_order_relaxed);
    ctx->currentResolutionWidth = 0;
    ctx->currentResolutionHeight = 0;

    ctx->encryptedControlStream = APP_VERSION_AT_LEAST_CTX(ctx->connectionContext, 7, 1, 431);
    memset(&ctx->incomingClipboardTransfer, 0, sizeof(ctx->incomingClipboardTransfer));

    if (ControlStreamAppVersionQuad[0] == 3) {
        ctx->packetTypes = (short*)packetTypesGen3;
        ctx->payloadLengths = (short*)payloadLengthsGen3;
        ctx->preconstructedPayloads = (char**)preconstructedPayloadsGen3;
        ctx->supportsIdrFrameRequest = true;
    }
    else if (ControlStreamAppVersionQuad[0] == 4) {
        ctx->packetTypes = (short*)packetTypesGen4;
        ctx->payloadLengths = (short*)payloadLengthsGen4;
        ctx->preconstructedPayloads = (char**)preconstructedPayloadsGen4;
        ctx->supportsIdrFrameRequest = true;
    }
    else if (ControlStreamAppVersionQuad[0] == 5) {
        ctx->packetTypes = (short*)packetTypesGen5;
        ctx->payloadLengths = (short*)payloadLengthsGen5;
        ctx->preconstructedPayloads = (char**)preconstructedPayloadsGen5;
        ctx->supportsIdrFrameRequest = false;
    }
    else {
        if (ctx->encryptedControlStream) {
            ctx->packetTypes = (short*)packetTypesGen7Enc;
            ctx->payloadLengths = (short*)payloadLengthsGen7Enc;
            ctx->preconstructedPayloads = (char**)preconstructedPayloadsGen7Enc;
            ctx->supportsIdrFrameRequest = true;
        }
        else {
            ctx->packetTypes = (short*)packetTypesGen7;
            ctx->payloadLengths = (short*)payloadLengthsGen7;
            ctx->preconstructedPayloads = (char**)preconstructedPayloadsGen7;
            ctx->supportsIdrFrameRequest = false;
        }
    }

    ctx->lastGoodFrame = 0;
    ctx->lastSeenFrame = 0;
    ctx->disconnectPending = false;
    ctx->intervalGoodFrameCount = 0;
    ctx->intervalTotalFrameCount = 0;
    ctx->intervalStartTimeMs = 0;
    ctx->lastIntervalLossPercentage = 0;
    atomic_store_explicit(&ctx->lastIntervalLossBasisPoints, 0, memory_order_relaxed);
    ctx->lastConnectionStatusUpdate = CONN_STATUS_OKAY;
    ctx->firstFrameTimeMs = 0;
    ctx->currentEnetSequenceNumber = 0;
    ctx->usePeriodicPing = APP_VERSION_AT_LEAST_CTX(ctx->connectionContext, 7, 1, 415);
    ctx->encryptionCtx = PltCreateCryptoContext();
    ctx->decryptionCtx = PltCreateCryptoContext();
    ctx->hdrEnabled = false;
    memset(&ctx->hdrMetadata, 0, sizeof(ctx->hdrMetadata));

    return 0;
}

static inline PML_CONTROL_STREAM_CONTEXT LiGetEffectiveControlContext(void) {
    return &LiGetEffectiveConnectionContext()->controlContext;
}

int initializeControlStream(void) {
    PML_CONNECTION_CONTEXT ctx = LiGetEffectiveConnectionContext();
    return initializeControlStreamCtx(&ctx->controlContext, ctx);
}

static void freeBasicLbqList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    PLINKED_BLOCKING_QUEUE_ENTRY nextEntry;

    while (entry != NULL) {
        nextEntry = entry->flink;
        free(entry->data);
        entry = nextEntry;
    }
}

static void freeIncomingClipboardTransfer(PML_CONTROL_STREAM_CONTEXT ctx) {
    free(ctx->incomingClipboardTransfer.mimeType);
    free(ctx->incomingClipboardTransfer.name);
    free(ctx->incomingClipboardTransfer.data);
    memset(&ctx->incomingClipboardTransfer, 0, sizeof(ctx->incomingClipboardTransfer));
}

static uint32_t clipboardFeatureFlagForItemType(uint8_t itemType) {
    switch (itemType) {
    case LI_CLIPBOARD_ITEM_TYPE_TEXT:
        return LI_FF_CLIPBOARD_TEXT;
    case LI_CLIPBOARD_ITEM_TYPE_IMAGE:
        return LI_FF_CLIPBOARD_IMAGE;
    default:
        return 0;
    }
}

static bool isClipboardControlContextReady(PML_CONTROL_STREAM_CONTEXT ctx) {
    return ctx != NULL &&
           ctx->connectionContext != NULL &&
           ctx->packetTypes != NULL &&
           !ctx->stopping;
}

static bool isClipboardExtensionSupported(PML_CONTROL_STREAM_CONTEXT ctx) {
    return isClipboardControlContextReady(ctx) &&
           IS_SUNSHINE_CTX(ctx->connectionContext) &&
           ctx->packetTypes[IDX_CLIPBOARD] != -1;
}

static bool isAnyClipboardFeatureSupportedByHost(PML_CONTROL_STREAM_CONTEXT ctx) {
    if (!isClipboardControlContextReady(ctx)) {
        return false;
    }

    return (SunshineFeatureFlags & (LI_FF_CLIPBOARD_TEXT |
                                    LI_FF_CLIPBOARD_IMAGE)) != 0;
}

static bool isClipboardItemTypeSupportedByHost(PML_CONTROL_STREAM_CONTEXT ctx,
                                               uint8_t itemType) {
    uint32_t featureFlag = clipboardFeatureFlagForItemType(itemType);
    if (!isClipboardControlContextReady(ctx)) {
        return false;
    }

    return featureFlag != 0 &&
           (SunshineFeatureFlags & featureFlag) != 0;
}

static uint64_t generateClipboardItemId(void) {
    return (uint64_t)atomic_fetch_add_explicit(&sClipboardItemCounter, 1,
                                               memory_order_relaxed);
}

static bool isClipboardTransferLengthValid(uint8_t itemType,
                                           uint32_t totalLength) {
    switch (itemType) {
    case LI_CLIPBOARD_ITEM_TYPE_NONE:
        return totalLength == 0;
    case LI_CLIPBOARD_ITEM_TYPE_IMAGE:
        return totalLength <= LI_CLIPBOARD_MAX_IMAGE_SIZE;
    case LI_CLIPBOARD_ITEM_TYPE_TEXT:
    default:
        return true;
    }
}

static char* allocClipboardStringFromBytes(PBYTE_BUFFER bb, uint16_t length) {
    char* value = malloc((size_t)length + 1);
    if (value == NULL) {
        return NULL;
    }

    if (length != 0 && !BbGetBytes(bb, (uint8_t*)value, length)) {
        free(value);
        return NULL;
    }

    value[length] = '\0';
    return value;
}

static int sendClipboardControlPacket(PML_CONTROL_STREAM_CONTEXT ctx,
                                      const void* payload,
                                      int payloadLength,
                                      bool moreData) {
    if (ctx == NULL ||
            ctx->connectionContext == NULL ||
            ctx->stopping ||
            ctx->packetTypes == NULL) {
        Limelog("Ignoring clipboard control packet because control stream is not ready\n");
        return -1;
    }

    if (APP_VERSION_AT_LEAST_CTX(ctx->connectionContext, 5, 0, 0)) {
        if (ctx->client == NULL ||
                ctx->peer == NULL ||
                ctx->peer->state != ENET_PEER_STATE_CONNECTED ||
                (ctx->encryptedControlStream && ctx->encryptionCtx == NULL)) {
            Limelog("Ignoring clipboard control packet because ENet control stream is unavailable\n");
            return -1;
        }
    }
    else if (ctx->ctlSock == INVALID_SOCKET ||
             (ctx->encryptedControlStream && ctx->encryptionCtx == NULL)) {
        Limelog("Ignoring clipboard control packet because TCP control stream is unavailable\n");
        return -1;
    }

    if (!isClipboardExtensionSupported(ctx) ||
            !isAnyClipboardFeatureSupportedByHost(ctx)) {
        return LI_ERR_UNSUPPORTED;
    }

    if (!sendMessageAndForget(ctx,
                              ctx->packetTypes[IDX_CLIPBOARD],
                              payloadLength,
                              payload,
                              CTRL_CHANNEL_GENERIC,
                              ENET_PACKET_FLAG_RELIABLE,
                              moreData)) {
        return LastSocketFail();
    }

    return 0;
}

static void sendClipboardCancelBestEffort(PML_CONTROL_STREAM_CONTEXT ctx,
                                          uint64_t itemId) {
    char payload[1 + sizeof(uint64_t)];
    BYTE_BUFFER bb;

    BbInitializeWrappedBuffer(&bb, payload, 0, sizeof(payload), BYTE_ORDER_LITTLE);
    BbPut8(&bb, LI_CLIPBOARD_MSG_ITEM_CANCEL);
    BbPut64(&bb, itemId);

    (void)sendClipboardControlPacket(ctx, payload, sizeof(payload), false);
}

// Cleans up control stream
void destroyControlStreamCtx(PML_CONTROL_STREAM_CONTEXT ctx) {
    LC_ASSERT(ctx->stopping);
    freeIncomingClipboardTransfer(ctx);
    if (tls_CurrentControlStreamCtx == ctx) {
        tls_CurrentControlStreamCtx = NULL;
    }
    PltDestroyCryptoContext(ctx->encryptionCtx);
    PltDestroyCryptoContext(ctx->decryptionCtx);
    ctx->encryptionCtx = NULL;
    ctx->decryptionCtx = NULL;
    ctx->connectionContext = NULL;
    ctx->packetTypes = NULL;
    ctx->payloadLengths = NULL;
    ctx->preconstructedPayloads = NULL;
    PltCloseEvent(&ctx->idrFrameRequiredEvent);
    freeBasicLbqList(LbqDestroyLinkedBlockingQueue(&ctx->invalidReferenceFrameTuples));
    freeBasicLbqList(LbqDestroyLinkedBlockingQueue(&ctx->frameFecStatusQueue));
    freeBasicLbqList(LbqDestroyLinkedBlockingQueue(&ctx->asyncCallbackQueue));

    PltDeleteMutex(&ctx->fecSummaryMutex);
    PltDeleteMutex(&ctx->enetMutex);
}

void destroyControlStream(void) {
    destroyControlStreamCtx(LiGetEffectiveControlContext());
}

void LiRequestIdrFrameCtx(PML_CONTROL_STREAM_CONTEXT ctx);

static void queueFrameInvalidationTuple(PML_CONTROL_STREAM_CONTEXT ctx, uint32_t startFrame, uint32_t endFrame) {
    LC_ASSERT(startFrame <= endFrame);

    if (isReferenceFrameInvalidationEnabledCtx(ctx->connectionContext)) {
        PQUEUED_REFERENCE_FRAME_CONTROL qfit;
        qfit = malloc(sizeof(*qfit));
        if (qfit != NULL) {
            *qfit = (QUEUED_REFERENCE_FRAME_CONTROL){
                .startFrame = startFrame,
                .endFrame = endFrame,
                .invalidate = true,
            };
            if (LbqOfferQueueItem(&ctx->invalidReferenceFrameTuples, qfit, &qfit->entry) == LBQ_BOUND_EXCEEDED) {
                // Too many invalidation tuples, so we need an IDR frame now
                Limelog("RFI range list reached maximum size limit\n");
                free(qfit);
                LiRequestIdrFrameCtx(ctx);
            }
        }
        else {
            LiRequestIdrFrameCtx(ctx);
        }
    }
    else {
        LiRequestIdrFrameCtx(ctx);
    }
}

// Request an IDR frame on demand by the decoder
void LiRequestIdrFrameCtx(PML_CONTROL_STREAM_CONTEXT ctx) {
    // Any reference frame invalidation requests should be dropped now.
    // We require a full IDR frame to recover.
    freeBasicLbqList(LbqFlushQueueItems(&ctx->invalidReferenceFrameTuples));

    // Request the IDR frame
    PltSetEvent(&ctx->idrFrameRequiredEvent);
}

void LiRequestIdrFrame(void) {
    LiRequestIdrFrameCtx(LiGetEffectiveControlContext());
}

// Invalidate reference frames lost by the network
void connectionDetectedFrameLossCtx(PML_CONTROL_STREAM_CONTEXT ctx, uint32_t startFrame, uint32_t endFrame) {
    queueFrameInvalidationTuple(ctx, startFrame, endFrame);
}

void connectionDetectedFrameLoss(uint32_t startFrame, uint32_t endFrame) {
    connectionDetectedFrameLossCtx(LiGetEffectiveControlContext(), startFrame, endFrame);
}

// When we receive a frame, update the number of our current frame
// and send ACK control message if the frame is LTR
void connectionReceivedCompleteFrameCtx(PML_CONTROL_STREAM_CONTEXT ctx, uint32_t frameIndex, bool frameIsLTR) {
    ctx->lastGoodFrame = frameIndex;
    ctx->intervalGoodFrameCount++;

    if (frameIsLTR && IS_SUNSHINE_CTX(ctx->connectionContext) &&
            isReferenceFrameInvalidationEnabledCtx(ctx->connectionContext)) {
        // Queue LTR frame ACK control message
        PQUEUED_REFERENCE_FRAME_CONTROL qfit;
        qfit = malloc(sizeof(*qfit));
        if (qfit != NULL) {
            *qfit = (QUEUED_REFERENCE_FRAME_CONTROL){
                .startFrame = frameIndex,
                .invalidate = false,
            };
            if (LbqOfferQueueItem(&ctx->invalidReferenceFrameTuples, qfit, &qfit->entry) == LBQ_BOUND_EXCEEDED) {
                // This shouldn't happen and indicates that something has gone wrong with the queue
                LC_ASSERT(false);
                Limelog("Couldn't queue LTR ACK because the list has reached maximum size limit\n");
                free(qfit);
                LiRequestIdrFrameCtx(ctx);
            }
        }
    }
}

void connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR) {
    connectionReceivedCompleteFrameCtx(LiGetEffectiveControlContext(), frameIndex, frameIsLTR);
}

void connectionRecordFecBlockCtx(PML_CONTROL_STREAM_CONTEXT ctx, uint32_t frame, uint8_t block, uint16_t data, uint16_t received) {
    if (!ctx->videoFecControlSupported || !data || received > data) return;
    PltLockMutex(&ctx->fecSummaryMutex);
    if ((!ctx->fecHasBlock || frame != ctx->fecLastFrame || block != ctx->fecLastBlock) &&
        data <= 1000000U - ctx->fecDataPackets) {
        ctx->fecDataPackets += data;
        ctx->fecMissingPackets += data - received;
        ctx->fecLastFrame = frame;
        ctx->fecLastBlock = block;
        ctx->fecHasBlock = true;
    }
    PltUnlockMutex(&ctx->fecSummaryMutex);
}

void connectionRecordFecBlock(uint32_t frame, uint8_t block, uint16_t data, uint16_t received) {
    connectionRecordFecBlockCtx(LiGetEffectiveControlContext(), frame, block, data, received);
}

void connectionSendFrameFecStatusCtx(PML_CONTROL_STREAM_CONTEXT ctx, PSS_FRAME_FEC_STATUS fecStatus) {
    // This is a Sunshine protocol extension
    if (!IS_SUNSHINE_CTX(ctx->connectionContext)) {
        return;
    }

    // Queue a frame FEC status message. This is best-effort only.
    PQUEUED_FRAME_FEC_STATUS queuedFecStatus = malloc(sizeof(*queuedFecStatus));
    if (queuedFecStatus != NULL) {
        queuedFecStatus->fecStatus = *fecStatus;
        if (LbqOfferQueueItem(&ctx->frameFecStatusQueue, queuedFecStatus, &queuedFecStatus->entry) == LBQ_BOUND_EXCEEDED) {
            free(queuedFecStatus);
        }
    }
}

void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS fecStatus) {
    connectionSendFrameFecStatusCtx(LiGetEffectiveControlContext(), fecStatus);
}

void connectionSawFrameCtx(PML_CONTROL_STREAM_CONTEXT ctx, uint32_t frameIndex) {
    LC_ASSERT_VT(!isBefore16(frameIndex, ctx->lastSeenFrame));

    uint64_t now = PltGetMillis();

    // Suppress connection status warnings for the first sampling period
    // to allow the network and host to settle.
    if (ctx->lastSeenFrame == 0) {
        ctx->lastSeenFrame = frameIndex;
        ctx->firstFrameTimeMs = now;
        return;
    }
    else if (now - ctx->firstFrameTimeMs < CONN_STATUS_SAMPLE_PERIOD) {
        ctx->lastSeenFrame = frameIndex;
        return;
    }

    if (now - ctx->intervalStartTimeMs >= CONN_STATUS_SAMPLE_PERIOD) {
        if (ctx->intervalTotalFrameCount != 0) {
            // Notify the client of connection status changes based on frame loss rate
            int frameLossPercent = 100 - (ctx->intervalGoodFrameCount * 100) / ctx->intervalTotalFrameCount;
            unsigned int frameLossBasisPoints = (unsigned int)llround(
                10000.0 * (ctx->intervalTotalFrameCount - ctx->intervalGoodFrameCount) / ctx->intervalTotalFrameCount);
            atomic_store_explicit(&ctx->lastIntervalLossBasisPoints, frameLossBasisPoints, memory_order_relaxed);
            if (ctx->lastConnectionStatusUpdate != CONN_STATUS_POOR &&
                    (frameLossPercent >= CONN_IMMEDIATE_POOR_LOSS_RATE ||
                     (frameLossPercent >= CONN_CONSECUTIVE_POOR_LOSS_RATE && ctx->lastIntervalLossPercentage >= CONN_CONSECUTIVE_POOR_LOSS_RATE))) {
                // We require 2 consecutive intervals above CONN_CONSECUTIVE_POOR_LOSS_RATE or a single
                // interval above CONN_IMMEDIATE_POOR_LOSS_RATE to notify of a poor connection.
                ListenerCallbacks.connectionStatusUpdate(CONN_STATUS_POOR);
                ctx->lastConnectionStatusUpdate = CONN_STATUS_POOR;
            }
            else if (frameLossPercent <= CONN_OKAY_LOSS_RATE && ctx->lastConnectionStatusUpdate != CONN_STATUS_OKAY) {
                ListenerCallbacks.connectionStatusUpdate(CONN_STATUS_OKAY);
                ctx->lastConnectionStatusUpdate = CONN_STATUS_OKAY;
            }

            ctx->lastIntervalLossPercentage = frameLossPercent;
        }

        // Reset interval
        ctx->intervalStartTimeMs = now;
        ctx->intervalGoodFrameCount = ctx->intervalTotalFrameCount = 0;
    }

    ctx->intervalTotalFrameCount += frameIndex - ctx->lastSeenFrame;
    ctx->lastSeenFrame = frameIndex;
}

void connectionSawFrame(uint32_t frameIndex) {
    connectionSawFrameCtx(LiGetEffectiveControlContext(), frameIndex);
}

bool LiGetFecFeedbackStatsCtx(PML_CONTROL_STREAM_CONTEXT ctx, uint32_t* sequence,
                              uint32_t* dataPackets, uint32_t* missingPackets) {
    if (ctx == NULL || !ctx->videoFecControlSupported) {
        return false;
    }

    *sequence = atomic_load_explicit(&ctx->fecLastSentSequence, memory_order_relaxed);
    *dataPackets = atomic_load_explicit(&ctx->fecLastSentDataPackets, memory_order_relaxed);
    *missingPackets = atomic_load_explicit(&ctx->fecLastSentMissingPackets, memory_order_relaxed);
    return true;
}

double LiGetEstimatedVideoFrameLossPercentage(void) {
    return atomic_load_explicit(&LiGetEffectiveControlContext()->lastIntervalLossBasisPoints,
                                memory_order_relaxed) / 100.0;
}

// Reads an NV control stream packet from the TCP connection
static PNVCTL_TCP_PACKET_HEADER readNvctlPacketTcp(PML_CONTROL_STREAM_CONTEXT ctx) {
    NVCTL_TCP_PACKET_HEADER staticHeader;
    PNVCTL_TCP_PACKET_HEADER fullPacket;
    SOCK_RET err;

    err = recv(ctx->ctlSock, (char*)&staticHeader, sizeof(staticHeader), 0);
    if (err != sizeof(staticHeader)) {
        return NULL;
    }

    staticHeader.type = LE16(staticHeader.type);
    staticHeader.payloadLength = LE16(staticHeader.payloadLength);

    fullPacket = (PNVCTL_TCP_PACKET_HEADER)malloc(staticHeader.payloadLength + sizeof(staticHeader));
    if (fullPacket == NULL) {
        return NULL;
    }

    memcpy(fullPacket, &staticHeader, sizeof(staticHeader));
    if (staticHeader.payloadLength != 0) {
        err = recv(ctx->ctlSock, (char*)(fullPacket + 1), staticHeader.payloadLength, 0);
        if (err != staticHeader.payloadLength) {
            free(fullPacket);
            return NULL;
        }
    }

    return fullPacket;
}

static bool encryptControlMessage(PML_CONTROL_STREAM_CONTEXT ctx, PNVCTL_ENCRYPTED_PACKET_HEADER encPacket, PNVCTL_ENET_PACKET_HEADER_V2 packet) {
    unsigned char iv[16] = { 0 };
    int ivSize;
    int encryptedSize = sizeof(*packet) + packet->payloadLength;

    // NB: Setting the IV must happen while encPacket->seq is still in native byte-order!
    if (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2) {
        // Populate the IV in little endian byte order
        iv[3] = (unsigned char)(encPacket->seq >> 24);
        iv[2] = (unsigned char)(encPacket->seq >> 16);
        iv[1] = (unsigned char)(encPacket->seq >> 8);
        iv[0] = (unsigned char)(encPacket->seq >> 0);

        // Set high bytes to something unique to ensure no IV collisions
        iv[10] = (unsigned char)'C'; // Client originated
        iv[11] = (unsigned char)'C'; // Control stream

        // Use 12-byte IV which is ideal for AES-GCM
        ivSize = 12;
    }
    else {
        // This is a truncating cast, but it's what Nvidia does, so we have to mimic it.
        iv[0] = (unsigned char)encPacket->seq;

        // Nvidia's old style encryption uses a 16-byte IV
        ivSize = 16;
    }

    encPacket->encryptedHeaderType = LE16(encPacket->encryptedHeaderType);
    encPacket->length = LE16(encPacket->length);
    encPacket->seq = LE32(encPacket->seq);

    packet->type = LE16(packet->type);
    packet->payloadLength = LE16(packet->payloadLength);

    LC_ASSERT(ivSize <= (int)sizeof(iv));
    LC_ASSERT(ivSize == 12 || ivSize == 16);
    return PltEncryptMessage(ctx->encryptionCtx, ALGORITHM_AES_GCM, 0,
                             (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                             iv, ivSize,
                             (unsigned char*)(encPacket + 1), AES_GCM_TAG_LENGTH, // Write tag into the space after the encrypted header
                             (unsigned char*)packet, encryptedSize,
                             ((unsigned char*)(encPacket + 1)) + AES_GCM_TAG_LENGTH, &encryptedSize); // Write ciphertext after the GCM tag
}

// Caller must free() *packet on success!!!
static bool decryptControlMessageToV1(PML_CONTROL_STREAM_CONTEXT ctx, PNVCTL_ENCRYPTED_PACKET_HEADER encPacket, int encPacketLength, PNVCTL_ENET_PACKET_HEADER_V1* packet, int* packetLength) {
    unsigned char iv[16] = { 0 };
    int ivSize;

    *packet = NULL;

    // It must be an encrypted packet to begin with
    LC_ASSERT(encPacket->encryptedHeaderType == 0x0001);

    // Make sure the host isn't lying to us about the packet length
    int expectedEncLength = encPacket->length + sizeof(encPacket->encryptedHeaderType) + sizeof(encPacket->length);
    LC_ASSERT(encPacketLength == expectedEncLength);
    if (encPacketLength < expectedEncLength) {
        Limelog("Length exceeds packet boundary (needed %d, got %d)\n", expectedEncLength, encPacketLength);
        return false;
    }

    // Check length first so we don't underflow
    if (encPacket->length < sizeof(encPacket->seq) + AES_GCM_TAG_LENGTH + sizeof(NVCTL_ENET_PACKET_HEADER_V2)) {
        Limelog("Received runt packet (%d). Unable to decrypt.\n", encPacket->length);
        return false;
    }

    if (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2) {
        // Populate the IV in little endian byte order
        iv[3] = (unsigned char)(encPacket->seq >> 24);
        iv[2] = (unsigned char)(encPacket->seq >> 16);
        iv[1] = (unsigned char)(encPacket->seq >> 8);
        iv[0] = (unsigned char)(encPacket->seq >> 0);

        // Set high bytes to something unique to ensure no IV collisions
        iv[10] = (unsigned char)'H'; // Host originated
        iv[11] = (unsigned char)'C'; // Control stream

        // Use 12-byte IV which is ideal for AES-GCM
        ivSize = 12;
    }
    else {
        // This is a truncating cast, but it's what Nvidia does, so we have to mimic it.
        iv[0] = (unsigned char)encPacket->seq;

        // Nvidia's old style encryption uses a 16-byte IV
        ivSize = 16;
    }

    int plaintextLength = encPacket->length - sizeof(encPacket->seq) - AES_GCM_TAG_LENGTH;
    *packet = malloc(plaintextLength);
    if (*packet == NULL) {
        return false;
    }

    LC_ASSERT(ivSize <= (int)sizeof(iv));
    LC_ASSERT(ivSize == 12 || ivSize == 16);
    if (!PltDecryptMessage(ctx->decryptionCtx, ALGORITHM_AES_GCM, 0,
                           (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                           iv, ivSize,
                           (unsigned char*)(encPacket + 1), AES_GCM_TAG_LENGTH, // The tag is located right after the header
                           ((unsigned char*)(encPacket + 1)) + AES_GCM_TAG_LENGTH, plaintextLength, // The ciphertext is after the tag
                           (unsigned char*)*packet, &plaintextLength)) {
        free(*packet);
        return false;
    }

    // Now we do an in-place V2 to V1 header conversion, so our existing parsing code doesn't have to change.
    // All we need to do is eliminate the new length field in V2 by shifting everything by 2 bytes.
    memmove(((unsigned char*)*packet) + 2, ((unsigned char*)*packet) + 4, plaintextLength - 4);
    *packetLength = plaintextLength - 2;

    return true;
}

static void enetPacketFreeCb(ENetPacket* packet) {
    if (packet->userData) {
        // userData contains a bool that we will set when freed
        *(volatile bool*)packet->userData = true;
    }
}


// Must be called with enetMutex held
static bool isPacketSentWaitingForAck(PML_CONTROL_STREAM_CONTEXT ctx, ENetPacket* packet) {
    ENetOutgoingCommand* outgoingCommand = NULL;
    ENetListIterator currentCommand;

    // Look for our packet on the sent commands list
    for (currentCommand = enet_list_begin(&ctx->peer->sentReliableCommands);
         currentCommand != enet_list_end(&ctx->peer->sentReliableCommands);
         currentCommand = enet_list_next(currentCommand))
    {
        outgoingCommand = (ENetOutgoingCommand*)currentCommand;
        if (outgoingCommand->packet == packet) {
            return true;
        }
    }

    return false;
}

static bool sendMessageEnet(PML_CONTROL_STREAM_CONTEXT ctx, short ptype, int paylen, const void* payload, uint8_t channelId, uint32_t flags, bool moreData) {
    ENetPacket* enetPacket;
    int err;

    LC_ASSERT(ControlStreamAppVersionQuad[0] >= 5);

    if (paylen < 0 || paylen > UINT16_MAX) {
        return false;
    }

    // Only send reliable packets to GFE
    if (!IS_SUNSHINE_CTX(ctx->connectionContext)) {
        flags = ENET_PACKET_FLAG_RELIABLE;
    }

    if (ctx->encryptedControlStream) {
        PNVCTL_ENCRYPTED_PACKET_HEADER encPacket;
        PNVCTL_ENET_PACKET_HEADER_V2 packet;
        size_t plaintextLen = sizeof(*packet) + (size_t)paylen;
        unsigned short encryptedLength = getEncryptedControlPacketLength(paylen);

        if (encryptedLength == 0) {
            return false;
        }

        enetPacket = enet_packet_create(NULL,
                                        sizeof(*encPacket) + AES_GCM_TAG_LENGTH + plaintextLen,
                                        flags);
        if (enetPacket == NULL) {
            return false;
        }

        // We (ab)use the enetMutex to protect currentEnetSequenceNumber and the cipherContext
        // used inside encryptControlMessage().
        PltLockMutex(&ctx->enetMutex);

        encPacket = (PNVCTL_ENCRYPTED_PACKET_HEADER)enetPacket->data;
        encPacket->encryptedHeaderType = 0x0001;
        encPacket->length = encryptedLength;
        encPacket->seq = ctx->currentEnetSequenceNumber++;

        // Construct the plaintext data in place, after the tag, and encrypt it
        // over itself (no separate plaintext buffer)
        packet = (PNVCTL_ENET_PACKET_HEADER_V2)(((unsigned char*)(encPacket + 1)) + AES_GCM_TAG_LENGTH);
        packet->type = ptype;
        packet->payloadLength = (unsigned short)paylen;
        memcpy(&packet[1], payload, (size_t)paylen);

        // Encrypt the data into the final packet (and byteswap for BE machines)
        if (!encryptControlMessage(ctx, encPacket, packet)) {
            Limelog("Failed to encrypt control stream message\n");
            enet_packet_destroy(enetPacket);
            PltUnlockMutex(&ctx->enetMutex);
            return false;
        }

        // enetMutex still locked here
    }
    else {
        PNVCTL_ENET_PACKET_HEADER_V1 packet;
        enetPacket = enet_packet_create(NULL, sizeof(*packet) + paylen,
                                        flags);
        if (enetPacket == NULL) {
            return false;
        }

        packet = (PNVCTL_ENET_PACKET_HEADER_V1)enetPacket->data;
        packet->type = LE16(ptype);
        memcpy(&packet[1], payload, paylen);

        PltLockMutex(&ctx->enetMutex);
    }

    volatile bool packetFreed = false;

    // Set a callback to use to let us know if the packet has been freed.
    // Freeing can only happen when the packet is acked or send fails.
    enetPacket->userData = (void*)&packetFreed;
    enetPacket->freeCallback = enetPacketFreeCb;

    // Always use channel 0 for GFE and if the requested channel exceeds
    // the peer's supported channel count.
    if (!IS_SUNSHINE_CTX(ctx->connectionContext) || channelId >= ctx->peer->channelCount) {
        channelId = 0;
    }

    // Queue the packet to be sent
    err = enet_peer_send(ctx->peer, channelId, enetPacket);
    bool packetQueued = (err == 0);

    // If there is no more data coming soon, send the packet now
    if (!moreData && packetQueued) {
        err = enet_host_service(ctx->client, NULL, 0);

        // Wait until the packet is actually sent to provide backpressure on senders
        if (flags & ENET_PACKET_FLAG_RELIABLE) {
            // Don't wait longer than 10 milliseconds to avoid blocking callers for too long
            for (int i = 0; err >= 0 && i < 10; i++) {
                // Break on disconnected, acked/freed, or sent (pending ack).
                if (ctx->peer->state != ENET_PEER_STATE_CONNECTED || packetFreed || isPacketSentWaitingForAck(ctx, enetPacket)) {
                    break;
                }

                // Release the lock before sleeping to allow another thread to send/receive
                PltUnlockMutex(&ctx->enetMutex);
                PltSleepMs(1);
                PltLockMutex(&ctx->enetMutex);

                // Try to send the packet again
                err = enet_host_service(ctx->client, NULL, 0);
            }

            if (err >= 0 && ctx->peer->state == ENET_PEER_STATE_CONNECTED && !packetFreed && !isPacketSentWaitingForAck(ctx, enetPacket)) {
                Limelog("Control message took over 10 ms to send (net latency: %u ms | packet loss: %f%%)\n",
                        ctx->peer->roundTripTime, ctx->peer->packetLoss / (float)ENET_PEER_PACKET_LOSS_SCALE);
            }
        }
    }

    // Remove the free callback now that the packet was sent
    if (!packetFreed) {
        enetPacket->userData = NULL;
        enetPacket->freeCallback = NULL;
    }

    PltUnlockMutex(&ctx->enetMutex);

    if (err < 0) {
        Limelog("Failed to send ENet control packet\n");
        if (!packetQueued) {
            enet_packet_destroy(enetPacket);
        }
        return false;
    }

    return true;
}

static bool sendMessageTcp(PML_CONTROL_STREAM_CONTEXT ctx, short ptype, int paylen, const void* payload) {
    PNVCTL_TCP_PACKET_HEADER packet;
    SOCK_RET err;

    LC_ASSERT(ControlStreamAppVersionQuad[0] < 5);

    if (paylen < 0 || paylen > UINT16_MAX) {
        return false;
    }

    packet = malloc(sizeof(*packet) + paylen);
    if (packet == NULL) {
        return false;
    }

    packet->type = LE16(ptype);
    packet->payloadLength = LE16((unsigned short)paylen);
    memcpy(&packet[1], payload, paylen);

    err = send(ctx->ctlSock, (char*) packet, sizeof(*packet) + paylen, 0);
    free(packet);

    if (err != (SOCK_RET)(sizeof(*packet) + paylen)) {
        return false;
    }

    return true;
}

static bool sendMessageAndForget(PML_CONTROL_STREAM_CONTEXT ctx, short ptype, int paylen, const void* payload, uint8_t channelId, uint32_t flags, bool moreData) {
    bool ret;

    // Unlike regular sockets, ENet sockets aren't safe to invoke from multiple
    // threads at once. We have to synchronize them with a lock.
    if (ControlStreamAppVersionQuad[0] >= 5) {
        ret = sendMessageEnet(ctx, ptype, paylen, payload, channelId, flags, moreData);
    }
    else {
        ret = sendMessageTcp(ctx, ptype, paylen, payload);
    }

    return ret;
}

static bool sendMessageAndDiscardReply(PML_CONTROL_STREAM_CONTEXT ctx, short ptype, int paylen, const void* payload, uint8_t channelId, uint32_t flags, bool moreData) {
    if (ControlStreamAppVersionQuad[0] >= 5) {
        if (!sendMessageEnet(ctx, ptype, paylen, payload, channelId, flags, moreData)) {
            return false;
        }
    }
    else {
        PNVCTL_TCP_PACKET_HEADER reply;

        if (!sendMessageTcp(ctx, ptype, paylen, payload)) {
            return false;
        }

        // Discard the response
        reply = readNvctlPacketTcp(ctx);
        if (reply == NULL) {
            return false;
        }

        free(reply);
    }

    return true;
}


// This intercept function drops disconnect events to allow us to process
// pending receives first. It works around what appears to be a bug in ENet
// where pending disconnects can cause loss of unprocessed received data.
static int ignoreDisconnectIntercept(ENetHost* host, ENetEvent* event) {
    PML_CONTROL_STREAM_CONTEXT ctx = tls_CurrentControlStreamCtx;

    if (host->receivedDataLength == sizeof(ENetProtocolHeader) + sizeof(ENetProtocolDisconnect)) {
        ENetProtocolHeader* protoHeader = (ENetProtocolHeader*)host->receivedData;
        ENetProtocolDisconnect* disconnect = (ENetProtocolDisconnect*)(protoHeader + 1);

        if ((disconnect->header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_DISCONNECT) {
            Limelog("ENet disconnect event pending\n");
            if (ctx != NULL) {
                ctx->disconnectPending = true;
            }
            if (event) {
                event->type = ENET_EVENT_TYPE_NONE;
            }
            return 1;
        }
    }

    return 0;
}

static void asyncCallbackThreadFunc(void* context) {
    PML_CONTROL_STREAM_CONTEXT ctx = (PML_CONTROL_STREAM_CONTEXT)context;
    LiSetThreadConnectionContext(ctx->connectionContext);
    PQUEUED_ASYNC_CALLBACK queuedCb, nextCb;

    while (LbqWaitForQueueElement(&ctx->asyncCallbackQueue, (void**)&queuedCb) == LBQ_SUCCESS) {
        switch (queuedCb->typeIndex) {
        case IDX_RUMBLE_DATA:
            // Look for another rumble packet to batch with
            while (LbqPeekQueueElement(&ctx->asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS) {
                // Don't batch with the next packet if it is a different type or controller number
                if (nextCb->typeIndex != queuedCb->typeIndex ||
                        nextCb->data.rumble.controllerNumber != queuedCb->data.rumble.controllerNumber) {
                    break;
                }

                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&ctx->asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.rumble(queuedCb->data.rumble.controllerNumber,
                                     queuedCb->data.rumble.lowFreqRumble,
                                     queuedCb->data.rumble.highFreqRumble);
            break;
        case IDX_RUMBLE_TRIGGER_DATA:
            // Look for another rumble triggers packet to batch with
            while (LbqPeekQueueElement(&ctx->asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS) {
                // Don't batch with the next packet if it is a different type or controller number
                if (nextCb->typeIndex != queuedCb->typeIndex ||
                        nextCb->data.rumbleTriggers.controllerNumber != queuedCb->data.rumbleTriggers.controllerNumber) {
                    break;
                }

                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&ctx->asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.rumbleTriggers(queuedCb->data.rumbleTriggers.controllerNumber,
                                             queuedCb->data.rumbleTriggers.leftTriggerMotor,
                                             queuedCb->data.rumbleTriggers.rightTriggerMotor);
            break;
        case IDX_SET_RGB_LED:
            // Look for another controller LED packet to batch with
            while (LbqPeekQueueElement(&ctx->asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS) {
                // Don't batch with the next packet if it is a different type or controller number
                if (nextCb->typeIndex != queuedCb->typeIndex ||
                        nextCb->data.setControllerLed.controllerNumber != queuedCb->data.setControllerLed.controllerNumber) {
                    break;
                }

                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&ctx->asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.setControllerLED(queuedCb->data.setControllerLed.controllerNumber,
                                               queuedCb->data.setControllerLed.r,
                                               queuedCb->data.setControllerLed.g,
                                               queuedCb->data.setControllerLed.b);
            break;
        case IDX_HDR_INFO:
            // HDR state is maintained globally, so we just invoke the client callback here.
            // These events are stateless, so we can consume all of them now.
            while (LbqPeekQueueElement(&ctx->asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS && nextCb->typeIndex == queuedCb->typeIndex) {
                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&ctx->asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.setHdrMode(ctx->hdrEnabled,
                                         IS_SUNSHINE_CTX(ctx->connectionContext) && ctx->hdrEnabled ? &ctx->hdrMetadata : NULL);
            break;

        case IDX_SET_MOTION_EVENT:
            // These events are infrequent and cannot be batched
            ListenerCallbacks.setMotionEventState(queuedCb->data.setMotionEventState.controllerNumber,
                                                  queuedCb->data.setMotionEventState.motionType,
                                                  queuedCb->data.setMotionEventState.reportRateHz);
            break;
        case IDX_DS_ADAPTIVE_TRIGGERS:
            ListenerCallbacks.setAdaptiveTriggers(queuedCb->data.dsAdaptiveTrigger.controllerNumber,
                                                  queuedCb->data.dsAdaptiveTrigger.eventFlags,
                                                  queuedCb->data.dsAdaptiveTrigger.typeLeft,
                                                  queuedCb->data.dsAdaptiveTrigger.typeRight,
                                                  queuedCb->data.dsAdaptiveTrigger.left,
                                                  queuedCb->data.dsAdaptiveTrigger.right);
            break;
        case IDX_RESOLUTION_CHANGE:
            // Resolution state is maintained in the control stream context, so we just
            // invoke the client callback here. These events are stateless, so we can
            // consume all of them now.
            while (LbqPeekQueueElement(&ctx->asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS && nextCb->typeIndex == queuedCb->typeIndex) {
                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&ctx->asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            if (ListenerCallbacks.resolutionChanged != NULL) {
                ListenerCallbacks.resolutionChanged(ctx->currentResolutionWidth, ctx->currentResolutionHeight);
            }
            break;
        default:
            // Unhandled packet type from queueAsyncCallback()
            LC_ASSERT(false);
            break;
        }

        free(queuedCb);
    }
}

static bool needsAsyncCallback(PML_CONTROL_STREAM_CONTEXT ctx, unsigned short packetType) {
    return packetType == ctx->packetTypes[IDX_RUMBLE_DATA] ||
           packetType == ctx->packetTypes[IDX_RUMBLE_TRIGGER_DATA] ||
           packetType == ctx->packetTypes[IDX_SET_MOTION_EVENT] ||
           packetType == ctx->packetTypes[IDX_SET_RGB_LED] ||
           packetType == ctx->packetTypes[IDX_HDR_INFO] ||
           packetType == ctx->packetTypes[IDX_DS_ADAPTIVE_TRIGGERS] ||
           // Sunshine unified dynamic parameter change message (0x5506). We currently map
           // RESOLUTION changes onto the existing resolutionChanged callback.
           packetType == ctx->packetTypes[IDX_DYNAMIC_PARAM_CHANGE] ||
           packetType == ctx->packetTypes[IDX_RESOLUTION_CHANGE];
}

static void queueAsyncCallback(PML_CONTROL_STREAM_CONTEXT ctx, PNVCTL_ENET_PACKET_HEADER_V1 ctlHdr, int packetLength) {
    BYTE_BUFFER bb;
    PQUEUED_ASYNC_CALLBACK queuedCb;
    int err;

    LC_ASSERT(needsAsyncCallback(ctx, ctlHdr->type));

    queuedCb = malloc(sizeof(*queuedCb));
    if (!queuedCb) {
        return;
    }

    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);

    if (ctlHdr->type == ctx->packetTypes[IDX_RUMBLE_DATA]) {
        BbAdvanceBuffer(&bb, 4);

        BbGet16(&bb, &queuedCb->data.rumble.controllerNumber);
        BbGet16(&bb, &queuedCb->data.rumble.lowFreqRumble);
        BbGet16(&bb, &queuedCb->data.rumble.highFreqRumble);

        queuedCb->typeIndex = IDX_RUMBLE_DATA;
    }
    else if (ctlHdr->type == ctx->packetTypes[IDX_RUMBLE_TRIGGER_DATA]) {
        BbGet16(&bb, &queuedCb->data.rumbleTriggers.controllerNumber);
        BbGet16(&bb, &queuedCb->data.rumbleTriggers.leftTriggerMotor);
        BbGet16(&bb, &queuedCb->data.rumbleTriggers.rightTriggerMotor);

        queuedCb->typeIndex = IDX_RUMBLE_TRIGGER_DATA;
    }
    else if (ctlHdr->type == ctx->packetTypes[IDX_SET_MOTION_EVENT]) {
        BbGet16(&bb, &queuedCb->data.setMotionEventState.controllerNumber);
        BbGet16(&bb, &queuedCb->data.setMotionEventState.reportRateHz);
        BbGet8(&bb, &queuedCb->data.setMotionEventState.motionType);

        // Remote demand must not depend on a delayed or full platform callback queue.
        setControllerMotionEnabledCtx(&ctx->connectionContext->inputContext,
                                   queuedCb->data.setMotionEventState.controllerNumber,
                                   queuedCb->data.setMotionEventState.motionType,
                                   queuedCb->data.setMotionEventState.reportRateHz > 0);
        queuedCb->typeIndex = IDX_SET_MOTION_EVENT;
    }
    else if (ctlHdr->type == ctx->packetTypes[IDX_SET_RGB_LED]) {
        BbGet16(&bb, &queuedCb->data.setControllerLed.controllerNumber);
        BbGet8(&bb, &queuedCb->data.setControllerLed.r);
        BbGet8(&bb, &queuedCb->data.setControllerLed.g);
        BbGet8(&bb, &queuedCb->data.setControllerLed.b);

        queuedCb->typeIndex = IDX_SET_RGB_LED;
    }
    else if (ctlHdr->type == ctx->packetTypes[IDX_HDR_INFO]) {
        queuedCb->typeIndex = IDX_HDR_INFO;
    }
    else if (ctlHdr->type == ctx->packetTypes[IDX_DS_ADAPTIVE_TRIGGERS]){
        BbGet16(&bb, &queuedCb->data.dsAdaptiveTrigger.controllerNumber);
        BbGet8(&bb, &queuedCb->data.dsAdaptiveTrigger.eventFlags);
        BbGet8(&bb, &queuedCb->data.dsAdaptiveTrigger.typeLeft);
        BbGet8(&bb, &queuedCb->data.dsAdaptiveTrigger.typeRight);

        BbGetBytes(&bb, queuedCb->data.dsAdaptiveTrigger.left, DS_EFFECT_PAYLOAD_SIZE);
        BbGetBytes(&bb, queuedCb->data.dsAdaptiveTrigger.right, DS_EFFECT_PAYLOAD_SIZE);
        queuedCb->typeIndex = IDX_DS_ADAPTIVE_TRIGGERS;
    }
    else if (ctlHdr->type == ctx->packetTypes[IDX_RESOLUTION_CHANGE]) {
        // Resolution data is already processed in the receive thread to update state
        queuedCb->typeIndex = IDX_RESOLUTION_CHANGE;
    }
    else if (ctlHdr->type == ctx->packetTypes[IDX_DYNAMIC_PARAM_CHANGE]) {
        // Sunshine unified dynamic parameter change message (0x5506).
        // Payload format (little-endian):
        // - int32 param_type
        // - param payload (for RESOLUTION: int32 width, int32 height)
        uint32_t paramType = 0;
        BbGet32(&bb, &paramType);

        if (paramType == SS_DYNAMIC_PARAM_TYPE_RESOLUTION) {
            // Resolution data is already processed in the receive thread.
            // Map onto the existing resolutionChanged callback.
            queuedCb->typeIndex = IDX_RESOLUTION_CHANGE;
        }
        else {
            // Ignore unsupported dynamic parameter types for now
            free(queuedCb);
            return;
        }
    }
    else {
        // Unhandled packet type from needsAsyncCallback()
        LC_ASSERT(false);
        free(queuedCb);
        return;
    }

    err = LbqOfferQueueItem(&ctx->asyncCallbackQueue, queuedCb, &queuedCb->entry);
    if (err != LBQ_SUCCESS) {
        Limelog("Failed to queue async callback: %d\n", err);
        free(queuedCb);
    }
}

static void handleClipboardPacket(PML_CONTROL_STREAM_CONTEXT ctx,
                                  PNVCTL_ENET_PACKET_HEADER_V1 ctlHdr,
                                  int packetLength) {
    BYTE_BUFFER bb;
    uint8_t kind;

    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr),
                              packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);
    if (!BbGet8(&bb, &kind)) {
        Limelog("Clipboard packet missing message kind\n");
        return;
    }

    // Foundation/Vplus clipboard agents use an opaque v1 payload beginning
    // with version=1 and a kind byte. Keep this before the legacy item switch:
    // legacy control messages start at 0x01 but are shorter than a v1 frame.
    if (kind == 1 && packetLength - (int)sizeof(*ctlHdr) >= 10) {
        uint8_t rawKind = 0;
        uint32_t rawToken = 0;
        uint32_t rawLength = 0;
        BYTE_BUFFER raw = bb;
        if (BbGet8(&raw, &rawKind) &&
            (rawKind == LI_CLIPBOARD_KIND_TEXT ||
             rawKind == LI_CLIPBOARD_KIND_PNG ||
             rawKind == LI_CLIPBOARD_KIND_REF) &&
            BbGet32(&raw, &rawToken) &&
            BbGet32(&raw, &rawLength) &&
            rawLength <= (uint32_t)(packetLength - (int)sizeof(*ctlHdr) - 10) &&
            ListenerCallbacks.clipboardDataReceived != NULL) {
            ListenerCallbacks.clipboardDataReceived(
                (const uint8_t *)ctlHdr + sizeof(*ctlHdr),
                (uint32_t)(packetLength - (int)sizeof(*ctlHdr)));
            return;
        }
    }

    switch (kind) {
    case LI_CLIPBOARD_MSG_ITEM_START: {
        uint8_t transferFlags;
        uint8_t itemType;
        uint8_t reserved;
        uint64_t itemId;
        uint64_t contentHash;
        uint32_t totalLength;
        uint16_t mimeTypeLength;
        uint16_t nameLength;
        char* mimeType = NULL;
        char* name = NULL;
        uint8_t* data = NULL;

        if (!BbGet8(&bb, &transferFlags) ||
                !BbGet8(&bb, &itemType) ||
                !BbGet8(&bb, &reserved) ||
                !BbGet64(&bb, &itemId) ||
                !BbGet64(&bb, &contentHash) ||
                !BbGet32(&bb, &totalLength) ||
                !BbGet16(&bb, &mimeTypeLength) ||
                !BbGet16(&bb, &nameLength)) {
            Limelog("Clipboard ITEM_START header truncated\n");
            return;
        }

        if (itemType != LI_CLIPBOARD_ITEM_TYPE_NONE &&
                itemType != LI_CLIPBOARD_ITEM_TYPE_TEXT &&
                itemType != LI_CLIPBOARD_ITEM_TYPE_IMAGE) {
            Limelog("Clipboard ITEM_START with invalid type: %u\n", itemType);
            return;
        }

        if (!isClipboardTransferLengthValid(itemType, totalLength)) {
            Limelog("Clipboard ITEM_START exceeded configured size limits: type=%u length=%u\n",
                    itemType, totalLength);
            return;
        }

        mimeType = allocClipboardStringFromBytes(&bb, mimeTypeLength);
        if (mimeType == NULL) {
            Limelog("Clipboard ITEM_START failed to allocate mime type\n");
            return;
        }

        name = allocClipboardStringFromBytes(&bb, nameLength);
        if (name == NULL) {
            Limelog("Clipboard ITEM_START failed to allocate item name\n");
            free(mimeType);
            return;
        }

        if (totalLength != 0) {
            data = malloc(totalLength);
            if (data == NULL) {
                Limelog("Clipboard ITEM_START failed to allocate %u bytes\n",
                        totalLength);
                free(mimeType);
                free(name);
                return;
            }
        }

        if (ctx->incomingClipboardTransfer.active) {
            Limelog("Clipboard ITEM_START replaced unfinished item %" PRIu64 "\n",
                    ctx->incomingClipboardTransfer.itemId);
            freeIncomingClipboardTransfer(ctx);
        }

        ctx->incomingClipboardTransfer.active = true;
        ctx->incomingClipboardTransfer.itemType = itemType;
        ctx->incomingClipboardTransfer.transferFlags = transferFlags;
        ctx->incomingClipboardTransfer.itemId = itemId;
        ctx->incomingClipboardTransfer.contentHash = contentHash;
        ctx->incomingClipboardTransfer.totalLength = totalLength;
        ctx->incomingClipboardTransfer.receivedLength = 0;
        ctx->incomingClipboardTransfer.mimeType = mimeType;
        ctx->incomingClipboardTransfer.name = name;
        ctx->incomingClipboardTransfer.data = data;
        break;
    }
    case LI_CLIPBOARD_MSG_ITEM_CHUNK: {
        uint8_t reserved;
        uint16_t chunkLength;
        uint64_t itemId;
        uint32_t chunkOffset;
        uint32_t newReceivedLength;

        if (!BbGet8(&bb, &reserved) ||
                !BbGet16(&bb, &chunkLength) ||
                !BbGet64(&bb, &itemId) ||
                !BbGet32(&bb, &chunkOffset)) {
            Limelog("Clipboard ITEM_CHUNK header truncated\n");
            return;
        }

        if (!ctx->incomingClipboardTransfer.active ||
                ctx->incomingClipboardTransfer.itemId != itemId) {
            Limelog("Clipboard ITEM_CHUNK without matching transfer: %" PRIu64 "\n",
                    itemId);
            return;
        }

        if (chunkOffset > ctx->incomingClipboardTransfer.totalLength ||
                chunkLength > ctx->incomingClipboardTransfer.totalLength - chunkOffset) {
            Limelog("Clipboard ITEM_CHUNK outside bounds: offset=%u length=%u total=%u\n",
                    chunkOffset, chunkLength, ctx->incomingClipboardTransfer.totalLength);
            freeIncomingClipboardTransfer(ctx);
            return;
        }

        if (chunkLength != 0 &&
                !BbGetBytes(&bb,
                            ctx->incomingClipboardTransfer.data + chunkOffset,
                            chunkLength)) {
            Limelog("Clipboard ITEM_CHUNK payload truncated\n");
            freeIncomingClipboardTransfer(ctx);
            return;
        }

        newReceivedLength = chunkOffset + chunkLength;
        if (newReceivedLength > ctx->incomingClipboardTransfer.receivedLength) {
            ctx->incomingClipboardTransfer.receivedLength = newReceivedLength;
        }
        break;
    }
    case LI_CLIPBOARD_MSG_ITEM_END: {
        uint64_t itemId;
        LI_CLIPBOARD_ITEM item;

        if (!BbGet64(&bb, &itemId)) {
            Limelog("Clipboard ITEM_END truncated\n");
            return;
        }

        if (!ctx->incomingClipboardTransfer.active ||
                ctx->incomingClipboardTransfer.itemId != itemId) {
            Limelog("Clipboard ITEM_END without matching transfer: %" PRIu64 "\n",
                    itemId);
            return;
        }

        if (ctx->incomingClipboardTransfer.receivedLength !=
                ctx->incomingClipboardTransfer.totalLength) {
            Limelog("Clipboard ITEM_END before transfer completion: %" PRIu64
                    " (%u/%u)\n",
                    itemId,
                    ctx->incomingClipboardTransfer.receivedLength,
                    ctx->incomingClipboardTransfer.totalLength);
            freeIncomingClipboardTransfer(ctx);
            return;
        }

        memset(&item, 0, sizeof(item));
        item.type = ctx->incomingClipboardTransfer.itemType;
        item.data = ctx->incomingClipboardTransfer.data;
        item.length = ctx->incomingClipboardTransfer.totalLength;
        item.mimeType = ctx->incomingClipboardTransfer.mimeType;
        item.name = ctx->incomingClipboardTransfer.name;
        item.itemId = ctx->incomingClipboardTransfer.itemId;
        item.contentHash = ctx->incomingClipboardTransfer.contentHash;
        item.flags = ctx->incomingClipboardTransfer.transferFlags;

        ListenerCallbacks.clipboardItemReceived(&item);
        freeIncomingClipboardTransfer(ctx);
        break;
    }
    case LI_CLIPBOARD_MSG_ITEM_CANCEL: {
        uint64_t itemId;

        if (!BbGet64(&bb, &itemId)) {
            Limelog("Clipboard ITEM_CANCEL truncated\n");
            return;
        }

        if (ctx->incomingClipboardTransfer.active &&
                (itemId == 0 || ctx->incomingClipboardTransfer.itemId == itemId)) {
            freeIncomingClipboardTransfer(ctx);
        }
        break;
    }
    case LI_CLIPBOARD_MSG_BIND:
    case LI_CLIPBOARD_MSG_UNBIND:
    case LI_CLIPBOARD_MSG_SNAPSHOT_REQUEST:
        Limelog("Ignoring unexpected host clipboard control message: %u\n", kind);
        break;
    default:
        Limelog("Unknown clipboard control message kind: %u\n", kind);
        break;
    }
}

static void controlReceiveThreadFunc(void* context) {
    PML_CONTROL_STREAM_CONTEXT ctx = (PML_CONTROL_STREAM_CONTEXT)context;
    LiSetThreadConnectionContext(ctx->connectionContext);
    int err;

    // This is only used for ENet
    if (ControlStreamAppVersionQuad[0] < 5) {
        return;
    }

    while (!PltIsThreadInterrupted(&ctx->controlReceiveThread)) {
        ENetEvent event;
        enet_uint32 waitTimeMs;

        PltLockMutex(&ctx->enetMutex);

        // Poll for new packets and process retransmissions
        err = serviceEnetHost(ctx->client, &event, 0);

        // Compute the next time we need to wake up to handle
        // the RTO timer or a ping.
        if (err == 0) {
            if (ENET_TIME_LESS(ctx->peer->nextTimeout, ctx->client->serviceTime)) {
                // This can happen when we have no unacked reliable messages
                waitTimeMs = 10;
            }
            else {
                // We add 1 ms just to ensure we're unlikely to undershoot the sleep() and have to
                // do a tiny sleep for another iteration before the timeout is ready to be serviced.
                waitTimeMs = ENET_TIME_DIFFERENCE(ctx->peer->nextTimeout, ctx->client->serviceTime) + 1;
            }

            // Ensure we don't sleep through a ping
            if (ctx->peer->lastReceiveTime && ctx->peer->lastSendTime) {
                enet_uint32 timeSinceLastRecv = ENET_TIME_DIFFERENCE(ctx->client->serviceTime, ctx->peer->lastReceiveTime);
                enet_uint32 timeSinceLastSend = ENET_TIME_DIFFERENCE(ctx->client->serviceTime, ctx->peer->lastSendTime);
                enet_uint32 timeSinceLastComm = MIN(timeSinceLastSend, timeSinceLastRecv);

                if (timeSinceLastComm >= ctx->peer->pingInterval) {
                    // Ping is due now for this peer
                    waitTimeMs = 0;
                } else {
                    waitTimeMs = MIN(waitTimeMs, ctx->peer->pingInterval - timeSinceLastComm);
                }
            }
            else {
                waitTimeMs = MIN(waitTimeMs, ctx->peer->pingInterval);
            }
        }

        PltUnlockMutex(&ctx->enetMutex);

        if (err == 0) {
            // Handle a pending disconnect after unsuccessfully polling
            // for new events to handle.
            if (ctx->disconnectPending) {
                PltLockMutex(&ctx->enetMutex);
                // Wait 100 ms for pending receives after a disconnect and
                // 1 second for the pending disconnect to be processed after
                // removing the intercept callback.
                err = serviceEnetHost(ctx->client, &event, ctx->client->intercept ? 100 : 1000);
                if (err == 0) {
                    if (ctx->client->intercept) {
                        // Now that no pending receive events remain, we can
                        // remove our intercept hook and allow the server's
                        // disconnect to be processed as expected. We will wait
                        // 1 second for this disconnect to be processed before
                        // we tear down the connection anyway.
                        ctx->client->intercept = NULL;
                        PltUnlockMutex(&ctx->enetMutex);
                        continue;
                    }
                    else {
                        // The 1 second timeout has expired with no disconnect event
                        // retransmission after the first notification. We can only
                        // assume the server died tragically, so go ahead and tear down.
                        PltUnlockMutex(&ctx->enetMutex);
                        Limelog("Disconnect event timeout expired\n");
                        ListenerCallbacks.connectionTerminated(-1);
                        return;
                    }
                }
                else {
                    PltUnlockMutex(&ctx->enetMutex);
                }
            }
            else {
                // No events ready - wait for readability or a local RTO timer to expire
                enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
                enet_socket_wait(ctx->client->socket, &condition, waitTimeMs);
                continue;
            }
        }

        if (err < 0) {
            // The error from serviceEnetHost() should be propagated via LastSocketError()
            LC_ASSERT(err == -1);

            err = LastSocketFail();
            Limelog("Control stream connection failed: %d\n", err);
            ListenerCallbacks.connectionTerminated(controlTransportError(ctx, err));
            return;
        }

        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            PNVCTL_ENET_PACKET_HEADER_V1 ctlHdr;
            int packetLength;

            if (event.packet->dataLength < sizeof(*ctlHdr)) {
                Limelog("Discarding runt control packet: %d < %d\n", event.packet->dataLength, (int)sizeof(*ctlHdr));
                enet_packet_destroy(event.packet);
                continue;
            }

            ctlHdr = (PNVCTL_ENET_PACKET_HEADER_V1)event.packet->data;
            ctlHdr->type = LE16(ctlHdr->type);

            if (ctx->encryptedControlStream) {
                // V2 headers can be interpreted as V1 headers for the purpose of examining type,
                // so this check is safe.
                if (ctlHdr->type == 0x0001) {
                    PNVCTL_ENCRYPTED_PACKET_HEADER encHdr;

                    if (event.packet->dataLength < sizeof(NVCTL_ENCRYPTED_PACKET_HEADER)) {
                        Limelog("Discarding runt encrypted control packet: %d < %d\n", event.packet->dataLength, (int)sizeof(NVCTL_ENCRYPTED_PACKET_HEADER));
                        enet_packet_destroy(event.packet);
                        continue;
                    }

                    // encryptedHeaderType is already byteswapped by aliasing through ctlHdr above
                    encHdr = (PNVCTL_ENCRYPTED_PACKET_HEADER)event.packet->data;
                    encHdr->length = LE16(encHdr->length);
                    encHdr->seq = LE32(encHdr->seq);

                    ctlHdr = NULL;
                    packetLength = (int)event.packet->dataLength;
                    if (!decryptControlMessageToV1(ctx, encHdr, packetLength, &ctlHdr, &packetLength)) {
                        Limelog("Failed to decrypt control packet of size %d\n", event.packet->dataLength);
                        enet_packet_destroy(event.packet);
                        continue;
                    }

                    // We need to byteswap the unsealed header too
                    ctlHdr->type = LE16(ctlHdr->type);
                }
                else {
                    LC_ASSERT_VT(false);
                    Limelog("Discarding unencrypted packet on encrypted control stream: %04x\n", ctlHdr->type);
                    enet_packet_destroy(event.packet);
                    continue;
                }
            }
            else {
                // Take ownership of the packet data directly for the non-encrypted case
                packetLength = (int)event.packet->dataLength;
                event.packet->data = NULL;
            }

            // We're done with the packet struct
            enet_packet_destroy(event.packet);

            // All below codepaths must free ctlHdr!!!

            // Process HDR data immediately to update global HDR enabled state and HDR metadata.
            // The actual client callback will be invoked in the async callback thread.
            if (ctlHdr->type == ctx->packetTypes[IDX_HDR_INFO]) {
                BYTE_BUFFER bb;
                uint8_t enableByte;

                BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);

                BbGet8(&bb, &enableByte);
                if (IS_SUNSHINE_CTX(ctx->connectionContext)) {
                    // Zero the metadata buffer to properly handle older servers if we have to add new fields
                    memset(&ctx->hdrMetadata, 0, sizeof(ctx->hdrMetadata));

                    // Sunshine sends HDR metadata in this message too
                    for (int i = 0; i < 3; i++) {
                        BbGet16(&bb, &ctx->hdrMetadata.displayPrimaries[i].x);
                        BbGet16(&bb, &ctx->hdrMetadata.displayPrimaries[i].y);
                    }
                    BbGet16(&bb, &ctx->hdrMetadata.whitePoint.x);
                    BbGet16(&bb, &ctx->hdrMetadata.whitePoint.y);
                    BbGet16(&bb, &ctx->hdrMetadata.maxDisplayLuminance);
                    BbGet16(&bb, &ctx->hdrMetadata.minDisplayLuminance);
                    BbGet16(&bb, &ctx->hdrMetadata.maxContentLightLevel);
                    BbGet16(&bb, &ctx->hdrMetadata.maxFrameAverageLightLevel);
                    BbGet16(&bb, &ctx->hdrMetadata.maxFullFrameLuminance);
                }

                ctx->hdrEnabled = (enableByte != 0);
            }

            // Process resolution change data immediately to update the resolution state.
            // The actual client callback will be invoked in the async callback thread.
            if (ctlHdr->type == ctx->packetTypes[IDX_RESOLUTION_CHANGE]) {
                BYTE_BUFFER bb;

                BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);

                BbGet32(&bb, &ctx->currentResolutionWidth);
                BbGet32(&bb, &ctx->currentResolutionHeight);
            }
            else if (ctlHdr->type == ctx->packetTypes[IDX_DYNAMIC_PARAM_CHANGE]) {
                // Sunshine unified dynamic parameter change message (0x5506).
                // If this is a RESOLUTION change, update the resolution state so the
                // async callback thread can notify the client via resolutionChanged.
                BYTE_BUFFER bb;
                uint32_t paramType = 0;

                BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);
                BbGet32(&bb, &paramType);

                if (paramType == SS_DYNAMIC_PARAM_TYPE_RESOLUTION) {
                    BbGet32(&bb, &ctx->currentResolutionWidth);
                    BbGet32(&bb, &ctx->currentResolutionHeight);
                }
            }

            // Process client callbacks in a separate thread
            if (needsAsyncCallback(ctx, ctlHdr->type)) {
                queueAsyncCallback(ctx, ctlHdr, packetLength);
            }
            else if (ctlHdr->type == ctx->packetTypes[IDX_TERMINATION]) {
                BYTE_BUFFER bb;


                uint32_t terminationErrorCode;

                if (packetLength >= 6) {
                    // This is the extended termination message which contains a full HRESULT
                    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_BIG);
                    BbGet32(&bb, &terminationErrorCode);

                    Limelog("Server notified termination reason: 0x%08x\n", terminationErrorCode);

                    // Normalize the termination error codes for specific values we recognize
                    switch (terminationErrorCode) {
                    case 0x800e9403: // NVST_DISCONN_SERVER_VIDEO_ENCODER_CONVERT_INPUT_FRAME_FAILED
                        terminationErrorCode = ML_ERROR_FRAME_CONVERSION;
                        break;
                    case 0x800e9302: // NVST_DISCONN_SERVER_VFP_PROTECTED_CONTENT
                        terminationErrorCode = ML_ERROR_PROTECTED_CONTENT;
                        break;
                    case 0x80030023: // NVST_DISCONN_SERVER_TERMINATED_CLOSED
                        if (ctx->lastSeenFrame != 0) {
                            // Pass error code 0 to notify the client that this was not an error
                            terminationErrorCode = ML_ERROR_GRACEFUL_TERMINATION;
                        }
                        else {
                            // We never saw a frame, so this is probably an error that caused
                            // NvStreamer to terminate prior to sending any frames.
                            terminationErrorCode = ML_ERROR_UNEXPECTED_EARLY_TERMINATION;
                        }
                        break;
                    default:
                        break;
                    }
                }
                else {
                    uint16_t terminationReason;

                    // This is the short termination message
                    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);
                    BbGet16(&bb, &terminationReason);

                    Limelog("Server notified termination reason: 0x%04x\n", terminationReason);

                    // SERVER_TERMINATED_INTENDED
                    if (terminationReason == 0x0100) {
                        if (ctx->lastSeenFrame != 0) {
                            // Pass error code 0 to notify the client that this was not an error
                            terminationErrorCode = ML_ERROR_GRACEFUL_TERMINATION;
                        }
                        else {
                            // We never saw a frame, so this is probably an error that caused
                            // NvStreamer to terminate prior to sending any frames.
                            terminationErrorCode = ML_ERROR_UNEXPECTED_EARLY_TERMINATION;
                        }
                    }
                    else {
                        // Otherwise pass the reason unmodified
                        terminationErrorCode = terminationReason;
                    }
                }

                // We used to wait for a ENET_EVENT_TYPE_DISCONNECT event, but since
                // GFE 3.20.3.63 we don't get one for 10 seconds after we first get
                // this termination message. The termination message should be reliable
                // enough to end the stream now, rather than waiting for an explicit
                // disconnect. The server will also not acknowledge our disconnect
                // message once it sends this message, so we mark the peer as fully
                // disconnected now to avoid delays waiting for an ack that will
                // never arrive.
                PltLockMutex(&ctx->enetMutex);
                enet_peer_disconnect_now(ctx->peer, 0);
                PltUnlockMutex(&ctx->enetMutex);
                ListenerCallbacks.connectionTerminated((int)terminationErrorCode);
                free(ctlHdr);
                return;
            }
            else if (ctlHdr->type == ctx->packetTypes[IDX_CLIPBOARD]) {
                handleClipboardPacket(ctx, ctlHdr, packetLength);
            }
            else if (ctlHdr->type == 0x550D) {
                // Absolute DualSense actuator controls (Sunshine protocol extension)
                uint16_t controller;
                const uint8_t* state;
                if (ctx->connectionContext->Ds5HapticsStateCallback != NULL && parseDs5HapticsState(
                        (const uint8_t*)(ctlHdr + 1), packetLength - (int)sizeof(*ctlHdr),
                        &controller, &state)) {
                    ctx->connectionContext->Ds5HapticsStateCallback(controller, state);
                }
            }
            else if (ctlHdr->type == 0x550E) {
                // Host interactive session lock state (Sunshine protocol extension)
                uint8_t sessionState;
                if (ctx->connectionContext->HostSessionStateCallback != NULL && parseHostSessionState(
                        (const uint8_t*)(ctlHdr + 1), packetLength - (int)sizeof(*ctlHdr),
                        &sessionState)) {
                    ctx->connectionContext->HostSessionStateCallback(sessionState);
                }
            }
            else if (ctlHdr->type == ctx->packetTypes[IDX_DS5_HAPTICS_IR_V2]) {
                if (ListenerCallbacks.ds5HapticsIrV2 != NULL &&
                    packetLength > (int)sizeof(*ctlHdr)) {
                    // A fixed-size frame is plain IR v2; anything else is the
                    // continuous-state/source-event enhancement.
                    const uint8_t* irPayload = (const uint8_t *)(ctlHdr + 1);
                    const int irLength = packetLength - (int)sizeof(*ctlHdr);
                    if (irLength == DS5_HAPTICS_IR_STREAM_WIRE_SIZE) {
                        processDs5HapticsIrStreamPacket(irPayload, irLength,
                                                        dispatchDs5HapticsIrV2,
                                                        (void *)ListenerCallbacks.ds5HapticsIrV2);
                    }
                    else if (ListenerCallbacks.ds5HapticsIrExtended != NULL) {
                        processDs5HapticsIrExtendedStreamPacket(irPayload, irLength,
                                                                dispatchDs5HapticsIrExtended,
                                                                (void *)ListenerCallbacks.ds5HapticsIrExtended);
                    }
                }
            }
            else if (ctlHdr->type == ctx->packetTypes[IDX_DS5_OUTPUT_EXTENSION]) {
                processDs5OutputExtension(ctx, (const uint8_t *)(ctlHdr + 1),
                                          packetLength - (int)sizeof(*ctlHdr));
            }
            else if (ctlHdr->type == ctx->packetTypes[IDX_REMOTE_TEXT_CONTEXT]) {
                if (packetLength > (int)sizeof(*ctlHdr)) {
                    processRemoteTextContext(ctx, (const uint8_t *)(ctlHdr + 1),
                                             packetLength - (int)sizeof(*ctlHdr));
                }
            }
            // The local cursor channel (0x5509) is not enabled in this client, so
            // those packets are ignored.
            else if (ctlHdr->type == ctx->packetTypes[IDX_DS5_HAPTICS_PCM]) {
                if (ListenerCallbacks.ds5HapticsPcm != NULL &&
                    packetLength > (int)sizeof(*ctlHdr)) {
                    processDs5HapticsStreamPacket(
                        (const uint8_t *)(ctlHdr + 1),
                        packetLength - (int)sizeof(*ctlHdr),
                        dispatchDs5HapticsPcm,
                        (void *)ListenerCallbacks.ds5HapticsPcm);
                }
            }

            free(ctlHdr);
        }
        else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            Limelog("Control stream received unexpected disconnect event\n");
            ListenerCallbacks.connectionTerminated(controlTransportError(ctx, CONTROL_STREAM_GENERIC_ERROR));
            return;
        }
    }
}

static void lossStatsThreadFunc(void* context) {
    PML_CONTROL_STREAM_CONTEXT ctx = (PML_CONTROL_STREAM_CONTEXT)context;
    LiSetThreadConnectionContext(ctx->connectionContext);
    BYTE_BUFFER byteBuffer;

    if (ctx->usePeriodicPing) {
        char periodicPingPayload[8];

        BbInitializeWrappedBuffer(&byteBuffer, periodicPingPayload, 0, sizeof(periodicPingPayload), BYTE_ORDER_LITTLE);
        BbPut32(&byteBuffer, 4); // Length of payload
        BbPut32(&byteBuffer, 0); // Timestamp?

        while (!PltIsThreadInterrupted(&ctx->lossStatsThread)) {
            if (ctx->videoFecControlSupported && PltGetMillis() - ctx->fecLastSummaryMs >= 1000) {
                SS_FEC_SUMMARY summary;
                uint32_t summarySequence, summaryData, summaryMissing;
                PltLockMutex(&ctx->fecSummaryMutex);
                summarySequence = ++ctx->fecSequence;
                summaryData = ctx->fecDataPackets;
                summaryMissing = ctx->fecMissingPackets;
                ctx->fecDataPackets = ctx->fecMissingPackets = 0;
                PltUnlockMutex(&ctx->fecSummaryMutex);
                summary.sequence = BE32(summarySequence);
                summary.dataPackets = BE32(summaryData);
                summary.missingDataPackets = BE32(summaryMissing);
                ctx->fecLastSummaryMs = PltGetMillis();
                if (summaryData) {
                    atomic_store_explicit(&ctx->fecLastSentSequence, summarySequence, memory_order_relaxed);
                    atomic_store_explicit(&ctx->fecLastSentDataPackets, summaryData, memory_order_relaxed);
                    atomic_store_explicit(&ctx->fecLastSentMissingPackets, summaryMissing, memory_order_relaxed);
                }
                if (summary.dataPackets && !sendMessageEnet(ctx, SS_FEC_SUMMARY_PTYPE, sizeof(summary), &summary,
                                                           CTRL_CHANNEL_GENERIC, 0, false)) {
                    ListenerCallbacks.connectionTerminated(LastSocketFail());
                    return;
                }
            }

            // For Sunshine servers, send the more detailed per-frame FEC messages
            if (IS_SUNSHINE_CTX(ctx->connectionContext)) {
                PQUEUED_FRAME_FEC_STATUS queuedFrameStatus;

                // Sunshine should always use ENet for control messages
                LC_ASSERT(ctx->peer != NULL);

                while (LbqPollQueueElement(&ctx->frameFecStatusQueue, (void**)&queuedFrameStatus) == LBQ_SUCCESS) {
                    // Send as an unreliable packet, since it's not a critical message
                    if (!sendMessageEnet(ctx, SS_FRAME_FEC_PTYPE,
                                         sizeof(queuedFrameStatus->fecStatus),
                                         &queuedFrameStatus->fecStatus,
                                         CTRL_CHANNEL_GENERIC,
                                         ENET_PACKET_FLAG_UNSEQUENCED,
                                         LbqGetItemCount(&ctx->frameFecStatusQueue) > 0)) {
                        Limelog("Loss Stats: Sending frame FEC status message failed: %d\n", (int)LastSocketError());
                        ListenerCallbacks.connectionTerminated(LastSocketFail());
                        free(queuedFrameStatus);
                        return;
                    }

                    free(queuedFrameStatus);
                }
            }

            // Send the message (and don't expect a response)
            //
            // NB: We send this periodic message as reliable to ensure the RTT is recomputed
            // regularly. This only happens when an ACK is received to a reliable packet.
            // Since the other traffic on this channel is unsequenced, it doesn't really
            // cause any negative HOL blocking side-effects.
            if (!sendMessageAndForget(ctx, 0x0200,
                                      sizeof(periodicPingPayload),
                                      periodicPingPayload,
                                      CTRL_CHANNEL_GENERIC,
                                      ENET_PACKET_FLAG_RELIABLE,
                                      false)) {
                Limelog("Loss Stats: Transaction failed: %d\n", (int)LastSocketError());
                ListenerCallbacks.connectionTerminated(controlTransportError(ctx, LastSocketFail()));
                return;
            }

            // Wait a bit
            PltSleepMsInterruptible(&ctx->lossStatsThread, PERIODIC_PING_INTERVAL_MS);
        }
    }
    else {
        char* lossStatsPayload;

        // Sunshine should use the newer codepath above
        LC_ASSERT(!IS_SUNSHINE_CTX(ctx->connectionContext));

        lossStatsPayload = malloc(ctx->payloadLengths[IDX_LOSS_STATS]);
        if (lossStatsPayload == NULL) {
            Limelog("Loss Stats: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }

        while (!PltIsThreadInterrupted(&ctx->lossStatsThread)) {
            // Construct the payload
            BbInitializeWrappedBuffer(&byteBuffer, lossStatsPayload, 0, ctx->payloadLengths[IDX_LOSS_STATS], BYTE_ORDER_LITTLE);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, LOSS_REPORT_INTERVAL_MS);
            BbPut32(&byteBuffer, 1000);
            BbPut64(&byteBuffer, ctx->lastGoodFrame);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, 0x14);

            // Send the message (and don't expect a response)
            if (!sendMessageAndForget(ctx, ctx->packetTypes[IDX_LOSS_STATS],
                                      ctx->payloadLengths[IDX_LOSS_STATS],
                                      lossStatsPayload,
                                      CTRL_CHANNEL_GENERIC,
                                      0,
                                      false)) {
                free(lossStatsPayload);
                Limelog("Loss Stats: Transaction failed: %d\n", (int)LastSocketError());
                ListenerCallbacks.connectionTerminated(LastSocketFail());
                return;
            }

            // Wait a bit
            PltSleepMsInterruptible(&ctx->lossStatsThread, LOSS_REPORT_INTERVAL_MS);
        }

        free(lossStatsPayload);
    }
}

static void requestIdrFrame(PML_CONTROL_STREAM_CONTEXT ctx) {
    // If this server does not have a known IDR frame request
    // message, we'll accomplish the same thing by creating a
    // reference frame invalidation request.
    if (!ctx->supportsIdrFrameRequest) {
        int64_t payload[3];

        // Form the payload
        if (ctx->lastSeenFrame < 0x20) {
            payload[0] = 0;
            payload[1] = LE64(ctx->lastSeenFrame);
        }
        else {
            payload[0] = LE64(ctx->lastSeenFrame - 0x20);
            payload[1] = LE64(ctx->lastSeenFrame);
        }

        payload[2] = 0;

        // Send the reference frame invalidation request and read the response
        if (!sendMessageAndDiscardReply(ctx, ctx->packetTypes[IDX_INVALIDATE_REF_FRAMES],
                                        sizeof(payload),
                                        payload,
                                        CTRL_CHANNEL_URGENT,
                                        ENET_PACKET_FLAG_RELIABLE,
                                        false)) {
            Limelog("Request IDR Frame: Transaction failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            return;
        }
    }
    else {
        // Send IDR frame request and read the response
        if (!sendMessageAndDiscardReply(ctx, ctx->packetTypes[IDX_REQUEST_IDR_FRAME],
                                        ctx->payloadLengths[IDX_REQUEST_IDR_FRAME],
                                        ctx->preconstructedPayloads[IDX_REQUEST_IDR_FRAME],
                                        CTRL_CHANNEL_URGENT,
                                        ENET_PACKET_FLAG_RELIABLE,
                                        false)) {
            Limelog("Request IDR Frame: Transaction failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            return;
        }
    }

    Limelog("IDR frame request sent\n");
}

static void requestInvalidateReferenceFrames(PML_CONTROL_STREAM_CONTEXT ctx, uint32_t startFrame, uint32_t endFrame) {
    int64_t payload[3];

    LC_ASSERT(startFrame <= endFrame);
    LC_ASSERT(isReferenceFrameInvalidationEnabled());

    payload[0] = LE64(startFrame);
    payload[1] = LE64(endFrame);
    payload[2] = 0;

    // Send the reference frame invalidation request and read the response
    if (!sendMessageAndDiscardReply(ctx, ctx->packetTypes[IDX_INVALIDATE_REF_FRAMES],
                                    sizeof(payload),
                                    payload, CTRL_CHANNEL_URGENT,
                                    ENET_PACKET_FLAG_RELIABLE,
                                    false)) {
        Limelog("Request Invaldiate Reference Frames: Transaction failed: %d\n", (int)LastSocketError());
        ListenerCallbacks.connectionTerminated(LastSocketFail());
        return;
    }

    Limelog("Invalidate reference frame request sent (%d to %d)\n", startFrame, endFrame);
}

static void confirmLongtermReferenceFrame(PML_CONTROL_STREAM_CONTEXT ctx, uint32_t frameIndex) {
    LC_ASSERT(isReferenceFrameInvalidationEnabled());

    SS_LTR_FRAME_ACK payload = {
        .frameIndex = LE32(frameIndex),
    };

    // Send LTR frame ACK and don't wait for response
    if (!sendMessageAndForget(ctx, SS_LTR_FRAME_ACK_PTYPE,
                              sizeof(payload),
                              &payload,
                              CTRL_CHANNEL_URGENT,
                              ENET_PACKET_FLAG_RELIABLE,
                              false)) {
        Limelog("LTR frame ACK: Transaction failed: %d\n", (int)LastSocketError());
        ListenerCallbacks.connectionTerminated(LastSocketFail());
        return;
    }
}

static void referenceFrameControlFunc(void* context) {
    PML_CONTROL_STREAM_CONTEXT ctx = (PML_CONTROL_STREAM_CONTEXT)context;
    LiSetThreadConnectionContext(ctx->connectionContext);
    LC_ASSERT(isReferenceFrameInvalidationEnabled());

    while (!PltIsThreadInterrupted(&ctx->invalidateRefFramesThread)) {
        PQUEUED_REFERENCE_FRAME_CONTROL qfit;
        uint32_t invalidateStartFrame = 0;
        uint32_t invalidateEndFrame = 0;
        bool invalidate = false;

        // Wait for a reference frame control message or a request to shutdown
        if (LbqWaitForQueueElement(&ctx->invalidReferenceFrameTuples, (void**)&qfit) != LBQ_SUCCESS) {
            // Bail if we're stopping
            return;
        }

        do {
            if (qfit->invalidate) {
                if (!invalidate) {
                    invalidateStartFrame = qfit->startFrame;
                    invalidateEndFrame = qfit->endFrame;
                    invalidate = true;
                }
                else {
                    // Aggregate all lost frames into one range
                    LC_ASSERT(qfit->endFrame >= invalidateEndFrame);
                    invalidateEndFrame = qfit->endFrame;
                }
            }
            else {
                // Send LTR frame ACK
                confirmLongtermReferenceFrame(ctx, qfit->startFrame);
            }
            free(qfit);
        } while (LbqPollQueueElement(&ctx->invalidReferenceFrameTuples, (void**)&qfit) == LBQ_SUCCESS);

        if (invalidate) {
            // Send the reference frame invalidation request
            requestInvalidateReferenceFrames(ctx, invalidateStartFrame, invalidateEndFrame);
        }
    }
}

static void requestIdrFrameFunc(void* context) {
    PML_CONTROL_STREAM_CONTEXT ctx = (PML_CONTROL_STREAM_CONTEXT)context;
    LiSetThreadConnectionContext(ctx->connectionContext);
    while (!PltIsThreadInterrupted(&ctx->requestIdrFrameThread)) {
        PltWaitForEvent(&ctx->idrFrameRequiredEvent);
        PltClearEvent(&ctx->idrFrameRequiredEvent);

        if (ctx->stopping) {
            // Bail if we're stopping
            return;
        }

        // Any pending RFI requests and LTR frame ACK messages are now redundant
        freeBasicLbqList(LbqFlushQueueItems(&ctx->invalidReferenceFrameTuples));

        // Request the IDR frame
        requestIdrFrame(ctx);
    }
}

// Stops the control stream
int stopControlStreamCtx(PML_CONTROL_STREAM_CONTEXT ctx) {
    ctx->stopping = true;
    if (tls_CurrentControlStreamCtx == ctx) {
        tls_CurrentControlStreamCtx = NULL;
    }
    LbqSignalQueueShutdown(&ctx->invalidReferenceFrameTuples);
    LbqSignalQueueShutdown(&ctx->frameFecStatusQueue);
    LbqSignalQueueDrain(&ctx->asyncCallbackQueue);
    PltSetEvent(&ctx->idrFrameRequiredEvent);

    // This must be set to stop in a timely manner
    LC_ASSERT(ConnectionInterrupted);

    if (ctx->ctlSock != INVALID_SOCKET) {
        shutdownTcpSocket(ctx->ctlSock);
    }

    PltInterruptThread(&ctx->lossStatsThread);
    PltInterruptThread(&ctx->requestIdrFrameThread);
    PltInterruptThread(&ctx->controlReceiveThread);
    PltInterruptThread(&ctx->asyncCallbackThread);

    PltJoinThread(&ctx->lossStatsThread);
    PltJoinThread(&ctx->requestIdrFrameThread);
    PltJoinThread(&ctx->controlReceiveThread);
    PltJoinThread(&ctx->asyncCallbackThread);

    // We will only have an RFI thread if RFI is enabled
    if (isReferenceFrameInvalidationEnabled()) {
        PltInterruptThread(&ctx->invalidateRefFramesThread);
        PltJoinThread(&ctx->invalidateRefFramesThread);
    }

    if (ctx->peer != NULL) {
        // Gracefully disconnect to ensure the remote host receives all of our final
        // outbound traffic, including any key up events that might be sent.
        gracefullyDisconnectEnetPeer(ctx->client, ctx->peer, CONTROL_STREAM_LINGER_TIMEOUT_SEC * 1000);
        ctx->peer = NULL;
    }
    if (ctx->client != NULL) {
        enet_host_destroy(ctx->client);
        ctx->client = NULL;
    }

    if (ctx->ctlSock != INVALID_SOCKET) {
        closeSocket(ctx->ctlSock);
        ctx->ctlSock = INVALID_SOCKET;
    }

    return 0;
}

int stopControlStream(void) {
    return stopControlStreamCtx(LiGetEffectiveControlContext());
}

// Called by the input stream to send a packet for Gen 5+ servers
int sendInputPacketOnControlStreamCtx(PML_CONTROL_STREAM_CONTEXT ctx, unsigned char* data, int length, uint8_t channelId, uint32_t flags, bool moreData) {
    LC_ASSERT(ControlStreamAppVersionQuad[0] >= 5);

    // Send the input data (no reply expected)
    if (sendMessageAndForget(ctx, ctx->packetTypes[IDX_INPUT_DATA], length, data, channelId, flags, moreData) == 0) {
        return -1;
    }

    return 0;
}

int sendInputPacketOnControlStream(unsigned char* data, int length, uint8_t channelId, uint32_t flags, bool moreData) {
    return sendInputPacketOnControlStreamCtx(LiGetEffectiveControlContext(), data, length, channelId, flags, moreData);
}

// Called by the input stream to flush queued packets before a batching wait
void flushInputOnControlStreamCtx(PML_CONTROL_STREAM_CONTEXT ctx) {
    if (ControlStreamAppVersionQuad[0] >= 5) {
        PltLockMutex(&ctx->enetMutex);
        enet_host_flush(ctx->client);
        PltUnlockMutex(&ctx->enetMutex);
    }
}

void flushInputOnControlStream(void) {
    flushInputOnControlStreamCtx(LiGetEffectiveControlContext());
}

bool isControlDataInTransitCtx(PML_CONTROL_STREAM_CONTEXT ctx) {
    bool ret = false;

    PltLockMutex(&ctx->enetMutex);
    if (ctx->peer != NULL && ctx->peer->state == ENET_PEER_STATE_CONNECTED) {
        if (ctx->peer->reliableDataInTransit != 0) {
            ret = true;
        }
    }
    PltUnlockMutex(&ctx->enetMutex);

    return ret;
}

bool isControlDataInTransit(void) {
    return isControlDataInTransitCtx(LiGetEffectiveControlContext());
}

bool LiGetEstimatedRttInfoCtx(PML_CONTROL_STREAM_CONTEXT ctx, uint32_t* estimatedRtt, uint32_t* estimatedRttVariance) {
    bool ret = false;

    PltLockMutex(&ctx->enetMutex);
    if (ctx->peer != NULL && ctx->peer->state == ENET_PEER_STATE_CONNECTED) {
        if (estimatedRtt != NULL) {
            *estimatedRtt = ctx->peer->roundTripTime;
        }

        if (estimatedRttVariance != NULL) {
            *estimatedRttVariance = ctx->peer->roundTripTimeVariance;
        }

        ret = true;
    }
    PltUnlockMutex(&ctx->enetMutex);

    return ret;
}

bool LiGetEstimatedRttInfo(uint32_t* estimatedRtt, uint32_t* estimatedRttVariance) {
    return LiGetEstimatedRttInfoCtx(LiGetEffectiveControlContext(), estimatedRtt, estimatedRttVariance);
}

static int sendSimpleClipboardCommand(PML_CONTROL_STREAM_CONTEXT ctx,
                                      uint8_t command) {
    uint8_t payload = command;
    return sendClipboardControlPacket(ctx, &payload, sizeof(payload), false);
}

int LiBindClipboardSessionCtx(PML_CONTROL_STREAM_CONTEXT ctx) {
    return sendSimpleClipboardCommand(ctx, LI_CLIPBOARD_MSG_BIND);
}

int LiBindClipboardSession(void) {
    return LiBindClipboardSessionCtx(LiGetEffectiveControlContext());
}

int LiUnbindClipboardSessionCtx(PML_CONTROL_STREAM_CONTEXT ctx) {
    return sendSimpleClipboardCommand(ctx, LI_CLIPBOARD_MSG_UNBIND);
}

int LiUnbindClipboardSession(void) {
    return LiUnbindClipboardSessionCtx(LiGetEffectiveControlContext());
}

int LiRequestClipboardSnapshotCtx(PML_CONTROL_STREAM_CONTEXT ctx) {
    return sendSimpleClipboardCommand(ctx, LI_CLIPBOARD_MSG_SNAPSHOT_REQUEST);
}

int LiRequestClipboardSnapshot(void) {
    return LiRequestClipboardSnapshotCtx(LiGetEffectiveControlContext());
}

int LiSendClipboardItemCtx(PML_CONTROL_STREAM_CONTEXT ctx,
                           const LI_CLIPBOARD_ITEM* item) {
    const char* mimeType;
    const char* name;
    uint16_t mimeTypeLength;
    uint16_t nameLength;
    uint64_t itemId;
    uint64_t contentHash;
    size_t startPayloadLength;
    char* startPayload;
    BYTE_BUFFER bb;
    uint32_t offset;
    int err;

    if (item == NULL) {
        return -1;
    }

    if (!isClipboardExtensionSupported(ctx)) {
        return LI_ERR_UNSUPPORTED;
    }

    if (!isClipboardItemTypeSupportedByHost(ctx, item->type)) {
        return LI_ERR_UNSUPPORTED;
    }

    if (item->length != 0 && item->data == NULL) {
        return -1;
    }

    mimeType = item->mimeType;
    if (mimeType == NULL) {
        switch (item->type) {
        case LI_CLIPBOARD_ITEM_TYPE_TEXT:
            mimeType = "text/plain;charset=utf-8";
            break;
        case LI_CLIPBOARD_ITEM_TYPE_IMAGE:
            mimeType = "image/png";
            break;
        default:
            mimeType = "";
            break;
        }
    }

    name = item->name != NULL ? item->name : "";
    mimeTypeLength = (uint16_t)strlen(mimeType);
    nameLength = (uint16_t)strlen(name);
    if (mimeTypeLength != strlen(mimeType) || nameLength != strlen(name)) {
        return -1;
    }

    itemId = item->itemId != 0 ? item->itemId : generateClipboardItemId();
    contentHash = item->contentHash;

    startPayloadLength = 1 + 1 + 1 + 1 + sizeof(uint64_t) + sizeof(uint64_t) +
                         sizeof(uint32_t) + sizeof(uint16_t) +
                         sizeof(uint16_t) + mimeTypeLength + nameLength;
    startPayload = malloc(startPayloadLength);
    if (startPayload == NULL) {
        return -1;
    }

    BbInitializeWrappedBuffer(&bb, startPayload, 0, (int)startPayloadLength,
                              BYTE_ORDER_LITTLE);
    BbPut8(&bb, LI_CLIPBOARD_MSG_ITEM_START);
    BbPut8(&bb, 0);
    BbPut8(&bb, item->type);
    BbPut8(&bb, 0);
    BbPut64(&bb, itemId);
    BbPut64(&bb, contentHash);
    BbPut32(&bb, item->length);
    BbPut16(&bb, mimeTypeLength);
    BbPut16(&bb, nameLength);
    BbPutBytes(&bb, (const uint8_t*)mimeType, mimeTypeLength);
    BbPutBytes(&bb, (const uint8_t*)name, nameLength);

    err = sendClipboardControlPacket(ctx, startPayload, (int)startPayloadLength,
                                     true);
    free(startPayload);
    if (err != 0) {
        return err;
    }

    for (offset = 0; offset < item->length; offset += LI_CLIPBOARD_MAX_CHUNK_SIZE) {
        uint16_t chunkLength =
            (uint16_t)MIN((uint32_t)LI_CLIPBOARD_MAX_CHUNK_SIZE,
                          item->length - offset);
        size_t chunkPayloadLength =
            1 + 1 + sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint32_t) +
            chunkLength;
        char* chunkPayload = malloc(chunkPayloadLength);
        if (chunkPayload == NULL) {
            sendClipboardCancelBestEffort(ctx, itemId);
            return -1;
        }

        BbInitializeWrappedBuffer(&bb, chunkPayload, 0, (int)chunkPayloadLength,
                                  BYTE_ORDER_LITTLE);
        BbPut8(&bb, LI_CLIPBOARD_MSG_ITEM_CHUNK);
        BbPut8(&bb, 0);
        BbPut16(&bb, chunkLength);
        BbPut64(&bb, itemId);
        BbPut32(&bb, offset);
        BbPutBytes(&bb, item->data + offset, chunkLength);

        err = sendClipboardControlPacket(ctx, chunkPayload,
                                         (int)chunkPayloadLength, true);
        free(chunkPayload);
        if (err != 0) {
            sendClipboardCancelBestEffort(ctx, itemId);
            return err;
        }
    }

    {
        char endPayload[1 + sizeof(uint64_t)];
        BbInitializeWrappedBuffer(&bb, endPayload, 0, sizeof(endPayload),
                                  BYTE_ORDER_LITTLE);
        BbPut8(&bb, LI_CLIPBOARD_MSG_ITEM_END);
        BbPut64(&bb, itemId);
        return sendClipboardControlPacket(ctx, endPayload, sizeof(endPayload),
                                          false);
    }
}

int LiSendClipboardItem(const LI_CLIPBOARD_ITEM* item) {
    return LiSendClipboardItemCtx(LiGetEffectiveControlContext(), item);
}

// Sends a raw Sunshine clipboard v1 payload over control packet 0x5508.
// common-c deliberately does not parse this higher-level payload.
int LiSendClipboardDataCtx(PML_CONTROL_STREAM_CONTEXT ctx,
                           const void* payload, int length) {
    // The 16-bit encrypted length includes the sequence, GCM tag and inner header.
    if (payload == NULL || length <= 0 || getEncryptedControlPacketLength(length) == 0) {
        return -1;
    }

    if (!isClipboardExtensionSupported(ctx)) {
        return -2;
    }

    return sendClipboardControlPacket(ctx, payload, length, false);
}

int LiSendClipboardData(const void* payload, int length) {
    return LiSendClipboardDataCtx(LiGetEffectiveControlContext(), payload, length);
}

// 0x5516, client to host. Sent only to a host that declared input-status=1.
int LiSendDs5InputStatusCtx(PML_CONTROL_STREAM_CONTEXT ctx, uint16_t controllerNumber, uint32_t validMask,
                            uint8_t headsetPresent, uint8_t microphonePresent, uint8_t microphoneMuted) {
    DS5EXT_INPUT_STATUS status;
    uint8_t body[DS5EXT_INPUT_STATUS_BODY_BYTES];

    if (ctx == NULL || ctx->connectionContext == NULL || ctx->packetTypes == NULL ||
            !IS_SUNSHINE_CTX(ctx->connectionContext) ||
            (ctx->ds5ExtensionMask & DS5EXT_MASK_INPUT_STATUS_V1) == 0 ||
            ctx->packetTypes[IDX_DS5_INPUT_STATUS] == -1 ||
            ctx->peer == NULL || ctx->peer->state != ENET_PEER_STATE_CONNECTED) {
        return LI_ERR_UNSUPPORTED;
    }
    status.controllerNumber = controllerNumber;
    status.validMask = validMask;
    status.headsetPresent = headsetPresent;
    status.microphonePresent = microphonePresent;
    status.microphoneMuted = microphoneMuted;
    if (ds5ExtEncodeInputStatus(&status, body, sizeof(body)) == 0) {
        return -1;
    }
    if (!sendMessageAndForget(ctx, ctx->packetTypes[IDX_DS5_INPUT_STATUS], (int)sizeof(body), body,
                              CTRL_CHANNEL_GENERIC, ENET_PACKET_FLAG_RELIABLE, false)) {
        return LastSocketFail();
    }
    return 0;
}

int LiSendDs5InputStatus(uint16_t controllerNumber, uint32_t validMask, uint8_t headsetPresent,
                         uint8_t microphonePresent, uint8_t microphoneMuted) {
    return LiSendDs5InputStatusCtx(LiGetEffectiveControlContext(), controllerNumber, validMask,
                                   headsetPresent, microphonePresent, microphoneMuted);
}

uint32_t LiGetHostDs5ExtensionsCtx(PML_CONNECTION_CONTEXT ctx) {
    return ctx != NULL ? ctx->controlContext.ds5ExtensionMask : 0;
}

uint32_t LiGetHostDs5Extensions(void) {
    return LiGetHostDs5ExtensionsCtx(LiGetEffectiveConnectionContext());
}

// Shared gate for the Sunshine dynamic parameter control message (0x5506).
// Returns LI_DYNAMIC_SDR_WHITE_OK (0) when a message can be sent right now.
static int checkDynamicParamSendable(PML_CONTROL_STREAM_CONTEXT ctx) {
    if (ctx == NULL || ctx->connectionContext == NULL || ctx->packetTypes == NULL ||
            !IS_SUNSHINE_CTX(ctx->connectionContext) || !ctx->encryptedControlStream ||
            ControlStreamAppVersionQuad[0] < 5 ||
            ctx->packetTypes[IDX_DYNAMIC_PARAM_CHANGE] == -1) {
        return LI_DYNAMIC_SDR_WHITE_ERR_UNSUPPORTED;
    }
    if (ctx->peer == NULL || ctx->peer->state != ENET_PEER_STATE_CONNECTED) {
        return LI_DYNAMIC_SDR_WHITE_ERR_NOT_CONNECTED;
    }
    return LI_DYNAMIC_SDR_WHITE_OK;
}

static int sendDynamicParamPayload(PML_CONTROL_STREAM_CONTEXT ctx, const uint32_t* words, int wordCount) {
    uint32_t payload[3];
    int i;

    LC_ASSERT(wordCount > 0 && wordCount <= (int)(sizeof(payload) / sizeof(payload[0])));
    for (i = 0; i < wordCount; i++) {
        payload[i] = LE32(words[i]);
    }

    // Reliable, tiny (at most 12 bytes), far below the signed 16-bit payload limit
    if (!sendMessageAndForget(ctx, ctx->packetTypes[IDX_DYNAMIC_PARAM_CHANGE], wordCount * (int)sizeof(uint32_t),
                              payload, CTRL_CHANNEL_GENERIC, ENET_PACKET_FLAG_RELIABLE, false)) {
        return LI_DYNAMIC_SDR_WHITE_ERR_SEND_FAILED;
    }
    return LI_DYNAMIC_SDR_WHITE_OK;
}

int LiSendClientSdrWhiteNitsCtx(PML_CONTROL_STREAM_CONTEXT ctx, float nits) {
    uint32_t words[2];
    uint32_t nitsBits;
    int err;

    if (!isfinite(nits) || nits < 50.0f || nits > 1000.0f) {
        return LI_DYNAMIC_SDR_WHITE_ERR_INVALID;
    }
    if (ctx != NULL && ctx->connectionContext != NULL &&
            (SunshineFeatureFlags & LI_FF_DYNAMIC_SDR_WHITE) == 0) {
        return LI_DYNAMIC_SDR_WHITE_ERR_UNSUPPORTED;
    }
    err = checkDynamicParamSendable(ctx);
    if (err != LI_DYNAMIC_SDR_WHITE_OK) {
        return err;
    }

    memcpy(&nitsBits, &nits, sizeof(nitsBits));
    words[0] = SS_DYNAMIC_PARAM_TYPE_CLIENT_SDR_WHITE_NITS;
    words[1] = nitsBits;
    return sendDynamicParamPayload(ctx, words, 2);
}

int LiSendClientSdrWhiteNits(float nits) {
    return LiSendClientSdrWhiteNitsCtx(LiGetEffectiveControlContext(), nits);
}

int LiSendStreamBitrateCtx(PML_CONTROL_STREAM_CONTEXT ctx, int bitrateKbps) {
    uint32_t words[2];
    int err;

    if (bitrateKbps <= 0 || bitrateKbps > LI_STREAM_BITRATE_MAX_KBPS) {
        return LI_DYNAMIC_SDR_WHITE_ERR_INVALID;
    }
    err = checkDynamicParamSendable(ctx);
    if (err != LI_DYNAMIC_SDR_WHITE_OK) {
        return err;
    }

    words[0] = SS_DYNAMIC_PARAM_TYPE_BITRATE;
    words[1] = (uint32_t)bitrateKbps;
    return sendDynamicParamPayload(ctx, words, 2);
}

int LiSendStreamFramerateCtx(PML_CONTROL_STREAM_CONTEXT ctx, float fps) {
    uint32_t words[2];
    uint32_t fpsBits;
    int err;

    if (!isfinite(fps) || fps <= 0.0f || fps > 1000.0f) {
        return LI_DYNAMIC_SDR_WHITE_ERR_INVALID;
    }
    err = checkDynamicParamSendable(ctx);
    if (err != LI_DYNAMIC_SDR_WHITE_OK) {
        return err;
    }

    memcpy(&fpsBits, &fps, sizeof(fpsBits));
    words[0] = SS_DYNAMIC_PARAM_TYPE_FPS;
    words[1] = fpsBits;
    return sendDynamicParamPayload(ctx, words, 2);
}

// Foundation Sunshine live resolution request (qiin/mic #48). Zero means sent,
// not applied. -1 invalid dimensions, -2 unsupported host, -3 disconnected,
// -4 send failed.
int LiRequestResolutionChangeCtx(PML_CONTROL_STREAM_CONTEXT ctx, int width, int height) {
    uint32_t words[3];

    if (width <= 0 || height <= 0 || width > LI_STREAM_RESOLUTION_MAX || height > LI_STREAM_RESOLUTION_MAX ||
            (width & 1) || (height & 1)) {
        return -1;
    }
    if (ctx == NULL || ctx->connectionContext == NULL || ctx->packetTypes == NULL ||
            !IS_SUNSHINE_CTX(ctx->connectionContext) || !ctx->encryptedControlStream ||
            ControlStreamAppVersionQuad[0] < 5 ||
            ctx->packetTypes[IDX_DYNAMIC_PARAM_CHANGE] == -1) {
        return -2;
    }
    if (ctx->stopping || ctx->peer == NULL || ctx->peer->state != ENET_PEER_STATE_CONNECTED) {
        return -3;
    }

    words[0] = SS_DYNAMIC_PARAM_TYPE_RESOLUTION;
    words[1] = (uint32_t)width;
    words[2] = (uint32_t)height;
    return sendDynamicParamPayload(ctx, words, 3) == LI_DYNAMIC_SDR_WHITE_OK ? 0 : -4;
}

int LiSendStreamBitrate(int bitrateKbps) {
    return LiSendStreamBitrateCtx(LiGetEffectiveControlContext(), bitrateKbps);
}

int LiSendStreamFramerate(float fps) {
    return LiSendStreamFramerateCtx(LiGetEffectiveControlContext(), fps);
}

int LiRequestResolutionChange(int width, int height) {
    return LiRequestResolutionChangeCtx(LiGetEffectiveControlContext(), width, height);
}

// The local cursor channel (0x5509) is not enabled in this client. The packet
// layout stays in CursorStream.[ch] for reference, but the mode is never sent.
int LiSetCursorMode(int cursorMode) {
    if (cursorMode != LI_CURSOR_MODE_VIDEO && cursorMode != LI_CURSOR_MODE_LOCAL) {
        return LI_CURSOR_MODE_ERR_INVALID;
    }

    return LI_CURSOR_MODE_ERR_UNSUPPORTED;
}

// Starts the control stream
int startControlStreamCtx(PML_CONTROL_STREAM_CONTEXT ctx) {
    int err;

    // Set the TLS context for the ENet intercept callback
    tls_CurrentControlStreamCtx = ctx;

    if (ControlStreamAppVersionQuad[0] >= 5) {
        ENetAddress remoteAddress, localAddress;
        ENetEvent event;

        LC_ASSERT(ControlPortNumber != 0);

        enet_address_set_address(&localAddress, (struct sockaddr *)&LocalAddr, AddrLen);
#ifdef __3DS__
        // binding to wildcard port is broken on the 3DS, so we need to define a port manually
        enet_address_set_port(&localAddress, htons(n3ds_udp_port++));
#else
        enet_address_set_port(&localAddress, 0); // Wildcard port
#endif

        enet_address_set_address(&remoteAddress, (struct sockaddr *)&RemoteAddr, AddrLen);
        enet_address_set_port(&remoteAddress, ControlPortNumber);

        // Create a client
        ctx->client = enet_host_create(RemoteAddr.ss_family,
                                  LocalAddr.ss_family != 0 ? &localAddress : NULL,
                                  1, CTRL_CHANNEL_COUNT, 0, 0);
        if (ctx->client == NULL) {
            ctx->stopping = true;
            return -1;
        }

        // The chosen interface must be set before the first datagram leaves
        if (bindEnetSocketToInterface((SOCKET)ctx->client->socket, RemoteAddr.ss_family) != 0) {
            ctx->stopping = true;
            enet_host_destroy(ctx->client);
            ctx->client = NULL;
            return -1;
        }

        ctx->client->intercept = ignoreDisconnectIntercept;

        // Enable high priority QoS marking on control stream traffic
        //
        // NB: It is important to do this before connecting because there's logic in the connect
        // retransmission code to detect QoS-intolerant routes and disable QoS marking for those.
        enet_socket_set_option (ctx->client->socket, ENET_SOCKOPT_QOS, 1);

        // Connect to the host
        ctx->peer = enet_host_connect(ctx->client, &remoteAddress, CTRL_CHANNEL_COUNT, ControlConnectData);
        if (ctx->peer == NULL) {
            ctx->stopping = true;
            enet_host_destroy(ctx->client);
            ctx->client = NULL;
            return -1;
        }

        // Wait for the connect to complete
        err = serviceEnetHost(ctx->client, &event, CONTROL_STREAM_TIMEOUT_SEC * 1000);
        if (err <= 0 || event.type != ENET_EVENT_TYPE_CONNECT) {
            if (err < 0) {
                Limelog("Failed to establish ENet connection on UDP port %u: error %d\n", ControlPortNumber, LastSocketFail());
            }
            else if (err == 0) {
                Limelog("Failed to establish ENet connection on UDP port %u: timed out\n", ControlPortNumber);
            }
            else {
                Limelog("Failed to establish ENet connection on UDP port %u: unexpected event %d (error: %d)\n", ControlPortNumber, (int)event.type, LastSocketError());
            }

            ctx->stopping = true;
            enet_peer_reset(ctx->peer);
            ctx->peer = NULL;
            enet_host_destroy(ctx->client);
            ctx->client = NULL;

            if (err == 0) {
                return ETIMEDOUT;
            }
            else if (err > 0 && event.type != ENET_EVENT_TYPE_CONNECT && LastSocketError() == 0) {
                // If we got an unexpected event type and have no other error to return, return the event type
                LC_ASSERT(event.type != ENET_EVENT_TYPE_NONE);
                return event.type != ENET_EVENT_TYPE_NONE ? (int)event.type : LastSocketFail();
            }
            else {
                return LastSocketFail();
            }
        }

        // Ensure the connect verify ACK is sent immediately
        enet_host_flush(ctx->client);

        LiRecordStreamSocketCtx(ctx->connectionContext, (SOCKET)ctx->client->socket, STREAM_SOCKET_SLOT_CONTROL);

#ifdef __3DS__
        // Set the peer timeout to 1 minute and limit backoff to 2x RTT
        // The 3DS can take a bit longer to set up when starting fresh
        enet_peer_timeout(ctx->peer, 2, 60000, 60000);
#else
        // Set the peer timeout to 10 seconds and limit backoff to 2x RTT
        enet_peer_timeout(ctx->peer, 2, 10000, 10000);
#endif
    }
    else {
        // NB: Do NOT use ControlPortNumber here. 47995 is correct for these old versions.
        LC_ASSERT(ControlPortNumber == 0);
        ctx->ctlSock = connectTcpSocket(&RemoteAddr, AddrLen,
            47995, CONTROL_STREAM_TIMEOUT_SEC);
        if (ctx->ctlSock == INVALID_SOCKET) {
            ctx->stopping = true;
            return LastSocketFail();
        }

        enableNoDelay(ctx->ctlSock);

        LiRecordStreamSocketCtx(ctx->connectionContext, ctx->ctlSock, STREAM_SOCKET_SLOT_CONTROL);
    }

    err = PltCreateThread("ControlRecv", controlReceiveThreadFunc, ctx, &ctx->controlReceiveThread);
    if (err != 0) {
        ctx->stopping = true;
        if (ctx->ctlSock != INVALID_SOCKET) {
            closeSocket(ctx->ctlSock);
            ctx->ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(ctx->peer, 0);
            ctx->peer = NULL;
            enet_host_destroy(ctx->client);
            ctx->client = NULL;
        }
        return err;
    }

    // Send START A
    if (!sendMessageAndDiscardReply(ctx, ctx->packetTypes[IDX_START_A],
                                    ctx->payloadLengths[IDX_START_A],
                                    ctx->preconstructedPayloads[IDX_START_A],
                                    CTRL_CHANNEL_GENERIC,
                                    ENET_PACKET_FLAG_RELIABLE,
                                    false)) {
        Limelog("Start A failed: %d\n", (int)LastSocketError());
        err = LastSocketFail();
        ctx->stopping = true;

        if (ctx->ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctx->ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&ctx->controlReceiveThread);
        PltJoinThread(&ctx->controlReceiveThread);

        if (ctx->ctlSock != INVALID_SOCKET) {
            closeSocket(ctx->ctlSock);
            ctx->ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(ctx->peer, 0);
            ctx->peer = NULL;
            enet_host_destroy(ctx->client);
            ctx->client = NULL;
        }
        return err;
    }

    // Send START B
    if (!sendMessageAndDiscardReply(ctx, ctx->packetTypes[IDX_START_B],
                                    ctx->payloadLengths[IDX_START_B],
                                    ctx->preconstructedPayloads[IDX_START_B],
                                    CTRL_CHANNEL_GENERIC,
                                    ENET_PACKET_FLAG_RELIABLE,
                                    false)) {
        Limelog("Start B failed: %d\n", (int)LastSocketError());
        err = LastSocketFail();
        ctx->stopping = true;

        if (ctx->ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctx->ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&ctx->controlReceiveThread);
        PltJoinThread(&ctx->controlReceiveThread);

        if (ctx->ctlSock != INVALID_SOCKET) {
            closeSocket(ctx->ctlSock);
            ctx->ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(ctx->peer, 0);
            ctx->peer = NULL;
            enet_host_destroy(ctx->client);
            ctx->client = NULL;
        }
        return err;
    }

    err = PltCreateThread("LossStats", lossStatsThreadFunc, ctx, &ctx->lossStatsThread);
    if (err != 0) {
        ctx->stopping = true;

        if (ctx->ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctx->ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&ctx->controlReceiveThread);
        PltJoinThread(&ctx->controlReceiveThread);

        if (ctx->ctlSock != INVALID_SOCKET) {
            closeSocket(ctx->ctlSock);
            ctx->ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(ctx->peer, 0);
            ctx->peer = NULL;
            enet_host_destroy(ctx->client);
            ctx->client = NULL;
        }
        return err;
    }

    err = PltCreateThread("ReqIdrFrame", requestIdrFrameFunc, ctx, &ctx->requestIdrFrameThread);
    if (err != 0) {
        ctx->stopping = true;

        if (ctx->ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctx->ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&ctx->lossStatsThread);
        PltJoinThread(&ctx->lossStatsThread);

        PltInterruptThread(&ctx->controlReceiveThread);
        PltJoinThread(&ctx->controlReceiveThread);

        if (ctx->ctlSock != INVALID_SOCKET) {
            closeSocket(ctx->ctlSock);
            ctx->ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(ctx->peer, 0);
            ctx->peer = NULL;
            enet_host_destroy(ctx->client);
            ctx->client = NULL;
        }

        return err;
    }

    err = PltCreateThread("CtrlAsyncCb", asyncCallbackThreadFunc, ctx, &ctx->asyncCallbackThread);
    if (err != 0) {
        ctx->stopping = true;
        PltSetEvent(&ctx->idrFrameRequiredEvent);

        if (ctx->ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctx->ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&ctx->lossStatsThread);
        PltJoinThread(&ctx->lossStatsThread);

        PltInterruptThread(&ctx->controlReceiveThread);
        PltJoinThread(&ctx->controlReceiveThread);

        PltInterruptThread(&ctx->requestIdrFrameThread);
        PltJoinThread(&ctx->requestIdrFrameThread);

        if (ctx->ctlSock != INVALID_SOCKET) {
            closeSocket(ctx->ctlSock);
            ctx->ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(ctx->peer, 0);
            ctx->peer = NULL;
            enet_host_destroy(ctx->client);
            ctx->client = NULL;
        }

        return err;
    }

    // Only create the reference frame invalidation thread if RFI is enabled
    if (isReferenceFrameInvalidationEnabled()) {
        err = PltCreateThread("InvRefFrames", referenceFrameControlFunc, ctx, &ctx->invalidateRefFramesThread);
        if (err != 0) {
            ctx->stopping = true;
            PltSetEvent(&ctx->idrFrameRequiredEvent);
            LbqSignalQueueShutdown(&ctx->asyncCallbackQueue);

            if (ctx->ctlSock != INVALID_SOCKET) {
                shutdownTcpSocket(ctx->ctlSock);
            }
            else {
                ConnectionInterrupted = true;
            }

            PltInterruptThread(&ctx->lossStatsThread);
            PltJoinThread(&ctx->lossStatsThread);

            PltInterruptThread(&ctx->controlReceiveThread);
            PltJoinThread(&ctx->controlReceiveThread);

            PltInterruptThread(&ctx->requestIdrFrameThread);
            PltJoinThread(&ctx->requestIdrFrameThread);

            PltInterruptThread(&ctx->asyncCallbackThread);
            PltJoinThread(&ctx->asyncCallbackThread);

            if (ctx->ctlSock != INVALID_SOCKET) {
                closeSocket(ctx->ctlSock);
                ctx->ctlSock = INVALID_SOCKET;
            }
            else {
                enet_peer_disconnect_now(ctx->peer, 0);
                ctx->peer = NULL;
                enet_host_destroy(ctx->client);
                ctx->client = NULL;
            }

            return err;
        }
    }

    return 0;
}

int startControlStream(void) {
    return startControlStreamCtx(LiGetEffectiveControlContext());
}

bool LiGetCurrentHostDisplayHdrModeCtx(PML_CONTROL_STREAM_CONTEXT ctx) {
    return ctx != NULL && ctx->hdrEnabled;
}

bool LiGetCurrentHostDisplayHdrMode(void) {
    return LiGetCurrentHostDisplayHdrModeCtx(LiGetEffectiveControlContext());
}

bool LiGetHdrMetadataCtx(PML_CONTROL_STREAM_CONTEXT ctx, PSS_HDR_METADATA metadata) {
    if (ctx == NULL || metadata == NULL || ctx->connectionContext == NULL) {
        return false;
    }

    if (!IS_SUNSHINE_CTX(ctx->connectionContext) || !ctx->hdrEnabled) {
        return false;
    }

    *metadata = ctx->hdrMetadata;
    return true;
}

bool LiGetHdrMetadata(PSS_HDR_METADATA metadata) {
    return LiGetHdrMetadataCtx(LiGetEffectiveControlContext(), metadata);
}
