#pragma once
#include "Limelight.h"
#include "MatLiveProfile.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MAT_LIVE_PLAYBACK MAT_LIVE_PLAYBACK;
MAT_LIVE_PLAYBACK* MatLivePlaybackOpen(const LI_MAT_LIVE_PROFILE*,const uint8_t key[16],const LI_MAT_PLAYBACK_CALLBACKS*);
int MatLivePlaybackPush(MAT_LIVE_PLAYBACK*,const uint8_t*,size_t);
void MatLivePlaybackFail(MAT_LIVE_PLAYBACK*);
void MatLivePlaybackClose(MAT_LIVE_PLAYBACK*);
#ifdef __cplusplus
}
#endif
