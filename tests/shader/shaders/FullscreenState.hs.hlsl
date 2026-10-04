// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
struct PatchFactors { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
struct Vertex { float4 position : SV_Position; };
PatchFactors Factors(InputPatch<Vertex, 3> patch)
{
    PatchFactors result;
    result.edges[0] = result.edges[1] = result.edges[2] = result.inside = 1.0f;
    return result;
}
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("Factors")]
Vertex main(InputPatch<Vertex, 3> patch, uint index : SV_OutputControlPointID)
{
    return patch[index];
}
