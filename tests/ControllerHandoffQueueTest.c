// Inspect the production sender's queue without starting a network thread.
#include "../src/InputStream.c"
// The test drives one input stream context of the library's global connection context.
static PML_INPUT_STREAM_CONTEXT ctx = &gConnectionContext.inputContext;
#include <assert.h>

static PPACKET_HOLDER pop(void) {
    PPACKET_HOLDER holder = NULL;
    assert(LbqPollQueueElement(&ctx->packetQueue, (void**)&holder) == LBQ_SUCCESS);
    return holder;
}

typedef struct _REJECTED_PRODUCER {
    PLT_EVENT start;
    int accepted;
} REJECTED_PRODUCER;

static void sendRejectedUpdates(void* context) {
    REJECTED_PRODUCER* producer = context;
    PltWaitForEvent(&producer->start);
    for (int i = 0; i < 5000; i++) {
        int result = LiSendMultiControllerEvent(0, 0, 0, 0, 0, 0, 0, 0, 0);
        assert(result == LBQ_BOUND_EXCEEDED || result == LBQ_SUCCESS);
        if (result == LBQ_SUCCESS) producer->accepted++;
    }
}

int main(void) {
    ctx->connectionContext = &gConnectionContext;
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 500;
    AppVersionQuad[3] = -1;
    assert(initializeInputStream() == 0);
    ctx->initialized = true;

    // Rapid withdrawal and arrival with identical zero buttons must retain the
    // removal packet, even while the sender hasn't consumed any of the queue.
    assert(LiSendMultiControllerEvent(0, 1, 0, 0, 0, 0, 0, 0, 0) == 0);
    assert(LiSendMultiControllerEvent(0, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    assert(LiSendControllerArrivalEvent(0, 1, LI_CTYPE_PS, 0, 0) == 0);
    assert(LiSendMultiControllerEvent(0, 1, 0, 0, 0, 0, 0, 0, 0) == 0);
    // The arrival event also queues the controller state that follows it.
    PPACKET_HOLDER before = pop(), removed = pop(), arrival = pop(), arrivalState = pop(), restored = pop();
    assert(arrivalState->packet.multiController.activeGamepadMask == LE16(1));
    assert(before->packet.multiController.activeGamepadMask == LE16(1));
    assert(removed->packet.multiController.activeGamepadMask == LE16(0));
    assert(arrival->packet.header.magic == LE32(SS_CONTROLLER_ARRIVAL_MAGIC));
    assert(restored->packet.multiController.activeGamepadMask == LE16(1));
    assert(restored->channelId == removed->channelId && arrival->channelId == removed->channelId);
    assert(removed->enetPacketFlags == ENET_PACKET_FLAG_RELIABLE);
    free(before); free(removed); free(arrival); free(arrivalState); free(restored);

    // Every update is queued on its own: batching of successive axis updates
    // happens in the input thread, which compares the active mask and the button
    // flags before it merges, so another player's queue stays intact.
    assert(LiSendMultiControllerEvent(1, 3, 0, 0, 0, 12, 0, 0, 0) == 0);
    assert(LiSendMultiControllerEvent(1, 3, 0, 0, 0, 34, 0, 0, 0) == 0);
    assert(LiSendMultiControllerEvent(0, 2, 0, 0, 0, 0, 0, 0, 0) == 0);
    PPACKET_HOLDER first = pop(), second = pop(), handoff = pop(), unused = NULL;
    assert(first->packet.multiController.leftStickX == LE16(12));
    assert(second->packet.multiController.leftStickX == LE16(34));
    assert(second->packet.multiController.activeGamepadMask == LE16(3));
    assert(handoff->packet.multiController.activeGamepadMask == LE16(2));
    assert(LbqPollQueueElement(&ctx->packetQueue, (void**)&unused) == LBQ_NO_ELEMENT);
    free(first); free(second); free(handoff);

    // A full queue reports the failure; a later retry must really enqueue,
    // rather than merge into the rejected holder sitting in the free list.
    for (int i = 0; i < MAX_QUEUED_INPUT_PACKETS; i++) {
        assert(LiSendMultiControllerEvent(0, 1, (i & 1) ? A_FLAG : 0, 0, 0, 0, 0, 0, 0) == 0);
    }

    // No removal update can be accepted while this full queue contains only
    // active-mask-1 packets. Concurrent producers must not batch into a rejected
    // mask-0 holder and report success for an update that will never be sent.
    REJECTED_PRODUCER producers[2] = {0};
    PLT_THREAD threads[2];
    for (int i = 0; i < 2; i++) {
        assert(PltCreateEvent(&producers[i].start) == 0);
        assert(PltCreateThread("Rejected input", sendRejectedUpdates, &producers[i], &threads[i]) == 0);
    }
    for (int i = 0; i < 2; i++) PltSetEvent(&producers[i].start);
    for (int i = 0; i < 2; i++) {
        PltJoinThread(&threads[i]);
        PltCloseEvent(&producers[i].start);
        assert(producers[i].accepted == 0);
    }

    assert(LiSendMultiControllerEvent(0, 0, 0, 0, 0, 0, 0, 0, 0) == LBQ_BOUND_EXCEEDED);
    for (int i = 0; i < MAX_QUEUED_INPUT_PACKETS; i++) free(pop());
    assert(LiSendMultiControllerEvent(0, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    assert(LbqGetItemCount(&ctx->packetQueue) == 1);
    free(pop());
    ctx->initialized = false;
    LbqSignalQueueShutdown(&ctx->packetQueue);
    LbqSignalQueueShutdown(&ctx->packetHolderFreeList);
    destroyInputStream();
    puts("Controller handoff queue tests passed");
    return 0;
}
