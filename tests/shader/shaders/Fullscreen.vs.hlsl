// SPDX-License-Identifier: GPL-3.0-or-later

struct FullscreenVertex
{
    float4 position : SV_POSITION0;
    float4 texcoord0 : TEXCOORD0;
    float4 texcoord1 : TEXCOORD1;
};

FullscreenVertex main(uint vertexId : SV_VertexID)
{
    const float2 position = float2(
        vertexId == 2u ? 3.0f : -1.0f,
        vertexId == 1u ? 3.0f : -1.0f);
    const float2 texcoord = float2(
        vertexId == 2u ? 2.0f : 0.0f,
        vertexId == 1u ? -1.0f : 1.0f);
    FullscreenVertex output;
    output.position = float4(position, 0.0f, 1.0f);
    output.texcoord0 = texcoord.xyxy;
    output.texcoord1 = texcoord.xyxy;
    return output;
}
