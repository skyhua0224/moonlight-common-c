#include "MatDepacketizer.h"
#include "MatWire.h"

void MatDepacketizerInit(MAT_DEPACKETIZER* s,MAT_WRITE_CYCLE write,void* context) {
    memset(s,0,sizeof(*s));s->write=write;s->context=context;s->stream_hash=2166136261u;
}
static int fail(MAT_DEPACKETIZER* s) { s->failed=1;return -1; }
int MatDepacketizerPush(MAT_DEPACKETIZER* s,const uint8_t* p,size_t n) {
    if(s->failed) return -1;
    if(!p || n!=MAT_WIRE_PACKET || memcmp(p,"SMAT",4) || p[4]!=1 ||
        p[6]!=MAT_WIRE_HEADER || p[7] || mat_get32(p+8)!=1 || mat_get32(p+12)!=1 ||
        !mat_get64(p+16) || !mat_get64(p+24) || mat_get32(p+48)!=48000 ||
        mat_get32(p+76) || mat_get32(p+72)!=mat_packet_hash(p)) return fail(s);
    uint64_t frame=mat_get64(p+32);
    if(frame>UINT64_MAX/960 || mat_get64(p+40)!=frame*960) return fail(s);
    if(s->started && (s->epoch!=mat_get64(p+16) || s->domain!=mat_get64(p+24))) return fail(s);
    if(!s->started) { s->started=1;s->epoch=mat_get64(p+16);s->domain=mat_get64(p+24); }
    if(p[5]==MAT_WIRE_SYNC || p[5]==MAT_WIRE_END) {
        if(mat_get32(p+52) || mat_get32(p+56) || mat_get32(p+60) || mat_get32(p+64)) return fail(s);
        for(unsigned i=MAT_WIRE_HEADER;i<MAT_WIRE_PACKET;i++) if(p[i]) return fail(s);
        if(p[5]==MAT_WIRE_SYNC) {
            if(s->ended || s->next_frame || s->mask || frame || mat_get32(p+68)) return fail(s);
            return 0;
        }
        if(!frame || frame!=s->next_frame || s->mask || mat_get32(p+68)!=s->stream_hash) return fail(s);
        s->ended=1;return 2; // Redundant END packets are deliberately idempotent.
    }
    if(p[5]!=MAT_WIRE_DATA || s->ended || frame!=s->next_frame ||
        mat_get32(p+52)!=960 || mat_get32(p+56)!=MAT_WIRE_CYCLE ||
        p[61] || p[62]!=MAT_WIRE_FRAGMENTS || p[63] || p[60]>=MAT_WIRE_FRAGMENTS ||
        mat_get32(p+64)!=MAT_WIRE_BODY) return fail(s);
    unsigned index=p[60];uint64_t bit=UINT64_C(1)<<index;
    if(s->mask && s->frame_hash!=mat_get32(p+68)) return fail(s);
    s->frame_hash=mat_get32(p+68);
    if(s->mask&bit) {
        if(memcmp(s->cycle+index*MAT_WIRE_BODY,p+MAT_WIRE_HEADER,MAT_WIRE_BODY)) return fail(s);
        return 0;
    }
    memcpy(s->cycle+index*MAT_WIRE_BODY,p+MAT_WIRE_HEADER,MAT_WIRE_BODY);s->mask|=bit;
    if(s->mask!=((UINT64_C(1)<<MAT_WIRE_FRAGMENTS)-1)) return 0;
    if(mat_hash(s->cycle,sizeof(s->cycle))!=s->frame_hash ||
        memcmp(s->cycle,"\x72\xf8\x1f\x4e",4) || (s->cycle[4]&31)!=0x16 || (s->cycle[4]&0x80)) return fail(s);
    unsigned pd=(unsigned)s->cycle[6]|(unsigned)s->cycle[7]<<8;
    if(pd<2 || pd>MAT_WIRE_CYCLE-8 || (pd&1) || !s->write || !s->write(s->context,s->cycle,sizeof(s->cycle))) return fail(s);
    s->stream_hash=mat_hash_update(s->stream_hash,s->cycle,sizeof(s->cycle));
    s->mask=0;++s->next_frame;return 1;
}
