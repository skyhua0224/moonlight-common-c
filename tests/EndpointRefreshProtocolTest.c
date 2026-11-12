#include "EndpointRefreshProtocol.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    const unsigned char id[] = "0123456789abcdef";
    const unsigned char expectedKey[32] = {
        0xba,0xcb,0x2c,0x6a,0x9a,0x4b,0xee,0x50,0xa7,0x34,0xe4,0xf9,0xcd,0xfb,0x51,0xe1,
        0xb0,0x68,0x79,0x93,0x8e,0xb2,0x18,0x95,0xca,0xfb,0x4d,0x1a,0x3f,0x02,0xf3,0x08
    };
    const unsigned char expectedTag[16] = {0x6b,0xd8,0x2a,0x6b,0x2c,0x8e,0xb4,0x2b,0x27,0x31,0x98,0xfe,0x05,0x10,0xed,0x14};
    unsigned char ri[16], key[32], packet[65], changed[64], otherKey[32];
    LI_ER_MESSAGE message;
    memset(ri, 0x11, sizeof(ri));
    CHECK(LiErDeriveKey(ri, id, key));
    CHECK(!memcmp(key, expectedKey, sizeof(key)));
    CHECK(LiErEncode(packet, LI_ER_PROBE, LI_ER_VIDEO, id, key, UINT64_C(0x0102030405060708), NULL));
    CHECK(!memcmp(packet + 48, expectedTag, 16));
    CHECK(LiErDecode(packet, 64, LI_ER_VIDEO, id, key, &message));
    CHECK(message.sequence == UINT64_C(0x0102030405060708));
    for (unsigned int i = 0; i < 64; i++) {
        memcpy(changed, packet, 64);
        changed[i] ^= 1;
        CHECK(!LiErDecode(changed, 64, LI_ER_VIDEO, id, key, &message));
    }
    CHECK(!LiErDecode(packet, 63, LI_ER_VIDEO, id, key, &message));
    CHECK(!LiErDecode(packet, 65, LI_ER_VIDEO, id, key, &message));
    CHECK(!LiErDecode(packet, 64, LI_ER_AUDIO, id, key, &message));
    CHECK(LiErDeriveKey(ri, (const unsigned char*)"fedcba9876543210", otherKey));
    CHECK(!LiErDecode(packet, 64, LI_ER_VIDEO, id, otherKey, &message));
    CHECK(!LiErDecode(packet, 64, LI_ER_VIDEO, (const unsigned char*)"fedcba9876543210", key, &message));
    CHECK(!LiErEncode(packet, LI_ER_PROBE, LI_ER_VIDEO, id, key, 0, NULL));
    CHECK(!LiErEncode(packet, 0, LI_ER_VIDEO, id, key, 1, NULL));
    CHECK(!LiErEncode(packet, LI_ER_PROBE, 0, id, key, 1, NULL));
    LiErWipe(key, sizeof(key));
    for (unsigned int i = 0; i < sizeof(key); i++) {
        CHECK(key[i] == 0);
    }
    puts("Endpoint refresh codec: known vector and rejection checks passed");
    return 0;
}
