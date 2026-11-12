#pragma once
/* Shared host/client experimental dump profile. NOT an E-AC3/Opus payload or a claim of
 * Android MAT decode support. All multibyte fields are little endian; no packed
 * structs, native alignment, pointers or untrusted allocation sizes on wire. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MAT_WIRE_HEADER 80u
#define MAT_WIRE_BODY 1024u
#define MAT_WIRE_PACKET (MAT_WIRE_HEADER + MAT_WIRE_BODY)
#define MAT_WIRE_CYCLE 61440u
#define MAT_WIRE_FRAGMENTS 60u
#define MAT_WIRE_DATA 1u
#define MAT_WIRE_SYNC 2u
#define MAT_WIRE_END 3u

static inline uint32_t mat_get32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline uint64_t mat_get64(const uint8_t* p) { return mat_get32(p) | (uint64_t)mat_get32(p+4)<<32; }
static inline void mat_put32(uint8_t* p, uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8)); }
static inline void mat_put64(uint8_t* p, uint64_t v) { mat_put32(p,(uint32_t)v);mat_put32(p+4,(uint32_t)(v>>32)); }
static inline uint32_t mat_hash_update(uint32_t h, const uint8_t* p, size_t n) {
    while(n--) { h^=*p++;h*=16777619u; } return h;
}
static inline uint32_t mat_hash(const uint8_t* p, size_t n) { return mat_hash_update(2166136261u,p,n); }
/* Accidental-corruption detection only: FNV and legacy AES-CBC are NOT MACs. */
static inline uint32_t mat_packet_hash(const uint8_t* p) {
    uint32_t h=2166136261u;
    for(unsigned i=0;i<MAT_WIRE_PACKET;i++) { h^=(i>=72 && i<76)?0:p[i];h*=16777619u; }
    return h;
}
static inline void mat_wire_make(uint8_t* p, uint8_t kind, uint64_t epoch,
        uint64_t domain, uint64_t frame, unsigned fragment, uint32_t frame_hash,
        const uint8_t* cycle) {
    memset(p,0,MAT_WIRE_PACKET);memcpy(p,"SMAT",4);p[4]=1;p[5]=kind;p[6]=MAT_WIRE_HEADER;
    mat_put32(p+8,1);mat_put32(p+12,1); /* MAT IEC carrier, carrier-48000 clock */
    mat_put64(p+16,epoch);mat_put64(p+24,domain);mat_put64(p+32,frame);
    mat_put64(p+40,frame*960);mat_put32(p+48,48000);
    if(kind==MAT_WIRE_DATA) {
        mat_put32(p+52,960);mat_put32(p+56,MAT_WIRE_CYCLE);
        p[60]=(uint8_t)fragment;p[62]=MAT_WIRE_FRAGMENTS;mat_put32(p+64,MAT_WIRE_BODY);
        memcpy(p+MAT_WIRE_HEADER,cycle+fragment*MAT_WIRE_BODY,MAT_WIRE_BODY);
    }
    mat_put32(p+68,frame_hash);mat_put32(p+72,mat_packet_hash(p));
}
