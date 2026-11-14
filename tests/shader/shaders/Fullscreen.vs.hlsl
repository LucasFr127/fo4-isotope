// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

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

// The game's backdrop vertex stage passes COLOR1 in the second varying.
struct UiBackdropVertex
{
    float4 position : SV_POSITION0;
    float4 texcoord : TEXCOORD0;
    float4 color : COLOR1;
};

UiBackdropVertex mainUiBackdrop(uint vertexId : SV_VertexID)
{
    const FullscreenVertex fullscreen = main(vertexId);
    UiBackdropVertex output;
    output.position = fullscreen.position;
    output.texcoord = fullscreen.texcoord0;
    output.color = 1.0f;
    return output;
}
