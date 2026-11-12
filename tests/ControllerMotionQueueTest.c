// Exercise the production sender, queue and stop handling without a network peer.
#undef NDEBUG
#include "../src/InputStream.c"
// The test drives one input stream context of the library's global connection context.
static PML_INPUT_STREAM_CONTEXT ctx = &gConnectionContext.inputContext;
#include <assert.h>
#include <math.h>

static PPACKET_HOLDER popMotion(bool expectedCurrent) {
    PPACKET_HOLDER holder = NULL;
    assert(LbqPollQueueElement(&ctx->packetQueue, (void**)&holder) == LBQ_SUCCESS);
    assert(holder->packet.header.magic == LE32(SS_CONTROLLER_MOTION_MAGIC));
    assert(prepareQueuedMotionPacket(ctx, holder) == expectedCurrent);
    return holder;
}

static float value(netfloat data) {
    uint32_t bits;
    float result;
    memcpy(&bits, data, sizeof(bits));
    bits = LE32(bits);
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static void enable(uint8_t slot) {
    assert(setControllerMotionEnabled(slot, LI_MOTION_TYPE_GYRO, true) == 0);
    assert(setControllerMotionEnabled(slot, LI_MOTION_TYPE_ACCEL, true) == 0);
}

static void emptyMotion(void) {
    while (LbqGetItemCount(&ctx->packetQueue)) freePacketHolder(ctx, popMotion(false));
    assert(ctx->queuedMotionPackets == 0);
}

int main(void) {
    ctx->connectionContext = &gConnectionContext;
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 500;
    AppVersionQuad[3] = -1;
    SunshineFeatureFlags = LI_FF_CONTROLLER_TOUCH_EVENTS;
    assert(initializeInputStream() == 0);
    // Host demand can arrive while the control stream starts, before the input thread.
    enable(0);
    ctx->initialized = true;

    assert(LiSendControllerMotionEvent(0, 2, 1, 2, 3) == 0);
    assert(LiSendControllerMotionEvent(0, 1, 10, 20, 30) == 0);
    assert(LiSendControllerMotionEvent(0, 2, 4, 5, 6) == 0);
    const float expected[] = {1, 10, 4};
    for (int i = 0; i < 3; i++) {
        PPACKET_HOLDER holder = popMotion(true);
        assert(value(holder->packet.controllerMotion.x) == expected[i]);
        freePacketHolder(ctx, holder);
    }

    // Paired reports and zeros are unchanged even around high frequency zero crossings.
    double integral = 0;
    for (int batch = 0; batch < 125; batch++) {
        for (int sample = 0; sample < 10; sample++) {
            float x = sample % 5 == 0 ? 0.0f : sample % 5 < 3 ? 1.0f : -1.0f;
            assert(LiSendControllerMotionFrame(0, x, x / 2, -x, 0, 9.80665f, 1) == 0);
        }
        for (int sample = 0; sample < 10; sample++) {
            float x = sample % 5 == 0 ? 0.0f : sample % 5 < 3 ? 1.0f : -1.0f;
            PPACKET_HOLDER gyro = popMotion(true), accel = popMotion(true);
            assert(gyro->packet.controllerMotion.motionType == 2);
            assert(accel->packet.controllerMotion.motionType == 1);
            assert(value(gyro->packet.controllerMotion.x) == x);
            assert(value(gyro->packet.controllerMotion.y) == x / 2);
            assert(value(gyro->packet.controllerMotion.z) == -x);
            assert(value(accel->packet.controllerMotion.y) == 9.80665f);
            integral += value(gyro->packet.controllerMotion.x) * .004;
            freePacketHolder(ctx, gyro); freePacketHolder(ctx, accel);
        }
    }
    assert(fabs(integral) < 1e-9 && LbqGetItemCount(&ctx->packetQueue) == 0);

    // One free entry in the shared queue cannot accept half a frame. A final
    // zero can still use an explicit stop when all shared queue entries are full.
    for (int i = 0; i < MAX_QUEUED_INPUT_PACKETS - 1; i++) assert(LiSendKeyboardEvent(0x41, KEY_ACTION_DOWN, 0) == 0);
    assert(LiSendControllerMotionFrame(0, 3, 0, 0, 1, 2, 3) == LBQ_BOUND_EXCEEDED);
    assert(LbqGetItemCount(&ctx->packetQueue) == MAX_QUEUED_INPUT_PACKETS - 1 && ctx->queuedMotionPackets == 0);
    assert(LiSendKeyboardEvent(0x41, KEY_ACTION_UP, 0) == 0);
    assert(LiSendControllerMotionFrame(0, 0, 0, 0, 1, 2, 3) == LBQ_BOUND_EXCEEDED);
    assert(ctx->pendingGyroStops == 0);
    assert(LiStopControllerMotionEvent(0) == 0);
    PACKET_HOLDER fullQueueStop;
    assert(takePendingMotionStop(ctx, &fullQueueStop) && motionStopIsCurrent(ctx, &fullQueueStop));
    finishMotionStop(ctx, &fullQueueStop);
    PPACKET_HOLDER queued;
    while (LbqPollQueueElement(&ctx->packetQueue, (void**)&queued) == LBQ_SUCCESS) freePacketHolder(ctx, queued);

    // A paired frame must not partly enter a nearly full motion budget.
    // Failed frames do not consume motion capacity, and buttons retain queue capacity.
    assert(LiSendControllerMotionEvent(0, 2, 1, 0, 0) == 0);
    for (int i = 1; i < MAX_QUEUED_MOTION_PACKETS; i++) {
        assert(LiSendControllerMotionEvent(0, 1, (float)i, 0, 0) == 0);
    }
    assert(LiSendControllerMotionFrame(0, 0, 0, 0, 1, 2, 3) != 0);
    assert(ctx->pendingGyroStops == 0);
    assert(LbqGetItemCount(&ctx->packetQueue) == MAX_QUEUED_MOTION_PACKETS);
    assert(LiSendKeyboardEvent(0x41, KEY_ACTION_DOWN, 0) == 0);
    for (int i = 0; i < MAX_QUEUED_MOTION_PACKETS; i++) freePacketHolder(ctx, popMotion(true));
    PPACKET_HOLDER keyboard = NULL;
    assert(LbqPollQueueElement(&ctx->packetQueue, (void**)&keyboard) == LBQ_SUCCESS);
    assert(keyboard->packet.header.magic == LE32(KEY_DOWN_EVENT_MAGIC));
    freePacketHolder(ctx, keyboard);

    // Stops need no queue capacity and invalidate preceding movement. A dequeued
    // stop cannot survive a source replacement or a newly accepted gyro sample.
    assert(LiSendControllerMotionFrame(0, 7, 0, 0, 1, 2, 3) == 0);
    assert(LiStopControllerMotionEvent(0) == 0);
    PACKET_HOLDER oldStop, replacementStop;
    assert(takePendingMotionStop(ctx, &oldStop));
    assert(LiResetControllerMotionEvents(0, false) == 0);
    assert(!motionStopIsCurrent(ctx, &oldStop));
    assert(takePendingMotionStop(ctx, &replacementStop));
    assert(LiSendControllerMotionEvent(0, 2, 8, 0, 0) == 0);
    assert(!motionStopIsCurrent(ctx, &replacementStop));
    freePacketHolder(ctx, popMotion(false)); freePacketHolder(ctx, popMotion(false));
    PPACKET_HOLDER fresh = popMotion(true);
    assert(value(fresh->packet.controllerMotion.x) == 8);
    freePacketHolder(ctx, fresh);
    assert(!takePendingMotionStop(ctx, &oldStop));

    // Rejected final zero still clears rotation when the queue is saturated.
    for (int i = 0; i < MAX_QUEUED_MOTION_PACKETS; i++) assert(LiSendControllerMotionEvent(0, 2, 9, 0, 0) == 0);
    assert(LiSendControllerMotionEvent(0, 2, 0, 0, 0) != 0);
    assert(LiStopControllerMotionEvent(0) == 0); // No extra pending stop.
    assert(takePendingMotionStop(ctx, &oldStop));
    assert(motionStopIsCurrent(ctx, &oldStop));
    assert(value(oldStop.packet.controllerMotion.x) == 0 && oldStop.enetPacketFlags == ENET_PACKET_FLAG_RELIABLE);
    finishMotionStop(ctx, &oldStop);
    assert(!takePendingMotionStop(ctx, &oldStop));
    emptyMotion();

    // The gyro-only frame fallback also rolls back without stopping a raw source.
    assert(setControllerMotionEnabled(0, LI_MOTION_TYPE_ACCEL, false) == 0);
    for (int i = 0; i < MAX_QUEUED_MOTION_PACKETS; i++) assert(LiSendControllerMotionEvent(0, 2, 9, 0, 0) == 0);
    assert(LiSendControllerMotionFrame(0, 0, 0, 0, 1, 2, 3) != 0);
    assert(ctx->pendingGyroStops == 0);
    for (int i = 0; i < MAX_QUEUED_MOTION_PACKETS; i++) freePacketHolder(ctx, popMotion(true));
    assert(setControllerMotionEnabled(0, LI_MOTION_TYPE_ACCEL, true) == 0);

    // Demand and generations are per sensor and per slot. A disabled gyro still
    // owes its stop, whereas removing its host slot cancels all remaining work.
    enable(1);
    assert(LiSendControllerMotionEvent(0, 2, 10, 0, 0) == 0);
    assert(setControllerMotionEnabled(0, 2, 0) == 0);
    assert(takePendingMotionStop(ctx, &oldStop) && motionStopIsCurrent(ctx, &oldStop));
    assert(LiSendControllerMotionEvent(0, 2, 99, 0, 0) == 0);
    assert(LiSendControllerMotionEvent(1, 2, 11, 0, 0) == 0);
    assert(LiResetControllerMotionEvents(0, true) == 0);
    assert(!motionStopIsCurrent(ctx, &oldStop));
    freePacketHolder(ctx, popMotion(false));
    fresh = popMotion(true);
    assert(fresh->packet.controllerMotion.controllerNumber == 1);
    freePacketHolder(ctx, fresh);
    assert(setControllerMotionEnabled(256, 2, true) == -3);
    assert(setControllerMotionEnabled(0, 3, true) == -3);
    assert(setControllerMotionEnabled(0, 0, true) == -3);
    assert(LiSendControllerMotionEvent(0, 0, 0, 0, 0) == -3);
    uint64_t sequence = ctx->currentGamepadSensorState[0][1].sequence;
    assert(LiSendControllerMotionFrame(16, 1, 0, 0, 0, 0, 0) == -3);
    assert(LiSendControllerMotionFrame(255, 1, 0, 0, 0, 0, 0) == -3);
    assert(ctx->currentGamepadSensorState[0][1].sequence == sequence);
    assert(LiStopControllerMotionEvent(16) == -3);
    assert(LiResetControllerMotionEvents(16, false) == -3);
    ctx->initialized = false;
    assert(LiSendControllerMotionFrame(0, 0, 0, 0, 0, 0, 0) == -2);
    LbqSignalQueueShutdown(&ctx->packetQueue);
    LbqSignalQueueShutdown(&ctx->packetHolderFreeList);
    destroyInputStream();
    assert(setControllerMotionEnabled(0, 2, true) == -2);
    puts("Shared motion queue, frame, pressure and stop lifecycle checks passed");
    return 0;
}
