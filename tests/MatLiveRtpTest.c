/* The real client RTP/FEC queue and AudioStream decode path. Synthetic carrier only. */
#include "../src/AudioStream.c"
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
#include "MatLiveCrypto.h"
#include <stdio.h>
static int failures,cycles,ends,terminated;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures;}}while(0)
static int cycle(void* c,const uint8_t* bytes,int n,uint64_t e,uint64_t d,uint64_t index,uint64_t pts){(void)c;(void)bytes;CHECK(n==61440 && e==123 && d==7 && index==(unsigned)cycles && pts==index*960);++cycles;return 1;}
static int end(void* c,uint64_t n){(void)c;CHECK(n==(unsigned)cycles);++ends;return 1;}
static void terminate(int error){(void)error;++terminated;}
static void packet(uint64_t sequence,const uint8_t* payload,size_t size){
 PQUEUED_AUDIO_PACKET p=calloc(1,sizeof(*p));if(!p){++failures;return;}
 p->header.size=sizeof(RTP_PACKET)+(int)size;PRTP_PACKET r=(PRTP_PACKET)p->data;
 r->header=0x80;r->packetType=97;r->sequenceNumber=(uint16_t)sequence;r->timestamp=(uint32_t)sequence;
 memcpy(r+1,payload,size);int status=RtpaAddPacket(&TA->rtpAudioQueue,r,(uint16_t)p->header.size);
 if(RTPQ_HANDLE_NOW(status))decodeInputData(TA,p);
 else {
  uint16_t length;PQUEUED_AUDIO_PACKET queued;
  while((queued=(PQUEUED_AUDIO_PACKET)RtpaGetQueuedPacket(&TA->rtpAudioQueue,sizeof(QUEUED_AUDIO_PACKET_HEADER),&length))!=NULL){queued->header.size=length;decodeInputData(TA,queued);free(queued);}
 }
 free(p);
}
int main(void){
 CHECK(initializePlatform()==0);TA->connectionContext=&gConnectionContext;AppVersionQuad[0]=7;AppVersionQuad[1]=1;AppVersionQuad[2]=431;AppVersionQuad[3]=-1;
 GC.NegotiatedAudioCodec=AUDIO_CODEC_MAT_LIVE;GC.AudioEncryptionEnabled=true;GC.AudioPacketDuration=1;GC.ListenerCallbacks.connectionTerminated=terminate;GC.ListenerCallbacks.logMessage=(ConnListenerLogMessage)printf;
 RtpaInitializeQueue(&TA->rtpAudioQueue);TA->lastSeq=3;
 LI_MAT_LIVE_PROFILE profile={123,7};uint8_t key[16]={};LI_MAT_PLAYBACK_CALLBACKS cb={0};cb.cycle=cycle;cb.end=end;
 matLive=MatLivePlaybackOpen(&profile,key,&cb);CHECK(matLive);
 MAT_LIVE_CIPHER cipher;CHECK(MatLiveCipherInit(&cipher,key,16,123,7));
 uint8_t plain[MAT_WIRE_PACKET],wire[MAT_LIVE_PACKET],carrier[MAT_WIRE_CYCLE]={0};
 memcpy(carrier,"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);
 mat_wire_make(plain,MAT_WIRE_SYNC,123,7,0,0,0,NULL);uint64_t next=0;
 for(unsigned i=0;i<4;i++){CHECK(MatLiveSeal(&cipher,next,plain,wire));packet(next++,wire,sizeof(wire));}
 uint32_t hash=2166136261u;
 for(unsigned i=0;i<1300;i++){
  carrier[8]=(uint8_t)i;uint32_t h=mat_hash(carrier,sizeof(carrier));
  for(unsigned j=0;j<60;j++){mat_wire_make(plain,MAT_WIRE_DATA,123,7,i,j,h,carrier);CHECK(MatLiveSeal(&cipher,next,plain,wire));packet(next++,wire,sizeof(wire));}
  hash=mat_hash_update(hash,carrier,sizeof(carrier));
 }
 mat_wire_make(plain,MAT_WIRE_END,123,7,1300,0,hash,NULL);
 for(unsigned i=0;i<4;i++){CHECK(MatLiveSeal(&cipher,next,plain,wire));packet(next++,wire,sizeof(wire));}
 fprintf(stderr,"cycles=%d ends=%d term=%d lastSeq=%u next=%llu\n",cycles,ends,terminated,TA->lastSeq,(unsigned long long)next);
 CHECK(cycles==1300 && ends==1 && !terminated && TA->lastSeq==(uint16_t)(next-1));
 MatLivePlaybackClose(matLive);matLive=NULL;RtpaCleanupQueue(&TA->rtpAudioQueue);cleanupPlatform();
 if(failures)return 1;puts("PASS actual RTP queue / AudioStream: 78008 packets, RTP16 wrap, legacy CBC bypass, exact authenticated cycles and EOS");return 0;
}
