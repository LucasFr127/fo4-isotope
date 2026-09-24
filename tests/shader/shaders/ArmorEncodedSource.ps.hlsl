// SPDX-License-Identifier: GPL-3.0-or-later
// Native encoded source fixture: deliberately performs no transfer or clamp.
cbuffer NativeSource : register(b0) { float4 Source; };
float4 main() : SV_Target0 { return Source; }
