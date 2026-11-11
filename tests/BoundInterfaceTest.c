// Interface pinning: LiBindSocketToInterface, the pinned createSocket path and its status counters.
// Runs against the loopback interface so it needs no network.
#include "PlatformSockets.h"

#include <stdio.h>
#include <string.h>

#if defined(__APPLE__) && defined(IP_BOUND_IF)
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static unsigned int readBoundIf(int fd, int family) {
    unsigned int value = 0xdeadbeef;
    socklen_t len = sizeof(value);
    int level = family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
    int opt = family == AF_INET ? IP_BOUND_IF : IPV6_BOUND_IF;
    if (getsockopt(fd, level, opt, &value, &len) < 0) {
        return 0xdeadbeef;
    }
    return value;
}

int main(void) {
    unsigned int lo = if_nametoindex("lo0");
    LI_BOUND_INTERFACE_STATUS status;
    int fd;

    CHECK(lo != 0);

    // The helper sets the option on IPv4 and IPv6, TCP and UDP.
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(readBoundIf(fd, AF_INET) == 0);
    CHECK(LiBindSocketToInterface(fd, AF_INET, lo) == 0);
    CHECK(readBoundIf(fd, AF_INET) == lo);
    close(fd);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(LiBindSocketToInterface(fd, AF_INET, lo) == 0);
    CHECK(readBoundIf(fd, AF_INET) == lo);
    close(fd);

    fd = socket(AF_INET6, SOCK_DGRAM, 0);
    CHECK(LiBindSocketToInterface(fd, AF_INET6, lo) == 0);
    CHECK(readBoundIf(fd, AF_INET6) == lo);
    close(fd);

    // Index 0 means "no pinning": success, socket untouched.
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(LiBindSocketToInterface(fd, AF_INET, 0) == 0);
    CHECK(readBoundIf(fd, AF_INET) == 0);
    // An interface that does not exist fails with an errno, and an unknown family is refused.
    CHECK(LiBindSocketToInterface(fd, AF_INET, 99999) != 0);
    CHECK(LiBindSocketToInterface(fd, AF_UNIX, lo) != 0);
    close(fd);

    // Pinned stream sockets: nothing set means untouched; set means every kind is bound and counted.
    CHECK(LiSetBoundInterface(0) == 0);
    fd = (int)createSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, false);
    CHECK(fd >= 0 && readBoundIf(fd, AF_INET) == 0);
    close(fd);

    CHECK(LiSetBoundInterface(lo) == 0);
    fd = (int)createSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, true);
    CHECK(fd >= 0 && readBoundIf(fd, AF_INET) == lo);
    close(fd);
    fd = (int)createSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, false);
    CHECK(fd >= 0 && readBoundIf(fd, AF_INET) == lo);
    close(fd);
    fd = (int)createSocket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, false);
    CHECK(fd >= 0 && readBoundIf(fd, AF_INET6) == lo);
    close(fd);

    // The ENet seam (called on the socket ENet made).
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(bindEnetSocketToInterface(fd, AF_INET) == 0);
    CHECK(readBoundIf(fd, AF_INET) == lo);
    close(fd);

    LiGetBoundInterfaceStatus(&status);
    CHECK(status.ifIndex == lo);
    CHECK(status.ok[LI_BOUND_IF_KIND_TCP] == 1);
    CHECK(status.ok[LI_BOUND_IF_KIND_UDP] == 2);
    CHECK(status.ok[LI_BOUND_IF_KIND_ENET] == 1);
    CHECK(status.failed[LI_BOUND_IF_KIND_TCP] + status.failed[LI_BOUND_IF_KIND_UDP] + status.failed[LI_BOUND_IF_KIND_ENET] == 0);
    CHECK(status.lastError == 0);

    // A pinned TCP connect over loopback really works through the pinned socket.
    {
        struct sockaddr_in listenAddr;
        struct sockaddr_storage target;
        socklen_t len = sizeof(listenAddr);
        int listener = socket(AF_INET, SOCK_STREAM, 0);
        SOCKET c;

        memset(&listenAddr, 0, sizeof(listenAddr));
        listenAddr.sin_family = AF_INET;
        listenAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(bind(listener, (struct sockaddr*)&listenAddr, sizeof(listenAddr)) == 0);
        CHECK(listen(listener, 1) == 0);
        CHECK(getsockname(listener, (struct sockaddr*)&listenAddr, &len) == 0);
        memset(&target, 0, sizeof(target));
        memcpy(&target, &listenAddr, sizeof(listenAddr));
        c = connectTcpSocket(&target, sizeof(listenAddr), ntohs(listenAddr.sin_port), 3);
        CHECK(c != INVALID_SOCKET);
        if (c != INVALID_SOCKET) {
            CHECK(readBoundIf((int)c, AF_INET) == lo);
            closeSocket(c);
        }
        close(listener);
    }

    // A failed bind fails the socket and is counted; no fallback to the default route.
    CHECK(LiSetBoundInterface(99999) == 0);
    CHECK(createSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, true) == INVALID_SOCKET);
    CHECK(createSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, false) == INVALID_SOCKET);
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(bindEnetSocketToInterface(fd, AF_INET) != 0);
    close(fd);
    LiGetBoundInterfaceStatus(&status);
    CHECK(status.ifIndex == 99999);
    CHECK(status.failed[LI_BOUND_IF_KIND_TCP] == 1);
    CHECK(status.failed[LI_BOUND_IF_KIND_UDP] == 1);
    CHECK(status.failed[LI_BOUND_IF_KIND_ENET] == 1);
    CHECK(status.lastError != 0);

    // Clearing resets the counters.
    CHECK(LiSetBoundInterface(0) == 0);
    LiGetBoundInterfaceStatus(&status);
    CHECK(status.ifIndex == 0 && status.failed[LI_BOUND_IF_KIND_TCP] == 0);

    if (failures == 0) {
        printf("bound interface tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
#else
int main(void) {
    printf("bound interface tests skipped: no IP_BOUND_IF on this platform\n");
    return 0;
}
#endif
