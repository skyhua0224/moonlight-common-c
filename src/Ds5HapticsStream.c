#include "Ds5HapticsStream.h"
#include <math.h>

#include <string.h>

static uint16_t readLe16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t readLe32(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t readLe64(const uint8_t* data) {
    return (uint64_t)readLe32(data) | ((uint64_t)readLe32(data + 4) << 32);
}

bool processDs5HapticsStreamPacket(const uint8_t* payload,
                                   int payloadLength,
                                   Ds5HapticsStreamCallback callback,
                                   void* context) {
    LI_DS5_HAPTICS_PCM_FRAME frame;
    uint16_t headerSize;
    uint16_t reserved;
    uint32_t expectedPcmBytes;
    const uint8_t knownFlags = LI_DS5_HAPTICS_PCM_FLAG_STREAM_START |
                               LI_DS5_HAPTICS_PCM_FLAG_STREAM_END |
                               LI_DS5_HAPTICS_PCM_FLAG_DISCONTINUITY;

    if (payload == NULL || callback == NULL ||
            payloadLength < DS5_HAPTICS_STREAM_WIRE_HEADER_SIZE) {
        return false;
    }

    memset(&frame, 0, sizeof(frame));
    frame.flags = payload[1];
    headerSize = readLe16(payload + 2);
    frame.controllerNumber = readLe16(payload + 4);
    frame.frameCount = readLe16(payload + 6);
    frame.sequenceNumber = readLe32(payload + 8);
    frame.presentationTimeUs = readLe64(payload + 12);
    frame.sampleRate = readLe32(payload + 20);
    frame.channelCount = payload[24];
    frame.bitsPerSample = payload[25];
    reserved = readLe16(payload + 26);

    if (payload[0] != DS5_HAPTICS_STREAM_PROTOCOL_VERSION ||
            (frame.flags & ~knownFlags) != 0 || reserved != 0 ||
            headerSize < DS5_HAPTICS_STREAM_WIRE_HEADER_SIZE ||
            headerSize > (uint16_t)payloadLength ||
            frame.sampleRate != 48000 || frame.channelCount != 2 ||
            frame.bitsPerSample != 16 ||
            frame.frameCount > DS5_HAPTICS_STREAM_MAX_FRAMES) {
        return false;
    }

    expectedPcmBytes = (uint32_t)frame.frameCount * frame.channelCount *
                       (frame.bitsPerSample / 8);
    if ((uint32_t)payloadLength != (uint32_t)headerSize + expectedPcmBytes) {
        return false;
    }

    frame.pcmData = payload + headerSize;
    frame.pcmDataLength = expectedPcmBytes;
    callback(&frame, context);
    return true;
}

// The Foundation wire contract is little-endian, with 2 * 5 float lanes.
static float readIrFloat(const uint8_t* data) {
    uint32_t bits = readLe32(data);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static bool irUnit(float value) {
    return isfinite(value) && value >= 0.0f && value <= 1.0f;
}
bool processDs5HapticsIrStreamPacket(const uint8_t* p, int length,
                                    ConnListenerDs5HapticsIrV2 callback) {
    if (p == NULL || callback == NULL || length != 72 || p[0] != 2 ||
        (p[1] & ~0x0f) != 0 || readLe16(p + 2) != 72 ||
        readLe16(p + 4) >= 16 || readLe16(p + 6) != 0 ||
        readLe32(p + 68) != 0) return false;
    LI_DS5_HAPTICS_IR_FRAME_V2 frame = {0};
    frame.flags = p[1];
    frame.controllerNumber = readLe16(p + 4);
    frame.sourceSequenceNumber = readLe32(p + 8);
    frame.timestampUs = readLe64(p + 12);
    frame.sourceFrameCount = readLe32(p + 20);
    for (int lane = 0; lane < 2; ++lane) {
        const uint8_t* w = p + 24 + lane * 20;
        LI_DS5_HAPTICS_IR_LANE_V2* out = &frame.lanes[lane];
        out->rmsAmplitude = readIrFloat(w);
        out->peakAmplitude = readIrFloat(w + 4);
        out->transientStrength = readIrFloat(w + 8);
        out->lowBandRatio = readIrFloat(w + 12);
        out->zeroCrossingRateHz = readIrFloat(w + 16);
        if (!irUnit(out->rmsAmplitude) || !irUnit(out->peakAmplitude) ||
            out->rmsAmplitude > out->peakAmplitude ||
            !irUnit(out->transientStrength) || !irUnit(out->lowBandRatio) ||
            !isfinite(out->zeroCrossingRateHz) ||
            out->zeroCrossingRateHz < 0 || out->zeroCrossingRateHz > 48000) return false;
    }
    frame.laneCorrelation = readIrFloat(p + 64);
    if (!isfinite(frame.laneCorrelation) || fabsf(frame.laneCorrelation) > 1) return false;
    callback(&frame);
    return true;
}
