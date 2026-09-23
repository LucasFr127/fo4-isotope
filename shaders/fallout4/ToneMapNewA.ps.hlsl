// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/15_tonemap_newa_0x67685D89.ps_5_0.hlsl
// Modified Fallout4HDR adaptation of RenoDX Fallout 4 shader 0x67685D89;
// see THIRD_PARTY_NOTICES.md.
#define FO4HDR_CB2_COUNT 5
#define FO4HDR_HAS_FINAL_AFFINE 0
#define FO4HDR_SCENE_TEXTURE t0.Sample(s0_s, texcoord).rgb
#define FO4HDR_BLOOM_UV (cb2[4].xy * texcoord)
#define FO4HDR_BLOOM_TEXTURE t1.Sample(s1_s, bloomUv).rgb
#include "ToneMapBypassCommon.hlsli"
