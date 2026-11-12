#pragma once

#include "Limelight.h"

#define DS5_HAPTICS_IR_STREAM_PROTOCOL_VERSION 2
#define DS5_HAPTICS_IR_STREAM_WIRE_SIZE 72
#define DS5_HAPTICS_IR_EXTENSION_VERSION 1
#define DS5_HAPTICS_IR_EXTENSION_BASE_SIZE 32
#define DS5_HAPTICS_IR_EXTENDED_STREAM_BASE_SIZE (DS5_HAPTICS_IR_STREAM_WIRE_SIZE + DS5_HAPTICS_IR_EXTENSION_BASE_SIZE)
#define DS5_HAPTICS_IR_EXTENDED_STREAM_EVENT_SIZE 20
#define DS5_HAPTICS_IR_EXTENDED_STREAM_MAX_SIZE (DS5_HAPTICS_IR_EXTENDED_STREAM_BASE_SIZE + LI_DS5_HAPTICS_IR_EXTENDED_MAX_EVENTS * DS5_HAPTICS_IR_EXTENDED_STREAM_EVENT_SIZE)

typedef void(*Ds5HapticsIrStreamCallback)(const LI_DS5_HAPTICS_IR_FRAME_V2* frame,
                                          void* context);

bool processDs5HapticsIrStreamPacket(const uint8_t* payload,
                                     int payloadLength,
                                     Ds5HapticsIrStreamCallback callback,
                                     void* context);

typedef void(*Ds5HapticsIrExtendedStreamCallback)(const LI_DS5_HAPTICS_IR_EXTENDED_FRAME* frame,
                                          void* context);

bool processDs5HapticsIrExtendedStreamPacket(const uint8_t* payload,
                                      int payloadLength,
                                      Ds5HapticsIrExtendedStreamCallback callback,
                                      void* context);
