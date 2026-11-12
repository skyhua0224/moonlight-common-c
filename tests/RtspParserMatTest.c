#include "Rtsp.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); failures++; } } while (0)

static void checkResponse(char* text, const char* expected, bool duplicate) {
    RTSP_MESSAGE msg = {0};
    int result = parseRtspMessage(&msg, text, (int)strlen(text));
    CHECK(result == RTSP_ERROR_SUCCESS);
    if (result != RTSP_ERROR_SUCCESS) return;
    char* value = getOptionContent(msg.options, "X-SS-Mat-Clock");
    CHECK(value && !strcmp(value, expected));
    int marked = 0;
    int nodes = 0;
    for (POPTION_ITEM opt = msg.options; opt; opt = opt->next) {
        nodes++;
        if (opt->flags & FLAG_DUPLICATE_OPTION) marked++;
    }
    CHECK(nodes == 2); // CSeq and one coalesced MAT header, including triples.
    CHECK(marked == (duplicate ? 1 : 0));
    CHECK(msg.sequenceNumber == 7);
    freeMessage(&msg);
}

int main(void) {
    checkResponse("RTSP/1.0 200 OK\r\ncseq:7\r\nX-SS-Mat-Clock:\t48000 \t\r\n\r\n", "48000", false);
    checkResponse("RTSP/1.0 200 OK\r\nCSeq:7\r\nX-SS-Mat-Clock: 1\r\nX-SS-Mat-Clock: 48000\r\n\r\n", "48000", true);
    checkResponse("RTSP/1.0 200 OK\r\nCSeq:7\r\nX-SS-Mat-Clock: 1\r\nx-ss-mat-clock: 48000\r\n\r\n", "48000", true);
    checkResponse("RTSP/1.0 200 OK\r\nCSeq:7\r\nX-SS-Mat-Clock: 1\r\nx-ss-mat-clock: 2\r\nX-SS-MAT-CLOCK: 48000\r\n\r\n", "48000", true);

    // Preserve upstream bare-LF / whitespace / binary payload handling.
    char binary[] = "RTSP/1.0\t200 OK\nCSeq:\t7\nContent-Length: 4\n\nA\0BC";
    RTSP_MESSAGE msg = {0};
    int result = parseRtspMessage(&msg, binary, (int)sizeof(binary) - 1);
    CHECK(result == RTSP_ERROR_SUCCESS);
    if (result == RTSP_ERROR_SUCCESS) {
        CHECK(msg.payloadLength == 4);
        CHECK(msg.payload && !memcmp(msg.payload, "A\0BC", 4));
        CHECK(msg.sequenceNumber == 7);
        freeMessage(&msg);
    }
    if (failures) return 1;
    puts("PASS RTSP upstream whitespace/binary and MAT duplicate ownership/flags");
    return 0;
}
