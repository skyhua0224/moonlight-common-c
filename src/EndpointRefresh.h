#pragma once
#include "PlatformSockets.h"
#include "PlatformThreads.h"
#include "EndpointRefreshProtocol.h"

typedef struct _LI_ENDPOINT_REFRESH {
    bool initialized;
    bool receiving;
    PLT_MUTEX mutex;
    SOCKET active;
    SOCKET candidate;
    SOCKET lastProbeSocket;
    struct sockaddr_storage remote;
    uint8_t stream;
    int qos;
    int bufferSize;
    unsigned char session[LI_ER_SESSION_SIZE], key[LI_ER_KEY_SIZE], nonce[LI_ER_NONCE_SIZE];
    uint64_t sequence, ackSequence, challengeSequence, candidateFirstSequence;
    uint64_t lastAck, lastMedia, candidateStarted, lastPoll, stableSince, receiveTime;
    uint64_t lastProbeAt, activeResponseAt, candidateResponseAt;
    unsigned int attempts;
    bool challenged;
    bool candidateConfirmed;
    bool candidateNext;
    bool failed;
} LI_ENDPOINT_REFRESH;

bool LiErClientInitialize(LI_ENDPOINT_REFRESH* state, SOCKET socket, uint8_t stream,
                          const char session[LI_ER_SESSION_SIZE], uint16_t remotePort, int qos, int bufferSize);
void LiErClientDestroy(LI_ENDPOINT_REFRESH* state);
bool LiErClientPing(LI_ENDPOINT_REFRESH* state);
// Distinguish handled protocol traffic from a real receive timeout/socket error.
#define LI_ER_RECV_HANDLED -2
#define LI_ER_RECV_FAILED -3
int LiErClientRecv(LI_ENDPOINT_REFRESH* state, SOCKET* socket, char* packet, int size, bool useSelect);
void LiErClientMedia(LI_ENDPOINT_REFRESH* state);

