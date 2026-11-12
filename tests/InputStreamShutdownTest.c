// Exercise the production sender with an ACK that never arrives during shutdown.
#define isControlDataInTransitCtx testControlDataInTransit
#define flushInputOnControlStreamCtx testFlushInputOnControlStream
#define sendInputPacketOnControlStreamCtx testSendInputPacketOnControlStream
#include "../src/InputStream.c"
#undef isControlDataInTransitCtx
#undef flushInputOnControlStreamCtx
#undef sendInputPacketOnControlStreamCtx
// The file-local context redirects must not capture member names below.
#undef AppVersionQuad
#undef StreamConfig
#undef SunshineFeatureFlags
#undef ListenerCallbacks
#undef RemoteAddr
#undef AddrLen
#undef ConnectionInterrupted

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

// The sender works on the library's global connection context (this test links the library).

static PLT_EVENT waitingForAck;
static int keyReleases;

bool testControlDataInTransit(PML_CONTROL_STREAM_CONTEXT control) {
    (void)control;
    PltSetEvent(&waitingForAck);
    return true;
}

void testFlushInputOnControlStream(PML_CONTROL_STREAM_CONTEXT control) { (void)control; }

int testSendInputPacketOnControlStream(PML_CONTROL_STREAM_CONTEXT control, unsigned char* data, int length,
                                      uint8_t channelId, uint32_t flags, bool moreData) {
    NV_INPUT_HEADER* header = (NV_INPUT_HEADER*)data;
    (void)control;
    (void)length;
    (void)channelId;
    (void)flags;
    (void)moreData;
    CHECK(header->magic == LE32(KEY_UP_EVENT_MAGIC));
    keyReleases++;
    return 0;
}

int main(void) {
    PML_CONNECTION_CONTEXT connection = &gConnectionContext;
    PML_INPUT_STREAM_CONTEXT ctx = &connection->inputContext;
    connection->AppVersionQuad[0] = 7;
    connection->AppVersionQuad[1] = 1;
    connection->AppVersionQuad[2] = 500;
    connection->AppVersionQuad[3] = 0; // Exercise GFE's ACK wait (Sunshine may skip it).
    CHECK(PltCreateEvent(&waitingForAck) == 0);
    CHECK(initializeInputStreamCtx(ctx, connection) == 0);
    ctx->initialized = true;

    CHECK(LiSendUtf8TextEventCtx(ctx, "ab", 2) == 0);
    CHECK(LiSendKeyboardEventCtx(ctx, 0x41, KEY_ACTION_UP, 0) == 0);
    CHECK(PltCreateThread("InputSend", inputSendThreadProc, ctx, &ctx->inputSendThread) == 0);
    PltWaitForEvent(&waitingForAck);
    LiInterruptConnectionCtx(connection);
    stopInputStreamCtx(ctx);

    CHECK(keyReleases == 1);
    CHECK(LbqGetItemCount(&ctx->packetQueue) == 0);

    destroyInputStreamCtx(ctx);
    PltCloseEvent(&waitingForAck);
    puts("Input stream shutdown regression passed");
    return 0;
}
