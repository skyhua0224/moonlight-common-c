#include "Ds5HapticsIrStream.h"

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

static float readLeFloat(const uint8_t* data) {
    uint32_t bits = readLe32(data);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool isNormalized(float value) {
    return isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool processDs5HapticsIrStreamPacket(const uint8_t* payload,
                                     int payloadLength,
                                     Ds5HapticsIrStreamCallback callback,
                                     void* context) {
    LI_DS5_HAPTICS_IR_FRAME_V2 frame;
    const uint8_t knownFlags = LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY |
                               LI_DS5_HAPTICS_IR_FLAG_PARTIAL |
                               LI_DS5_HAPTICS_IR_FLAG_STREAM_END |
                               LI_DS5_HAPTICS_IR_FLAG_SILENT;
    int lane;

    if (payload == NULL || callback == NULL ||
            payloadLength != DS5_HAPTICS_IR_STREAM_WIRE_SIZE ||
            payload[0] != DS5_HAPTICS_IR_STREAM_PROTOCOL_VERSION ||
            (payload[1] & ~knownFlags) != 0 ||
            readLe16(payload + 2) != DS5_HAPTICS_IR_STREAM_WIRE_SIZE ||
            readLe16(payload + 6) != 0 || readLe32(payload + 68) != 0) {
        return false;
    }

    memset(&frame, 0, sizeof(frame));
    frame.flags = payload[1];
    frame.controllerNumber = readLe16(payload + 4);
    frame.sourceSequenceNumber = readLe32(payload + 8);
    frame.timestampUs = readLe64(payload + 12);
    frame.sourceFrameCount = readLe32(payload + 20);

    for (lane = 0; lane < 2; lane++) {
        const uint8_t* wireLane = payload + 24 + lane * 20;
        LI_DS5_HAPTICS_IR_LANE_V2* outLane = &frame.lanes[lane];
        outLane->rmsAmplitude = readLeFloat(wireLane);
        outLane->peakAmplitude = readLeFloat(wireLane + 4);
        outLane->transientStrength = readLeFloat(wireLane + 8);
        outLane->lowBandRatio = readLeFloat(wireLane + 12);
        outLane->zeroCrossingRateHz = readLeFloat(wireLane + 16);
        if (!isNormalized(outLane->rmsAmplitude) ||
                !isNormalized(outLane->peakAmplitude) ||
                outLane->rmsAmplitude > outLane->peakAmplitude ||
                !isNormalized(outLane->transientStrength) ||
                !isNormalized(outLane->lowBandRatio) ||
                !isfinite(outLane->zeroCrossingRateHz) ||
                outLane->zeroCrossingRateHz < 0.0f ||
                outLane->zeroCrossingRateHz > 48000.0f) {
            return false;
        }
    }

    frame.laneCorrelation = readLeFloat(payload + 64);
    if (!isfinite(frame.laneCorrelation) ||
            frame.laneCorrelation < -1.0f || frame.laneCorrelation > 1.0f) {
        return false;
    }

    callback(&frame, context);
    return true;
}

static void captureBaseFrame(const LI_DS5_HAPTICS_IR_FRAME_V2* base, void* context) {
    *(LI_DS5_HAPTICS_IR_FRAME_V2*)context = *base;
}

bool processDs5HapticsIrExtendedStreamPacket(const uint8_t* payload,
                                      int payloadLength,
                                      Ds5HapticsIrExtendedStreamCallback callback,
                                      void* context) {
    LI_DS5_HAPTICS_IR_EXTENDED_FRAME frame;
    const uint8_t* extension;
    unsigned int i;
    if (payload == NULL || callback == NULL ||
            payloadLength < DS5_HAPTICS_IR_EXTENDED_STREAM_BASE_SIZE ||
            payloadLength > DS5_HAPTICS_IR_EXTENDED_STREAM_MAX_SIZE) return false;
    extension = payload + DS5_HAPTICS_IR_STREAM_WIRE_SIZE;
    if (extension[0] != DS5_HAPTICS_IR_EXTENSION_VERSION ||
            extension[1] > LI_DS5_HAPTICS_IR_EXTENDED_MAX_EVENTS ||
            readLe16(extension + 2) != payloadLength - DS5_HAPTICS_IR_STREAM_WIRE_SIZE ||
            payloadLength != DS5_HAPTICS_IR_EXTENDED_STREAM_BASE_SIZE +
                extension[1] * DS5_HAPTICS_IR_EXTENDED_STREAM_EVENT_SIZE) return false;

    memset(&frame, 0, sizeof(frame));
    // Reuse the production v2 decoder, including its unchanged units and guards.
    if (!processDs5HapticsIrStreamPacket(payload, DS5_HAPTICS_IR_STREAM_WIRE_SIZE,
                                        captureBaseFrame, &frame.base)) return false;
    if (frame.base.sourceFrameCount == 0 &&
            !(frame.base.flags & LI_DS5_HAPTICS_IR_FLAG_STREAM_END)) return false;
    frame.epoch = readLe32(extension + 4);
    frame.windowEndUs = readLe64(extension + 8);
    frame.eventCount = extension[1];
    if (frame.windowEndUs < frame.base.timestampUs) return false;
    for (i = 0; i < 2; i++) {
        frame.lanes[i].continuousLow = readLeFloat(extension + 16 + i * 8);
        frame.lanes[i].continuousHigh = readLeFloat(extension + 20 + i * 8);
        if (!isNormalized(frame.lanes[i].continuousLow) ||
                !isNormalized(frame.lanes[i].continuousHigh)) return false;
    }
    for (i = 0; i < frame.eventCount; i++) {
        const uint8_t* event = payload + DS5_HAPTICS_IR_EXTENDED_STREAM_BASE_SIZE +
            i * DS5_HAPTICS_IR_EXTENDED_STREAM_EVENT_SIZE;
        LI_DS5_HAPTICS_IR_EVENT* out = &frame.events[i];
        out->eventId = readLe32(event);
        out->sourceTimeUs = readLe64(event + 4);
        out->leftStrength = readLeFloat(event + 12);
        out->rightStrength = readLeFloat(event + 16);
        if (out->sourceTimeUs > frame.windowEndUs ||
                !isNormalized(out->leftStrength) || !isNormalized(out->rightStrength)) return false;
    }
    callback(&frame, context);
    return true;
}
