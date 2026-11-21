#pragma once

#include <math.h>

// Frame-loss figures for one connection-status interval.
//
// The good-frame counter is incremented when a frame finishes reassembly, while
// the total-frame counter advances when the next frame's first packet arrives.
// At an interval boundary the two can straddle the reset: the frame that closed
// the old interval counts toward its total, and its completion counts as good in
// the new one. The interval then holds one more good frame than total frames.
// Computing (total - good) anyway yields -1 frame, which rounds to -56 basis
// points at 180 frames per interval and reached clients as an unsigned
// 4294967240, i.e. 42949672.40 percent network loss. Report zero instead: the
// skew is an accounting artifact, not lost frames.
static inline void computeIntervalFrameLoss(int totalFrames, int goodFrames,
                                            int* lossPercent, unsigned int* lossBasisPoints) {
    if (goodFrames >= totalFrames) {
        *lossPercent = 0;
        *lossBasisPoints = 0;
        return;
    }

    *lossPercent = 100 - (goodFrames * 100) / totalFrames;
    *lossBasisPoints = (unsigned int)llround(10000.0 * (totalFrames - goodFrames) / totalFrames);
}
