// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "reference/ShaderPixels.hpp"
#include "FO4Isotope/tonemap/OperatorLut.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fo4hdr::tests::gpu {

using Microsoft::WRL::ComPtr;

void CheckHresult(HRESULT result, std::string_view operation);

struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

struct Texture
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Texture2D> staging;
};

struct Texture3D
{
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
};

struct TextureSpec
{
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    UINT bindFlags;
    const D3D11_SUBRESOURCE_DATA* initialData{};
};

struct FullscreenDraw
{
    ID3D11VertexShader* vertexShader;
    ID3D11PixelShader* pixelShader;
    ID3D11Buffer* cb2;
    ID3D11Buffer* toneMapConstants;
    ID3D11ShaderResourceView* const* resources;
    UINT resourceCount;
    ID3D11SamplerState* const* samplers;
    UINT samplerCount;
    ID3D11RenderTargetView* const* targets;
    UINT targetCount;
};

struct ComputeDispatch
{
    ID3D11ComputeShader* shader;
    ID3D11Buffer* constants;
    ID3D11ShaderResourceView* const* resources;
    UINT resourceCount;
    ID3D11UnorderedAccessView* output;
};

template <typename Constants>
[[nodiscard]] ComPtr<ID3D11Buffer> CreateConstantBuffer(
    ID3D11Device& device,
    const Constants& constants)
{
    static_assert(sizeof(Constants) % 16U == 0U);
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(sizeof(Constants));
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{&constants, 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    CheckHresult(
        device.CreateBuffer(&description, &data, &buffer),
        "ID3D11Device::CreateBuffer(constants)");
    return buffer;
}

template <class Shader, std::size_t Size, class Factory>
[[nodiscard]] ComPtr<Shader> CreateTypedShader(ID3D11Device& device,
    const std::array<std::byte, Size>& bytecode, Factory factory, std::string_view operation)
{
    ComPtr<Shader> shader;
    CheckHresult((device.*factory)(bytecode.data(), bytecode.size(), nullptr,
        shader.GetAddressOf()), operation);
    return shader;
}

template <std::size_t Size>
[[nodiscard]] ComPtr<ID3D11ComputeShader> CreateShader(ID3D11Device& device,
    const std::array<std::byte, Size>& bytecode)
{
    return CreateTypedShader<ID3D11ComputeShader>(device, bytecode,
        &ID3D11Device::CreateComputeShader, "ID3D11Device::CreateComputeShader(shipping bytecode)");
}

template <std::size_t Size>
[[nodiscard]] ComPtr<ID3D11PixelShader> CreatePixelShader(ID3D11Device& device,
    const std::array<std::byte, Size>& bytecode)
{
    return CreateTypedShader<ID3D11PixelShader>(device, bytecode,
        &ID3D11Device::CreatePixelShader, "ID3D11Device::CreatePixelShader(shipping bytecode)");
}

template <std::size_t Size>
[[nodiscard]] ComPtr<ID3D11VertexShader> CreateVertexShader(ID3D11Device& device,
    const std::array<std::byte, Size>& bytecode)
{
    return CreateTypedShader<ID3D11VertexShader>(device, bytecode,
        &ID3D11Device::CreateVertexShader, "ID3D11Device::CreateVertexShader(test bytecode)");
}

void CheckDebugMessages(Device& device);

[[nodiscard]] Device CreateDevice(bool hardware, bool debug = false);

[[nodiscard]] Texture CreateSizedTexture(ID3D11Device& device, const TextureSpec& spec);

[[nodiscard]] Texture CreateTexture(
    ID3D11Device& device,
    DXGI_FORMAT format,
    UINT bindFlags,
    const D3D11_SUBRESOURCE_DATA* initialData = nullptr);

[[nodiscard]] Texture3D CreateIdentityLut(ID3D11Device& device);

[[nodiscard]] ComPtr<ID3D11SamplerState> CreateClampSampler(
    ID3D11Device& device, D3D11_FILTER filter, std::string_view operation);

[[nodiscard]] ComPtr<ID3D11SamplerState> CreateClampPointSampler(ID3D11Device& device);

[[nodiscard]] ComPtr<ID3D11SamplerState> CreateClampLinearSampler(ID3D11Device& device);

void DrawFullscreen(ID3D11DeviceContext& context, const FullscreenDraw& draw);

void Dispatch(ID3D11DeviceContext& context, const ComputeDispatch& dispatch);
[[nodiscard]] Texture3D CreateOperatorLut(
    ID3D11Device& device,
    const fo4hdr::OperatorLutData& lut);
[[nodiscard]] Texture3D CreateOperatorLut(
    ID3D11Device& device,
    fo4hdr::ProductionToneMapperAlgorithm algorithm,
    float displayPeakNits);

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

struct CompositionConstants
{
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mode;
    float uiPaperWhiteNits;
    float sceneReferenceWhiteNits;
    float displayPeakNits;
    std::uint32_t gameUiBackdropCaptured;
    std::uint32_t gameUiCaptured;
    std::uint32_t settingsOverlayPresent{1};
    std::uint32_t sharedUiEncoded{};
    std::uint32_t assumedContent{};
    float armorPeakUiRelative{};
};
static_assert(sizeof(CompositionConstants) == 48);
static_assert(offsetof(CompositionConstants, sharedUiEncoded) == 36);
static_assert(offsetof(CompositionConstants, armorPeakUiRelative) == 44);

struct OutputConstants
{
    std::uint32_t width;
    std::uint32_t height;
    float relativeWhiteNits;
    std::uint32_t mode;
};

struct ToneMapConstants
{
    std::uint32_t productionMode;
    float displayPeakNits;
    float sceneReferenceWhiteNits;
    float uiTransportWhiteNits;
    float sceneExposureMultiplier;
    float videoWhiteNits;
    float operatorShaperAngleGain;
    float operatorShaperBias;
    float bloomStrength{1.0F};
    float vanillaGradeStrength{1.0F};
    float contrastSlope{1.0F};
    float highlightsEv{};
    float shadowsEv{};
    float saturation{1.0F};
    std::uint32_t liveSettingsReserved{};
    float blacksStrength{};
    Float4 colorAdaptationRow0{1.0F, 0.0F, 0.0F, 0.0F};
    Float4 colorAdaptationRow1{0.0F, 1.0F, 0.0F, 0.0F};
    Float4 colorAdaptationRow2{0.0F, 0.0F, 1.0F, 0.0F};
    std::uint32_t selectiveColorActive{};
    std::uint32_t liveSettingsReserved1{};
    std::array<float, 2> operatorShaperParameters{};
    std::array<Float4, 7> selectiveColorRegions{};
    Float4 selectiveColorPlaneA{};
    Float4 selectiveColorPlaneB{};
    Float4 lutProfileLuminanceRemoval{};
    Float4 lutProfileColorRemoval{};
    std::uint32_t lutProfilesActive{};
    std::array<std::uint32_t, 3> lutProfilesReserved{};
};
static_assert(sizeof(ToneMapConstants) == 320);
static_assert(offsetof(ToneMapConstants, blacksStrength) == 60);
static_assert(offsetof(ToneMapConstants, liveSettingsReserved1) == 116);
static_assert(offsetof(ToneMapConstants, selectiveColorPlaneA) == 240);
static_assert(offsetof(ToneMapConstants, selectiveColorPlaneA) +
    offsetof(Float4, alpha) == 252);
static_assert(offsetof(ToneMapConstants, selectiveColorPlaneB) == 256);
static_assert(offsetof(ToneMapConstants, selectiveColorPlaneB) +
    offsetof(Float4, alpha) == 268);
static_assert(offsetof(ToneMapConstants, lutProfileLuminanceRemoval) == 272);
static_assert(offsetof(ToneMapConstants, lutProfileColorRemoval) == 288);
static_assert(offsetof(ToneMapConstants, lutProfilesActive) == 304);

[[nodiscard]] ComPtr<ID3D11Buffer> CreateConstantBuffer(
    ID3D11Device& device, ToneMapConstants constants);

struct ImageSpaceConstants
{
    std::array<Float4, 5> values;
};
static_assert(sizeof(ImageSpaceConstants) == 80);

struct ImageSpaceConstantsWithFinalAffine
{
    std::array<Float4, 6> values;
};
static_assert(sizeof(ImageSpaceConstantsWithFinalAffine) == 96);

struct FxaaConstants
{
    Float4 texelSize;
};

struct LutConstants
{
    std::array<Float4, 2> values;
};
static_assert(sizeof(LutConstants) == 32);

struct TemporalConstants
{
    std::array<Float4, 6> values;
};
static_assert(sizeof(TemporalConstants) == 96);

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

[[nodiscard]] Texture3D CreateNativeGrade(ID3D11Device& device, unsigned grade);

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

extern int g_failures;
extern int g_skips;

void Check(bool condition, std::string_view expression,
    std::string_view file, int line);

struct PresentationDispatch
{
    ID3D11ComputeShader* shader;
    ID3D11Buffer* constants;
    std::array<ID3D11ShaderResourceView*, 4> inputs;
    ID3D11UnorderedAccessView* output;
    UINT width, height;
};

void DispatchPresentation(Device& device, const PresentationDispatch& request);

struct PresentationShaders
{
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;
};

PresentationShaders CreatePresentationShaders(ID3D11Device& device);

struct PresentationDraw
{
    const PresentationShaders* shaders;
    ID3D11Buffer* constants;
    std::array<ID3D11ShaderResourceView*, 4> inputs;
    ID3D11RenderTargetView* output;
    UINT width, height;
};

void DrawPresentation(Device& device, const PresentationDraw& request);

struct PresentationWindow
{
    HWND handle;
    PresentationWindow();
    ~PresentationWindow();
};

ComPtr<IDXGISwapChain1> CreatePresentationChain(Device& device, HWND window,
    UINT width, UINT height);

Texture GetPresentationBuffer(Device& device, IDXGISwapChain1& chain);

}  // namespace fo4hdr::tests::gpu

#define FO4HDR_WARP_CHECK(expression) \
    ::fo4hdr::tests::gpu::Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
