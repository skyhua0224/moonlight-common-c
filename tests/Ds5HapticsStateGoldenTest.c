#include "Ds5HapticsStateStream.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    const uint8_t fixture[12] = {1, 0, 3, 0, 2, 4, 0x62, 0x55, 0x33, 0x10, 0x23, 1};
    uint16_t controller = 0;
    const uint8_t* state = NULL;
    assert(parseDs5HapticsState(fixture, 12, &controller, &state));
    assert(controller == 3 && memcmp(state, fixture + 4, 8) == 0);
    for (int length = 0; length < 12; length++)
        assert(!parseDs5HapticsState(fixture, length, &controller, &state));
    assert(!parseDs5HapticsState(fixture, 13, &controller, &state));
    assert(!parseDs5HapticsState(NULL, 12, &controller, &state));
    const int offsets[] = {0, 1, 2, 3, 4, 5, 6, 11};
    const uint8_t invalid[] = {2, 1, 16, 1, 4, 1, 0x80, 2};
    for (unsigned int i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint8_t packet[12];
        memcpy(packet, fixture, 12);
        packet[offsets[i]] = invalid[i];
        assert(!parseDs5HapticsState(packet, 12, &controller, &state));
    }
    puts("DS5 haptics state golden tests passed");
    return 0;
}
