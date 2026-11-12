#pragma once
#include "MatDepacketizer.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t epoch, domain, index, pts48k;
} MAT_CYCLE_INFO;
/* Called synchronously. Copy bytes before returning; false is a terminal failure.
 * complete means input EOS, NOT that the hardware has finished playing. */
typedef int (*MAT_CONSUME_CYCLE)(void*, const MAT_CYCLE_INFO*, const uint8_t*, size_t);
typedef int (*MAT_COMPLETE_STREAM)(void*, uint64_t);
typedef struct {
    MAT_DEPACKETIZER depacketizer;
    MAT_CONSUME_CYCLE consume;
    MAT_COMPLETE_STREAM complete;
    void* context;
    unsigned failed, completed;
} MAT_RECEIVER;
int MatCarrierIsValid(const uint8_t*, size_t);
void MatReceiverInit(MAT_RECEIVER*, MAT_CONSUME_CYCLE, MAT_COMPLETE_STREAM, void*);
int MatReceiverPush(MAT_RECEIVER*, const uint8_t*, size_t);
void MatReceiverAbort(MAT_RECEIVER*);
#ifdef __cplusplus
}
#endif
