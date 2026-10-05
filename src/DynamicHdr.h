// Dynamic HDR negotiation constants used by the Sunshine extension.
//
// These values are opt-in wire constants. Keeping them in a separate header
// prevents collisions with host-side C++ enum names and lets legacy clients
// continue sending no negotiation attributes.
#pragma once

#define DYNAMIC_HDR_CAPS_HDR10_PLUS (1 << 0)
#define DYNAMIC_HDR_CAPS_VIVID_PQ (1 << 1)
#define DYNAMIC_HDR_CAPS_VIVID_HLG (1 << 2)
#define DYNAMIC_HDR_CAPS_DOLBY_VISION_81 (1 << 3)
#define DYNAMIC_HDR_CAPS_DOLBY_VISION_84 (1 << 4)

#define DYNAMIC_HDR_FORMAT_NONE 0
#define DYNAMIC_HDR_FORMAT_HDR10_PLUS 1
#define DYNAMIC_HDR_FORMAT_VIVID_PQ 2
#define DYNAMIC_HDR_FORMAT_VIVID_HLG 3
#define DYNAMIC_HDR_FORMAT_DOLBY_VISION_PROFILE_81 4
#define DYNAMIC_HDR_FORMAT_DOLBY_VISION_PROFILE_84 5

#define DYNAMIC_HDR_FALLBACK_NONE 0
#define DYNAMIC_HDR_FALLBACK_CODEC_UNSUPPORTED 2
#define DYNAMIC_HDR_FALLBACK_COLORSPACE_UNSUPPORTED 3
#define DYNAMIC_HDR_FALLBACK_CLIENT_CAPS_MISSING 4
#define DYNAMIC_HDR_FALLBACK_DIRECT_SURFACE_MISSING 5
#define DYNAMIC_HDR_FALLBACK_PREFERENCE 6
