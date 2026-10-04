// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
interface PixelValue { float4 Read(); };
class ForeignPixelValue : PixelValue
{
    float4 Read() { return float4(0.25f, 0.5f, 0.75f, 1.0f); }
};
class AlternatePixelValue : PixelValue
{
    float4 Read() { return float4(0.75f, 0.5f, 0.25f, 1.0f); }
};
PixelValue SelectedPixelValue;
float4 main() : SV_Target0 { return SelectedPixelValue.Read(); }
