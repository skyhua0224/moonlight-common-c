// Exercise the real client state machine over loopback UDP with a controlled clock.
#include "Limelight-internal.h"
#include "EndpointRefresh.h"
#include <stdio.h>

// The state machine reads the stream configuration and addresses through the
// effective connection context; this test supplies the global one with no thread override.
ML_CONNECTION_CONTEXT gConnectionContext;
PML_CONNECTION_CONTEXT LiGetThreadConnectionContext(void) { return NULL; }
static uint64_t now = 10000;
static unsigned int opened, closed;
static unsigned int clockReads, dualSocketPolls;

uint64_t PltGetMillis(void) { clockReads++; return now; }
int PltCreateMutex(PLT_MUTEX* mutex) {
#ifdef LC_WINDOWS
    InitializeSRWLock(mutex); return 0;
#else
    return pthread_mutex_init(mutex, NULL);
#endif
}
void PltDeleteMutex(PLT_MUTEX* mutex) {
#ifdef LC_WINDOWS
    (void)mutex;
#else
    pthread_mutex_destroy(mutex);
#endif
}
void PltLockMutex(PLT_MUTEX* mutex) {
#ifdef LC_WINDOWS
    AcquireSRWLockExclusive(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}
void PltUnlockMutex(PLT_MUTEX* mutex) {
#ifdef LC_WINDOWS
    ReleaseSRWLockExclusive(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}
void LiReplaceStreamSocketCtx(PML_CONNECTION_CONTEXT ctx, SOCKET previous, SOCKET replacement, int slot) { (void)ctx; (void)previous; (void)replacement; (void)slot; }
int setNonFatalRecvTimeoutMs(SOCKET s, int timeout) { (void)s; (void)timeout; return 0; }
void closeSocket(SOCKET s) {
    closed++;
#ifdef LC_WINDOWS
    closesocket(s);
#else
    close(s);
#endif
}
SOCKET bindUdpSocket(int family, struct sockaddr_storage* local, SOCKADDR_LEN length, int bufferSize, int qos) {
    (void)bufferSize; (void)qos;
    SOCKET s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return s;
    }
    if (bind(s, (struct sockaddr*)local, length)) {
        closeSocket(s);
        return INVALID_SOCKET;
    }
    opened++;
    return s;
}

int pollSockets(struct pollfd* fds, int count, int timeout) {
    // Clock advances explicitly in this test. Use real UDP readiness without
    // sleeping through every simulated timeout in the retry-budget checks.
    (void)timeout;
    if (count == 2) {
        dualSocketPolls++;
    }
    fd_set reads;
    struct timeval immediate = { 0, 0 };
    SOCKET highest = 0;
    FD_ZERO(&reads);
    for (int i = 0; i < count; i++) {
        fds[i].revents = 0;
        FD_SET(fds[i].fd, &reads);
        if (fds[i].fd > highest) {
            highest = fds[i].fd;
        }
    }
    int result = select((int)(highest + 1), &reads, NULL, NULL, &immediate);
    if (result > 0) {
        for (int i = 0; i < count; i++) {
            if (FD_ISSET(fds[i].fd, &reads)) {
                fds[i].revents = POLLIN;
            }
        }
    }
    return result;
}

int recvUdpSocket(SOCKET socket, char* buffer, int size, bool useSelect) {
    (void)useSelect;
    struct pollfd readable = { .fd = socket, .events = POLLIN };
    int result = pollSockets(&readable, 1, 0);
    return result > 0 ? (int)recvfrom(socket, buffer, size, 0, NULL, NULL) : result;
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); return 1; } } while (0)

static int readProbe(SOCKET host, struct sockaddr_storage* peer, LI_ENDPOINT_REFRESH* state, LI_ER_MESSAGE* message) {
    fd_set reads;
    struct timeval timeout = { 1, 0 };
    unsigned char packet[64];
    SOCKADDR_LEN length = sizeof(*peer);
    FD_ZERO(&reads); FD_SET(host, &reads);
    if (select((int)(host + 1), &reads, NULL, NULL, &timeout) != 1) {
        return 0;
    }
    int bytes = recvfrom(host, (char*)packet, sizeof(packet), 0, (struct sockaddr*)peer, &length);
    return bytes == 64 && LiErDecode(packet, 64, LI_ER_VIDEO, state->session, state->key, message);
}

static int sendPacket(SOCKET host, const struct sockaddr_storage* peer, const unsigned char* packet, int size) {
    return sendto(host, (const char*)packet, size, 0, (const struct sockaddr*)peer, AddrLen);
}

static bool hostEmpty(SOCKET host) {
    struct pollfd readable = { .fd = host, .events = POLLIN };
    return pollSockets(&readable, 1, 0) == 0;
}

int main(void) {
    AddrLen = sizeof(struct sockaddr_in);
#ifdef LC_WINDOWS
    WSADATA wsa;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    struct sockaddr_in* local = (struct sockaddr_in*)&LocalAddr;
    local->sin_family = AF_INET;
    local->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SOCKET host = bindUdpSocket(AF_INET, &LocalAddr, AddrLen, 0, 0);
    CHECK(host != INVALID_SOCKET);
    SOCKADDR_LEN addressLength = sizeof(RemoteAddr);
    CHECK(getsockname(host, (struct sockaddr*)&RemoteAddr, &addressLength) == 0);
    SOCKET active = bindUdpSocket(AF_INET, &LocalAddr, AddrLen, 0, 0);
    CHECK(active != INVALID_SOCKET);
    struct sockaddr_storage originalPeer;
    addressLength = sizeof(originalPeer);
    CHECK(getsockname(active, (struct sockaddr*)&originalPeer, &addressLength) == 0);
    unsigned char media[16] = { 0x80, 97, 1, 2, 3 }, buffer[256];
    unsigned char packet[64], nonce[16];
    memset(nonce, 0x42, sizeof(nonce));
    LI_ENDPOINT_REFRESH state;
    gConnectionContext.EndpointRefreshRequested = false;
    CHECK(LiErClientInitialize(&state, active, LI_ER_VIDEO, "0123456789abcdef", ntohs(((struct sockaddr_in*)&RemoteAddr)->sin_port), 0, 0));
    CHECK(!LiErClientPing(&state));
    CHECK(sendPacket(host, &originalPeer, media, sizeof(media)) == sizeof(media));
    unsigned int readsBefore = clockReads;
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == sizeof(media));
    CHECK(clockReads == readsBefore && dualSocketPolls == 0); // Legacy fast path unchanged.
    LiErClientDestroy(&state);
    gConnectionContext.EndpointRefreshRequested = true;
    CHECK(LiErClientInitialize(&state, active, LI_ER_VIDEO, "0123456789abcdef", ntohs(((struct sockaddr_in*)&RemoteAddr)->sin_port), 0, 0));
    CHECK(LiErClientPing(&state) && state.sequence == 0); // Pending ANNOUNCE emits nothing.
    LiSetEndpointAutoRefreshEnabled(true);
    CHECK(LiErClientPing(&state));
    struct sockaddr_storage peer;
    LI_ER_MESSAGE message;
    CHECK(readProbe(host, &peer, &state, &message));
    CHECK(message.type == LI_ER_PROBE);
    for (int i = 0; i < 100; i++) {
        CHECK(LiErClientPing(&state));
    }
    CHECK(state.sequence == 1 && hostEmpty(host)); // Repeated callers cannot flood probes.
    CHECK(LiErEncode(packet, LI_ER_CHALLENGE, LI_ER_VIDEO, state.session, state.key, message.sequence, nonce));
    CHECK(sendPacket(host, &originalPeer, packet, sizeof(packet)) == 64);
    readsBefore = clockReads;
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    CHECK(clockReads == readsBefore + 1);
    CHECK(readProbe(host, &peer, &state, &message) && message.type == LI_ER_RESPONSE);
    CHECK(state.candidate == INVALID_SOCKET); // NAT rebinding can keep the active socket.
    for (int i = 0; i < 50; i++) {
        CHECK(sendPacket(host, &originalPeer, packet, sizeof(packet)) == 64);
        CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    }
    CHECK(hostEmpty(host)); // Authenticated challenge replay is also rate limited.
    now += 250;
    CHECK(sendPacket(host, &originalPeer, packet, sizeof(packet)) == 64);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    CHECK(readProbe(host, &peer, &state, &message) && message.type == LI_ER_RESPONSE);
    CHECK(LiErEncode(packet, LI_ER_ACK, LI_ER_VIDEO, state.session, state.key, message.sequence, nonce));
    CHECK(sendPacket(host, &originalPeer, packet, sizeof(packet)) == 64);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    CHECK(sendPacket(host, &originalPeer, media, sizeof(media)) == sizeof(media));
    readsBefore = clockReads;
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == sizeof(media));
    LiErClientMedia(&state);
    CHECK(clockReads == readsBefore + 1 && state.lastMedia == now && dualSocketPolls == 0);

    now += 2100;
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == 0);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == 0);
    CHECK(state.candidate != INVALID_SOCKET && dualSocketPolls > 0);
    SOCKET original = active;
    // No periodic ping call: socket creation itself must send the first probe.
    CHECK(readProbe(host, &peer, &state, &message) && message.type == LI_ER_PROBE);
    CHECK(LiErEncode(packet, LI_ER_CHALLENGE, LI_ER_VIDEO, state.session, state.key, message.sequence, nonce));
    CHECK(sendPacket(host, &peer, packet, sizeof(packet)) == 64);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    CHECK(active == original && state.challenged);
    CHECK(readProbe(host, &peer, &state, &message) && message.type == LI_ER_RESPONSE);
    CHECK(LiErEncode(packet, LI_ER_ACK, LI_ER_VIDEO, state.session, state.key, message.sequence, nonce));
    CHECK(sendPacket(host, &originalPeer, packet, sizeof(packet)) == 64);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    CHECK(active == original); // An ACK received on the old socket cannot promote the candidate.
    packet[48] ^= 1;
    CHECK(sendPacket(host, &peer, packet, sizeof(packet)) == 64);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    CHECK(active == original);
    packet[48] ^= 1;
    // Simultaneous old media and unverified candidate media must not starve the old path.
    CHECK(sendPacket(host, &peer, media, sizeof(media)) == sizeof(media));
    CHECK(sendPacket(host, &originalPeer, media, sizeof(media)) == sizeof(media));
    int accepted = 0;
    for (int i = 0; i < 2; i++) {
        int result = LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false);
        CHECK(result == sizeof(media) || result == LI_ER_RECV_HANDLED);
        if (result == sizeof(media)) { accepted++; LiErClientMedia(&state); }
    }
    CHECK(accepted == 1 && active == original);
    CHECK(sendPacket(host, &peer, packet, sizeof(packet)) == 64);
    CHECK(sendPacket(host, &peer, media, sizeof(media)) == sizeof(media));
    CHECK(sendPacket(host, &peer, media, sizeof(media)) == sizeof(media));
    // Clock does not advance: neither challenge nor ACK waits for a 100ms polling tick.
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    CHECK(active != original && state.candidate == INVALID_SOCKET && state.attempts == 1);
    for (int i = 0; i < 2; i++) {
        CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == sizeof(media));
        CHECK(!memcmp(buffer, media, sizeof(media)));
        LiErClientMedia(&state); // Media queued after the ACK must be delivered, not batch-drained.
    }

    // A lost commit ACK can be recovered by a later probe ACK with the same nonce.
    now += 2100;
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == 0);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == 0);
    CHECK(state.candidate != INVALID_SOCKET && state.attempts == 2);
    CHECK(readProbe(host, &peer, &state, &message) && message.type == LI_ER_PROBE);
    nonce[0]++;
    CHECK(LiErEncode(packet, LI_ER_CHALLENGE, LI_ER_VIDEO, state.session, state.key, message.sequence, nonce));
    CHECK(sendPacket(host, &peer, packet, sizeof(packet)) == 64);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    CHECK(readProbe(host, &peer, &state, &message) && message.type == LI_ER_RESPONSE);
    CHECK(sendPacket(host, &peer, media, sizeof(media)) == sizeof(media));
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    now += 500;
    CHECK(LiErClientPing(&state));
    CHECK(readProbe(host, &peer, &state, &message) && message.type == LI_ER_PROBE);
    CHECK(LiErEncode(packet, LI_ER_ACK, LI_ER_VIDEO, state.session, state.key, message.sequence, nonce));
    CHECK(sendPacket(host, &peer, packet, sizeof(packet)) == 64);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
    CHECK(state.candidate == INVALID_SOCKET && state.attempts == 2);

    // Scattered failures must not exhaust a session's lifetime budget. Exercise
    // real ACK/media traffic between five migrations, beyond the stable window.
    for (int migration = 0; migration < 5; migration++) {
        for (int tick = 0; tick < 64; tick++) {
            now += 500;
            CHECK(LiErClientPing(&state));
            CHECK(readProbe(host, &peer, &state, &message) && message.type == LI_ER_PROBE);
            CHECK(LiErEncode(packet, LI_ER_ACK, LI_ER_VIDEO, state.session, state.key, message.sequence, nonce));
            CHECK(sendPacket(host, &peer, packet, sizeof(packet)) == 64);
            CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
            CHECK(sendPacket(host, &peer, media, sizeof(media)) == sizeof(media));
            CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == sizeof(media));
            LiErClientMedia(&state);
        }
        CHECK(state.attempts == 0 && !state.failed);
        now += 2100;
        CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == 0);
        CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == 0);
        CHECK(state.candidate != INVALID_SOCKET && state.attempts == 1);
        original = active;
        CHECK(readProbe(host, &peer, &state, &message) && message.type == LI_ER_PROBE);
        nonce[0]++;
        CHECK(LiErEncode(packet, LI_ER_CHALLENGE, LI_ER_VIDEO, state.session, state.key, message.sequence, nonce));
        CHECK(sendPacket(host, &peer, packet, sizeof(packet)) == 64);
        CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
        CHECK(readProbe(host, &peer, &state, &message) && message.type == LI_ER_RESPONSE);
        CHECK(LiErEncode(packet, LI_ER_ACK, LI_ER_VIDEO, state.session, state.key, message.sequence, nonce));
        CHECK(sendPacket(host, &peer, packet, sizeof(packet)) == 64);
        CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_HANDLED);
        CHECK(active != original && state.candidate == INVALID_SOCKET && state.attempts == 1);
    }

    int result = 0;
    for (int i = 0; i < 110 && result != LI_ER_RECV_FAILED; i++) {
        now += 100;
        result = LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false);
    }
    CHECK(result == LI_ER_RECV_FAILED && state.attempts == 3 && state.candidate == INVALID_SOCKET);
    while (!hostEmpty(host)) {
        CHECK(readProbe(host, &peer, &state, &message));
    }
    uint64_t terminalSequence = state.sequence;
    for (int i = 0; i < 100; i++) {
        now += 500;
        CHECK(LiErClientPing(&state));
    }
    CHECK(state.sequence == terminalSequence && hostEmpty(host));
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == LI_ER_RECV_FAILED);
    LiErClientDestroy(&state);
    closeSocket(active);
    CHECK(opened == closed + 1); // Keep a reachable peer for the stop-during-probe case.
    active = bindUdpSocket(AF_INET, &LocalAddr, AddrLen, 0, 0);
    CHECK(active != INVALID_SOCKET);
    CHECK(LiErClientInitialize(&state, active, LI_ER_VIDEO, "0123456789abcdef", ntohs(((struct sockaddr_in*)&RemoteAddr)->sin_port), 0, 0));
    now += 10000; // Slow RTSP/startup must not count as a receive failure.
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == 0);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == 0);
    CHECK(state.candidate == INVALID_SOCKET && state.attempts == 0);
    now += 2100;
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == 0);
    CHECK(LiErClientRecv(&state, &active, (char*)buffer, sizeof(buffer), false) == 0);
    CHECK(state.candidate != INVALID_SOCKET);
    LiErClientDestroy(&state); // Stop while a candidate is still awaiting proof.
    closeSocket(active);
    closeSocket(host);
    CHECK(opened == closed);
#ifdef LC_WINDOWS
    WSACleanup();
#endif
    puts("Endpoint refresh client: startup grace, repeated migration, dual receive, ACK isolation, rate limits and bounded cleanup passed");
    return 0;
}
