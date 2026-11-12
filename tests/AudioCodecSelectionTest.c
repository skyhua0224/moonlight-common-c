/* Exercise the real audio startup with a sink that stops before hardware/threads. */
#define PltGetMillis MatTestGetMillis
#include "../src/AudioStream.c"
#undef PltGetMillis
// AudioStream.c redirects state through a local ctx; the test addresses the
// global connection context explicitly.
#undef AudioPortNumber
#undef RemoteAddr
#undef AudioPingPayload
#undef AddrLen
#undef StreamConfig
#undef AudioCallbacks
#undef ListenerCallbacks
#undef AudioEncryptionEnabled
#undef HighQualitySurroundEnabled
#undef HighQualitySurroundSupported
#undef HighQualityOpusConfig
#undef NormalQualityOpusConfig
#undef AudioPacketDuration
#define GC gConnectionContext
#define TA (&gConnectionContext.audioContext)
#include "MatWire.h"
#include "MatReceiver.h"
#include "MatLiveCrypto.h"
#include <stdio.h>
static int failures,expectedCodec;
static uint64_t testNow;
static bool publishDuringClock;
uint64_t MatTestGetMillis(void) {
    if(publishDuringClock)atomic_store_explicit(&matLastAcceptedMs,testNow+1,memory_order_relaxed);
    return testNow;
}
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c);failures++;} } while(0)
static int received,ended,terminated,rejectCycle,rejectEnd;
static int consumeCycle(void* context,const unsigned char* bytes,int n,uint64_t epoch,uint64_t domain,uint64_t index,uint64_t pts) {
    (void)context;CHECK(MatCarrierIsValid(bytes,(size_t)n));CHECK(epoch==1&&domain==1&&index==0&&pts==0);
    ++received;return !rejectCycle;
}
static int consumeEnd(void* context,uint64_t count){(void)context;CHECK(count==1);testNow+=5000;CHECK(!matInputTimedOut(TA));++ended;return !rejectEnd;}
static void terminate(int error){(void)error;++terminated;}
static void liveCallbacks(void) {
    unsigned char carrier[MAT_WIRE_CYCLE]={0},packet[MAT_WIRE_PACKET],wire[MAT_LIVE_PACKET],key[16]={0};
    memcpy(carrier,"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);
    MAT_LIVE_CIPHER cipher;
    LI_MAT_LIVE_PROFILE profile={1,1};
    CHECK(MatLiveCipherInit(&cipher,key,sizeof(key),1,1));
    LI_MAT_PLAYBACK_CALLBACKS callbacks={consumeCycle,consumeEnd,NULL};
    LiSetMatPlaybackCallbacks(&callbacks);GC.ListenerCallbacks.connectionTerminated=terminate;
    for(int test=0;test<4;test++) {
        received=ended=terminated=0;rejectCycle=test==1;rejectEnd=test==2;
        matPlaybackFailedReported=matPlaybackCompleteReported=false;GC.NegotiatedAudioCodec=AUDIO_CODEC_MAT_LIVE;
        LI_MAT_PLAYBACK_CALLBACKS guarded=matCallbacks;guarded.end=matEnd;
        testNow=1000;atomic_store(&matInputEnded,false);atomic_store(&matLastAcceptedMs,testNow);
        matLive=MatLivePlaybackOpen(&profile,key,&guarded);CHECK(matLive!=NULL);
        for(unsigned fragment=0;fragment<MAT_WIRE_FRAGMENTS;fragment++) {
            mat_wire_make(packet,MAT_WIRE_DATA,1,1,0,fragment,mat_hash(carrier,sizeof(carrier)),carrier);
            CHECK(MatLiveSeal(&cipher,4+fragment,packet,wire));
            consumeAudio(TA,(char*)wire,sizeof(wire));
        }
        mat_wire_make(packet,MAT_WIRE_END,1,1,1,0,mat_hash(carrier,sizeof(carrier)),NULL);
        CHECK(MatLiveSeal(&cipher,64,packet,wire));
        if(test!=3)consumeAudio(TA,(char*)wire,sizeof(wire));
        else {testNow+=2000;CHECK(matInputTimedOut(TA));CHECK(!atomic_load(&matInputEnded));}
        CHECK(received==1);CHECK(ended==(test==0||test==2));CHECK(terminated==(test==1||test==2));
        MatLivePlaybackClose(matLive);matLive=NULL;
    }
    LiSetMatPlaybackCallbacks(NULL);
    CHECK(MatLivePlaybackOpen(&profile,key,&matCallbacks)==NULL);
}
static void watchdog(void) {
    const int codecs[]={AUDIO_CODEC_OPUS,AUDIO_CODEC_MAT_LIVE};
    for(unsigned i=0;i<sizeof(codecs)/sizeof(codecs[0]);i++) {
        GC.NegotiatedAudioCodec=codecs[i];atomic_store(&matInputEnded,false);atomic_store(&matLastAcceptedMs,1000);
        testNow=2999;CHECK(!matInputTimedOut(TA));testNow=3000;CHECK(matInputTimedOut(TA)==AUDIO_CODEC_IS_MAT(codecs[i]));
        atomic_store(&matInputEnded,true);testNow=10000;CHECK(!matInputTimedOut(TA));
    }
    GC.NegotiatedAudioCodec=AUDIO_CODEC_MAT_LIVE;atomic_store(&matInputEnded,false);atomic_store(&matLastAcceptedMs,1000);
    testNow=1000;publishDuringClock=true;CHECK(!matInputTimedOut(TA));CHECK(atomic_load(&matLastAcceptedMs)==1001);publishDuringClock=false;
}
static int stopBeforeHardware(int configuration,const POPUS_MULTISTREAM_CONFIGURATION format,void* context,int flags) {
    (void)configuration;(void)context;(void)flags;
    int mat=AUDIO_CODEC_IS_MAT(expectedCodec);
    CHECK(TA->packetQueue.sizeBound==(mat?512:30));
    CHECK(TA->lastSeq==(mat?3:0));
    CHECK(format->sampleRate==48000);
    CHECK(format->samplesPerFrame==(mat?960:240));
    CHECK(matLive==NULL);
    return -123;
}
int main(void) {
    CHECK(initializePlatform()==0);TA->connectionContext=&gConnectionContext;
    liveCallbacks();
    watchdog();
    const int requests[]={AUDIO_CODEC_MAT_AUTO,AUDIO_CODEC_MAT_AUTO,AUDIO_CODEC_OPUS,AUDIO_CODEC_MAT_LIVE};
    const int selections[]={AUDIO_CODEC_MAT_LIVE,AUDIO_CODEC_OPUS,AUDIO_CODEC_OPUS,AUDIO_CODEC_MAT_LIVE};
    for(unsigned i=0;i<sizeof(requests)/sizeof(requests[0]);i++) {
        GC.StreamConfig.audioCodec=requests[i];
        GC.StreamConfig.audioConfiguration=AUDIO_CONFIGURATION_STEREO;
        GC.NegotiatedAudioCodec=selections[i];expectedCodec=selections[i];
        GC.NormalQualityOpusConfig=(OPUS_MULTISTREAM_CONFIGURATION){0};
        GC.NormalQualityOpusConfig.sampleRate=48000;GC.NormalQualityOpusConfig.channelCount=2;GC.NormalQualityOpusConfig.streams=1;
        GC.HighQualitySurroundEnabled=false;GC.AudioPacketDuration=5;GC.AudioCallbacks.init=stopBeforeHardware;
        CHECK(initializeAudioStream()==0);
        CHECK(TA->packetQueue.sizeBound==30 && TA->lastSeq==0);
        CHECK(startAudioStream(NULL,0)==-123);
        CHECK(!TA->pingThreadStarted);
        destroyAudioStream();
    }
    cleanupPlatform();
    if(failures)return 1;
    puts("PASS live/Opus startup, authenticated callbacks/consumer failures, missing EOS watchdog, verified drain and concurrent timestamp publication");
    return 0;
}
