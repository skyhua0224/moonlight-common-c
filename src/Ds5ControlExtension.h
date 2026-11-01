#pragma once

// DualSense control extensions v1.
//
//   0x5515  host to client  output extension (16-byte body): player and mute
//           LEDs, speaker / headset / microphone volume, microphone mute,
//           audio routing, lightbar brightness
//   0x5516  client to host  input status (12-byte body): headset, microphone
//           presence and microphone mute
//
// Both are negotiated through a versioned SDP namespace and never through a
// controller capability bit:
//   client offer   a=x-ml-general.ds5Extensions:output=1,input-status=1
//   host answer    a=x-ss-general.ds5Extensions:output=1,input-status=1
// "output=1" means the peer accepts 0x5515, "input-status=1" means the peer
// accepts 0x5516. A peer sends only the direction the other side declared.
//
// The bodies are little-endian. The framing header (type, length) is added by
// the control stream. A body that is not exactly the documented form is
// rejected whole: wrong length, version other than 1, reserved bytes not zero,
// unknown valid bits, a field outside its range, or a field that is not set in
// the valid mask but is not zero.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DS5EXT_PTYPE_OUTPUT 0x5515
#define DS5EXT_PTYPE_INPUT_STATUS 0x5516

#define DS5EXT_VERSION 1
#define DS5EXT_OUTPUT_BODY_BYTES 16
#define DS5EXT_INPUT_STATUS_BODY_BYTES 12

#define DS5EXT_SDP_CLIENT_ATTRIBUTE "x-ml-general.ds5Extensions"
#define DS5EXT_SDP_HOST_ATTRIBUTE "x-ss-general.ds5Extensions"
#define DS5EXT_SDP_OFFER "output=1,input-status=1"
// Offered when the client can play the pad's speaker channels: the haptics PCM packet may then carry four channels.
#define DS5EXT_SDP_OFFER_PAD_AUDIO "output=1,input-status=1,pad-audio=1"

#define DS5EXT_MASK_OUTPUT_V1 0x00000001u
#define DS5EXT_MASK_INPUT_STATUS_V1 0x00000002u
#define DS5EXT_MASK_PAD_AUDIO_V1 0x00000004u

#define DS5EXT_OUT_PLAYER_LEDS 0x00000001u
#define DS5EXT_OUT_MUTE_LED 0x00000002u
#define DS5EXT_OUT_SPEAKER_VOLUME 0x00000004u
#define DS5EXT_OUT_HEADPHONE_VOLUME 0x00000008u
#define DS5EXT_OUT_MIC_VOLUME 0x00000010u
#define DS5EXT_OUT_MIC_MUTE 0x00000020u
#define DS5EXT_OUT_AUDIO_ROUTING 0x00000040u
#define DS5EXT_OUT_LIGHTBAR_BRIGHTNESS 0x00000080u
#define DS5EXT_OUT_KNOWN 0x000000ffu

#define DS5EXT_IN_HEADSET 0x00000001u
#define DS5EXT_IN_MICROPHONE 0x00000002u
#define DS5EXT_IN_MIC_MUTED 0x00000004u
#define DS5EXT_IN_KNOWN 0x00000007u

typedef struct _DS5EXT_OUTPUT {
    uint16_t controllerNumber;
    uint32_t validMask;
    uint8_t playerLedMask;
    uint8_t muteLed;
    uint8_t speakerVolume;
    uint8_t headphoneVolume;
    uint8_t micVolume;
    uint8_t micMute;
    uint8_t audioRouting;
    uint8_t lightbarBrightness;
} DS5EXT_OUTPUT;

typedef struct _DS5EXT_INPUT_STATUS {
    uint16_t controllerNumber;
    uint32_t validMask;
    uint8_t headsetPresent;
    uint8_t microphonePresent;
    uint8_t microphoneMuted;
} DS5EXT_INPUT_STATUS;

static inline uint16_t ds5ExtRead16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t ds5ExtRead32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void ds5ExtWrite16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static inline void ds5ExtWrite32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)(v >> 24);
}

// Parses "name=version" tokens separated by ',' or ';'. Unknown names, versions
// other than 1 and malformed tokens are ignored. Returns DS5EXT_MASK_* bits.
static inline uint32_t ds5ExtParseSdpValue(const char* value, size_t length) {
    uint32_t mask = 0;
    size_t offset = 0;

    while (offset < length) {
        size_t start, end, nameLength, versionLength, index;
        const char* token;
        const char* equals = NULL;

        while (offset < length && (value[offset] == ',' || value[offset] == ';' ||
                                   value[offset] == ' ' || value[offset] == '\t')) {
            offset++;
        }
        start = offset;
        while (offset < length && value[offset] != ',' && value[offset] != ';') {
            offset++;
        }
        end = offset;
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
            end--;
        }
        token = value + start;
        for (index = 0; index < end - start; index++) {
            if (token[index] == '=') {
                equals = token + index;
                break;
            }
        }
        if (equals == NULL) {
            continue;
        }
        nameLength = (size_t)(equals - token);
        versionLength = (end - start) - nameLength - 1;
        if (versionLength == 1 && equals[1] == '1') {
            if (nameLength == 6 && memcmp(token, "output", 6) == 0) {
                mask |= DS5EXT_MASK_OUTPUT_V1;
            }
            else if (nameLength == 12 && memcmp(token, "input-status", 12) == 0) {
                mask |= DS5EXT_MASK_INPUT_STATUS_V1;
            }
            else if (nameLength == 9 && memcmp(token, "pad-audio", 9) == 0) {
                mask |= DS5EXT_MASK_PAD_AUDIO_V1;
            }
        }
    }
    return mask;
}

// Decodes the 16-byte body of 0x5515.
static inline bool ds5ExtDecodeOutput(const uint8_t* body, size_t length, DS5EXT_OUTPUT* out) {
    DS5EXT_OUTPUT parsed;

    if (body == NULL || out == NULL || length != DS5EXT_OUTPUT_BODY_BYTES ||
            body[0] != DS5EXT_VERSION || body[1] != 0) {
        return false;
    }
    memset(&parsed, 0, sizeof(parsed));
    parsed.controllerNumber = ds5ExtRead16(body + 2);
    parsed.validMask = ds5ExtRead32(body + 4);
    parsed.playerLedMask = body[8];
    parsed.muteLed = body[9];
    parsed.speakerVolume = body[10];
    parsed.headphoneVolume = body[11];
    parsed.micVolume = body[12];
    parsed.micMute = body[13];
    parsed.audioRouting = body[14];
    parsed.lightbarBrightness = body[15];

    if ((parsed.validMask & ~DS5EXT_OUT_KNOWN) != 0) return false;
    if (!(parsed.validMask & DS5EXT_OUT_PLAYER_LEDS) && parsed.playerLedMask != 0) return false;
    if (!(parsed.validMask & DS5EXT_OUT_MUTE_LED) && parsed.muteLed != 0) return false;
    if (!(parsed.validMask & DS5EXT_OUT_SPEAKER_VOLUME) && parsed.speakerVolume != 0) return false;
    if (!(parsed.validMask & DS5EXT_OUT_HEADPHONE_VOLUME) && parsed.headphoneVolume != 0) return false;
    if (!(parsed.validMask & DS5EXT_OUT_MIC_VOLUME) && parsed.micVolume != 0) return false;
    if (!(parsed.validMask & DS5EXT_OUT_MIC_MUTE) && parsed.micMute != 0) return false;
    if (!(parsed.validMask & DS5EXT_OUT_AUDIO_ROUTING) && parsed.audioRouting != 0) return false;
    if (!(parsed.validMask & DS5EXT_OUT_LIGHTBAR_BRIGHTNESS) && parsed.lightbarBrightness != 0) return false;
    if (parsed.playerLedMask > 0x1f || parsed.muteLed > 1 || parsed.micMute > 1) return false;

    *out = parsed;
    return true;
}

// Encodes the 16-byte body of 0x5515 (used by tests and loopback checks).
static inline size_t ds5ExtEncodeOutput(const DS5EXT_OUTPUT* in, uint8_t* body, size_t capacity) {
    if (in == NULL || body == NULL || capacity < DS5EXT_OUTPUT_BODY_BYTES ||
            (in->validMask & ~DS5EXT_OUT_KNOWN) != 0 || in->playerLedMask > 0x1f ||
            in->muteLed > 1 || in->micMute > 1) {
        return 0;
    }
    body[0] = DS5EXT_VERSION;
    body[1] = 0;
    ds5ExtWrite16(body + 2, in->controllerNumber);
    ds5ExtWrite32(body + 4, in->validMask);
    body[8] = (in->validMask & DS5EXT_OUT_PLAYER_LEDS) ? in->playerLedMask : 0;
    body[9] = (in->validMask & DS5EXT_OUT_MUTE_LED) ? in->muteLed : 0;
    body[10] = (in->validMask & DS5EXT_OUT_SPEAKER_VOLUME) ? in->speakerVolume : 0;
    body[11] = (in->validMask & DS5EXT_OUT_HEADPHONE_VOLUME) ? in->headphoneVolume : 0;
    body[12] = (in->validMask & DS5EXT_OUT_MIC_VOLUME) ? in->micVolume : 0;
    body[13] = (in->validMask & DS5EXT_OUT_MIC_MUTE) ? in->micMute : 0;
    body[14] = (in->validMask & DS5EXT_OUT_AUDIO_ROUTING) ? in->audioRouting : 0;
    body[15] = (in->validMask & DS5EXT_OUT_LIGHTBAR_BRIGHTNESS) ? in->lightbarBrightness : 0;
    return DS5EXT_OUTPUT_BODY_BYTES;
}

// Encodes the 12-byte body of 0x5516.
static inline size_t ds5ExtEncodeInputStatus(const DS5EXT_INPUT_STATUS* in, uint8_t* body, size_t capacity) {
    if (in == NULL || body == NULL || capacity < DS5EXT_INPUT_STATUS_BODY_BYTES ||
            (in->validMask & ~DS5EXT_IN_KNOWN) != 0 || in->headsetPresent > 1 ||
            in->microphonePresent > 1 || in->microphoneMuted > 1) {
        return 0;
    }
    body[0] = DS5EXT_VERSION;
    body[1] = 0;
    ds5ExtWrite16(body + 2, in->controllerNumber);
    ds5ExtWrite32(body + 4, in->validMask);
    body[8] = (in->validMask & DS5EXT_IN_HEADSET) ? in->headsetPresent : 0;
    body[9] = (in->validMask & DS5EXT_IN_MICROPHONE) ? in->microphonePresent : 0;
    body[10] = (in->validMask & DS5EXT_IN_MIC_MUTED) ? in->microphoneMuted : 0;
    body[11] = 0;
    return DS5EXT_INPUT_STATUS_BODY_BYTES;
}

// Decodes the 12-byte body of 0x5516 (host side and tests).
static inline bool ds5ExtDecodeInputStatus(const uint8_t* body, size_t length, DS5EXT_INPUT_STATUS* out) {
    DS5EXT_INPUT_STATUS parsed;

    if (body == NULL || out == NULL || length != DS5EXT_INPUT_STATUS_BODY_BYTES ||
            body[0] != DS5EXT_VERSION || body[1] != 0 || body[11] != 0) {
        return false;
    }
    memset(&parsed, 0, sizeof(parsed));
    parsed.controllerNumber = ds5ExtRead16(body + 2);
    parsed.validMask = ds5ExtRead32(body + 4);
    parsed.headsetPresent = body[8];
    parsed.microphonePresent = body[9];
    parsed.microphoneMuted = body[10];
    if ((parsed.validMask & ~DS5EXT_IN_KNOWN) != 0) return false;
    if (!(parsed.validMask & DS5EXT_IN_HEADSET) && parsed.headsetPresent != 0) return false;
    if (!(parsed.validMask & DS5EXT_IN_MICROPHONE) && parsed.microphonePresent != 0) return false;
    if (!(parsed.validMask & DS5EXT_IN_MIC_MUTED) && parsed.microphoneMuted != 0) return false;
    if (parsed.headsetPresent > 1 || parsed.microphonePresent > 1 || parsed.microphoneMuted > 1) return false;
    *out = parsed;
    return true;
}
