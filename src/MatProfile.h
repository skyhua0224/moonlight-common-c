#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define MAT_CARRIER "iec61937-192000-8-s16le"
#define MAT_CLOCK "48000"
/* Exact SDP attribute name, no duplicate values or truncation. */
static inline int MatSdpValue(const char* sdp,const char* name,char* out,size_t cap) {
    int found=0;size_t key=strlen(name);out[0]=0;
    for(const char* p=sdp;p && *p;) {
        const char* end=strchr(p,'\n');if(!end)end=p+strlen(p);
        const char* trimmed=end;if(trimmed>p && trimmed[-1]=='\r')--trimmed;
        if((size_t)(trimmed-p)>=key+3 && !memcmp(p,"a=",2) && !memcmp(p+2,name,key) && p[key+2]==':') {
            size_t n=(size_t)(trimmed-p)-key-3;if(found || n>=cap)return -1;
            memcpy(out,p+key+3,n);out[n]=0;found=1;
        }
        p=*end?end+1:end;
    }return found;
}
