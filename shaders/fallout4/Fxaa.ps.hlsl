// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/games/fallout4/fxaa_0x4FB4DA20.ps_5_0.hlsl
// Whole-file adaptation for FO4Isotope. Modification: gather green luma as the
// original Fallout 4 bytecode does, read every edge-search luma through a
// gamma-2.2 encoding of the linear HDR input, and return the unmodified center
// or filtered sample instead of the source's sign and absolute-value handling.
// See THIRD_PARTY_NOTICES.md.

Texture2D<float4> t0 : register(t0);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2) {
  float4 cb2[1];
}

// 3Dmigoto declarations
#define cmp -

// The game applies FXAA's 0.0833 and 0.166 thresholds to gamma-encoded
// display codes, so linear luma is encoded the same way before every test.
// shortcut: the 0.0833 floor tracks UI white (about 0.4-1.3 nits), as in RenoDX; bind b13 and scale by UI/scene white if it must not.
float Fo4HdrFxaaLuma(float green) { return pow(max(green, 0.0f), 1.0f / 2.2f); }
float3 Fo4HdrFxaaLuma(float3 green) { return pow(max(green, 0.0f), 1.0f / 2.2f); }

void main(
  float4 v0 : SV_POSITION0,
              float4 v1 : TEXCOORD0,
                          float4 v2 : TEXCOORD1,
                                      out float4 o0 : SV_Target0
) {
  float4 r0, r1, r2, r3, r4, r5;

  // Without an edge the center sample passes through unchanged.
  o0.xyzw = t0.SampleLevel(s0_s, v1.xy, 0).xyzw;
  r0.y = Fo4HdrFxaaLuma(o0.y);
  r1.xyz = Fo4HdrFxaaLuma(t0.GatherGreen(s0_s, v1.xy).xyz);
  r2.xyz = Fo4HdrFxaaLuma(t0.GatherGreen(s0_s, v1.xy, int2(-1, -1)).xzw);

  r1.w = max(r1.x, r0.y);
  r2.w = min(r1.x, r0.y);
  r1.w = max(r1.z, r1.w);
  r2.w = min(r2.w, r1.z);
  r3.x = max(r2.y, r2.x);
  r3.y = min(r2.y, r2.x);
  r1.w = max(r3.x, r1.w);
  r2.w = min(r3.y, r2.w);
  r3.x = 0.165999994 * r1.w;
  r1.w = -r2.w + r1.w;
  r2.w = max(0.0833000019, r3.x);
  r2.w = cmp(r1.w >= r2.w);
  if (r2.w != 0) {
    r2.w = Fo4HdrFxaaLuma(t0.SampleLevel(s0_s, v1.xy, 0, int2(1, -1)).y);
    r3.x = Fo4HdrFxaaLuma(t0.SampleLevel(s0_s, v1.xy, 0, int2(-1, 1)).y);

    r3.yz = r2.yx + r1.xz;
    r1.w = 1 / r1.w;
    r3.w = r3.y + r3.z;
    r3.yz = r0.yy * float2(-2, -2) + r3.yz;
    r4.x = r2.w + r1.y;
    r2.w = r2.z + r2.w;
    r4.y = r1.z * -2 + r4.x;
    r2.w = r2.y * -2 + r2.w;
    r2.z = r3.x + r2.z;
    r1.y = r3.x + r1.y;
    r3.x = abs(r3.y) * 2 + abs(r4.y);
    r2.w = abs(r3.z) * 2 + abs(r2.w);
    r3.y = r2.x * -2 + r2.z;
    r1.y = r1.x * -2 + r1.y;
    r3.x = abs(r3.y) + r3.x;
    r1.y = abs(r1.y) + r2.w;
    r2.z = r2.z + r4.x;
    r1.y = cmp(r3.x >= r1.y);
    r2.z = r3.w * 2 + r2.z;
    r2.x = r1.y ? r2.y : r2.x;
    r1.x = r1.y ? r1.x : r1.z;
    r1.z = r1.y ? cb2[0].y : cb2[0].x;
    r2.y = r2.z * 0.0833333358 + -r0.y;
    r2.z = r2.x + -r0.y;
    r2.w = r1.x + -r0.y;
    r2.x = r2.x + r0.y;
    r1.x = r1.x + r0.y;
    r3.x = cmp(abs(r2.z) >= abs(r2.w));
    r2.z = max(abs(r2.z), abs(r2.w));
    r1.z = r3.x ? -r1.z : r1.z;
    r1.w = saturate(abs(r2.y) * r1.w);
    r2.y = r1.y ? cb2[0].x : 0;
    r2.w = r1.y ? 0 : cb2[0].y;
    r3.yz = r1.zz * float2(0.5, 0.5) + v1.xy;
    r3.y = r1.y ? v1.x : r3.y;
    r3.z = r1.y ? r3.z : v1.y;
    r4.xy = r3.yz + -r2.yw;
    r5.xy = r3.yz + r2.yw;
    r3.y = r1.w * -2 + 3;
    r3.z = Fo4HdrFxaaLuma(t0.SampleLevel(s0_s, r4.xy, 0).y);
    r1.w = r1.w * r1.w;
    r3.w = Fo4HdrFxaaLuma(t0.SampleLevel(s0_s, r5.xy, 0).y);
    r1.x = r3.x ? r2.x : r1.x;
    r2.x = 0.25 * r2.z;
    r2.z = -r1.x * 0.5 + r0.y;
    r1.w = r3.y * r1.w;
    r2.z = cmp(r2.z < 0);
    r3.x = -r1.x * 0.5 + r3.z;
    r3.y = -r1.x * 0.5 + r3.w;
    r3.zw = cmp(abs(r3.xy) >= r2.xx);
    r4.z = -r2.y * 1.5 + r4.x;
    r4.x = r3.z ? r4.x : r4.z;
    r4.w = -r2.w * 1.5 + r4.y;
    r4.z = r3.z ? r4.y : r4.w;
    r4.yw = ~(int2)r3.zw;
    r4.y = (int)r4.w | (int)r4.y;
    r4.w = r2.y * 1.5 + r5.x;
    r5.x = r3.w ? r5.x : r4.w;
    r4.w = r2.w * 1.5 + r5.y;
    r5.z = r3.w ? r5.y : r4.w;
    if (r4.y != 0) {
      if (r3.z == 0) {
        r3.x = Fo4HdrFxaaLuma(t0.SampleLevel(s0_s, r4.xz, 0).y);
      }
      if (r3.w == 0) {
        r3.y = Fo4HdrFxaaLuma(t0.SampleLevel(s0_s, r5.xz, 0).y);
      }
      r4.y = -r1.x * 0.5 + r3.x;
      r3.x = r3.z ? r3.x : r4.y;
      r3.z = -r1.x * 0.5 + r3.y;
      r3.y = r3.w ? r3.y : r3.z;
      r3.zw = cmp(abs(r3.xy) >= r2.xx);
      r4.y = -r2.y * 2 + r4.x;
      r4.x = r3.z ? r4.x : r4.y;
      r4.y = -r2.w * 2 + r4.z;
      r4.z = r3.z ? r4.z : r4.y;
      r4.yw = ~(int2)r3.zw;
      r4.y = (int)r4.w | (int)r4.y;
      r4.w = r2.y * 2 + r5.x;
      r5.x = r3.w ? r5.x : r4.w;
      r4.w = r2.w * 2 + r5.z;
      r5.z = r3.w ? r5.z : r4.w;
      if (r4.y != 0) {
        if (r3.z == 0) {
          r3.x = Fo4HdrFxaaLuma(t0.SampleLevel(s0_s, r4.xz, 0).y);
        }
        if (r3.w == 0) {
          r3.y = Fo4HdrFxaaLuma(t0.SampleLevel(s0_s, r5.xz, 0).y);
        }
        r4.y = -r1.x * 0.5 + r3.x;
        r3.x = r3.z ? r3.x : r4.y;
        r3.z = -r1.x * 0.5 + r3.y;
        r3.y = r3.w ? r3.y : r3.z;
        r3.zw = cmp(abs(r3.xy) >= r2.xx);
        r4.y = -r2.y * 4 + r4.x;
        r4.x = r3.z ? r4.x : r4.y;
        r4.y = -r2.w * 4 + r4.z;
        r4.z = r3.z ? r4.z : r4.y;
        r4.yw = ~(int2)r3.zw;
        r4.y = (int)r4.w | (int)r4.y;
        r4.w = r2.y * 4 + r5.x;
        r5.x = r3.w ? r5.x : r4.w;
        r4.w = r2.w * 4 + r5.z;
        r5.z = r3.w ? r5.z : r4.w;
        if (r4.y != 0) {
          if (r3.z == 0) {
            r3.x = Fo4HdrFxaaLuma(t0.SampleLevel(s0_s, r4.xz, 0).y);
          }
          if (r3.w == 0) {
            r3.y = Fo4HdrFxaaLuma(t0.SampleLevel(s0_s, r5.xz, 0).y);
          }
          r4.y = -r1.x * 0.5 + r3.x;
          r3.x = r3.z ? r3.x : r4.y;
          r1.x = -r1.x * 0.5 + r3.y;
          r3.y = r3.w ? r3.y : r1.x;
          r3.zw = cmp(abs(r3.xy) >= r2.xx);
          r1.x = -r2.y * 12 + r4.x;
          r4.x = r3.z ? r4.x : r1.x;
          r1.x = -r2.w * 12 + r4.z;
          r4.z = r3.z ? r4.z : r1.x;
          r1.x = r2.y * 12 + r5.x;
          r5.x = r3.w ? r5.x : r1.x;
          r1.x = r2.w * 12 + r5.z;
          r5.z = r3.w ? r5.z : r1.x;
        }
      }
    }
    r1.x = v1.x + -r4.x;
    r2.y = v1.y + -r4.z;
    r1.x = r1.y ? r1.x : r2.y;
    r2.xy = -v1.xy + r5.xz;
    r2.x = r1.y ? r2.x : r2.y;
    r2.yw = cmp(r3.xy < float2(0, 0));
    r3.x = r2.x + r1.x;
    r2.yz = cmp((int2)r2.zz != (int2)r2.yw);
    r2.w = 1 / r3.x;
    r3.x = cmp(r1.x < r2.x);
    r1.x = min(r2.x, r1.x);
    r2.x = r3.x ? r2.y : r2.z;
    r1.w = r1.w * r1.w;
    r1.x = r1.x * -r2.w + 0.5;
    r1.w = 0.75 * r1.w;
    r1.x = (int)r1.x & (int)r2.x;
    r1.x = max(r1.x, r1.w);
    r1.xz = r1.xx * r1.zz + v1.xy;
    r2.x = r1.y ? v1.x : r1.x;
    r2.y = r1.y ? r1.z : v1.y;
    o0.xyz = t0.SampleLevel(s0_s, r2.xy, 0).xyz;
  }
  return;
}
