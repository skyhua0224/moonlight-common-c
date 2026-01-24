#include "Limelight-internal.h"
#include "PlatformSockets.h"

// Macros to redirect global access to context
#ifdef RemoteAddr
#undef RemoteAddr
#endif
#ifdef LocalAddr
#undef LocalAddr
#endif
#ifdef AddrLen
#undef AddrLen
#endif
#ifdef MicPortNumber
#undef MicPortNumber
#endif

#define RemoteAddr (ctx->connectionContext->RemoteAddr)
#define LocalAddr (ctx->connectionContext->LocalAddr)
#define AddrLen (ctx->connectionContext->AddrLen)
#define MicPortNumber (ctx->connectionContext->MicPortNumber)

typedef struct _MICROPHONE_PACKET_HEADER {
  uint8_t flags;
  uint8_t packetType;
  uint16_t sequenceNumber;
  uint32_t timestamp;
  uint32_t ssrc;
} MICROPHONE_PACKET_HEADER, *PMICROPHONE_PACKET_HEADER;

#define MIC_PACKET_TYPE_OPUS 0x61 // 'a'

// 初始化麦克风流
int initializeMicrophoneStreamCtx(PML_MICROPHONE_STREAM_CONTEXT ctx, PML_CONNECTION_CONTEXT connectionContext) {
  if (ctx == NULL || connectionContext == NULL) {
    return -1;
  }

  ctx->connectionContext = connectionContext;
  ctx->micAddrValid = false;
  ctx->micAddrLen = 0;
  ctx->micPortNumber = 0;

  // 如果已经初始化，直接返回成功
  if (ctx->micSocket != INVALID_SOCKET) {
    return 0;
  }

  ctx->micEncryptionCtx = PltCreateCryptoContext();
  if (ctx->micEncryptionCtx == NULL) {
    return -1;
  }

  // 创建UDP socket
  ctx->micSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen, 0,
                            SOCK_QOS_TYPE_AUDIO);
  if (ctx->micSocket == INVALID_SOCKET) {
    PltDestroyCryptoContext(ctx->micEncryptionCtx);
    ctx->micEncryptionCtx = NULL;
    return LastSocketFail();
  }

  // Cache address details for mic sends to avoid dereferencing connectionContext later
  if (AddrLen > 0 && AddrLen <= sizeof(ctx->micRemoteAddr) && RemoteAddr.ss_family != AF_UNSPEC) {
    memcpy(&ctx->micRemoteAddr, &RemoteAddr, sizeof(ctx->micRemoteAddr));
    ctx->micAddrLen = AddrLen;
    ctx->micPortNumber = MicPortNumber;
    ctx->micAddrValid = true;
  }

  return 0;
}

int initializeMicrophoneStream(void) {
  PML_CONNECTION_CONTEXT ctx = LiGetEffectiveConnectionContext();
  return initializeMicrophoneStreamCtx(&ctx->micContext, ctx);
}

// 关闭麦克风流
void destroyMicrophoneStreamCtx(PML_MICROPHONE_STREAM_CONTEXT ctx) {
  if (ctx == NULL) {
    return;
  }
  if (ctx->micSocket != INVALID_SOCKET) {
    closeSocket(ctx->micSocket);
    ctx->micSocket = INVALID_SOCKET;
  }

  if (ctx->micEncryptionCtx != NULL) {
    PltDestroyCryptoContext(ctx->micEncryptionCtx);
    ctx->micEncryptionCtx = NULL;
  }

  ctx->micAddrValid = false;
  ctx->micAddrLen = 0;
  ctx->micPortNumber = 0;
  memset(&ctx->micRemoteAddr, 0, sizeof(ctx->micRemoteAddr));
  ctx->connectionContext = NULL;
}

void destroyMicrophoneStream(void) {
  PML_CONNECTION_CONTEXT ctx = LiGetEffectiveConnectionContext();
  destroyMicrophoneStreamCtx(&ctx->micContext);
}

// 发送麦克风数据
int sendMicrophoneDataCtx(PML_MICROPHONE_STREAM_CONTEXT ctx, const char* data, int length) {
  LC_SOCKADDR saddr;
  ssize_t err;

  if (ctx == NULL || ctx->connectionContext == NULL) {
    return -1;
  }

  if (!ctx->micAddrValid || ctx->micAddrLen == 0 || ctx->micAddrLen > sizeof(saddr)) {
    return -1;
  }

  if (((struct sockaddr *)&ctx->micRemoteAddr)->sa_family == AF_UNSPEC) {
    return -1;
  }

  if (ctx->micSocket == INVALID_SOCKET) {
    return -1;
  }

  memset(&saddr, 0, sizeof(saddr));
  memcpy(&saddr, &ctx->micRemoteAddr, sizeof(saddr));
  SET_PORT(&saddr, ctx->micPortNumber);

  err = sendto(ctx->micSocket, data, length, 0, (struct sockaddr *)&saddr, ctx->micAddrLen);
  if (err < 0) {
    return (int)LastSocketError();
  }

  return (int)err;
}

int sendMicrophoneData(const char* data, int length) {
  PML_CONNECTION_CONTEXT ctx = LiGetEffectiveConnectionContext();
  return sendMicrophoneDataCtx(&ctx->micContext, data, length);
}
