#include "EndpointRefreshProtocol.h"
#include <string.h>
#ifdef USE_MBEDTLS
// The generic message-digest API provides SHA-256 HMAC for Mbed TLS builds.
#include <mbedtls/md.h>
#else
// OpenSSL provides the equivalent HMAC-SHA-256 implementation here.
#include <openssl/hmac.h>
#endif

static bool hmacSha256(const unsigned char* key, size_t keyLength,
                       const unsigned char* data, size_t length, unsigned char out[LI_ER_KEY_SIZE]) {
#ifdef USE_MBEDTLS
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info && mbedtls_md_hmac(info, key, keyLength, data, length, out) == 0;
#else
    unsigned int written = 0;
    return HMAC(EVP_sha256(), key, (int)keyLength, data, length, out, &written) != NULL && written == 32;
#endif
}

void LiErWipe(void* data, size_t length) {
    if (data == NULL || length == 0) {
        return;
    }
    // Volatile writes prevent the compiler from removing this best-effort
    // scrub. The caller still must pass a valid buffer and its true size.
    volatile unsigned char* bytes = data;
    while (length--) {
        *bytes++ = 0;
    }
}

static bool equal(const unsigned char* a, const unsigned char* b, size_t length) {
    unsigned char difference = 0;
    while (length--) {
        difference |= *a++ ^ *b++;
    }
    return difference == 0;
}

bool LiErDeriveKey(const unsigned char riKey[LI_ER_RI_KEY_SIZE],
                   const unsigned char session[LI_ER_SESSION_SIZE],
                   unsigned char key[LI_ER_KEY_SIZE]) {
    // Domain separation prevents this extension from reusing an HMAC output
    // as a key for another protocol purpose. This is a fixed HMAC-based KDF
    // for compatibility, rather than a general-purpose HKDF interface.
    static const unsigned char domain[] = "sunshine/endpoint_auto_refresh/v1";
    unsigned char context[sizeof(domain) - 1 + LI_ER_SESSION_SIZE];
    memcpy(context, domain, sizeof(domain) - 1);
    memcpy(context + sizeof(domain) - 1, session, LI_ER_SESSION_SIZE);
    return hmacSha256(riKey, LI_ER_RI_KEY_SIZE, context, sizeof(context), key);
}

bool LiErEncode(unsigned char packet[LI_ER_PACKET_SIZE], uint8_t type, uint8_t stream,
                const unsigned char session[LI_ER_SESSION_SIZE], const unsigned char key[LI_ER_KEY_SIZE],
                uint64_t sequence, const unsigned char nonce[LI_ER_NONCE_SIZE]) {
    unsigned char tag[LI_ER_KEY_SIZE];
    unsigned int i;
    if (!sequence || type < LI_ER_PROBE || type > LI_ER_ACK || stream < LI_ER_VIDEO || stream > LI_ER_AUDIO) {
        return false;
    }
    memset(packet, 0, LI_ER_PACKET_SIZE);
    memcpy(packet, "EAPR", 4);
    packet[4] = LI_ER_VERSION;
    packet[5] = type;
    packet[6] = stream;
    memcpy(packet + 8, session, LI_ER_SESSION_SIZE);
    for (i = 0; i < 8; i++) {
        packet[31 - i] = (unsigned char)(sequence >> (i * 8));
    }
    if (nonce) {
        memcpy(packet + 32, nonce, LI_ER_NONCE_SIZE);
    }
    if (!hmacSha256(key, LI_ER_KEY_SIZE, packet, LI_ER_AUTHENTICATED_SIZE, tag)) {
        return false;
    }
    memcpy(packet + LI_ER_AUTHENTICATED_SIZE, tag, LI_ER_AUTH_TAG_SIZE);
    LiErWipe(tag, sizeof(tag));
    return true;
}

bool LiErDecode(const void* data, size_t length, uint8_t stream,
                const unsigned char session[LI_ER_SESSION_SIZE], const unsigned char key[LI_ER_KEY_SIZE],
                LI_ER_MESSAGE* message) {
    const unsigned char* packet = data;
    unsigned char tag[LI_ER_KEY_SIZE];
    uint64_t sequence = 0;
    unsigned int i;
    bool valid;
    if (length != LI_ER_PACKET_SIZE || memcmp(packet, "EAPR", 4) || packet[4] != LI_ER_VERSION ||
        packet[7] || packet[6] != stream || packet[5] < LI_ER_PROBE || packet[5] > LI_ER_ACK ||
        !equal(packet + 8, session, LI_ER_SESSION_SIZE)) {
        return false;
    }
    if (!hmacSha256(key, LI_ER_KEY_SIZE, packet, LI_ER_AUTHENTICATED_SIZE, tag)) {
        return false;
    }
    valid = equal(tag, packet + LI_ER_AUTHENTICATED_SIZE, LI_ER_AUTH_TAG_SIZE);
    LiErWipe(tag, sizeof(tag));
    if (!valid) {
        return false;
    }
    for (i = 24; i < 32; i++) {
        sequence = (sequence << 8) | packet[i];
    }
    if (!sequence) {
        return false;
    }
    message->type = packet[5];
    message->sequence = sequence;
    memcpy(message->nonce, packet + 32, LI_ER_NONCE_SIZE);
    return true;
}
