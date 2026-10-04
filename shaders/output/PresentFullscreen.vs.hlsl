// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

float4 main(uint vertex : SV_VertexID) : SV_Position
{
    const float2 corner = float2((vertex << 1u) & 2u, vertex & 2u);
    return float4(corner * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
