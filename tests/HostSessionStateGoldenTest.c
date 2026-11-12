#include "HostSessionStateStream.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    const uint8_t locked[4] = {1, 1, 0, 0};
    const uint8_t unlocked[4] = {1, 0, 0, 0};
    uint8_t state = 0xff;

    assert(parseHostSessionState(locked, 4, &state));
    assert(state == 1);
    assert(parseHostSessionState(unlocked, 4, &state));
    assert(state == 0);

    for (int length = 0; length < 4; length++)
        assert(!parseHostSessionState(locked, length, &state));
    assert(!parseHostSessionState(locked, 5, &state));
    assert(!parseHostSessionState(NULL, 4, &state));
    assert(!parseHostSessionState(locked, 4, NULL));

    const uint8_t invalid[][4] = {
        {0, 1, 0, 0},
        {2, 1, 0, 0},
        {1, 2, 0, 0},
        {1, 1, 1, 0},
        {1, 1, 0, 1},
    };
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(!parseHostSessionState(invalid[i], 4, &state));

    puts("Host session state golden tests passed");
    return 0;
}
