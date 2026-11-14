// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Late native UI backdrop with the game's bindings and arithmetic. The game draws
// it onto an 8-bit image, which turns every output into [0, 1], NaN into zero,
// before blending. On the FP16 public image this shader makes that conversion
// itself; a zero-height gradient quad otherwise leaves a band of NaN pixels.

SamplerState UiSampler : register(s0);
Texture2D<float4> UiImage : register(t0);

cbuffer BackdropQuadConstants : register(b0)
{
    float4 BackdropUnused[2];
    // x quad count, y edge falloff, z linear-color flag, w opacity.
    float4 BackdropControl;
    // Left, top, right, bottom in UI coordinates; a negative left marks a
    // vertical gradient.
    float4 BackdropQuads[16];
    float4 BackdropColor;
};

cbuffer BackdropAlphaConstants : register(b1)
{
    float4 BackdropAlphaScale;
};

cbuffer BackdropAlphaTestConstants : register(b2)
{
    float4 BackdropAlphaTestUnused[13];
    float4 BackdropAlphaGain;
    // x minimum and y maximum of the UI alpha; the maximum applies below 1.
    float4 BackdropAlphaRange;
};

float4 Fo4HdrUnormOutput(float4 value)
{
    // Inspect IEEE payloads so optimizing finite arithmetic cannot drop the NaN test.
    const float4 contained = (asuint(value) & 0x7fffffffu) > 0x7f800000u ? 0.0f : value;
    return saturate(contained);
}

float4 main(
    float4 position : SV_POSITION0,
    float4 texcoord : TEXCOORD0,
    float4 vertexColor : COLOR1) : SV_Target0
{
    const float uiAlpha = UiImage.Sample(UiSampler, texcoord.xy).a;
    const float scaled = uiAlpha * BackdropAlphaScale.w;
    if (mad(scaled, BackdropAlphaGain.w, -BackdropAlphaRange.x) < 0.0f)
        discard;
    if (BackdropAlphaRange.y < 1.0f && BackdropAlphaRange.y - uiAlpha < 0.0f)
        discard;
    float alpha = scaled * BackdropAlphaGain.w;
    float coverage = 0.0f;
    const int quadCount = (int)BackdropControl.x;
    [loop]
    for (int index = 0; index < quadCount; ++index)
    {
        const float4 quad = BackdropQuads[index];
        const bool gradient = quad.x < 0.0f;
        const float left = gradient ? -quad.x : quad.x;
        const float2 outside = max(float2(left - texcoord.x, quad.y - texcoord.y),
            texcoord.xy - quad.zw);
        coverage = max(coverage, 1.0f - BackdropControl.y * max(outside.y, outside.x));
        if (gradient && texcoord.y >= quad.y - 0.0025f && quad.w + 0.0025f >= texcoord.y)
            alpha *= (texcoord.y - quad.y) / (quad.w - quad.y);
    }
    // The game raises the color to 2.2 through log2 and exp2, including for zero.
    const float3 color = BackdropControl.z != 0.0f ?
        BackdropColor.rgb : exp2(log2(BackdropColor.rgb) * 2.2f);
    return Fo4HdrUnormOutput(float4(color, alpha * saturate(coverage) * BackdropControl.w));
}
