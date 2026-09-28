// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/15_tonemap_b_0x676B8B5D.ps_5_0.hlsl
// Modified FO4Isotope adaptation of RenoDX Fallout 4 shader 0x676B8B5D;
// see THIRD_PARTY_NOTICES.md.
#define FO4HDR_CB2_COUNT 6
#define FO4HDR_HAS_FINAL_AFFINE 1
#define FO4HDR_SCENE_TEXTURE t1.Sample(s1_s, texcoord).rgb
#define FO4HDR_BLOOM_UV (cb2[4].zw * texcoord)
#define FO4HDR_BLOOM_TEXTURE t0.Sample(s0_s, bloomUv).rgb
#include "ToneMapBypassCommon.hlsli"
