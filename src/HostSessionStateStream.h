#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HOST_SESSION_STATE_WIRE_VERSION 1
#define HOST_SESSION_STATE_WIRE_SIZE 4

// 0x550E v1: version:u8, state:u8 (0 unlocked, 1 locked), reserved:u16 (zero).
// Sent by the host once after the control stream starts and on every change.
static inline bool parseHostSessionState(const uint8_t* wire, int length, uint8_t* state) {
    if (wire == NULL || state == NULL || length != HOST_SESSION_STATE_WIRE_SIZE ||
            wire[0] != HOST_SESSION_STATE_WIRE_VERSION || wire[1] > 1 ||
            wire[2] != 0 || wire[3] != 0) {
        return false;
    }
    *state = wire[1];
    return true;
}
