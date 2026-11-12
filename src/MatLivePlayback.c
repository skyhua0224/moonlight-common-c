#include "MatLivePlayback.h"
#include "MatLiveCrypto.h"
#include "MatReceiver.h"
#include <stdlib.h>
struct MAT_LIVE_PLAYBACK {
    MAT_RECEIVER receiver;MAT_LIVE_CIPHER cipher;LI_MAT_PLAYBACK_CALLBACKS callbacks;
    uint64_t nextPacket;int failed;
};
static int cycle(void* context,const MAT_CYCLE_INFO* info,const uint8_t* bytes,size_t n) {
    MAT_LIVE_PLAYBACK* s=context;
    return s->callbacks.cycle(s->callbacks.context,bytes,(int)n,info->epoch,info->domain,info->index,info->pts48k);
}
static int end(void* context,uint64_t count) {
    MAT_LIVE_PLAYBACK* s=context;return s->callbacks.end(s->callbacks.context,count);
}
MAT_LIVE_PLAYBACK* MatLivePlaybackOpen(const LI_MAT_LIVE_PROFILE* p,const uint8_t key[16],const LI_MAT_PLAYBACK_CALLBACKS* callbacks) {
    if(!p||!callbacks||!callbacks->cycle||!callbacks->end)return NULL;
    MAT_LIVE_PLAYBACK* s=calloc(1,sizeof(*s));if(!s)return NULL;
    if(!MatLiveCipherInit(&s->cipher,key,16,p->epoch,p->domain)){free(s);return NULL;}
    s->callbacks=*callbacks;s->nextPacket=4; /* four authenticated transport SYNC packets precede RTP data */
    MatReceiverInit(&s->receiver,cycle,end,s);return s;
}
int MatLivePlaybackPush(MAT_LIVE_PLAYBACK* s,const uint8_t* wire,size_t length) {
    uint8_t plain[MAT_WIRE_PACKET];
    if(!s||s->failed)return -1;
    if(!MatLiveOpenPacket(&s->cipher,s->nextPacket,wire,length,plain) ||
        mat_get64(plain+16)!=s->cipher.epoch||mat_get64(plain+24)!=s->cipher.domain) {
        MatLivePlaybackFail(s);return -1;
    }
    int result=MatReceiverPush(&s->receiver,plain,sizeof(plain));MatLiveWipe(plain,sizeof(plain));
    if(result<0){MatLivePlaybackFail(s);return -1;}
    ++s->nextPacket;return result;
}
void MatLivePlaybackFail(MAT_LIVE_PLAYBACK* s){if(s){s->failed=1;MatReceiverAbort(&s->receiver);}}
void MatLivePlaybackClose(MAT_LIVE_PLAYBACK* s){if(s){MatLiveWipe(s,sizeof(*s));free(s);}}
