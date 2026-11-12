#include "MatReceiver.h"
#include <string.h>
static int fail(MAT_RECEIVER* s) {
    s->failed = 1; s->depacketizer.failed = 1; return -1;
}
int MatCarrierIsValid(const uint8_t* p, size_t n) {
    if (!p || n != 61440 || memcmp(p, "\x72\xf8\x1f\x4e", 4) ||
        (p[4] & 31) != 0x16 || (p[4] & 0x80)) return 0;
    unsigned pd = (unsigned)p[6] | (unsigned)p[7] << 8;
    return pd >= 2 && pd <= 61432 && !(pd & 1);
}
static int consume(void* context, const uint8_t* p, size_t n) {
    MAT_RECEIVER* s = context;
    MAT_DEPACKETIZER* d = &s->depacketizer;
    MAT_CYCLE_INFO info = {d->epoch, d->domain, d->next_frame, d->next_frame * 960};
    return s->consume && s->consume(s->context, &info, p, n);
}
void MatReceiverInit(MAT_RECEIVER* s, MAT_CONSUME_CYCLE callback,
                     MAT_COMPLETE_STREAM complete, void* context) {
    memset(s, 0, sizeof(*s));
    s->consume = callback; s->complete = complete; s->context = context;
    MatDepacketizerInit(&s->depacketizer, consume, s);
}
static int complete(MAT_RECEIVER* s) {
    if (!s->completed) {
        if (s->complete && !s->complete(s->context, s->depacketizer.next_frame)) return fail(s);
        s->completed = 1;
    }
    return 2;
}
int MatReceiverPush(MAT_RECEIVER* s, const uint8_t* p, size_t n) {
    if (s->failed) return fail(s);
    int result = MatDepacketizerPush(&s->depacketizer, p, n);
    if (result < 0) return fail(s);
    return result == 2 ? complete(s) : result;
}
void MatReceiverAbort(MAT_RECEIVER* s) { if (s) fail(s); }
