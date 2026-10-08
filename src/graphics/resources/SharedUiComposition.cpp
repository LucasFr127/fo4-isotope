// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "SharedUiComposition.hpp"
#include "../d3d11/BorrowedFullscreenState.hpp"
#include <FO4Isotope/ShippingShaders.generated.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace fo4hdr {
namespace {
using Microsoft::WRL::ComPtr;

struct SharedUiSeedConstants
{
    SharedUiWhite white;
    SharedUiSeedMode mode{};
    std::uint32_t reserved{};
};
static_assert(sizeof(SharedUiSeedConstants) == 16);

bool SameObject(IUnknown* left, IUnknown* right) noexcept
{
    if (!left || !right) return false;
    ComPtr<IUnknown> leftIdentity, rightIdentity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity))) &&
        leftIdentity.Get() == rightIdentity.Get();
}

bool WhiteValid(SharedUiWhite white) noexcept
{
    const std::array valid{std::isfinite(white.sceneWhiteNits), white.sceneWhiteNits > 0,
        std::isfinite(white.uiWhiteNits), white.uiWhiteNits > 0,
        std::isfinite(white.sceneWhiteNits / white.uiWhiteNits)};
    return std::ranges::all_of(valid, [](bool value) { return value; });
}

bool ViewsValid(const SharedUiSeedInputs& inputs) noexcept
{
    if (!inputs.scene || !inputs.output) return false;
    if (!WhiteValid(inputs.white)) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC scene{};
    inputs.scene->GetDesc(&scene);
    D3D11_RENDER_TARGET_VIEW_DESC output{};
    inputs.output->GetDesc(&output);
    if (scene.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    if (output.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return false;
    const std::array valid{scene.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        scene.Texture2D.MostDetailedMip == 0, scene.Texture2D.MipLevels == 1,
        output.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        output.Texture2D.MipSlice == 0};
    return std::ranges::all_of(valid, [](bool value) { return value; });
}
} // namespace

bool SharedUiComposition::Create(ID3D11Device* device,
    std::uint32_t width, std::uint32_t height) noexcept
{
    if (!device) return false;
    if (width == 0 || height == 0) return false;
    SharedUiComposition candidate;
    candidate.device_ = device;
    candidate.width_ = width;
    candidate.height_ = height;
    const bool complete = candidate.CreateTextures() && candidate.CreatePipeline() &&
        candidate.CreateConstants();
    if (!complete) return false;
    constexpr char scratchName[] = "FO4Isotope shared UI seed source";
    (void)candidate.scratch_->SetPrivateData(WKPDID_D3DDebugObjectName,
        static_cast<UINT>(sizeof(scratchName) - 1), scratchName);
    *this = std::move(candidate);
    return true;
}

bool SharedUiComposition::CreateTextures() noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width_;
    description.Height = height_;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(device_->CreateTexture2D(&description, nullptr, &scratch_)) &&
        SUCCEEDED(device_->CreateShaderResourceView(scratch_.Get(), nullptr, &scratchSrv_));
}

bool SharedUiComposition::CreatePipeline() noexcept
{
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device_->CreateVertexShader(generated::kPresentFullscreenVs.data(),
            generated::kPresentFullscreenVs.size(), nullptr, &vertex_)) &&
        SUCCEEDED(device_->CreatePixelShader(generated::kSharedUiSeedPs.data(),
            generated::kSharedUiSeedPs.size(), nullptr, &pixel_)) &&
        SUCCEEDED(device_->CreateRasterizerState(&rasterizer, &rasterizer_)) &&
        SUCCEEDED(device_->CreateDepthStencilState(&depth, &depth_)) &&
        SUCCEEDED(device_->CreateBlendState(&blend, &blend_));
}

bool SharedUiComposition::CreateConstants() noexcept
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(SharedUiSeedConstants);
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device_->CreateBuffer(&description, nullptr, &constants_));
}

bool SharedUiComposition::ContextMatches(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1) const noexcept
{
    if (!device_ || !SameObject(context, context1)) return false;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    return context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE &&
        SameObject(device.Get(), device_.Get());
}

bool SharedUiComposition::TextureMatches(ID3D11Resource* resource) const noexcept
{
    if (!resource) return false;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return false;
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    const std::array valid{SameObject(device.Get(), device_.Get()), description.Width == width_,
        description.Height == height_, description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        description.MipLevels == 1, description.ArraySize == 1,
        description.SampleDesc.Count == 1, description.SampleDesc.Quality == 0};
    return std::ranges::all_of(valid, [](bool value) { return value; });
}

bool SharedUiComposition::ResolveInputs(const SharedUiSeedInputs& inputs,
    ComPtr<ID3D11Resource>& scene, ComPtr<ID3D11Resource>& output) const noexcept
{
    if (!ViewsValid(inputs)) return false;
    inputs.scene->GetResource(&scene);
    inputs.output->GetResource(&output);
    return TextureMatches(scene.Get()) && TextureMatches(output.Get());
}

bool SharedUiComposition::InputsCompatible(const SharedUiSeedInputs& inputs) const noexcept
{
    ComPtr<ID3D11Resource> scene, output;
    return ResolveInputs(inputs, scene, output);
}

bool SharedUiComposition::UploadWhite(ID3D11DeviceContext* context, const SharedUiSeedInputs& inputs) noexcept
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    const SharedUiSeedConstants constants{inputs.white, inputs.mode};
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(constants_.Get(), 0);
    return true;
}

void SharedUiComposition::Draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* output) noexcept
{
    auto* input = scratchSrv_.Get();
    auto* constants = constants_.Get();
    context->PSSetShaderResources(0, 1, &input);
    context->OMSetRenderTargets(1, &output, nullptr);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->VSSetShader(vertex_.Get(), nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(pixel_.Get(), nullptr, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    context->RSSetState(rasterizer_.Get());
    context->RSSetViewports(1, &viewport);
    context->OMSetDepthStencilState(depth_.Get(), 0);
    context->OMSetBlendState(blend_.Get(), nullptr, UINT(-1));
    context->Draw(3, 0);
}

bool SharedUiComposition::Seed(ID3D11DeviceContext* context,
    ID3D11DeviceContext1* context1, const SharedUiSeedInputs& inputs) noexcept
{
    if (!ContextMatches(context, context1)) return false;
    ComPtr<ID3D11Resource> scene, output;
    if (!ResolveInputs(inputs, scene, output)) return false;
    BorrowedFullscreenState borrowed(context, context1, output.Get());
    if (!borrowed.Ready() || !UploadWhite(context, inputs)) return false;
    // A private copy permits seeding the same image that currently holds the scene.
    context->CopyResource(scratch_.Get(), scene.Get());
    Draw(context, inputs.output);
    return true;
}

void SharedUiComposition::Reset() noexcept
{
    *this = {};
}

} // namespace fo4hdr
