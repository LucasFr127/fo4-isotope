// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
struct PatchFactors { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
struct Vertex { float4 position : SV_Position; };
[domain("tri")]
float4 main(PatchFactors factors, float3 location : SV_DomainLocation,
    const OutputPatch<Vertex, 3> patch) : SV_Position
{
    return patch[0].position * location.x + patch[1].position * location.y + patch[2].position * location.z;
}
