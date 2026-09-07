// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "d3d11/GpuHarness.hpp"
#include "HdrRenderer.hpp"
#include "ShaderTestSuite.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace fo4hdr {
class PresentationPass;
}

namespace fo4hdr::tests::gpu {

namespace detail {

inline constexpr UINT width = 4;
inline constexpr UINT height = 2;
inline constexpr UINT lutSize = 16;
inline constexpr Float4 sentinelColor{0.0625f, 0.125f, 0.25f, 0.5f};
using Pixels = std::array<Float4, width * height>;
using HalfPixel = std::array<std::uint16_t, 4>;
using LutPixels = std::vector<Float4>;
using Microsoft::WRL::ComPtr;

struct PixelFixture
{
    Device gpu{CreateWarpDevice()};
    ScenePassRenderer renderer;
    SceneTextureHandle input;
    SceneTextureHandle destination;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> plain;
    ComPtr<ID3D11PixelShader> sentinel;
    ComPtr<ID3D11ClassLinkage> linkage;
    ComPtr<ID3D11ClassInstance> instance;
    ComPtr<ID3D11Buffer> sentinelConstants;
    ComPtr<ID3D11SamplerState> point;
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11RasterizerState> rasterizer;
    UINT extentWidth;
    UINT extentHeight;
    LayerWhiteConfiguration whites;

    explicit PixelFixture(const std::filesystem::path& directory,
        UINT imageWidth = width, UINT imageHeight = height, LayerWhiteConfiguration layerWhites = {},
        const OperatorLutData* operatorLut = nullptr, SceneGradeConfiguration sceneGrade = {});
    ComPtr<ID3D11PixelShader> ObservedShader(const std::filesystem::path& path,
        ID3D11ClassLinkage* classes);
    void Bind(bool dynamic = false);
    void CheckRestored(bool dynamic);
};

struct LineageScenes
{
    SceneTextureHandle raw;
    SceneTextureHandle tone;
    SceneTextureHandle grade;
    SceneTextureHandle temporal;
};

D3D11_TEXTURE2D_DESC SceneDescription(UINT extentWidth = width, UINT extentHeight = height);
void UploadScene(ID3D11DeviceContext* context, ID3D11Texture2D* texture, std::span<const Float4> pixels);
std::vector<Float4> ReadScene(const Device& gpu, ID3D11Texture2D* texture);
void CheckPixel(const Float4& actual, const Float4& expected);
double DecodeAuthoredSrgb(double code);
ComPtr<ID3D11Buffer> BindPostProcess(PixelFixture& fixture);
void CheckBoundInputs(PixelFixture& fixture, UINT slot, ID3D11Buffer* constants);
ComPtr<ID3D11Buffer> ConstantBuffer(ID3D11Device* device, std::span<const Float4> values);
std::array<Float4, 6> ToneConstants();
LutPixels LutInputs(unsigned variant);
Float4 LutExpected(const Float4& scene, const std::array<LutPixels, 4>& textures);
std::size_t LutIndex(UINT x, UINT y, UINT z);
ComPtr<ID3D11ShaderResourceView> LutTexture(ID3D11Device* device, const LutPixels& pixels);
SceneDrawOutcome DrawPass(PixelFixture& fixture, std::size_t program, bool dynamic = false);
SceneTextureHandle AdditionalScene(PixelFixture& fixture);
void CheckSelection(PixelFixture& fixture, SceneTextureHandle expected);

}  // namespace detail

void VerifyNativeRouting(detail::PixelFixture& fixture, const std::filesystem::path& directory);
void VerifyNativePresentationProxy(detail::PixelFixture& fixture, IDXGISwapChain* chain);
void VerifyRetainedNativeSdrFrame(detail::PixelFixture& fixture, PresentationPass& pass,
    IDXGISwapChain* chain);

}  // namespace fo4hdr::tests::gpu
