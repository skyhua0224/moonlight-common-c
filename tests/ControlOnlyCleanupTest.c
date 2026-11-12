// Include the actual lifecycle implementation to exercise its private stage.
// Startup-only dependencies abort if cleanup accidentally enters them.
#include "../src/Connection.c"
#undef StreamConfig
#include <stdio.h>

static int avCalls, controlCalls, inputCalls, platformCalls;
void LiSetEndpointAutoRefreshEnabled(bool enabled) {
    (void) enabled;
}
// The connection lifecycle works on the connection context; count the
// per-stream teardown calls it makes on it.
void destroyMicrophoneStream(void) {}
int stopInputStreamCtx(PML_INPUT_STREAM_CONTEXT c) { (void)c; inputCalls++; return 0; }
void destroyInputStreamCtx(PML_INPUT_STREAM_CONTEXT c) { (void)c; inputCalls++; }
int stopControlStreamCtx(PML_CONTROL_STREAM_CONTEXT c) { (void)c; controlCalls++; return 0; }
void destroyControlStreamCtx(PML_CONTROL_STREAM_CONTEXT c) { (void)c; controlCalls++; }
void stopAudioStreamCtx(PML_AUDIO_STREAM_CONTEXT c) { (void)c; avCalls++; }
void stopVideoStreamCtx(PML_VIDEO_STREAM_CONTEXT c) { (void)c; avCalls++; }
void destroyAudioStreamCtx(PML_AUDIO_STREAM_CONTEXT c) { (void)c; avCalls++; }
void destroyVideoStreamCtx(PML_VIDEO_STREAM_CONTEXT c) { (void)c; avCalls++; }
void cleanupPlatform(void) { platformCalls++; }

int PltCreateThread(const char* n, ThreadEntry e, void* c, PLT_THREAD* t) { abort(); }
void PltDetachThread(PLT_THREAD* t) { abort(); }
void PltSleepMs(int ms) { abort(); }
int initializePlatform(void) { abort(); }
int extractVersionQuadFromString(const char* s, int* q) { abort(); }
void fixupMissingCallbacks(PDECODER_RENDERER_CALLBACKS* d, PAUDIO_RENDERER_CALLBACKS* a, PCONNECTION_LISTENER_CALLBACKS* c) { abort(); }
int resolveHostName(const char* h, int f, int p, struct sockaddr_storage* a, SOCKADDR_LEN* l) { abort(); }
int getLocalAddressByUdpConnect(const struct sockaddr_storage* r, SOCKADDR_LEN rl, unsigned short p, struct sockaddr_storage* l, SOCKADDR_LEN* ll) { abort(); }
bool isNat64SynthesizedAddress(struct sockaddr_storage* a) { abort(); }
bool isPrivateNetworkAddress(struct sockaddr_storage* a) { abort(); }
volatile int Ds5PadAudioOfferMode;
int initializeAudioStreamCtx(PML_AUDIO_STREAM_CONTEXT c, PML_CONNECTION_CONTEXT cc) { abort(); }
int performRtspHandshakeCtx(PML_CONNECTION_CONTEXT c, PSERVER_INFORMATION i) { abort(); }
int initializeControlStreamCtx(PML_CONTROL_STREAM_CONTEXT c, PML_CONNECTION_CONTEXT cc) { abort(); }
void initializeVideoStreamCtx(PML_VIDEO_STREAM_CONTEXT c, PML_CONNECTION_CONTEXT cc) { abort(); }
int initializeInputStreamCtx(PML_INPUT_STREAM_CONTEXT c, PML_CONNECTION_CONTEXT cc) { abort(); }
int startControlStreamCtx(PML_CONTROL_STREAM_CONTEXT c) { abort(); }
int startVideoStreamCtx(PML_VIDEO_STREAM_CONTEXT c, void* r, int f) { abort(); }
int startAudioStreamCtx(PML_AUDIO_STREAM_CONTEXT c, void* r, int f) { abort(); }
int startInputStreamCtx(PML_INPUT_STREAM_CONTEXT c) { abort(); }
int initializeMicrophoneStream(void) { abort(); }
int LiSendMouseMoveEvent(short x, short y) { abort(); }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    for (unsigned int controlOnly = 0; controlOnly < 2; controlOnly++) {
        avCalls = controlCalls = inputCalls = platformCalls = 0;
        gConnectionContext.StreamConfig.controlOnly = controlOnly;
        gConnectionContext.stage = STAGE_INPUT_STREAM_START;
        LiStopConnection();
        CHECK(gConnectionContext.stage == STAGE_NONE);
        CHECK(avCalls == (controlOnly ? 0 : 4));
        CHECK(controlCalls == 2 && inputCalls == 2 && platformCalls == 1);
        // A failed handshake must likewise avoid resources skipped at startup.
        avCalls = controlCalls = inputCalls = platformCalls = 0;
        gConnectionContext.stage = STAGE_RTSP_HANDSHAKE;
        LiStopConnection();
        CHECK(gConnectionContext.stage == STAGE_NONE && controlCalls == 0 && inputCalls == 0 && platformCalls == 1);
        CHECK(avCalls == (controlOnly ? 0 : 1));
    }
    puts("Control-only cleanup: skipped AV resources untouched on stop and failed handshake");
    return 0;
}
