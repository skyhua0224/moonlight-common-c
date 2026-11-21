#include "ConnectionStatusInterval.h"

#include <stdio.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void checkInterval(int totalFrames, int goodFrames,
                          int expectedPercent, unsigned int expectedBasisPoints) {
    int lossPercent = -1;
    unsigned int lossBasisPoints = 1;
    computeIntervalFrameLoss(totalFrames, goodFrames, &lossPercent, &lossBasisPoints);
    if (lossPercent != expectedPercent || lossBasisPoints != expectedBasisPoints) {
        fprintf(stderr, "total=%d good=%d: expected %d%% / %u bp, got %d%% / %u bp\n",
                totalFrames, goodFrames, expectedPercent, expectedBasisPoints,
                lossPercent, lossBasisPoints);
        failures++;
    }
}

int main(void) {
    // One frame completing after the interval boundary: the interval holds one
    // more good frame than total frames. This is the reported case, where the
    // old arithmetic produced -56 basis points and read as 42949672.40 percent.
    checkInterval(180, 181, 0, 0);
    checkInterval(181, 182, 0, 0);

    // Every frame accounted for: no loss.
    checkInterval(180, 180, 0, 0);
    checkInterval(1, 1, 0, 0);

    // One lost frame of a full 3 s window at 60 fps: the 0.55/0.56 percent the
    // client logged next to the wrapped value.
    checkInterval(181, 180, 1, 55);
    checkInterval(180, 179, 1, 56);

    // Ordinary loss keeps the historical figures, including the truncating
    // integer percent used for the connection-status thresholds.
    checkInterval(100, 90, 10, 1000);
    checkInterval(100, 99, 1, 100);
    checkInterval(3, 0, 100, 10000);

    // The caller skips empty intervals; the helper must not divide by zero either.
    checkInterval(0, 0, 0, 0);

    if (failures != 0) {
        fprintf(stderr, "%d connection-status interval checks failed\n", failures);
        return 1;
    }

    puts("connection status interval loss checks passed");
    return 0;
}
