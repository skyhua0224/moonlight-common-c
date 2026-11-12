#pragma once
#include "MatWire.h"
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
#include <psa/crypto.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif
#define MAT_LIVE_HEADER 32u
#define MAT_LIVE_TAG 16u
#define MAT_LIVE_PACKET (MAT_LIVE_HEADER+MAT_WIRE_PACKET+MAT_LIVE_TAG)
typedef struct { uint8_t key[16];uint64_t epoch,domain; } MAT_LIVE_CIPHER;
static inline void MatLiveWipe(void* p,size_t n) { volatile uint8_t* b=(volatile uint8_t*)p;while(n--)*b++=0; }
static inline int MatLiveHmac(const uint8_t* key,size_t size,const uint8_t* data,size_t length,uint8_t out[32]) {
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    psa_key_attributes_t a=PSA_KEY_ATTRIBUTES_INIT;psa_key_id_t id=0;size_t written=0;
    psa_set_key_type(&a,PSA_KEY_TYPE_HMAC);psa_set_key_bits(&a,size*8);
    psa_set_key_usage_flags(&a,PSA_KEY_USAGE_SIGN_MESSAGE);psa_set_key_algorithm(&a,PSA_ALG_HMAC(PSA_ALG_SHA_256));
    if(psa_crypto_init()!=PSA_SUCCESS || psa_import_key(&a,key,size,&id)!=PSA_SUCCESS)return 0;
    psa_status_t result=psa_mac_compute(id,PSA_ALG_HMAC(PSA_ALG_SHA_256),data,length,out,32,&written);
    psa_destroy_key(id);return result==PSA_SUCCESS && written==32;
#else
    unsigned written=0;
    return HMAC(EVP_sha256(),key,(int)size,data,length,out,&written)!=NULL && written==32;
#endif
}
/* RFC5869, one expansion block. A separate key per protocol/epoch/domain prevents
 * overlap with GameStream control/audio keys; no RTP16 value is used as a nonce. */
static inline int MatLiveCipherInit(MAT_LIVE_CIPHER* c,const uint8_t* key,size_t length,uint64_t epoch,uint64_t domain) {
    static const uint8_t salt[]="Sunshine MAT live v1";
    static const uint8_t label[]="MATL AES128 GCM";
    uint8_t prk[32],expanded[32],info[sizeof(label)-1+17];int okay=0;
    memset(c,0,sizeof(*c));
    if(!key||length!=16||!epoch||epoch>INT64_MAX||!domain||domain>INT64_MAX)return 0;
    memcpy(info,label,sizeof(label)-1);mat_put64(info+sizeof(label)-1,epoch);
    mat_put64(info+sizeof(label)-1+8,domain);info[sizeof(info)-1]=1;
    if(MatLiveHmac(salt,sizeof(salt)-1,key,length,prk) && MatLiveHmac(prk,32,info,sizeof(info),expanded)) {
        memcpy(c->key,expanded,16);c->epoch=epoch;c->domain=domain;okay=1;
    }
    MatLiveWipe(prk,sizeof(prk));MatLiveWipe(expanded,sizeof(expanded));return okay;
}
static inline void MatLiveNonce(uint8_t nonce[12],uint64_t packet) {
    memcpy(nonce,"MATL",4);mat_put64(nonce+4,packet);
}
static inline int MatLiveSeal(const MAT_LIVE_CIPHER* c,uint64_t sequence,const uint8_t plain[MAT_WIRE_PACKET],uint8_t wire[MAT_LIVE_PACKET]) {
    uint8_t nonce[12];MatLiveNonce(nonce,sequence);
    if(!c||!c->epoch||sequence>INT64_MAX)return 0;
    memset(wire,0,MAT_LIVE_HEADER);memcpy(wire,"MLIV",4);wire[4]=1;wire[5]=MAT_LIVE_HEADER;
    mat_put64(wire+8,c->epoch);mat_put64(wire+16,sequence);mat_put32(wire+24,MAT_WIRE_PACKET);
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    psa_key_attributes_t a=PSA_KEY_ATTRIBUTES_INIT;psa_key_id_t id=0;size_t written=0;
    psa_set_key_type(&a,PSA_KEY_TYPE_AES);psa_set_key_bits(&a,128);psa_set_key_usage_flags(&a,PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&a,PSA_ALG_GCM);
    if(psa_crypto_init()!=PSA_SUCCESS||psa_import_key(&a,c->key,16,&id)!=PSA_SUCCESS)return 0;
    psa_status_t result=psa_aead_encrypt(id,PSA_ALG_GCM,nonce,12,wire,MAT_LIVE_HEADER,plain,MAT_WIRE_PACKET,
        wire+MAT_LIVE_HEADER,MAT_WIRE_PACKET+MAT_LIVE_TAG,&written);
    psa_destroy_key(id);return result==PSA_SUCCESS && written==MAT_WIRE_PACKET+MAT_LIVE_TAG;
#else
    EVP_CIPHER_CTX* ctx=EVP_CIPHER_CTX_new();int n=0,tail=0,okay=0;
    if(ctx && EVP_EncryptInit_ex(ctx,EVP_aes_128_gcm(),NULL,NULL,NULL)==1 &&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,12,NULL)==1 &&
        EVP_EncryptInit_ex(ctx,NULL,NULL,c->key,nonce)==1 &&
        EVP_EncryptUpdate(ctx,NULL,&n,wire,MAT_LIVE_HEADER)==1 &&
        EVP_EncryptUpdate(ctx,wire+MAT_LIVE_HEADER,&n,plain,MAT_WIRE_PACKET)==1 && n==MAT_WIRE_PACKET &&
        EVP_EncryptFinal_ex(ctx,wire+MAT_LIVE_HEADER+n,&tail)==1 && tail==0 &&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_GET_TAG,MAT_LIVE_TAG,wire+MAT_LIVE_HEADER+MAT_WIRE_PACKET)==1)okay=1;
    EVP_CIPHER_CTX_free(ctx);return okay;
#endif
}
/* The caller must not expose plain before authentication succeeds. */
static inline int MatLiveOpenPacket(const MAT_LIVE_CIPHER* c,uint64_t sequence,const uint8_t* wire,size_t length,uint8_t plain[MAT_WIRE_PACKET]) {
    uint8_t nonce[12];MatLiveNonce(nonce,sequence);
    if(!c||!wire||length!=MAT_LIVE_PACKET||memcmp(wire,"MLIV",4)||wire[4]!=1||wire[5]!=MAT_LIVE_HEADER||
        wire[6]||wire[7]||mat_get64(wire+8)!=c->epoch||mat_get64(wire+16)!=sequence||
        sequence>INT64_MAX||mat_get32(wire+24)!=MAT_WIRE_PACKET||mat_get32(wire+28))return 0;
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    psa_key_attributes_t a=PSA_KEY_ATTRIBUTES_INIT;psa_key_id_t id=0;size_t written=0;
    psa_set_key_type(&a,PSA_KEY_TYPE_AES);psa_set_key_bits(&a,128);psa_set_key_usage_flags(&a,PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a,PSA_ALG_GCM);
    if(psa_crypto_init()!=PSA_SUCCESS||psa_import_key(&a,c->key,16,&id)!=PSA_SUCCESS)return 0;
    psa_status_t result=psa_aead_decrypt(id,PSA_ALG_GCM,nonce,12,wire,MAT_LIVE_HEADER,wire+MAT_LIVE_HEADER,
        MAT_WIRE_PACKET+MAT_LIVE_TAG,plain,MAT_WIRE_PACKET,&written);
    psa_destroy_key(id);
    if(result==PSA_SUCCESS&&written==MAT_WIRE_PACKET)return 1;
#else
    EVP_CIPHER_CTX* ctx=EVP_CIPHER_CTX_new();int n=0,tail=0,okay=0;
    if(ctx && EVP_DecryptInit_ex(ctx,EVP_aes_128_gcm(),NULL,NULL,NULL)==1 &&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,12,NULL)==1 &&
        EVP_DecryptInit_ex(ctx,NULL,NULL,c->key,nonce)==1 &&
        EVP_DecryptUpdate(ctx,NULL,&n,wire,MAT_LIVE_HEADER)==1 &&
        EVP_DecryptUpdate(ctx,plain,&n,wire+MAT_LIVE_HEADER,MAT_WIRE_PACKET)==1 && n==MAT_WIRE_PACKET &&
        EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_TAG,MAT_LIVE_TAG,(void*)(wire+MAT_LIVE_HEADER+MAT_WIRE_PACKET))==1 &&
        EVP_DecryptFinal_ex(ctx,plain+n,&tail)==1 && tail==0)okay=1;
    EVP_CIPHER_CTX_free(ctx);if(okay)return 1;
#endif
    MatLiveWipe(plain,MAT_WIRE_PACKET);return 0;
}
