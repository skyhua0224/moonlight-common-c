#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Endpoint refresh is a private Sunshine/Moonlight extension. It is not an
 * IETF wire protocol; it uses standard HMAC-SHA-256 primitives with a fixed
 * packet layout:
 *
 *   0..3   "EAPR" magic
 *   4      protocol version
 *   5      message type
 *   6      media stream
 *   7      reserved, must be zero
 *   8..23  RTSP session identifier
 *   24..31 big-endian sequence number
 *   32..47 challenge nonce
 *   48..63 first 128 bits of HMAC-SHA-256 over bytes 0..47
 *
 * The HMAC key is derived per RTSP session from the existing RI key. A new
 * UDP endpoint is accepted only after an authenticated challenge/response.
 */
#define LI_ER_VERSION 1
#define LI_ER_PACKET_SIZE 64
#define LI_ER_RI_KEY_SIZE 16
#define LI_ER_KEY_SIZE 32
#define LI_ER_SESSION_SIZE 16
#define LI_ER_NONCE_SIZE 16
#define LI_ER_AUTH_TAG_SIZE 16
#define LI_ER_AUTHENTICATED_SIZE (LI_ER_PACKET_SIZE - LI_ER_AUTH_TAG_SIZE)
#define LI_ER_VIDEO 1
#define LI_ER_AUDIO 2
#define LI_ER_PROBE 1
#define LI_ER_CHALLENGE 2
#define LI_ER_RESPONSE 3
#define LI_ER_ACK 4

typedef struct _LI_ER_MESSAGE {
    uint8_t type;
    uint64_t sequence;
    unsigned char nonce[LI_ER_NONCE_SIZE];
} LI_ER_MESSAGE;

bool LiErDeriveKey(const unsigned char riKey[LI_ER_RI_KEY_SIZE],
                   const unsigned char session[LI_ER_SESSION_SIZE],
                   unsigned char key[LI_ER_KEY_SIZE]);
bool LiErEncode(unsigned char packet[LI_ER_PACKET_SIZE], uint8_t type, uint8_t stream,
                const unsigned char session[LI_ER_SESSION_SIZE], const unsigned char key[LI_ER_KEY_SIZE],
                uint64_t sequence, const unsigned char nonce[LI_ER_NONCE_SIZE]);
bool LiErDecode(const void* packet, size_t length, uint8_t stream,
                const unsigned char session[LI_ER_SESSION_SIZE], const unsigned char key[LI_ER_KEY_SIZE],
                LI_ER_MESSAGE* message);
/*
 * Wipe caller-owned sensitive storage. Current callers pass sizeof() of a
 * local or state object; length is never taken from a received packet.
 */
void LiErWipe(void* data, size_t length);

#ifdef __cplusplus
}
#endif
