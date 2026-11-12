#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Live carrier reassembly AFTER existing RTP reorder/FEC + authentication. One bounded
 * carrier buffer; in-cycle reorder/identical duplicates allowed. Missing data,
 * epoch changes, cross-cycle gaps and missing EOS are terminal, never concealed.
 * Callback must copy/write synchronously; zero return means consumer failure. */
typedef int (*MAT_WRITE_CYCLE)(void*,const uint8_t*,size_t);
typedef struct {
    uint64_t epoch,domain,next_frame,mask;
    uint32_t frame_hash,stream_hash;
    unsigned started,ended,failed;
    MAT_WRITE_CYCLE write;
    void* context;
    uint8_t cycle[61440];
} MAT_DEPACKETIZER;
void MatDepacketizerInit(MAT_DEPACKETIZER*,MAT_WRITE_CYCLE,void*);
/* -1 sticky failure; 0 accepted, 1 whole cycle written, 2 complete EOS. */
int MatDepacketizerPush(MAT_DEPACKETIZER*,const uint8_t*,size_t);
#ifdef __cplusplus
}
#endif
