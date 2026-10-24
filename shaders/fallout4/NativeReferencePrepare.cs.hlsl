// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "GradeTransfer.hlsli"

Texture3D<float4> NativeLook0 : register(t0);
Texture3D<float4> NativeLook1 : register(t1);
Texture3D<float4> NativeLook2 : register(t2);
Texture3D<float4> NativeLook3 : register(t3);

cbuffer NativeLookConstants : register(b2)
{
    float4 NativeUnused;
    float4 NativeWeights;
};

RWTexture3D<float4> PreparedLook : register(u0);

float3 WeightedNode(int3 coordinate)
{
    float3 value = 0.0f;
    [branch] if (NativeWeights.x != 0.0f) value += NativeWeights.x * NativeLook0.Load(int4(coordinate, 0)).rgb;
    [branch] if (NativeWeights.y != 0.0f) value += NativeWeights.y * NativeLook1.Load(int4(coordinate, 0)).rgb;
    [branch] if (NativeWeights.z != 0.0f) value += NativeWeights.z * NativeLook2.Load(int4(coordinate, 0)).rgb;
    [branch] if (NativeWeights.w != 0.0f) value += NativeWeights.w * NativeLook3.Load(int4(coordinate, 0)).rgb;
    return value;
}

[numthreads(8, 8, 8)]
void main(uint3 coordinate : SV_DispatchThreadID)
{
    float4 encoded = 0.0f;
    [branch]
    if (NativeWeights.x != 0.0f)
        encoded += NativeWeights.x * NativeLook0.Load(int4(coordinate, 0));
    [branch]
    if (NativeWeights.y != 0.0f)
        encoded += NativeWeights.y * NativeLook1.Load(int4(coordinate, 0));
    [branch]
    if (NativeWeights.z != 0.0f)
        encoded += NativeWeights.z * NativeLook2.Load(int4(coordinate, 0));
    [branch]
    if (NativeWeights.w != 0.0f)
        encoded += NativeWeights.w * NativeLook3.Load(int4(coordinate, 0));

    // Along the neutral diagonal, trilinear interpolation is cubic in one fraction.
    // Read native nodes directly so coefficients need no cross-workgroup barrier.
    if (coordinate.z == 0 && coordinate.y < 15 && coordinate.x < 4)
    {
        const int3 cell = int3(coordinate.y, coordinate.y, coordinate.y);
        const float3 a = WeightedNode(cell);
        const float3 b = WeightedNode(cell + int3(1, 0, 0)) +
            WeightedNode(cell + int3(0, 1, 0)) + WeightedNode(cell + int3(0, 0, 1));
        const float3 c = WeightedNode(cell + int3(1, 1, 0)) +
            WeightedNode(cell + int3(1, 0, 1)) + WeightedNode(cell + int3(0, 1, 1));
        const float3 d = WeightedNode(cell + 1);
        const float3 coefficient = coordinate.x == 0 ? a : coordinate.x == 1 ? b - 3.0f * a : coordinate.x == 2 ? 3.0f * a - 2.0f * b + c : d - c + b - a;
        PreparedLook[uint3(coordinate.x + 16, coordinate.y, 0)] = float4(coefficient, 0.0f);
    }
    // Node (0,0,0) is the weighted black. Decode it once per draw with each
    // grading curve: Gamma 2.2 at (16,15,0) and sRGB at (17,15,0).
    if (all(coordinate == 0u))
    {
        const float3 black = Fo4HdrGradeCleanScene(encoded.rgb);
        PreparedLook[uint3(16, 15, 0)] = float4(Fo4HdrGradeCleanScene(
            Fo4HdrGradeDecode(black, Fo4HdrGradeCurveGamma22)), 0.0f);
        PreparedLook[uint3(17, 15, 0)] = float4(Fo4HdrGradeCleanScene(
            Fo4HdrGradeDecode(black, Fo4HdrGradeCurveSrgb)), 0.0f);
    }
    PreparedLook[coordinate] = encoded;
}
