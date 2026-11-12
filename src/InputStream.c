
#include "Limelight-internal.h"
#include <stddef.h>

#ifdef StreamConfig
#undef StreamConfig
#endif
#ifdef SunshineFeatureFlags
#undef SunshineFeatureFlags
#endif
#ifdef ListenerCallbacks
#undef ListenerCallbacks
#endif
#ifdef RemoteAddr
#undef RemoteAddr
#endif
#ifdef AddrLen
#undef AddrLen
#endif
#ifdef ConnectionInterrupted
#undef ConnectionInterrupted
#endif

#define AppVersionQuad (ctx->connectionContext->AppVersionQuad)
#define StreamConfig (ctx->connectionContext->StreamConfig)
#define SunshineFeatureFlags (ctx->connectionContext->SunshineFeatureFlags)
#define ListenerCallbacks (ctx->connectionContext->ListenerCallbacks)
#define RemoteAddr (ctx->connectionContext->RemoteAddr)
#define AddrLen (ctx->connectionContext->AddrLen)
#define ConnectionInterrupted (ctx->connectionContext->ConnectionInterrupted)

// Limited by number of bits in activeGamepadMask
#define MAX_GAMEPADS 16

// Accelerometer and gyro
#define MAX_MOTION_EVENTS 2

#define CLAMP(val, min, max)                                                   \
  (((val) < (min)) ? (min) : (((val) > (max)) ? (max) : (val)))

#define MAX_INPUT_PACKET_SIZE 128
#define INPUT_STREAM_TIMEOUT_SEC 10

#define MAX_QUEUED_INPUT_PACKETS 150
// Keep the old motion queue's maximum footprint; buttons/releases retain queue capacity.
#define MAX_QUEUED_MOTION_PACKETS (MAX_GAMEPADS * MAX_MOTION_EVENTS)

#define PAYLOAD_SIZE(x) BE32((x)->packet.header.size)
#define PACKET_SIZE(x) (PAYLOAD_SIZE(x) + sizeof(uint32_t))

// Matches Win32 WHEEL_DELTA definition
#define LI_WHEEL_DELTA 120

// If we try to send more than one gamepad or mouse motion event
// per millisecond, we'll wait a little bit to try to batch with
// the next one. This batching wait paradoxically _decreases_
// effective input latency by avoiding packet queuing in ENet.
#define CONTROLLER_BATCHING_INTERVAL_MS 1
#define MOUSE_BATCHING_INTERVAL_MS 1
#define PEN_BATCHING_INTERVAL_MS 1
#define TOUCHPAD_FLUSH_INTERVAL_MS 1

// Don't batch up/down/cancel events
#define TOUCH_EVENT_IS_BATCHABLE(x)                                            \
  ((x) == LI_TOUCH_EVENT_HOVER || (x) == LI_TOUCH_EVENT_MOVE)

// Contains input stream packets
typedef struct _PACKET_HOLDER {
  LINKED_BLOCKING_QUEUE_ENTRY entry;
  uint32_t enetPacketFlags;
  uint8_t channelId;
  uint64_t motionSequence;
  uint64_t scrollTraceId;
  uint64_t scrollTraceStartMs;
  uint64_t scrollTraceLocalDispatchMs;
  uint64_t scrollTraceQueuedMs;

  // The union must be the last member since we abuse the NV_UNICODE_PACKET
  // text field to store variable length data which gets split before being
  // sent to the host.
  union {
    NV_INPUT_HEADER header;
    NV_KEYBOARD_PACKET keyboard;
    NV_REL_MOUSE_MOVE_PACKET mouseMoveRel;
    NV_ABS_MOUSE_MOVE_PACKET mouseMoveAbs;
    NV_MOUSE_BUTTON_PACKET mouseButton;
    NV_CONTROLLER_PACKET controller;
    NV_MULTI_CONTROLLER_PACKET multiController;
    NV_SCROLL_PACKET scroll;
    SS_HSCROLL_PACKET hscroll;
    NV_HAPTICS_PACKET haptics;
    SS_TOUCH_PACKET touch;
    SS_TOUCHPAD_PACKET touchpad;
    SS_TOUCHPAD_FRAME_PACKET touchpadFrame;
    SS_PEN_PACKET pen;
    SS_CONTROLLER_ARRIVAL_PACKET controllerArrival;
    SS_CONTROLLER_TOUCH_PACKET controllerTouch;
    SS_CONTROLLER_MOTION_PACKET controllerMotion;
    SS_CONTROLLER_BATTERY_PACKET controllerBattery;
    SS_MICROPHONE_PACKET microphone;
    NV_UNICODE_PACKET unicode;
  } packet;
} PACKET_HOLDER, *PPACKET_HOLDER;

static void attachScrollTraceMetadata(PML_INPUT_STREAM_CONTEXT ctx,
                                      PPACKET_HOLDER holder,
                                      uint64_t queuedMs) {
  if (ctx == NULL || holder == NULL) {
    return;
  }

  holder->scrollTraceId = LiGetScrollTraceIdCtx(ctx);
  holder->scrollTraceStartMs = LiGetScrollTraceStartMsCtx(ctx);
  holder->scrollTraceLocalDispatchMs = LiGetScrollTraceLocalDispatchMsCtx(ctx);
  holder->scrollTraceQueuedMs = queuedMs;
}

// Initializes the input stream
int initializeInputStreamCtx(PML_INPUT_STREAM_CONTEXT ctx, PML_CONNECTION_CONTEXT connectionContext) {
  ctx->connectionContext = connectionContext;
  ctx->inputSock = INVALID_SOCKET;
  ctx->initialized = false;
  memcpy(ctx->currentAesIv, StreamConfig.remoteInputAesIv, sizeof(ctx->currentAesIv));

  // Set a high maximum queue size limit to ensure input isn't dropped
  // while the input send thread is blocked for short periods.
  LbqInitializeLinkedBlockingQueue(&ctx->packetQueue, MAX_QUEUED_INPUT_PACKETS);
  LbqInitializeLinkedBlockingQueue(&ctx->packetHolderFreeList,
                                   MAX_QUEUED_INPUT_PACKETS);

  ctx->cryptoContext = PltCreateCryptoContext();
  ctx->encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431);

  // FIXME: Unsure if this is exactly right, but it's probably good enough.
  //
  // GFE 3.13.1.30 is not using NVVHCI for mouse/keyboard (and is confirmed
  // unaffected) GFE 3.15.0.164 seems to be the first release using NVVHCI for
  // mouse/keyboard
  //
  // Sunshine also uses SendInput() so it's not affected either.
  ctx->needsBatchedScroll = APP_VERSION_AT_LEAST(7, 1, 409) && !IS_SUNSHINE();
  ctx->batchedScrollDelta = 0;
  atomic_init(&ctx->scrollTraceLoggingEnabled, false);
  atomic_init(&ctx->scrollTraceAwaitingRender, false);
  atomic_init(&ctx->scrollTraceLastDispatchHighRes, false);
  atomic_init(&ctx->scrollTraceLastDispatchHorizontal, false);
  atomic_init(&ctx->scrollTraceId, 0);
  atomic_init(&ctx->scrollTraceStartMs, 0);
  atomic_init(&ctx->scrollTraceLastLocalDispatchMs, 0);
  atomic_init(&ctx->scrollTraceLastQueuedMs, 0);
  atomic_init(&ctx->scrollTraceLastSentMs, 0);
  atomic_init(&ctx->scrollTraceLastDispatchAmount, 0);

  ctx->currentPenButtonState = 0;
  ctx->currentTouchpadButtonState = 0;

  // Start with the virtual mouse centered
  ctx->absCurrentPosX = ctx->absCurrentPosY = 0.5f;

  memset(ctx->currentGamepadSensorState, 0, sizeof(ctx->currentGamepadSensorState));
  ctx->pendingGyroStops = 0;
  ctx->queuedMotionPackets = 0;
  memset(&ctx->currentRelativeMouseState, 0, sizeof(ctx->currentRelativeMouseState));
  memset(&ctx->currentAbsoluteMouseState, 0, sizeof(ctx->currentAbsoluteMouseState));
  PltCreateMutex(&ctx->batchedInputMutex);
  atomic_store(&ctx->motionStateReady, true);

  return 0;
}

uint32_t LiGetInputContextStructSize(void) {
  return (uint32_t)sizeof(ML_INPUT_STREAM_CONTEXT);
}

uint32_t LiGetInputContextOffsetInitialized(void) {
  return (uint32_t)offsetof(ML_INPUT_STREAM_CONTEXT, initialized);
}

uint32_t LiGetInputContextOffsetConnectionContext(void) {
  return (uint32_t)offsetof(ML_INPUT_STREAM_CONTEXT, connectionContext);
}

int LiInputContextIsInitialized(PML_INPUT_STREAM_CONTEXT ctx) {
  return (ctx != NULL && ctx->initialized) ? 1 : 0;
}

void* LiInputContextGetConnectionCtx(PML_INPUT_STREAM_CONTEXT ctx) {
  return ctx != NULL ? (void*)ctx->connectionContext : NULL;
}

static inline PML_INPUT_STREAM_CONTEXT LiGetEffectiveInputContext(void) {
  return &LiGetEffectiveConnectionContext()->inputContext;
}

int initializeInputStream(void) {
    PML_CONNECTION_CONTEXT ctx = LiGetEffectiveConnectionContext();
    return initializeInputStreamCtx(&ctx->inputContext, ctx);
}

// Destroys and cleans up the input stream
void destroyInputStreamCtx(PML_INPUT_STREAM_CONTEXT ctx) {
  PLINKED_BLOCKING_QUEUE_ENTRY entry, nextEntry;
  atomic_store(&ctx->motionStateReady, false);

  PltDestroyCryptoContext(ctx->cryptoContext);

  entry = LbqDestroyLinkedBlockingQueue(&ctx->packetQueue);

  while (entry != NULL) {
    nextEntry = entry->flink;

    // The entry is stored in the data buffer
    free(entry->data);

    entry = nextEntry;
  }

  entry = LbqDestroyLinkedBlockingQueue(&ctx->packetHolderFreeList);

  while (entry != NULL) {
    nextEntry = entry->flink;

    // The entry is stored in the data buffer
    free(entry->data);

    entry = nextEntry;
  }

  PltDeleteMutex(&ctx->batchedInputMutex);
}

void destroyInputStream(void) {
    destroyInputStreamCtx(LiGetEffectiveInputContext());
}

static int encryptData(PML_INPUT_STREAM_CONTEXT ctx, unsigned char *plaintext, int plaintextLen,
                       unsigned char *ciphertext, int *ciphertextLen) {
  // Starting in Gen 7, AES GCM is used for encryption
  if (AppVersionQuad[0] >= 7) {
    if (!PltEncryptMessage(ctx->cryptoContext, ALGORITHM_AES_GCM, 0,
                           (unsigned char *)StreamConfig.remoteInputAesKey,
                           sizeof(StreamConfig.remoteInputAesKey), ctx->currentAesIv,
                           sizeof(ctx->currentAesIv), ciphertext, 16, plaintext,
                           plaintextLen, &ciphertext[16], ciphertextLen)) {
      return -1;
    }

    // Increment the ciphertextLen to account for the tag
    *ciphertextLen += 16;
    return 0;
  } else {
    // PKCS7 padding may need to be added in-place, so we must copy this into a
    // buffer that can safely be modified.
    unsigned char paddedData[ROUND_TO_PKCS7_PADDED_LEN(MAX_INPUT_PACKET_SIZE)];

    memcpy(paddedData, plaintext, plaintextLen);

    // Prior to Gen 7, 128-bit AES CBC is used for encryption with each message
    // padded to the block size to ensure messages are not delayed within the
    // cipher.
    return PltEncryptMessage(
               ctx->cryptoContext, ALGORITHM_AES_CBC, CIPHER_FLAG_PAD_TO_BLOCK_SIZE,
               (unsigned char *)StreamConfig.remoteInputAesKey,
               sizeof(StreamConfig.remoteInputAesKey), ctx->currentAesIv,
               sizeof(ctx->currentAesIv), NULL, 0, paddedData, plaintextLen,
               ciphertext, ciphertextLen)
               ? 0
               : -1;
  }
}

static void freePacketHolder(PML_INPUT_STREAM_CONTEXT ctx, PPACKET_HOLDER holder) {
  LC_ASSERT(holder->packet.header.size != 0);

  // Place the packet holder back into the free list if it's a standard size
  // entry
  if (PACKET_SIZE(holder) > (int)sizeof(*holder) ||
      LbqOfferQueueItem(&ctx->packetHolderFreeList, holder, &holder->entry) !=
          LBQ_SUCCESS) {
    free(holder);
  }
}

static PPACKET_HOLDER allocatePacketHolder(PML_INPUT_STREAM_CONTEXT ctx, int extraLength) {
  PPACKET_HOLDER holder;
  int err;

  // If we're using an extended packet holder, we can't satisfy
  // this allocation from the packet holder free list.
  if (extraLength > 0) {
    // We over-allocate here a bit since we're always adding sizeof(*holder),
    // but this is on purpose. It allows us assume we have a full holder even
    // if packetLength < sizeof(*holder) and put this allocation into the free
    // list.
    return malloc(sizeof(*holder) + extraLength);
  }

  // Grab an entry from the free list (if available)
  err = LbqPollQueueElement(&ctx->packetHolderFreeList, (void **)&holder);
  if (err == LBQ_SUCCESS) {
    return holder;
  } else if (err == LBQ_INTERRUPTED) {
    // We're shutting down. Don't bother allocating.
    return NULL;
  } else {
    LC_ASSERT(err == LBQ_NO_ELEMENT);

    // Otherwise we'll have to allocate
    return malloc(sizeof(*holder));
  }
}

static bool scrollPacketInfoFromHolder(PPACKET_HOLDER holder, bool* horizontal, short* amount) {
  if (holder == NULL || horizontal == NULL || amount == NULL) {
    return false;
  }

  if (holder->packet.header.magic == LE32(SS_HSCROLL_MAGIC)) {
    *horizontal = true;
    *amount = (short)BE16(holder->packet.hscroll.scrollAmount);
    return true;
  }

  if (holder->packet.header.magic == LE32(SCROLL_MAGIC) ||
      holder->packet.header.magic == LE32(SCROLL_MAGIC_GEN5)) {
    *horizontal = false;
    *amount = (short)BE16(holder->packet.scroll.scrollAmt1);
    return true;
  }

  return false;
}

static void logScrollTraceAccumulation(PML_INPUT_STREAM_CONTEXT ctx,
                                       bool horizontal,
                                       short amount,
                                       int batchedDelta) {
  uint64_t traceId;
  uint64_t nowMs;
  uint64_t startMs;
  uint64_t localDispatchMs;
  unsigned long long startAgeMs;
  unsigned long long localAgeMs;

  traceId = LiGetScrollTraceIdCtx(ctx);
  if (traceId == 0) {
    return;
  }

  nowMs = LiGetMillis();
  startMs = LiGetScrollTraceStartMsCtx(ctx);
  localDispatchMs = LiGetScrollTraceLocalDispatchMsCtx(ctx);
  startAgeMs = startMs != 0 && nowMs >= startMs ? nowMs - startMs : 0;
  localAgeMs = localDispatchMs != 0 && nowMs >= localDispatchMs ? nowMs - localDispatchMs : 0;

  Limelog("[inputdiag] scroll-trace accumulate trace=%llu axis=%c amount=%d batched=%d threshold=%d startAge=%llums localAge=%llums\n",
          (unsigned long long)traceId,
          horizontal ? 'H' : 'V',
          (int)amount,
          batchedDelta,
          LI_WHEEL_DELTA,
          startAgeMs,
          localAgeMs);
}

static void logScrollTracePacketStage(PML_INPUT_STREAM_CONTEXT ctx,
                                      const char* stage,
                                      PPACKET_HOLDER holder,
                                      int queueDepth,
                                      bool moreData) {
  bool horizontal;
  short amount;
  uint64_t nowMs;
  uint64_t traceId;
  uint64_t startMs;
  uint64_t localDispatchMs;
  uint64_t queuedMs;
  uint64_t sentMs;
  unsigned long long startAgeMs;
  unsigned long long localAgeMs;
  unsigned long long queueAgeMs;
  unsigned long long sentAgeMs;

  if (!scrollPacketInfoFromHolder(holder, &horizontal, &amount)) {
    return;
  }

  traceId = LiGetScrollTraceIdCtx(ctx);
  if (holder != NULL && holder->scrollTraceId != 0) {
    traceId = holder->scrollTraceId;
    startMs = holder->scrollTraceStartMs;
    localDispatchMs = holder->scrollTraceLocalDispatchMs;
    queuedMs = holder->scrollTraceQueuedMs;
  } else {
    traceId = LiGetScrollTraceIdCtx(ctx);
    if (traceId == 0) {
      return;
    }

    startMs = LiGetScrollTraceStartMsCtx(ctx);
    localDispatchMs = LiGetScrollTraceLocalDispatchMsCtx(ctx);
    queuedMs = LiGetScrollTraceQueuedMsCtx(ctx);
  }

  nowMs = LiGetMillis();
  sentMs = LiGetScrollTraceSentMsCtx(ctx);
  startAgeMs = startMs != 0 && nowMs >= startMs ? nowMs - startMs : 0;
  localAgeMs = localDispatchMs != 0 && nowMs >= localDispatchMs ? nowMs - localDispatchMs : 0;
  queueAgeMs = queuedMs != 0 && nowMs >= queuedMs ? nowMs - queuedMs : 0;
  sentAgeMs = sentMs != 0 && nowMs >= sentMs ? nowMs - sentMs : 0;

  Limelog("[inputdiag] scroll-trace %s trace=%llu axis=%c amount=%d queueDepth=%d more=%d startAge=%llums localAge=%llums queueAge=%llums sendAge=%llums\n",
          stage != NULL ? stage : "unknown",
          (unsigned long long)traceId,
          horizontal ? 'H' : 'V',
          (int)amount,
          queueDepth,
          moreData ? 1 : 0,
          startAgeMs,
          localAgeMs,
          queueAgeMs,
          sentAgeMs);
}

static bool sendInputPacket(PML_INPUT_STREAM_CONTEXT ctx, PPACKET_HOLDER holder, bool moreData) {
  SOCK_RET err;

  // On GFE 3.22, the entire control stream is encrypted (and support for
  // separate RI encrypted) has been removed. We send the plaintext packet
  // through and the control stream code will do the encryption.
  if (ctx->encryptedControlStream) {
    // Use the context version if possible, otherwise rely on the global one (for now)
    // or we assume ctx->controlContext is set?
    // For now, let's keep using the global one if controlContext is null, or use the macro...
    // Wait, the goal is to remove global dependency.
    // If ctx->controlContext is set, use it.
    if (ctx->connectionContext) {
        err = (SOCK_RET)sendInputPacketOnControlStreamCtx(
            &ctx->connectionContext->controlContext,
            (unsigned char *)&holder->packet, PACKET_SIZE(holder),
            holder->channelId, holder->enetPacketFlags, moreData);
    } else {
        // Fallback to global for legacy compatibility
        err = (SOCK_RET)sendInputPacketOnControlStream(
            (unsigned char *)&holder->packet, PACKET_SIZE(holder),
            holder->channelId, holder->enetPacketFlags, moreData);
    }

    if (err < 0) {
      Limelog("Input: sendInputPacketOnControlStream() failed: %d\n", (int)err);
      ListenerCallbacks.connectionTerminated(err);
      return false;
    }
  } else {
    char encryptedBuffer[MAX_INPUT_PACKET_SIZE];
    uint32_t encryptedSize;
    uint32_t encryptedLengthPrefix;

    // Encrypt the message into the output buffer while leaving room for the
    // length
    encryptedSize = sizeof(encryptedBuffer) - sizeof(encryptedLengthPrefix);
    err = encryptData(
        ctx,
        (unsigned char *)&holder->packet, PACKET_SIZE(holder),
        (unsigned char *)&encryptedBuffer[sizeof(encryptedLengthPrefix)],
        (int *)&encryptedSize);
    if (err != 0) {
      Limelog("Input: Encryption failed: %d\n", (int)err);
      ListenerCallbacks.connectionTerminated(err);
      return false;
    }

    // Prepend the length to the message
    encryptedLengthPrefix = BE32(encryptedSize);
    memcpy(&encryptedBuffer[0], &encryptedLengthPrefix,
           sizeof(encryptedLengthPrefix));

    if (AppVersionQuad[0] < 5) {
      // Send the encrypted payload
      err = send(ctx->inputSock, (const char *)encryptedBuffer,
                 (int)(encryptedSize + sizeof(encryptedLengthPrefix)), 0);
      if (err <= 0) {
        Limelog("Input: send() failed: %d\n", (int)LastSocketError());
        ListenerCallbacks.connectionTerminated(LastSocketFail());
        return false;
      }
    } else {
      // For reasons that I can't understand, NVIDIA decides to use the last 16
      // bytes of ciphertext in the most recent game controller packet as the IV
      // for future encryption. I think it may be a buffer overrun on their end
      // but we'll have to mimic it to work correctly.
      if (AppVersionQuad[0] >= 7 &&
          encryptedSize >= 16 + sizeof(ctx->currentAesIv)) {
        memcpy(ctx->currentAesIv,
               &encryptedBuffer[4 + encryptedSize - sizeof(ctx->currentAesIv)],
               sizeof(ctx->currentAesIv));
      }

      if (ctx->connectionContext) {
          err = (SOCK_RET)sendInputPacketOnControlStreamCtx(
              &ctx->connectionContext->controlContext,
              (unsigned char *)encryptedBuffer,
              (int)(encryptedSize + sizeof(encryptedLengthPrefix)),
              holder->channelId, holder->enetPacketFlags, moreData);
      } else {
          err = (SOCK_RET)sendInputPacketOnControlStream(
              (unsigned char *)encryptedBuffer,
              (int)(encryptedSize + sizeof(encryptedLengthPrefix)),
              holder->channelId, holder->enetPacketFlags, moreData);
      }

      if (err < 0) {
        Limelog("Input: sendInputPacketOnControlStream() failed: %d\n",
                (int)err);
        ListenerCallbacks.connectionTerminated(err);
        return false;
      }
    }
  }

  return true;
}

static uint16_t normalizedFloatToUint16(float in) {
  float clamped = CLAMP(in, 0.0f, 1.0f);
  return (uint16_t)(clamped * 65535.0f + 0.5f);
}

static void floatToNetfloat(float in, netfloat out) {
  if (IS_LITTLE_ENDIAN()) {
    memcpy(out, &in, sizeof(in));
  } else {
    uint8_t *inb = (uint8_t *)&in;
    out[0] = inb[3];
    out[1] = inb[2];
    out[2] = inb[1];
    out[3] = inb[0];
  }
}

// Input thread proc
static void initializeMotionPacket(PPACKET_HOLDER holder, uint8_t controller, uint8_t type,
                                   float x, float y, float z) {
  holder->channelId = CTRL_CHANNEL_SENSOR_BASE + controller;
  holder->enetPacketFlags = type == LI_MOTION_TYPE_GYRO && x == 0 && y == 0 && z == 0
      ? ENET_PACKET_FLAG_RELIABLE : 0;
  holder->packet.controllerMotion.header.size = BE32(sizeof(SS_CONTROLLER_MOTION_PACKET) - sizeof(uint32_t));
  holder->packet.controllerMotion.header.magic = LE32(SS_CONTROLLER_MOTION_MAGIC);
  holder->packet.controllerMotion.controllerNumber = controller;
  holder->packet.controllerMotion.motionType = type;
  memset(holder->packet.controllerMotion.zero, 0, sizeof(holder->packet.controllerMotion.zero));
  floatToNetfloat(x, holder->packet.controllerMotion.x);
  floatToNetfloat(y, holder->packet.controllerMotion.y);
  floatToNetfloat(z, holder->packet.controllerMotion.z);
}

// Caller holds batchedInputMutex. Stops use a fixed slot, even when the input queue is full.
static void stopControllerMotionLocked(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controller) {
  if (!ctx->currentGamepadSensorState[controller][1].mayBeMoving ||
      ctx->currentGamepadSensorState[controller][1].stopPending) return;
  ctx->currentGamepadSensorState[controller][1].discardThrough = ++ctx->currentGamepadSensorState[controller][1].sequence;
  ctx->currentGamepadSensorState[controller][1].stopPending = true;
  ctx->pendingGyroStops |= 1u << controller;
  LbqSignalQueueUserWake(&ctx->packetQueue);
}

static bool takePendingMotionStop(PML_INPUT_STREAM_CONTEXT ctx, PPACKET_HOLDER holder) {
  bool found = false;
  PltLockMutex(&ctx->batchedInputMutex);
  for (uint8_t i = 0; i < MAX_GAMEPADS; i++) {
    if (ctx->pendingGyroStops & (1u << i)) {
      ctx->pendingGyroStops &= ~(1u << i);
      initializeMotionPacket(holder, i, LI_MOTION_TYPE_GYRO, 0, 0, 0);
      holder->motionSequence = ctx->currentGamepadSensorState[i][1].sequence;
      found = true;
      break;
    }
  }
  PltUnlockMutex(&ctx->batchedInputMutex);
  return found;
}

static bool prepareQueuedMotionPacket(PML_INPUT_STREAM_CONTEXT ctx, PPACKET_HOLDER holder) {
  uint8_t controller = holder->packet.controllerMotion.controllerNumber;
  uint8_t sensor = holder->packet.controllerMotion.motionType - 1;
  bool current;
  PltLockMutex(&ctx->batchedInputMutex);
  LC_ASSERT(ctx->queuedMotionPackets > 0);
  ctx->queuedMotionPackets--;
  current = ctx->currentGamepadSensorState[controller][sensor].enabled &&
      holder->motionSequence > ctx->currentGamepadSensorState[controller][sensor].discardThrough;
  if (current && sensor == 1 && holder->motionSequence == ctx->currentGamepadSensorState[controller][sensor].sequence &&
      holder->enetPacketFlags == ENET_PACKET_FLAG_RELIABLE) {
    ctx->currentGamepadSensorState[controller][sensor].mayBeMoving = false;
  }
  PltUnlockMutex(&ctx->batchedInputMutex);
  return current;
}

static bool motionStopIsCurrent(PML_INPUT_STREAM_CONTEXT ctx, PPACKET_HOLDER holder) {
  uint8_t controller = holder->packet.controllerMotion.controllerNumber;
  bool current;
  PltLockMutex(&ctx->batchedInputMutex);
  current = ctx->currentGamepadSensorState[controller][1].stopPending &&
      holder->motionSequence == ctx->currentGamepadSensorState[controller][1].sequence;
  PltUnlockMutex(&ctx->batchedInputMutex);
  return current;
}

static void finishMotionStop(PML_INPUT_STREAM_CONTEXT ctx, PPACKET_HOLDER holder) {
  uint8_t controller = holder->packet.controllerMotion.controllerNumber;
  PltLockMutex(&ctx->batchedInputMutex);
  if (holder->motionSequence == ctx->currentGamepadSensorState[controller][1].sequence) {
    ctx->currentGamepadSensorState[controller][1].stopPending = false;
    ctx->currentGamepadSensorState[controller][1].mayBeMoving = false;
  }
  PltUnlockMutex(&ctx->batchedInputMutex);
}

// Returns the length in bytes of the UTF-8 code point at the given offset in the text,
// or 0 if the byte at that offset does not start a valid UTF-8 code point.
static uint32_t getUtf8CodePointLength(PML_INPUT_STREAM_CONTEXT ctx, const char* utf8Text, uint32_t offset) {
  uint8_t firstByte = (uint8_t)utf8Text[offset];
  uint32_t codePointLength;

  if ((firstByte & 0x80) == 0x00) {
    // 1 byte code point
    codePointLength = 1;
  } else if ((firstByte & 0xE0) == 0xC0) {
    // 2 byte code point
    codePointLength = 2;
  } else if ((firstByte & 0xF0) == 0xE0) {
    // 3 byte code point
    codePointLength = 3;
  } else if ((firstByte & 0xF8) == 0xF0) {
    // 4 byte code point
    codePointLength = 4;
  } else {
    Limelog("Invalid unicode code point starting byte: %02x\n", firstByte);
    return 0;
  }

  return codePointLength;
}

static void inputSendThreadProc(void *context) {
  PML_INPUT_STREAM_CONTEXT ctx = (PML_INPUT_STREAM_CONTEXT)context;
  LiSetThreadConnectionContext(ctx->connectionContext);
  SOCK_RET err;
  PPACKET_HOLDER holder;
  uint32_t multiControllerMagicLE;
  uint32_t relMouseMagicLE;

  if (AppVersionQuad[0] >= 5) {
    multiControllerMagicLE = LE32(MULTI_CONTROLLER_MAGIC_GEN5);
    relMouseMagicLE = LE32(MOUSE_MOVE_REL_MAGIC_GEN5);
  } else {
    multiControllerMagicLE = LE32(MULTI_CONTROLLER_MAGIC);
    relMouseMagicLE = LE32(MOUSE_MOVE_REL_MAGIC);
  }

  uint64_t lastControllerPacketTime[MAX_GAMEPADS] = {0};
  uint64_t lastMousePacketTime = 0;
  uint64_t lastPenPacketTime = 0;
  uint64_t lastTouchpadFlushTime = 0;

  while (!PltIsThreadInterrupted(&ctx->inputSendThread)) {
    bool scrollPacket;
    bool scrollHorizontal;
    bool moreData;
    short scrollAmount;

    PACKET_HOLDER stop;
    while (takePendingMotionStop(ctx, &stop)) {
      if (motionStopIsCurrent(ctx, &stop)) {
        if (!sendInputPacket(ctx, &stop, false)) return;
        finishMotionStop(ctx, &stop);
      }
    }
    err = LbqWaitForQueueElement(&ctx->packetQueue, (void **)&holder);
    if (err == LBQ_USER_WAKE) continue;
    if (err != LBQ_SUCCESS) {
      return;
    }

    scrollPacket = scrollPacketInfoFromHolder(holder, &scrollHorizontal, &scrollAmount);
    (void)scrollHorizontal;
    (void)scrollAmount;

    // If it's a multi-controller packet we can do batching
    if (holder->packet.header.magic == multiControllerMagicLE) {
      PPACKET_HOLDER controllerBatchHolder;
      PNV_MULTI_CONTROLLER_PACKET origPkt;
      short controllerNumber =
          LE16(holder->packet.multiController.controllerNumber);
      uint64_t now = PltGetMillis();

      LC_ASSERT(controllerNumber < MAX_GAMEPADS);

      // Delay for batching if required
      if (now < lastControllerPacketTime[controllerNumber] +
                    CONTROLLER_BATCHING_INTERVAL_MS) {
        flushInputOnControlStreamCtx(&ctx->connectionContext->controlContext);
        PltSleepMs((int)(lastControllerPacketTime[controllerNumber] +
                         CONTROLLER_BATCHING_INTERVAL_MS - now));
        now = PltGetMillis();
      }

      origPkt = &holder->packet.multiController;
      for (;;) {
        PNV_MULTI_CONTROLLER_PACKET newPkt;

        // Peek at the next packet
        if (LbqPeekQueueElement(
          &ctx->packetQueue, (void **)&controllerBatchHolder) != LBQ_SUCCESS) {
          break;
        }

        // If it's not a controller packet, we're done
        if (controllerBatchHolder->packet.header.magic !=
            multiControllerMagicLE) {
          break;
        }

        // Check if it's able to be batched
        // NB: GFE does some discarding of gamepad packets received very soon
        // after another. Thus, this batching is needed for correctness in some
        // cases, as GFE will inexplicably drop *newer* packets in that
        // scenario. The brokenness can be tested with consecutive calls to
        // LiSendMultiControllerEvent() with different values for analog sticks
        // (max -> zero).
        newPkt = &controllerBatchHolder->packet.multiController;
        if (newPkt->buttonFlags != origPkt->buttonFlags ||
            newPkt->buttonFlags2 != origPkt->buttonFlags2 ||
            newPkt->controllerNumber != origPkt->controllerNumber ||
            newPkt->activeGamepadMask != origPkt->activeGamepadMask) {
          // Batching not allowed
          break;
        }

        // Remove the batchable controller packet
        if (LbqPollQueueElement(
          &ctx->packetQueue, (void **)&controllerBatchHolder) != LBQ_SUCCESS) {
          break;
        }

        // Update the original packet
        origPkt->leftTrigger = newPkt->leftTrigger;
        origPkt->rightTrigger = newPkt->rightTrigger;
        origPkt->leftStickX = newPkt->leftStickX;
        origPkt->leftStickY = newPkt->leftStickY;
        origPkt->rightStickX = newPkt->rightStickX;
        origPkt->rightStickY = newPkt->rightStickY;

        // Free the batched packet holder
        freePacketHolder(ctx, controllerBatchHolder);
      }

      lastControllerPacketTime[controllerNumber] = now;
    }
    // If it's a relative mouse move packet, we can also do batching
    else if (holder->packet.header.magic == relMouseMagicLE) {
      uint64_t now = PltGetMillis();

      // Delay for batching if required
      if (now < lastMousePacketTime + MOUSE_BATCHING_INTERVAL_MS) {
        flushInputOnControlStreamCtx(&ctx->connectionContext->controlContext);
        PltSleepMs(
            (int)(lastMousePacketTime + MOUSE_BATCHING_INTERVAL_MS - now));
        now = PltGetMillis();
      }

      PltLockMutex(&ctx->batchedInputMutex);

      // Send as many packets as it takes to get the entire delta through
      while (ctx->currentRelativeMouseState.deltaX != 0 ||
             ctx->currentRelativeMouseState.deltaY != 0) {
        bool more = false;

        if (ctx->currentRelativeMouseState.deltaX < INT16_MIN) {
          holder->packet.mouseMoveRel.deltaX = BE16(INT16_MIN);
          ctx->currentRelativeMouseState.deltaX -= INT16_MIN;
          more = true;
        } else if (ctx->currentRelativeMouseState.deltaX > INT16_MAX) {
          holder->packet.mouseMoveRel.deltaX = BE16(INT16_MAX);
          ctx->currentRelativeMouseState.deltaX -= INT16_MAX;
          more = true;
        } else {
          holder->packet.mouseMoveRel.deltaX =
              BE16(ctx->currentRelativeMouseState.deltaX);
          ctx->currentRelativeMouseState.deltaX = 0;
        }

        if (ctx->currentRelativeMouseState.deltaY < INT16_MIN) {
          holder->packet.mouseMoveRel.deltaY = BE16(INT16_MIN);
          ctx->currentRelativeMouseState.deltaY -= INT16_MIN;
          more = true;
        } else if (ctx->currentRelativeMouseState.deltaY > INT16_MAX) {
          holder->packet.mouseMoveRel.deltaY = BE16(INT16_MAX);
          ctx->currentRelativeMouseState.deltaY -= INT16_MAX;
          more = true;
        } else {
          holder->packet.mouseMoveRel.deltaY =
              BE16(ctx->currentRelativeMouseState.deltaY);
          ctx->currentRelativeMouseState.deltaY = 0;
        }

        // Don't hold the batching lock while we're doing network I/O
        PltUnlockMutex(&ctx->batchedInputMutex);

        // Encrypt and send the split packet
        if (!sendInputPacket(ctx, holder, more)) {
          freePacketHolder(ctx, holder);
          return;
        }

        PltLockMutex(&ctx->batchedInputMutex);
      }

      // The state change is no longer pending
      ctx->currentRelativeMouseState.dirty = false;

      PltUnlockMutex(&ctx->batchedInputMutex);

      lastMousePacketTime = now;

      // We sent everything we needed in the loop above, so we can just free the
      // holder of the original packet and wait for another input event.
      freePacketHolder(ctx, holder);
      continue;
    }
    // If it's an absolute mouse move packet, we should only send the latest
    else if (holder->packet.header.magic == LE32(MOUSE_MOVE_ABS_MAGIC)) {
      uint64_t now = PltGetMillis();

      // Delay for batching if required
      if (now < lastMousePacketTime + MOUSE_BATCHING_INTERVAL_MS) {
        flushInputOnControlStreamCtx(&ctx->connectionContext->controlContext);
        PltSleepMs(
            (int)(lastMousePacketTime + MOUSE_BATCHING_INTERVAL_MS - now));
        now = PltGetMillis();
      }

      PltLockMutex(&ctx->batchedInputMutex);

      // Populate the packet with the latest state
      holder->packet.mouseMoveAbs.x = BE16(ctx->currentAbsoluteMouseState.x);
      holder->packet.mouseMoveAbs.y = BE16(ctx->currentAbsoluteMouseState.y);

      // There appears to be a rounding error in GFE's scaling calculation which
      // prevents the cursor from reaching the far edge of the screen when
      // streaming at smaller resolutions with a higher desktop resolution (like
      // streaming 720p with a desktop resolution of 1080p, or streaming
      // 720p/1080p with a desktop resolution of 4K). Subtracting one from the
      // reference dimensions seems to work around this issue.
        holder->packet.mouseMoveAbs.width =
          BE16(ctx->currentAbsoluteMouseState.width - 1);
        holder->packet.mouseMoveAbs.height =
          BE16(ctx->currentAbsoluteMouseState.height - 1);

      // The state change is no longer pending
      ctx->currentAbsoluteMouseState.dirty = false;

      PltUnlockMutex(&ctx->batchedInputMutex);

      lastMousePacketTime = now;
    }
    // Pace pen motion packets to avoid queuing in ENet. Preserve every contact
    // MOVE sample because dropping them creates visible gaps in drawn strokes.
    // HOVER samples may still be coalesced because no stroke geometry is lost
    // while the pen is raised.
    else if (holder->packet.header.magic == LE32(SS_PEN_MAGIC) &&
             TOUCH_EVENT_IS_BATCHABLE(holder->packet.pen.eventType)) {
      uint64_t now = PltGetMillis();

      // Delay for batching if required
      if (now < lastPenPacketTime + PEN_BATCHING_INTERVAL_MS) {
        flushInputOnControlStreamCtx(&ctx->connectionContext->controlContext);
        PltSleepMs((int)(lastPenPacketTime + PEN_BATCHING_INTERVAL_MS - now));
        now = PltGetMillis();
      }

      if (holder->packet.pen.eventType == LI_TOUCH_EVENT_HOVER) {
        for (;;) {
          PPACKET_HOLDER penBatchHolder;

          // Peek at the next packet
          if (LbqPeekQueueElement(&ctx->packetQueue, (void **)&penBatchHolder) !=
              LBQ_SUCCESS) {
            break;
          }

          // If it's not a pen packet, we're done
          if (penBatchHolder->packet.header.magic != LE32(SS_PEN_MAGIC)) {
            break;
          }

          // If the buttons, event type, or reliability is different, we cannot batch
          if (holder->packet.pen.penButtons !=
                  penBatchHolder->packet.pen.penButtons ||
              holder->packet.pen.eventType !=
                  penBatchHolder->packet.pen.eventType ||
              holder->enetPacketFlags != penBatchHolder->enetPacketFlags) {
            break;
          }

          // Remove the next packet
          if (LbqPollQueueElement(&ctx->packetQueue, (void **)&penBatchHolder) !=
              LBQ_SUCCESS) {
            break;
          }

          // Replace the current packet with the new one
          freePacketHolder(ctx, holder);
          holder = penBatchHolder;
        }
      }

      lastPenPacketTime = now;
    }
    // Motion values are immutable after enqueue, including ordinary zero samples.
    else if (holder->packet.header.magic == LE32(SS_CONTROLLER_MOTION_MAGIC)) {
      if (!prepareQueuedMotionPacket(ctx, holder)) {
        freePacketHolder(ctx, holder);
        if (LbqGetItemCount(&ctx->packetQueue) == 0) {
          flushInputOnControlStreamCtx(&ctx->connectionContext->controlContext);
        }
        continue;
      }
    }
    // If it's a UTF-8 text packet, we may need to split it into a several
    // packets to send
    else if (holder->packet.header.magic == LE32(UTF8_TEXT_EVENT_MAGIC)) {
      PACKET_HOLDER splitPacket;
      uint32_t totalLength = PAYLOAD_SIZE(holder) - sizeof(uint32_t);
      uint32_t i = 0;

      if (!IS_SUNSHINE()) {
        // HACK: This is a workaround for the fact that GFE doesn't appear to
        // synchronize keyboard and UTF-8 text events with each other. We need to
        // make sure any previous keyboard events have been processed prior to
        // sending these UTF-8 events to avoid interference between the two
        // (especially with modifier keys).
        flushInputOnControlStreamCtx(&ctx->connectionContext->controlContext);
        while (!ConnectionInterrupted &&
               !PltIsThreadInterrupted(&ctx->inputSendThread) &&
               isControlDataInTransitCtx(&ctx->connectionContext->controlContext)) {
          PltSleepMs(10);
        }

        // stopInputStream() drains the queue rather than interrupting this thread.
        // Do not wait for host ACKs during teardown, but keep draining key releases.
        if (ConnectionInterrupted) {
          freePacketHolder(ctx, holder);
          continue;
        }

        // Finally, sleep an additional 50 ms to allow the events to be processed
        // by Windows
        PltSleepMs(50);
      }

      while (i < totalLength && !ConnectionInterrupted &&
             !PltIsThreadInterrupted(&ctx->inputSendThread)) {
        uint32_t chunkLength = 0;
        uint32_t remaining = totalLength - i;

        if (IS_SUNSHINE()) {
          // Sunshine accepts multiple code points per packet, so we pack as many as
          // fit within the NV_UNICODE_PACKET to send fewer packets overall. Code points
          // are never split across packet boundaries.
          while (chunkLength < remaining) {
            uint32_t codePointLength = getUtf8CodePointLength(ctx, holder->packet.unicode.text, i + chunkLength);
            if (codePointLength == 0) {
              break;
            }
            if (chunkLength + codePointLength > UTF8_TEXT_EVENT_MAX_COUNT) {
              break;
            }
            chunkLength += codePointLength;
          }

          // No code point fit, so stop sending
          if (chunkLength == 0) {
            break;
          }
        } else {
          // We send each Unicode code point individually. This way we can always
          // ensure they will never straddle a packet boundary (which will cause a
          // parsing error on the host).
          uint32_t codePointLength = getUtf8CodePointLength(ctx, holder->packet.unicode.text, i);
          if (codePointLength == 0) {
            break;
          }
          chunkLength = codePointLength;
        }

        // Clamp the chunk if the final code point is truncated
        if (chunkLength > remaining) {
          chunkLength = remaining;
        }

        // Use the original packet as a template and fixup to send the chunk
        splitPacket = *holder;
        splitPacket.packet.unicode.header.size =
            BE32(sizeof(uint32_t) + chunkLength);
        memcpy(splitPacket.packet.unicode.text, &holder->packet.unicode.text[i],
               chunkLength);

        // Encrypt and send the chunk
        if (!sendInputPacket(ctx, &splitPacket, i + chunkLength < totalLength)) {
          freePacketHolder(ctx, holder);
          return;
        }

        i += chunkLength;
      }

      freePacketHolder(ctx, holder);
      continue;
    }

    // Encrypt and send the input packet
    moreData = LbqGetItemCount(&ctx->packetQueue) > 0;

    // Flush touchpad frames periodically so a deep queue doesn't delay them
    if (holder->packet.header.magic == LE32(SS_TOUCHPAD_FRAME_MAGIC) &&
        holder->enetPacketFlags == 0) {
      uint64_t now = PltGetMillis();

      if (moreData && now >= lastTouchpadFlushTime + TOUCHPAD_FLUSH_INTERVAL_MS) {
        moreData = false;
      }

      if (!moreData) {
        lastTouchpadFlushTime = now;
      }
    }

    if (!sendInputPacket(ctx, holder, moreData)) {
      freePacketHolder(ctx, holder);
      return;
    }
    if (scrollPacket) {
      LiNoteScrollTraceSentCtx(ctx, LiGetMillis());
      logScrollTracePacketStage(ctx,
                                "sent",
                                holder,
                                LbqGetItemCount(&ctx->packetQueue),
                                moreData);
    }

    freePacketHolder(ctx, holder);
  }
}

// This function tells GFE that we support haptics and it should send rumble
// events to us
static int sendEnableHaptics(PML_INPUT_STREAM_CONTEXT ctx) {
  PPACKET_HOLDER holder;
  int err;

  // Avoid sending this on earlier server versions, since they may terminate
  // the connection upon receiving an unexpected packet.
  if (!APP_VERSION_AT_LEAST(7, 1, 0)) {
    return 0;
  }

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  holder->channelId = CTRL_CHANNEL_GENERIC;
  holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;
  holder->packet.haptics.header.size =
      BE32(sizeof(NV_HAPTICS_PACKET) - sizeof(uint32_t));
  holder->packet.haptics.header.magic = LE32(ENABLE_HAPTICS_MAGIC);
  holder->packet.haptics.enable = LE16(1);

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

// Begin the input stream
int startInputStreamCtx(PML_INPUT_STREAM_CONTEXT ctx) {
  int err;

  Limelog("Input: starting stream ctx=%p conn=%p appver=%d\n", (void*)ctx,
          ctx ? (void*)ctx->connectionContext : NULL, AppVersionQuad[0]);

  // After Gen 5, we send input on the control stream
  if (AppVersionQuad[0] < 5) {
    ctx->inputSock =
        connectTcpSocket(&RemoteAddr, AddrLen, 35043, INPUT_STREAM_TIMEOUT_SEC);
    if (ctx->inputSock == INVALID_SOCKET) {
      return LastSocketFail();
    }

    enableNoDelay(ctx->inputSock);
  }

    err =
      PltCreateThread("InputSend", inputSendThreadProc, ctx, &ctx->inputSendThread);
  if (err != 0) {
    if (ctx->inputSock != INVALID_SOCKET) {
      closeSocket(ctx->inputSock);
      ctx->inputSock = INVALID_SOCKET;
    }
    return err;
  }

  // Allow input packets to be queued now
  ctx->initialized = true;

  Limelog("Input: initialized ctx=%p initialized=%d\n", (void*)ctx, ctx->initialized);

  // GFE will not send haptics events without this magic packet first
  sendEnableHaptics(ctx);

  return err;
}

int startInputStream(void) {
    return startInputStreamCtx(LiGetEffectiveInputContext());
}

// Stops the input stream
int stopInputStreamCtx(PML_INPUT_STREAM_CONTEXT ctx) {
  // No more packets should be queued now
  ctx->initialized = false;
  atomic_store(&ctx->motionStateReady, false);
  LbqSignalQueueShutdown(&ctx->packetHolderFreeList);

  // Signal the input send thread to drain all pending
  // input packets before shutting down.
  LbqSignalQueueDrain(&ctx->packetQueue);
  PltJoinThread(&ctx->inputSendThread);

  if (ctx->inputSock != INVALID_SOCKET) {
    shutdownTcpSocket(ctx->inputSock);
  }

  if (ctx->inputSock != INVALID_SOCKET) {
    closeSocket(ctx->inputSock);
    ctx->inputSock = INVALID_SOCKET;
  }

  return 0;
}

int stopInputStream(void) {
    return stopInputStreamCtx(LiGetEffectiveInputContext());
}

// Send a mouse move event to the streaming machine
int LiSendMouseMoveEventCtx(PML_INPUT_STREAM_CONTEXT ctx, short deltaX, short deltaY) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  if (deltaX == 0 && deltaY == 0) {
    return 0;
  }

  PltLockMutex(&ctx->batchedInputMutex);

  // Combine the previous deltas with the new one
  ctx->currentRelativeMouseState.deltaX += deltaX;
  ctx->currentRelativeMouseState.deltaY += deltaY;

  // Queue a packet holder if this is the only pending relative mouse event
  if (!ctx->currentRelativeMouseState.dirty) {
    holder = allocatePacketHolder(ctx, 0);
    if (holder == NULL) {
      PltUnlockMutex(&ctx->batchedInputMutex);
      return -1;
    }

    holder->channelId = CTRL_CHANNEL_MOUSE;

    // TODO: Send this as unreliable sequenced when we have a delayed reliable
    // retransmission thread and protocol updates to allow us to determine which
    // unreliable messages were dropped.
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

    holder->packet.mouseMoveRel.header.size =
        BE32(sizeof(NV_REL_MOUSE_MOVE_PACKET) - sizeof(uint32_t));
    if (AppVersionQuad[0] >= 5) {
      holder->packet.mouseMoveRel.header.magic =
          LE32(MOUSE_MOVE_REL_MAGIC_GEN5);
    } else {
      holder->packet.mouseMoveRel.header.magic = LE32(MOUSE_MOVE_REL_MAGIC);
    }

    // Remaining fields are set in the input thread based on the latest
    // currentRelativeMouseState values

    err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
    if (err == LBQ_SUCCESS) {
      ctx->currentRelativeMouseState.dirty = true;
    } else {
      LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
      Limelog("Input queue reached maximum size limit\n");
      freePacketHolder(ctx, holder);
    }
  } else {
    // There's already a packet holder queued to send this event
    err = 0;
  }

  PltUnlockMutex(&ctx->batchedInputMutex);

  return err;
}

int LiSendMouseMoveEvent(short deltaX, short deltaY) {
    return LiSendMouseMoveEventCtx(LiGetEffectiveInputContext(), deltaX, deltaY);
}

// Send a mouse position update to the streaming machine
int LiSendMousePositionEventCtx(PML_INPUT_STREAM_CONTEXT ctx, short x, short y, short referenceWidth,
                             short referenceHeight) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  PltLockMutex(&ctx->batchedInputMutex);

  // Overwrite the previous mouse location with the new one
  ctx->currentAbsoluteMouseState.x = x;
  ctx->currentAbsoluteMouseState.y = y;
  ctx->currentAbsoluteMouseState.width = referenceWidth;
  ctx->currentAbsoluteMouseState.height = referenceHeight;

  // Queue a packet holder if this is the only pending absolute mouse event
  if (!ctx->currentAbsoluteMouseState.dirty) {
    holder = allocatePacketHolder(ctx, 0);
    if (holder == NULL) {
      PltUnlockMutex(&ctx->batchedInputMutex);
      return -1;
    }

    holder->channelId = CTRL_CHANNEL_MOUSE;

    // TODO: Send this as unreliable sequenced when we have a delayed reliable
    // retransmission thread
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

    holder->packet.mouseMoveAbs.header.size =
        BE32(sizeof(NV_ABS_MOUSE_MOVE_PACKET) - sizeof(uint32_t));
    holder->packet.mouseMoveAbs.header.magic = LE32(MOUSE_MOVE_ABS_MAGIC);
    holder->packet.mouseMoveAbs.unused = 0;

    // Remaining fields are set in the input thread based on the latest
    // currentAbsoluteMouseState values

    err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
    if (err == LBQ_SUCCESS) {
      ctx->currentAbsoluteMouseState.dirty = true;
    } else {
      LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
      Limelog("Input queue reached maximum size limit\n");
      freePacketHolder(ctx, holder);
    }
  } else {
    // There's already a packet holder queued to send this event
    err = 0;
  }

  PltUnlockMutex(&ctx->batchedInputMutex);

  // This is not thread safe, but it's not a big deal because callers that want
  // to use LiSendRelativeMotionAsMousePositionEvent() must not mix these
  // function without synchronization (otherwise the state of the cursor on the
  // host is undefined anyway).
  ctx->absCurrentPosX =
      CLAMP(x, 0, referenceWidth - 1) / (float)(referenceWidth - 1);
  ctx->absCurrentPosY =
      CLAMP(y, 0, referenceHeight - 1) / (float)(referenceHeight - 1);

  return err;
}

int LiSendMousePositionEvent(short x, short y, short referenceWidth, short referenceHeight) {
    return LiSendMousePositionEventCtx(LiGetEffectiveInputContext(), x, y, referenceWidth, referenceHeight);
}

// Send a relative motion event using absolute position to the streaming machine
int LiSendMouseMoveAsMousePositionEventCtx(PML_INPUT_STREAM_CONTEXT ctx, short deltaX, short deltaY,
                                        short referenceWidth,
                                        short referenceHeight) {
  // Convert the current position to be relative to the provided reference
  // dimensions
  short oldPositionX = (short)(ctx->absCurrentPosX * referenceWidth);
  short oldPositionY = (short)(ctx->absCurrentPosY * referenceHeight);

  return LiSendMousePositionEventCtx(
      ctx,
      CLAMP(oldPositionX + deltaX, 0, referenceWidth),
      CLAMP(oldPositionY + deltaY, 0, referenceHeight), referenceWidth,
      referenceHeight);
}

int LiSendMouseMoveAsMousePositionEvent(short deltaX, short deltaY, short referenceWidth, short referenceHeight) {
    return LiSendMouseMoveAsMousePositionEventCtx(LiGetEffectiveInputContext(), deltaX, deltaY, referenceWidth, referenceHeight);
}

// Send a mouse button event to the streaming machine
int LiSendMouseButtonEventCtx(PML_INPUT_STREAM_CONTEXT ctx, char action, int button) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  holder->channelId = CTRL_CHANNEL_MOUSE;
  holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;
  holder->packet.mouseButton.header.size =
      BE32(sizeof(NV_MOUSE_BUTTON_PACKET) - sizeof(uint32_t));
  holder->packet.mouseButton.header.magic = (uint8_t)action;
  if (AppVersionQuad[0] >= 5) {
    holder->packet.mouseButton.header.magic++;
  }
  holder->packet.mouseButton.header.magic =
      LE32(holder->packet.mouseButton.header.magic);
  holder->packet.mouseButton.button = (uint8_t)button;

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

int LiSendMouseButtonEvent(char action, int button) {
    return LiSendMouseButtonEventCtx(LiGetEffectiveInputContext(), action, button);
}

// Send a key press event to the streaming machine
int LiSendKeyboardEvent2Ctx(PML_INPUT_STREAM_CONTEXT ctx, short keyCode, char keyAction, char modifiers,
                         char flags) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  holder->channelId = CTRL_CHANNEL_KEYBOARD;
  holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

  // For proper behavior, the MODIFIER flag must not be set on the modifier key
  // down event itself for the extended modifiers on the right side of the
  // keyboard. If the MODIFIER flag is set, GFE will synthesize an errant key
  // down event for the non-extended key, causing that key to be stuck down
  // after the extended modifier key is raised. For non-extended keys, we must
  // set the MODIFIER flag for correct behavior.
  if (!IS_SUNSHINE()) {
    switch (keyCode & 0xFF) {
    case 0x5B: // VK_LWIN
    case 0x5C: // VK_RWIN
      // Any keyboard event with the META modifier flag is dropped by all known
      // GFE versions. This prevents us from sending shortcuts involving the
      // meta key (Win+X, Win+Tab, etc). The catch is that the meta key event
      // itself would actually work if it didn't set its own modifier flag, so
      // we'll clear that here. This should be safe even if a new GFE release
      // comes out that stops dropping events with MODIFIER_META flag.
      modifiers &= ~MODIFIER_META;
      break;

    case 0xA0: // VK_LSHIFT
      modifiers |= MODIFIER_SHIFT;
      break;
    case 0xA1: // VK_RSHIFT
      modifiers &= ~MODIFIER_SHIFT;
      break;

    case 0xA2: // VK_LCONTROL
      modifiers |= MODIFIER_CTRL;
      break;
    case 0xA3: // VK_RCONTROL
      modifiers &= ~MODIFIER_CTRL;
      break;

    case 0xA4: // VK_LMENU
      modifiers |= MODIFIER_ALT;
      break;
    case 0xA5: // VK_RMENU
      modifiers &= ~MODIFIER_ALT;
      break;

    default:
      // No fixups
      break;
    }

    // MODIFIER_EXTENDED is a Sunshine extension
    modifiers &= ~MODIFIER_EXTENDED;
  }

  holder->packet.keyboard.header.size =
      BE32(sizeof(NV_KEYBOARD_PACKET) - sizeof(uint32_t));
  holder->packet.keyboard.header.magic = LE32((uint32_t)keyAction);
  holder->packet.keyboard.flags = IS_SUNSHINE() ? flags : 0;
  holder->packet.keyboard.keyCode = LE16(keyCode);
  holder->packet.keyboard.modifiers = modifiers;
  holder->packet.keyboard.zero2 = 0;

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

int LiSendKeyboardEvent2(short keyCode, char keyAction, char modifiers, char flags) {
    return LiSendKeyboardEvent2Ctx(LiGetEffectiveInputContext(), keyCode, keyAction, modifiers, flags);
}

int LiSendKeyboardEventCtx(PML_INPUT_STREAM_CONTEXT ctx, short keyCode, char keyAction, char modifiers) {
  return LiSendKeyboardEvent2Ctx(ctx, keyCode, keyAction, modifiers, 0);
}

int LiSendKeyboardEvent(short keyCode, char keyAction, char modifiers) {
  return LiSendKeyboardEvent2Ctx(LiGetEffectiveInputContext(), keyCode, keyAction, modifiers, 0);
}

int LiSendUtf8TextEventCtx(PML_INPUT_STREAM_CONTEXT ctx, const char *text, unsigned int length) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  holder = allocatePacketHolder(ctx, length);
  if (holder == NULL) {
    return -1;
  }

  holder->channelId = CTRL_CHANNEL_KEYBOARD;
  holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

  // Magic + string length
  holder->packet.unicode.header.size = BE32(sizeof(uint32_t) + length);
  holder->packet.unicode.header.magic = LE32(UTF8_TEXT_EVENT_MAGIC);
  memcpy(holder->packet.unicode.text, text, length);

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

int LiSendUtf8TextEvent(const char *text, unsigned int length) {
    return LiSendUtf8TextEventCtx(LiGetEffectiveInputContext(), text, length);
}

static int sendControllerEventInternal(PML_INPUT_STREAM_CONTEXT ctx, short controllerNumber,
                                       short activeGamepadMask, int buttonFlags,
                                       unsigned char leftTrigger,
                                       unsigned char rightTrigger,
                                       short leftStickX, short leftStickY,
                                       short rightStickX, short rightStickY) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  // HACK: We previously used a short for the buttonFlags argument, but we
  // switched to an int to support additional buttons with Sunshine.
  // Unfortunately, some clients still pass a short, which gets sign extended to
  // an int. This causes all the new button flags to be set any time the user
  // presses the Y button on their gamepad (since Y is 0x8000). To deal with
  // these clients, we will detect this condition by checking if the sign bit is
  // set. Since we know there's no valid button flag that uses the 31st bit, any
  // case where the input value is negative is an instance of bug so only the
  // botton 16 bits are valid.
  if (buttonFlags < 0) {
    buttonFlags &= 0xFFFF;
  }

  if (!IS_SUNSHINE()) {
    // GFE only supports a maximum of 4 controllers
    controllerNumber %= 4;
    activeGamepadMask &= 0xF;

    // GFE doesn't support buttons that aren't present on an Xbox 360
    // controller, so the extended button flags won't even be sent. For
    // convenience, let's map the MISC button to the SPECIAL (Guide) button.
    // Some platforms reserve the Guide button for OS functionality (Game Bar,
    // Home button, etc.), so this allows otherwise unused buttons to activate
    // that functionality.
    if (buttonFlags & MISC_FLAG) {
      buttonFlags |= SPECIAL_FLAG;
    }
  } else {
    // Sunshine supports up to 16 (max number of bits in activeGamepadMask)
    controllerNumber %= MAX_GAMEPADS;
  }

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  // Send each controller on a separate channel
  holder->channelId = CTRL_CHANNEL_GAMEPAD_BASE + controllerNumber;

  // TODO: Send this as unreliable sequenced when we have a delayed reliable
  // retransmission thread
  holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

  if (AppVersionQuad[0] == 3) {
    // Generation 3 servers don't support multiple controllers so we send
    // the legacy packet
    holder->packet.controller.header.size =
        BE32(sizeof(NV_CONTROLLER_PACKET) - sizeof(uint32_t));
    holder->packet.controller.header.magic = LE32(CONTROLLER_MAGIC);
    holder->packet.controller.headerB = LE16(C_HEADER_B);
    holder->packet.controller.buttonFlags = LE16(buttonFlags);
    holder->packet.controller.leftTrigger = leftTrigger;
    holder->packet.controller.rightTrigger = rightTrigger;
    holder->packet.controller.leftStickX = LE16(leftStickX);
    holder->packet.controller.leftStickY = LE16(leftStickY);
    holder->packet.controller.rightStickX = LE16(rightStickX);
    holder->packet.controller.rightStickY = LE16(rightStickY);
    holder->packet.controller.tailA = LE32(C_TAIL_A);
    holder->packet.controller.tailB = LE16(C_TAIL_B);
  } else {
    // Generation 4+ servers support passing the controller number
    holder->packet.multiController.header.size =
        BE32(sizeof(NV_MULTI_CONTROLLER_PACKET) - sizeof(uint32_t));

    // On Gen 5 servers, the header code is decremented by one
    if (AppVersionQuad[0] >= 5) {
      holder->packet.multiController.header.magic =
          LE32(MULTI_CONTROLLER_MAGIC_GEN5);
    } else {
      holder->packet.multiController.header.magic =
          LE32(MULTI_CONTROLLER_MAGIC);
    }

    holder->packet.multiController.headerB = LE16(MC_HEADER_B);
    holder->packet.multiController.controllerNumber = LE16(controllerNumber);
    holder->packet.multiController.activeGamepadMask = LE16(activeGamepadMask);
    holder->packet.multiController.midB = LE16(MC_MID_B);
    holder->packet.multiController.buttonFlags = LE16((short)buttonFlags);
    holder->packet.multiController.leftTrigger = leftTrigger;
    holder->packet.multiController.rightTrigger = rightTrigger;
    holder->packet.multiController.leftStickX = LE16(leftStickX);
    holder->packet.multiController.leftStickY = LE16(leftStickY);
    holder->packet.multiController.rightStickX = LE16(rightStickX);
    holder->packet.multiController.rightStickY = LE16(rightStickY);
    holder->packet.multiController.tailA = LE16(MC_TAIL_A);
    holder->packet.multiController.buttonFlags2 =
        IS_SUNSHINE() ? LE16((short)(buttonFlags >> 16)) : 0;
    holder->packet.multiController.tailB = LE16(MC_TAIL_B);
  }

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

// Send a controller event to the streaming machine
int LiSendControllerEventCtx(PML_INPUT_STREAM_CONTEXT ctx, int buttonFlags, unsigned char leftTrigger,
                          unsigned char rightTrigger, short leftStickX,
                          short leftStickY, short rightStickX,
                          short rightStickY) {
  return sendControllerEventInternal(ctx, 0, 0x1, buttonFlags, leftTrigger,
                                     rightTrigger, leftStickX, leftStickY,
                                     rightStickX, rightStickY);
}

int LiSendControllerEvent(int buttonFlags, unsigned char leftTrigger,
                          unsigned char rightTrigger, short leftStickX,
                          short leftStickY, short rightStickX,
                          short rightStickY) {
    return LiSendControllerEventCtx(LiGetEffectiveInputContext(), buttonFlags, leftTrigger, rightTrigger, leftStickX, leftStickY, rightStickX, rightStickY);
}

// Send a controller event to the streaming machine
int LiSendMultiControllerEventCtx(PML_INPUT_STREAM_CONTEXT ctx, short controllerNumber, short activeGamepadMask,
                               int buttonFlags, unsigned char leftTrigger,
                               unsigned char rightTrigger, short leftStickX,
                               short leftStickY, short rightStickX,
                               short rightStickY) {
  return sendControllerEventInternal(
      ctx,
      controllerNumber, activeGamepadMask, buttonFlags, leftTrigger,
      rightTrigger, leftStickX, leftStickY, rightStickX, rightStickY);
}

int LiSendMultiControllerEvent(short controllerNumber, short activeGamepadMask,
                               int buttonFlags, unsigned char leftTrigger,
                               unsigned char rightTrigger, short leftStickX,
                               short leftStickY, short rightStickX,
                               short rightStickY) {
    return LiSendMultiControllerEventCtx(LiGetEffectiveInputContext(), controllerNumber, activeGamepadMask, buttonFlags, leftTrigger, rightTrigger, leftStickX, leftStickY, rightStickX, rightStickY);
}

// Send a high resolution scroll event to the streaming machine
int LiSendHighResScrollEventCtx(PML_INPUT_STREAM_CONTEXT ctx, short scrollAmount) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  if (scrollAmount == 0) {
    return 0;
  }

  // Newer version of GFE that use virtual HID devices have a bug that requires
  // the scroll events to be batched to WHEEL_DELTA. Due to the their HID report
  // descriptor, they don't actually support smooth scrolling. _Any_ scroll gets
  // converted into a full WHEEL_DELTA scroll, even if the actual delta is tiny.
  // Similarly, large scrolls are capped at +/- WHEEL_DELTA too so we'll need to
  // split those up too.
  if (ctx->needsBatchedScroll) {
    if ((ctx->batchedScrollDelta < 0 && scrollAmount > 0) ||
        (ctx->batchedScrollDelta > 0 && scrollAmount < 0)) {
      // Reset the accumulated scroll delta when the direction changes
      // FIXME: Maybe reset accumulated delta based on time too?
      ctx->batchedScrollDelta = 0;
    }

    ctx->batchedScrollDelta += scrollAmount;
    if (abs(ctx->batchedScrollDelta) < LI_WHEEL_DELTA) {
      logScrollTraceAccumulation(ctx, false, scrollAmount, ctx->batchedScrollDelta);
    }

    while (abs(ctx->batchedScrollDelta) >= LI_WHEEL_DELTA) {
      scrollAmount = ctx->batchedScrollDelta > 0 ? LI_WHEEL_DELTA : -LI_WHEEL_DELTA;

      holder = allocatePacketHolder(ctx, 0);
      if (holder == NULL) {
        return -1;
      }

      holder->channelId = CTRL_CHANNEL_MOUSE;
      holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

      holder->packet.scroll.header.size =
          BE32(sizeof(NV_SCROLL_PACKET) - sizeof(uint32_t));
      if (AppVersionQuad[0] >= 5) {
        holder->packet.scroll.header.magic = LE32(SCROLL_MAGIC_GEN5);
      } else {
        holder->packet.scroll.header.magic = LE32(SCROLL_MAGIC);
      }
      holder->packet.scroll.scrollAmt1 = BE16(scrollAmount);
      holder->packet.scroll.scrollAmt2 = holder->packet.scroll.scrollAmt1;
      holder->packet.scroll.zero3 = 0;
      attachScrollTraceMetadata(ctx, holder, LiGetMillis());

      err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
      if (err != LBQ_SUCCESS) {
        LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
        Limelog("Input queue reached maximum size limit\n");
        freePacketHolder(ctx, holder);
        return err;
      }

      LiNoteScrollTraceQueuedCtx(ctx, LiGetMillis());
      logScrollTracePacketStage(ctx,
                                "queued",
                                holder,
                                LbqGetItemCount(&ctx->packetQueue),
                                false);

      ctx->batchedScrollDelta -= scrollAmount;
    }

    err = 0;
  } else {
    holder = allocatePacketHolder(ctx, 0);
    if (holder == NULL) {
      return -1;
    }

    holder->channelId = CTRL_CHANNEL_MOUSE;
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

    holder->packet.scroll.header.size =
        BE32(sizeof(NV_SCROLL_PACKET) - sizeof(uint32_t));
    if (AppVersionQuad[0] >= 5) {
      holder->packet.scroll.header.magic = LE32(SCROLL_MAGIC_GEN5);
    } else {
      holder->packet.scroll.header.magic = LE32(SCROLL_MAGIC);
    }
    holder->packet.scroll.scrollAmt1 = BE16(scrollAmount);
    holder->packet.scroll.scrollAmt2 = holder->packet.scroll.scrollAmt1;
    holder->packet.scroll.zero3 = 0;
    attachScrollTraceMetadata(ctx, holder, LiGetMillis());

    err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
      LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
      Limelog("Input queue reached maximum size limit\n");
      freePacketHolder(ctx, holder);
    }
    else {
      LiNoteScrollTraceQueuedCtx(ctx, LiGetMillis());
      logScrollTracePacketStage(ctx,
                                "queued",
                                holder,
                                LbqGetItemCount(&ctx->packetQueue),
                                false);
    }
  }

  return err;
}

int LiSendHighResScrollEvent(short scrollAmount) {
    return LiSendHighResScrollEventCtx(LiGetEffectiveInputContext(), scrollAmount);
}

// Send a scroll event to the streaming machine
int LiSendScrollEventCtx(PML_INPUT_STREAM_CONTEXT ctx, signed char scrollClicks) {
  return LiSendHighResScrollEventCtx(ctx, scrollClicks * LI_WHEEL_DELTA);
}

int LiSendScrollEvent(signed char scrollClicks) {
    return LiSendScrollEventCtx(LiGetEffectiveInputContext(), scrollClicks);
}

// Send a high resolution horizontal scroll event
int LiSendHighResHScrollEventCtx(PML_INPUT_STREAM_CONTEXT ctx, short scrollAmount) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  // This is a protocol extension only supported with Sunshine
  if (!IS_SUNSHINE()) {
    return LI_ERR_UNSUPPORTED;
  }

  if (scrollAmount == 0) {
    return 0;
  }

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  holder->channelId = CTRL_CHANNEL_MOUSE;
  holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

  holder->packet.hscroll.header.size =
      BE32(sizeof(SS_HSCROLL_PACKET) - sizeof(uint32_t));
  holder->packet.hscroll.header.magic = LE32(SS_HSCROLL_MAGIC);
  holder->packet.hscroll.scrollAmount = BE16(scrollAmount);
  attachScrollTraceMetadata(ctx, holder, LiGetMillis());

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }
  else {
    LiNoteScrollTraceQueuedCtx(ctx, LiGetMillis());
    logScrollTracePacketStage(ctx,
                              "queued",
                              holder,
                              LbqGetItemCount(&ctx->packetQueue),
                              false);
  }

  return err;
}

int LiSendHighResHScrollEvent(short scrollAmount) {
    return LiSendHighResHScrollEventCtx(LiGetEffectiveInputContext(), scrollAmount);
}

int LiSendHScrollEventCtx(PML_INPUT_STREAM_CONTEXT ctx, signed char scrollClicks) {
  return LiSendHighResHScrollEventCtx(ctx, scrollClicks * LI_WHEEL_DELTA);
}

int LiSendHScrollEvent(signed char scrollClicks) {
    return LiSendHScrollEventCtx(LiGetEffectiveInputContext(), scrollClicks);
}

int LiSendMicrophoneControlCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t control, int sampleRate, int channelCount,
                            int bitrate) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  holder->channelId = CTRL_CHANNEL_GENERIC;
  holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

  holder->packet.microphone.header.size =
      BE32(sizeof(SS_MICROPHONE_PACKET) - sizeof(uint32_t));
  holder->packet.microphone.header.magic = LE32(SS_MICROPHONE_MAGIC);
  holder->packet.microphone.packetMagic = LE32(MIC_PACKET_MAGIC);
  holder->packet.microphone.control = control;
  memset(holder->packet.microphone.reserved, 0,
         sizeof(holder->packet.microphone.reserved));
  holder->packet.microphone.config.sampleRate = LE32((uint32_t)sampleRate);
  holder->packet.microphone.config.channelCount = LE32((uint32_t)channelCount);
  holder->packet.microphone.config.bitrate = LE32((uint32_t)bitrate);

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

int LiSendMicrophoneControl(uint8_t control, int sampleRate, int channelCount,
                            int bitrate) {
    return LiSendMicrophoneControlCtx(LiGetEffectiveInputContext(), control, sampleRate, channelCount, bitrate);
}

int LiSendTouchEventCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t eventType, uint32_t pointerId, float x, float y,
                     float pressureOrDistance, float contactAreaMajor,
                     float contactAreaMinor, uint16_t rotation) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  // This is a protocol extension only supported with Sunshine
  if (!(SunshineFeatureFlags & LI_FF_PEN_TOUCH_EVENTS)) {
    return LI_ERR_UNSUPPORTED;
  }

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  holder->channelId = CTRL_CHANNEL_TOUCH;

  // Allow move and hover events to be dropped if a newer one arrives, but don't
  // allow state changing events like up/down/leave events to be dropped.
  holder->enetPacketFlags =
      TOUCH_EVENT_IS_BATCHABLE(eventType) ? 0 : ENET_PACKET_FLAG_RELIABLE;

  holder->packet.touch.header.size =
      BE32(sizeof(SS_TOUCH_PACKET) - sizeof(uint32_t));
  holder->packet.touch.header.magic = LE32(SS_TOUCH_MAGIC);
  holder->packet.touch.eventType = eventType;
  holder->packet.touch.pointerId = LE32(pointerId);
  holder->packet.touch.rotation = LE16(rotation);
  memset(holder->packet.touch.zero, 0, sizeof(holder->packet.touch.zero));
  floatToNetfloat(x, holder->packet.touch.x);
  floatToNetfloat(y, holder->packet.touch.y);
  floatToNetfloat(pressureOrDistance, holder->packet.touch.pressureOrDistance);
  floatToNetfloat(contactAreaMajor, holder->packet.touch.contactAreaMajor);
  floatToNetfloat(contactAreaMinor, holder->packet.touch.contactAreaMinor);

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

int LiSendTouchEvent(uint8_t eventType, uint32_t pointerId, float x, float y,
                     float pressureOrDistance, float contactAreaMajor,
                     float contactAreaMinor, uint16_t rotation) {
    return LiSendTouchEventCtx(LiGetEffectiveInputContext(), eventType, pointerId, x, y, pressureOrDistance, contactAreaMajor, contactAreaMinor, rotation);
}

int LiSendTouchpadEventCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t eventType, uint32_t pointerId, float x, float y,
                        float pressure, float contactAreaMajor, float contactAreaMinor,
                        uint16_t rotation, uint16_t deviceWidthMm, uint16_t deviceHeightMm,
                        uint8_t buttonState) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  // This is a protocol extension only supported with Sunshine
  if (!(SunshineFeatureFlags & LI_FF_TOUCHPAD_EVENTS)) {
    return LI_ERR_UNSUPPORTED;
  }

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  holder->channelId = CTRL_CHANNEL_TOUCH;

  // Allow move events to be dropped if a newer one arrives (if no buttons
  // changed), but don't allow state changing events like up/down/button/cancel
  // events or button transitions to be dropped.
  holder->enetPacketFlags = (TOUCH_EVENT_IS_BATCHABLE(eventType) &&
                             !(buttonState ^ ctx->currentTouchpadButtonState))
                                ? 0
                                : ENET_PACKET_FLAG_RELIABLE;
  ctx->currentTouchpadButtonState = buttonState;

  holder->packet.touchpad.header.size =
      BE32(sizeof(SS_TOUCHPAD_PACKET) - sizeof(uint32_t));
  holder->packet.touchpad.header.magic = LE32(SS_TOUCHPAD_MAGIC);
  holder->packet.touchpad.eventType = eventType;
  holder->packet.touchpad.buttonState = buttonState;
  holder->packet.touchpad.rotation = LE16(rotation);
  holder->packet.touchpad.pointerId = LE32(pointerId);
  holder->packet.touchpad.deviceWidthMm = LE16(deviceWidthMm);
  holder->packet.touchpad.deviceHeightMm = LE16(deviceHeightMm);
  floatToNetfloat(x, holder->packet.touchpad.x);
  floatToNetfloat(y, holder->packet.touchpad.y);
  floatToNetfloat(pressure, holder->packet.touchpad.pressure);
  floatToNetfloat(contactAreaMajor, holder->packet.touchpad.contactAreaMajor);
  floatToNetfloat(contactAreaMinor, holder->packet.touchpad.contactAreaMinor);

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

int LiSendTouchpadEvent(uint8_t eventType, uint32_t pointerId, float x, float y, float pressure,
                        float contactAreaMajor, float contactAreaMinor, uint16_t rotation,
                        uint16_t deviceWidthMm, uint16_t deviceHeightMm, uint8_t buttonState) {
    return LiSendTouchpadEventCtx(LiGetEffectiveInputContext(), eventType, pointerId, x, y, pressure,
                                  contactAreaMajor, contactAreaMinor, rotation,
                                  deviceWidthMm, deviceHeightMm, buttonState);
}

int LiSendTouchpadFrameEventCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t contactCount, const uint8_t* eventTypes,
                                const uint32_t* pointerIds, const float* x, const float* y,
                                const float* pressure, uint16_t rotation, uint16_t deviceWidthMm,
                                uint16_t deviceHeightMm, uint8_t buttonState) {
  PPACKET_HOLDER holder;
  int err;
  bool batchable = true;

  if (!ctx->initialized) {
    return -2;
  }

  if (!(SunshineFeatureFlags & LI_FF_TOUCHPAD_FRAME_EVENTS)) {
    return LI_ERR_UNSUPPORTED;
  }

  if (contactCount > SS_TOUCHPAD_FRAME_MAX_CONTACTS ||
      (contactCount > 0 && (eventTypes == NULL || pointerIds == NULL || x == NULL ||
                            y == NULL || pressure == NULL))) {
    return -3;
  }

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  holder->channelId = CTRL_CHANNEL_TOUCH;

  holder->packet.touchpadFrame.header.size =
      BE32(sizeof(SS_TOUCHPAD_FRAME_PACKET) - sizeof(uint32_t));
  holder->packet.touchpadFrame.header.magic = LE32(SS_TOUCHPAD_FRAME_MAGIC);
  holder->packet.touchpadFrame.contactCount = contactCount;
  holder->packet.touchpadFrame.buttonState = buttonState;
  holder->packet.touchpadFrame.rotation = LE16(rotation);
  holder->packet.touchpadFrame.deviceWidthMm = LE16(deviceWidthMm);
  holder->packet.touchpadFrame.deviceHeightMm = LE16(deviceHeightMm);
  memset(holder->packet.touchpadFrame.contacts, 0,
         sizeof(holder->packet.touchpadFrame.contacts));

  for (uint8_t i = 0; i < contactCount; i++) {
    holder->packet.touchpadFrame.contacts[i].eventType = eventTypes[i];
    holder->packet.touchpadFrame.contacts[i].pointerId = LE32(pointerIds[i]);
    holder->packet.touchpadFrame.contacts[i].x = LE16(normalizedFloatToUint16(x[i]));
    holder->packet.touchpadFrame.contacts[i].y = LE16(normalizedFloatToUint16(y[i]));
    holder->packet.touchpadFrame.contacts[i].pressure = LE16(normalizedFloatToUint16(pressure[i]));

    if (!TOUCH_EVENT_IS_BATCHABLE(eventTypes[i])) {
      batchable = false;
    }
  }

  // Allow the frame to be dropped if a newer one arrives only when every
  // contact is a batchable move/hover event and the button state did not
  // change. A button transition must be delivered reliably so clicks aren't lost.
  holder->enetPacketFlags = (batchable && !(buttonState ^ ctx->currentTouchpadButtonState))
                                ? 0
                                : ENET_PACKET_FLAG_RELIABLE;
  ctx->currentTouchpadButtonState = buttonState;

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

int LiSendTouchpadFrameEvent(uint8_t contactCount, const uint8_t* eventTypes, const uint32_t* pointerIds,
                             const float* x, const float* y, const float* pressure, uint16_t rotation,
                             uint16_t deviceWidthMm, uint16_t deviceHeightMm, uint8_t buttonState) {
    return LiSendTouchpadFrameEventCtx(LiGetEffectiveInputContext(), contactCount, eventTypes, pointerIds,
                                       x, y, pressure, rotation, deviceWidthMm, deviceHeightMm, buttonState);
}

int LiSendPenEventCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t eventType, uint8_t toolType, uint8_t penButtons,
                   float x, float y, float pressureOrDistance,
                   float contactAreaMajor, float contactAreaMinor,
                   uint16_t rotation, uint8_t tilt) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  // This is a protocol extension only supported with Sunshine
  if (!(SunshineFeatureFlags & LI_FF_PEN_TOUCH_EVENTS)) {
    return LI_ERR_UNSUPPORTED;
  }

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  holder->channelId = CTRL_CHANNEL_PEN;

  // Allow move and hover events to be dropped if a newer one arrives (if no
  // buttons changed), but don't allow state changing events like up/down/leave
  // events to be dropped.
  holder->enetPacketFlags = (TOUCH_EVENT_IS_BATCHABLE(eventType) &&
                             !(penButtons ^ ctx->currentPenButtonState))
                                ? 0
                                : ENET_PACKET_FLAG_RELIABLE;
  ctx->currentPenButtonState = penButtons;

  holder->packet.pen.header.size =
      BE32(sizeof(SS_PEN_PACKET) - sizeof(uint32_t));
  holder->packet.pen.header.magic = LE32(SS_PEN_MAGIC);
  holder->packet.pen.eventType = eventType;
  holder->packet.pen.toolType = toolType;
  holder->packet.pen.penButtons = penButtons;
  memset(holder->packet.pen.zero, 0, sizeof(holder->packet.pen.zero));
  floatToNetfloat(x, holder->packet.pen.x);
  floatToNetfloat(y, holder->packet.pen.y);
  floatToNetfloat(pressureOrDistance, holder->packet.pen.pressureOrDistance);
  holder->packet.pen.rotation = LE16(rotation);
  holder->packet.pen.tilt = tilt;
  memset(holder->packet.pen.zero2, 0, sizeof(holder->packet.pen.zero2));
  floatToNetfloat(contactAreaMajor, holder->packet.pen.contactAreaMajor);
  floatToNetfloat(contactAreaMinor, holder->packet.pen.contactAreaMinor);

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

int LiSendPenEvent(uint8_t eventType, uint8_t toolType, uint8_t penButtons,
                   float x, float y, float pressureOrDistance,
                   float contactAreaMajor, float contactAreaMinor,
                   uint16_t rotation, uint8_t tilt) {
    return LiSendPenEventCtx(LiGetEffectiveInputContext(), eventType, toolType, penButtons, x, y, pressureOrDistance, contactAreaMajor, contactAreaMinor, rotation, tilt);
}

int LiSendControllerArrivalEventCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controllerNumber,
                                 uint16_t activeGamepadMask, uint8_t type,
                                 uint32_t supportedButtonFlags,
                                 uint16_t capabilities) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  // Sunshine supports up to 16 controllers
  controllerNumber %= MAX_GAMEPADS;

  // Always set the older touchpad cap if we have dual touchpads
  if (capabilities & LI_CCAP_DUAL_TOUCHPAD) {
    capabilities |= LI_CCAP_TOUCHPAD;
  }

  // The arrival event is only supported by Sunshine
  if (IS_SUNSHINE()) {
    holder = allocatePacketHolder(ctx, 0);
    if (holder == NULL) {
      return -1;
    }

    // Send each controller on a separate channel
    holder->channelId = CTRL_CHANNEL_GAMEPAD_BASE + controllerNumber;
    holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

    holder->packet.controllerArrival.header.size =
        BE32(sizeof(SS_CONTROLLER_ARRIVAL_PACKET) - sizeof(uint32_t));
    holder->packet.controllerArrival.header.magic =
        LE32(SS_CONTROLLER_ARRIVAL_MAGIC);
    holder->packet.controllerArrival.controllerNumber = controllerNumber;
    holder->packet.controllerArrival.type = type;
    holder->packet.controllerArrival.capabilities = LE16(capabilities);
    holder->packet.controllerArrival.supportedButtonFlags =
        LE32(supportedButtonFlags);

    err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
      LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
      Limelog("Input queue reached maximum size limit\n");
      freePacketHolder(ctx, holder);
      return err;
    }
  }

  // Send a MC event just in case the host software doesn't support arrival
  // events.
  return LiSendMultiControllerEventCtx(ctx, controllerNumber, activeGamepadMask, 0, 0,
                                    0, 0, 0, 0, 0);
}

int LiSendControllerArrivalEvent(uint8_t controllerNumber,
                                 uint16_t activeGamepadMask, uint8_t type,
                                 uint32_t supportedButtonFlags,
                                 uint16_t capabilities) {
    return LiSendControllerArrivalEventCtx(LiGetEffectiveInputContext(), controllerNumber, activeGamepadMask, type, supportedButtonFlags, capabilities);
}

int LiSendControllerTouchEvent2Ctx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controllerNumber, uint8_t eventType,
                               uint8_t touchpadIndex, uint32_t pointerId, float x, float y,
                               float pressure) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  // This is a protocol extension only supported with Sunshine
  if (!(SunshineFeatureFlags & LI_FF_CONTROLLER_TOUCH_EVENTS)) {
    return LI_ERR_UNSUPPORTED;
  }

  // Sunshine supports up to 16 controllers
  controllerNumber %= MAX_GAMEPADS;

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  // Send each controller on a separate channel
  holder->channelId = CTRL_CHANNEL_GAMEPAD_BASE + controllerNumber;

  // Allow move and hover events to be dropped if a newer one arrives, but don't
  // allow state changing events like up/down/leave events to be dropped.
  holder->enetPacketFlags =
      TOUCH_EVENT_IS_BATCHABLE(eventType) ? 0 : ENET_PACKET_FLAG_RELIABLE;

  holder->packet.controllerTouch.header.size =
      BE32(sizeof(SS_CONTROLLER_TOUCH_PACKET) - sizeof(uint32_t));
  holder->packet.controllerTouch.header.magic = LE32(SS_CONTROLLER_TOUCH_MAGIC);
  holder->packet.controllerTouch.controllerNumber = controllerNumber;
  holder->packet.controllerTouch.eventType = eventType;
  memset(&holder->packet.controllerTouch.zero, 0,
         sizeof(holder->packet.controllerTouch.zero));
  holder->packet.controllerTouch.touchpadIndex = touchpadIndex;
  holder->packet.controllerTouch.pointerId = LE32(pointerId);
  floatToNetfloat(x, holder->packet.controllerTouch.x);
  floatToNetfloat(y, holder->packet.controllerTouch.y);
  floatToNetfloat(pressure, holder->packet.controllerTouch.pressure);

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

int LiSendControllerTouchEventCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controllerNumber, uint8_t eventType,
                               uint32_t pointerId, float x, float y,
                               float pressure) {
    return LiSendControllerTouchEvent2Ctx(ctx, controllerNumber, eventType, 0, pointerId, x, y, pressure);
}

int LiSendControllerTouchEvent2(uint8_t controllerNumber, uint8_t eventType, uint8_t touchpadIndex,
                                uint32_t pointerId, float x, float y, float pressure) {
    return LiSendControllerTouchEvent2Ctx(LiGetEffectiveInputContext(), controllerNumber, eventType, touchpadIndex, pointerId, x, y, pressure);
}

int LiSendControllerTouchEvent(uint8_t controllerNumber, uint8_t eventType,
                               uint32_t pointerId, float x, float y,
                               float pressure) {
    return LiSendControllerTouchEventCtx(LiGetEffectiveInputContext(), controllerNumber, eventType, pointerId, x, y, pressure);
}

static void acceptControllerMotionLocked(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controller, uint8_t type,
                                         float x, float y, float z) {
  ctx->queuedMotionPackets++;
  ctx->currentGamepadSensorState[controller][type - 1].sequence++;
  if (type == LI_MOTION_TYPE_GYRO) {
    ctx->pendingGyroStops &= ~(1u << controller);
    ctx->currentGamepadSensorState[controller][1].stopPending = false;
    ctx->currentGamepadSensorState[controller][1].mayBeMoving |= x != 0 || y != 0 || z != 0;
  }
}

// Caller holds batchedInputMutex. A failed movement is never retained for later replay.
static int enqueueControllerMotionLocked(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controller, uint8_t type,
                                         float x, float y, float z) {
  PPACKET_HOLDER holder;
  int err;
  if (!ctx->currentGamepadSensorState[controller][type - 1].enabled) return 0;
  holder = ctx->queuedMotionPackets < MAX_QUEUED_MOTION_PACKETS ? allocatePacketHolder(ctx, 0) : NULL;
  if (holder == NULL) {
    err = -1;
  } else {
    initializeMotionPacket(holder, controller, type, x, y, z);
    holder->motionSequence = ctx->currentGamepadSensorState[controller][type - 1].sequence + 1;
    err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) freePacketHolder(ctx, holder);
  }
  if (err == LBQ_SUCCESS) {
    acceptControllerMotionLocked(ctx, controller, type, x, y, z);
  }
  return err;
}

int LiSendControllerMotionEventCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controllerNumber, uint8_t motionType,
                                   float x, float y, float z) {
  int err;
  if (!ctx->initialized) return -2;
  if (motionType < 1 || motionType > MAX_MOTION_EVENTS) return -3;
  if (!(SunshineFeatureFlags & LI_FF_CONTROLLER_TOUCH_EVENTS)) return LI_ERR_UNSUPPORTED;
  controllerNumber %= MAX_GAMEPADS;
  PltLockMutex(&ctx->batchedInputMutex);
  err = enqueueControllerMotionLocked(ctx, controllerNumber, motionType, x, y, z);
  if (err != LBQ_SUCCESS && motionType == LI_MOTION_TYPE_GYRO && x == 0 && y == 0 && z == 0) {
    stopControllerMotionLocked(ctx, controllerNumber);
  }
  PltUnlockMutex(&ctx->batchedInputMutex);
  return err;
}

int LiSendControllerMotionEvent(uint8_t controllerNumber, uint8_t motionType,
                                float x, float y, float z) {
  return LiSendControllerMotionEventCtx(LiGetEffectiveInputContext(), controllerNumber, motionType, x, y, z);
}

int LiSendControllerMotionFrameCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controller,
                                   float gx, float gy, float gz, float ax, float ay, float az) {
  PPACKET_HOLDER gyro = NULL, accel = NULL;
  int err = LBQ_SUCCESS;
  if (!ctx->initialized) return -2;
  if (!(SunshineFeatureFlags & LI_FF_CONTROLLER_TOUCH_EVENTS)) return LI_ERR_UNSUPPORTED;
  if (controller >= MAX_GAMEPADS) return -3;
  PltLockMutex(&ctx->batchedInputMutex);
  if (!ctx->currentGamepadSensorState[controller][0].enabled || !ctx->currentGamepadSensorState[controller][1].enabled) {
    err = enqueueControllerMotionLocked(ctx, controller, LI_MOTION_TYPE_GYRO, gx, gy, gz);
    if (err == 0) err = enqueueControllerMotionLocked(ctx, controller, LI_MOTION_TYPE_ACCEL, ax, ay, az);
  } else {
    if (ctx->queuedMotionPackets + 2 <= MAX_QUEUED_MOTION_PACKETS) {
      gyro = allocatePacketHolder(ctx, 0);
      accel = allocatePacketHolder(ctx, 0);
    }
    if (gyro) initializeMotionPacket(gyro, controller, LI_MOTION_TYPE_GYRO, gx, gy, gz);
    if (accel) initializeMotionPacket(accel, controller, LI_MOTION_TYPE_ACCEL, ax, ay, az);
    if (gyro == NULL || accel == NULL) err = -1;
    else {
      gyro->motionSequence = ctx->currentGamepadSensorState[controller][1].sequence + 1;
      accel->motionSequence = ctx->currentGamepadSensorState[controller][0].sequence + 1;
      gyro->entry.data = gyro;
      gyro->entry.flink = &accel->entry;
      accel->entry.data = accel;
      accel->entry.blink = &gyro->entry;
      err = LbqOfferQueueItems(&ctx->packetQueue, &gyro->entry, &accel->entry, 2);
    }
    if (err == LBQ_SUCCESS) {
      acceptControllerMotionLocked(ctx, controller, LI_MOTION_TYPE_GYRO, gx, gy, gz);
      acceptControllerMotionLocked(ctx, controller, LI_MOTION_TYPE_ACCEL, ax, ay, az);
    } else {
      if (gyro) freePacketHolder(ctx, gyro);
      if (accel) freePacketHolder(ctx, accel);
    }
  }
  PltUnlockMutex(&ctx->batchedInputMutex);
  return err;
}

int LiSendControllerMotionFrame(uint8_t controller, float gx, float gy, float gz, float ax, float ay, float az) {
  return LiSendControllerMotionFrameCtx(LiGetEffectiveInputContext(), controller, gx, gy, gz, ax, ay, az);
}

int LiStopControllerMotionEventCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controller) {
  if (!ctx->initialized) return -2;
  if (controller >= MAX_GAMEPADS) return -3;
  PltLockMutex(&ctx->batchedInputMutex);
  stopControllerMotionLocked(ctx, controller);
  PltUnlockMutex(&ctx->batchedInputMutex);
  return 0;
}

int LiStopControllerMotionEvent(uint8_t controller) {
  return LiStopControllerMotionEventCtx(LiGetEffectiveInputContext(), controller);
}

int LiResetControllerMotionEventsCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controller, bool clearDemand) {
  if (!atomic_load(&ctx->motionStateReady)) return -2;
  if (controller >= MAX_GAMEPADS) return -3;
  PltLockMutex(&ctx->batchedInputMutex);
  for (int sensor = 0; sensor < MAX_MOTION_EVENTS; sensor++) {
    ctx->currentGamepadSensorState[controller][sensor].discardThrough = ++ctx->currentGamepadSensorState[controller][sensor].sequence;
    if (clearDemand) ctx->currentGamepadSensorState[controller][sensor].enabled = false;
  }
  if (clearDemand) {
    ctx->pendingGyroStops &= ~(1u << controller);
    ctx->currentGamepadSensorState[controller][1].mayBeMoving = false;
    ctx->currentGamepadSensorState[controller][1].stopPending = false;
  } else {
    ctx->currentGamepadSensorState[controller][1].stopPending = false;
    stopControllerMotionLocked(ctx, controller);
  }
  PltUnlockMutex(&ctx->batchedInputMutex);
  return 0;
}

int LiResetControllerMotionEvents(uint8_t controller, bool clearDemand) {
  return LiResetControllerMotionEventsCtx(LiGetEffectiveInputContext(), controller, clearDemand);
}

// Can arrive before startInputStream(), after the queue and mutex have been initialized.
int setControllerMotionEnabledCtx(PML_INPUT_STREAM_CONTEXT ctx, uint16_t controller, uint8_t type, bool enabled) {
  if (!atomic_load(&ctx->motionStateReady)) return -2;
  if (controller >= MAX_GAMEPADS || type < 1 || type > MAX_MOTION_EVENTS) return -3;
  PltLockMutex(&ctx->batchedInputMutex);
  ctx->currentGamepadSensorState[controller][type - 1].enabled = enabled;
  if (!enabled) {
    if (type == LI_MOTION_TYPE_GYRO) stopControllerMotionLocked(ctx, (uint8_t)controller);
    else ctx->currentGamepadSensorState[controller][0].discardThrough = ++ctx->currentGamepadSensorState[controller][0].sequence;
  }
  PltUnlockMutex(&ctx->batchedInputMutex);
  return 0;
}

int setControllerMotionEnabled(uint16_t controller, uint8_t type, bool enabled) {
  return setControllerMotionEnabledCtx(LiGetEffectiveInputContext(), controller, type, enabled);
}

int LiSendControllerBatteryEventCtx(PML_INPUT_STREAM_CONTEXT ctx, uint8_t controllerNumber, uint8_t batteryState,
                                 uint8_t batteryPercentage) {
  PPACKET_HOLDER holder;
  int err;

  if (!ctx->initialized) {
    return -2;
  }

  // This is a protocol extension only supported with Sunshine
  if (!IS_SUNSHINE()) {
    return LI_ERR_UNSUPPORTED;
  }

  // Sunshine supports up to 16 controllers
  controllerNumber %= MAX_GAMEPADS;

  holder = allocatePacketHolder(ctx, 0);
  if (holder == NULL) {
    return -1;
  }

  // Send each controller on a separate channel
  holder->channelId = CTRL_CHANNEL_GAMEPAD_BASE + controllerNumber;
  holder->enetPacketFlags = ENET_PACKET_FLAG_RELIABLE;

  holder->packet.controllerBattery.header.size =
      BE32(sizeof(SS_CONTROLLER_BATTERY_PACKET) - sizeof(uint32_t));
  holder->packet.controllerBattery.header.magic =
      LE32(SS_CONTROLLER_BATTERY_MAGIC);
  holder->packet.controllerBattery.controllerNumber = controllerNumber;
  holder->packet.controllerBattery.batteryState = batteryState;
  holder->packet.controllerBattery.batteryPercentage = batteryPercentage;
  memset(holder->packet.controllerBattery.zero, 0,
         sizeof(holder->packet.controllerBattery.zero));

  err = LbqOfferQueueItem(&ctx->packetQueue, holder, &holder->entry);
  if (err != LBQ_SUCCESS) {
    LC_ASSERT(err == LBQ_BOUND_EXCEEDED);
    Limelog("Input queue reached maximum size limit\n");
    freePacketHolder(ctx, holder);
  }

  return err;
}

int LiSendControllerBatteryEvent(uint8_t controllerNumber, uint8_t batteryState,
                                 uint8_t batteryPercentage) {
    return LiSendControllerBatteryEventCtx(LiGetEffectiveInputContext(), controllerNumber, batteryState, batteryPercentage);
}
