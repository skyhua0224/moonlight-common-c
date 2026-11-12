#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 0x550D v1: version:u8, reserved:u8, controller:u16le, then eight absolute bytes:
// mode0 (&3), mode2 (&4), known control groups (&0x62), right/left motor,
// power-save, vibration reduction, low-pass filter (&1). Reliable control lane.
static inline bool parseDs5HapticsState(const uint8_t* wire, int length,
                                       uint16_t* controller, const uint8_t** state) {
    if (wire == NULL || length != 12 || controller == NULL || state == NULL ||
            wire[0] != 1 || wire[1] != 0 || wire[3] != 0 || wire[2] > 15 ||
            (wire[4] & ~3) != 0 || (wire[5] & ~4) != 0 ||
            (wire[6] & ~0x62) != 0 || (wire[11] & ~1) != 0) return false;
    *controller = wire[2];
    *state = wire + 4;
    return true;
}
