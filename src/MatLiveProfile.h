#pragma once
#include "MatProfile.h"
#define MAT_LIVE_CODEC 7
#define MAT_LIVE_NAME "mat-live-v1"
#define MAT_LIVE_CRYPTO "aes128-gcm-hkdf-sha256"
typedef struct { uint64_t epoch,domain; } LI_MAT_LIVE_PROFILE;
static inline int MatLiveDecimal(const char* text,uint64_t* value) {
    uint64_t n=0;
    if(!text || !*text || *text=='0')return 0;
    for(const char* p=text;*p;p++) {
        if(*p<'0'||*p>'9'||n>((uint64_t)INT64_MAX-(unsigned)(*p-'0'))/10)return 0;
        n=n*10+(unsigned)(*p-'0');
    }
    *value=n;return n!=0;
}
static inline int MatLiveProfileParse(LI_MAT_LIVE_PROFILE* out,const char* sdp) {
    char version[8],carrier[48],clock[16],crypto[48],epoch[24],domain[24];
    memset(out,0,sizeof(*out));
    if(!sdp)return 0;
    return MatSdpValue(sdp,"x-ss-audio.mat-live",version,sizeof(version))==1 && !strcmp(version,"1") &&
        MatSdpValue(sdp,"x-ss-audio.mat-live-carrier",carrier,sizeof(carrier))==1 && !strcmp(carrier,MAT_CARRIER) &&
        MatSdpValue(sdp,"x-ss-audio.mat-live-clock",clock,sizeof(clock))==1 && !strcmp(clock,MAT_CLOCK) &&
        MatSdpValue(sdp,"x-ss-audio.mat-live-crypto",crypto,sizeof(crypto))==1 && !strcmp(crypto,MAT_LIVE_CRYPTO) &&
        MatSdpValue(sdp,"x-ss-audio.mat-live-epoch",epoch,sizeof(epoch))==1 && MatLiveDecimal(epoch,&out->epoch) &&
        MatSdpValue(sdp,"x-ss-audio.mat-live-domain",domain,sizeof(domain))==1 && MatLiveDecimal(domain,&out->domain);
}
static inline int MatLiveProfileConfirm(const LI_MAT_LIVE_PROFILE* expected,const char* codec,
        const char* carrier,const char* clock,const char* crypto,const char* epoch,const char* domain) {
    uint64_t e=0,d=0;
    return expected && codec && !strcmp(codec,MAT_LIVE_NAME) &&
        carrier && !strcmp(carrier,MAT_CARRIER) && clock && !strcmp(clock,MAT_CLOCK) &&
        crypto && !strcmp(crypto,MAT_LIVE_CRYPTO) && MatLiveDecimal(epoch,&e) && MatLiveDecimal(domain,&d) &&
        e==expected->epoch && d==expected->domain;
}
