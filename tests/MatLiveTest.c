/* Synthetic carrier bytes exercise transport only. No recorded MAT input or Dolby decode claim. */
#include "MatLivePlayback.h"
#include "MatLiveCrypto.h"
#include <stdio.h>
#include <stdlib.h>
static int failures,cycles,ends,reject;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures;}}while(0)
static int cycle(void* c,const uint8_t* b,int n,uint64_t e,uint64_t d,uint64_t i,uint64_t pts){
 (void)c;CHECK(n==61440 && b[0]==0x72 && e==123 && d==7 && i==(unsigned)cycles && pts==i*960);++cycles;return !reject;
}
static int end(void* c,uint64_t count){(void)c;CHECK(count==(unsigned)cycles);++ends;return !reject;}
static uint8_t key[16],carrier[MAT_WIRE_CYCLE],plain[MAT_WIRE_PACKET],wire[MAT_LIVE_PACKET];
static LI_MAT_LIVE_PROFILE profile={123,7};
static MAT_LIVE_CIPHER cipher;
static MAT_LIVE_PLAYBACK* open_receiver(void){LI_MAT_PLAYBACK_CALLBACKS cb={0};cb.cycle=cycle;cb.end=end;cycles=ends=reject=0;return MatLivePlaybackOpen(&profile,key,&cb);}
static int push(MAT_LIVE_PLAYBACK* receiver,uint64_t sequence){CHECK(MatLiveSeal(&cipher,sequence,plain,wire));return MatLivePlaybackPush(receiver,wire,sizeof(wire));}
int main(int argc,char** argv){
 for(unsigned i=0;i<16;i++)key[i]=(uint8_t)i;
 CHECK(MatLiveCipherInit(&cipher,key,16,123,7));
 if(argc==2){for(unsigned i=0;i<MAT_WIRE_PACKET;i++)plain[i]=(uint8_t)i;CHECK(MatLiveSeal(&cipher,65536,plain,wire));FILE* f=fopen(argv[1],"wb");if(!f)return 2;fwrite(cipher.key,1,16,f);fwrite(wire,1,sizeof(wire),f);fclose(f);}
 memcpy(carrier,"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);
 MAT_LIVE_PLAYBACK* receiver=open_receiver();CHECK(receiver);uint32_t hash=2166136261u;uint64_t packet=4;
 for(unsigned i=0;i<1300;i++){
   carrier[8]=(uint8_t)i;carrier[9]=(uint8_t)(i>>8);uint32_t h=mat_hash(carrier,sizeof(carrier));
   for(unsigned j=0;j<60;j++){mat_wire_make(plain,MAT_WIRE_DATA,123,7,i,j,h,carrier);CHECK(push(receiver,packet++)==(j==59?1:0));}
   hash=mat_hash_update(hash,carrier,sizeof(carrier));
 }
 CHECK(packet==78004 && cycles==1300 && !ends);
 mat_wire_make(plain,MAT_WIRE_END,123,7,1300,0,hash,NULL);
 for(unsigned j=0;j<4;j++)CHECK(push(receiver,packet++)==2);
 CHECK(ends==1);MatLivePlaybackClose(receiver);
 // Missing EOS/close must never drain the renderer as a successful stream.
 receiver=open_receiver();mat_wire_make(plain,MAT_WIRE_DATA,123,7,0,0,mat_hash(carrier,sizeof(carrier)),carrier);CHECK(push(receiver,4)==0);MatLivePlaybackClose(receiver);CHECK(!ends);
 // Replay, gaps, ciphertext/tag/header corruption, wrong key/domain and sticky failure.
 for(unsigned test=0;test<8;test++){
   receiver=open_receiver();mat_wire_make(plain,MAT_WIRE_DATA,123,7,0,0,mat_hash(carrier,sizeof(carrier)),carrier);
   if(test==5){MAT_LIVE_CIPHER wrong=cipher;wrong.key[0]^=1;CHECK(MatLiveSeal(&wrong,4,plain,wire));}
   else if(test==6){mat_put64(plain+24,8);mat_put32(plain+72,mat_packet_hash(plain));CHECK(MatLiveSeal(&cipher,4,plain,wire));}
   else CHECK(MatLiveSeal(&cipher,test==0?3:test==1?5:4,plain,wire));
   if(test==2)wire[32]^=1;if(test==3)wire[sizeof(wire)-1]^=1;if(test==4)wire[8]^=1;
   if(test==7){CHECK(MatLivePlaybackPush(receiver,wire,sizeof(wire))==0);}
   CHECK(MatLivePlaybackPush(receiver,wire,sizeof(wire))<0);CHECK(MatLivePlaybackPush(receiver,wire,sizeof(wire))<0);CHECK(!cycles&&!ends);MatLivePlaybackClose(receiver);
 }
 receiver=open_receiver();reject=1;for(unsigned j=0;j<60;j++){mat_wire_make(plain,MAT_WIRE_DATA,123,7,0,j,mat_hash(carrier,sizeof(carrier)),carrier);CHECK(push(receiver,4+j)==(j==59?-1:0));}CHECK(cycles==1&&!ends);MatLivePlaybackClose(receiver);
 uint64_t number;CHECK(!MatLiveDecimal("01",&number));CHECK(!MatLiveDecimal("0",&number));CHECK(!MatLiveDecimal("9223372036854775808",&number));CHECK(MatLiveDecimal("9223372036854775807",&number));
 if(failures)return 1;puts("PASS live MAT: 78000 authenticated data packets, RTP16 wrap, integrity/replay/gap/identity/EOF/callback failures");return 0;
}
