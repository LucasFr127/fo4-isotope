// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
struct Vertex { float4 position : SV_Position; };
[maxvertexcount(3)]
void main(triangle Vertex input[3], inout TriangleStream<Vertex> output)
{
    for (uint index = 0; index < 3; ++index) output.Append(input[index]);
}
