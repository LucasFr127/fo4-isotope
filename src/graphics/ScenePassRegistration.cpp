// SPDX-License-Identifier: GPL-3.0-or-later

#include "HdrRenderer.hpp"
#include "RenoDxShaderIdentity.hpp"
#include "resources/HdrResourcePolicy.hpp"

#include <algorithm>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;

ComPtr<IUnknown> CanonicalIdentity(IUnknown* object) noexcept
{
    ComPtr<IUnknown> identity;
    if (object == nullptr) return identity;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) identity.Reset();
    return identity;
}

bool SameIdentity(IUnknown* left, IUnknown* right) noexcept
{
    const auto leftIdentity = CanonicalIdentity(left);
    const auto rightIdentity = CanonicalIdentity(right);
    return leftIdentity && leftIdentity.Get() == rightIdentity.Get();
}

bool SameDevice(ID3D11DeviceChild* child, ID3D11Device* device) noexcept
{
    ComPtr<ID3D11Device> owner;
    child->GetDevice(&owner);
    return SameIdentity(owner.Get(), device);
}

HRESULT PrepareSceneViews(ID3D11Device* device, ID3D11Texture2D* texture,
    ComPtr<ID3D11ShaderResourceView>& shaderResource,
    ComPtr<ID3D11RenderTargetView>& renderTarget) noexcept
{
    auto result = device->CreateShaderResourceView(texture, nullptr, &shaderResource);
    if (FAILED(result)) return result;
    if (!shaderResource) return E_FAIL;
    result = device->CreateRenderTargetView(texture, nullptr, &renderTarget);
    if (FAILED(result)) return result;
    return renderTarget ? S_OK : E_FAIL;
}

bool SingleSceneSubresource(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    if (description.MipLevels != 1 || description.ArraySize != 1) return false;
    return description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0;
}

bool GpuSceneStorage(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    if (description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0) return false;
    if (description.MiscFlags != 0) return false;
    return description.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
}

bool SupportedSceneLayout(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    return SingleSceneSubresource(description) && GpuSceneStorage(description);
}

}  // namespace

HRESULT ScenePassRenderer::CreateObservedPixelShader(std::span<const std::byte> bytecode,
    ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) noexcept
{
    if (!Active() || invoking_) return E_UNEXPECTED;
    const auto result = device_->CreatePixelShader(
        bytecode.data(), bytecode.size(), linkage, shader);
    // Validation-only S_FALSE/null outputs are not created shader associations.
    if (result == S_OK && shader != nullptr)
        static_cast<void>(ObserveCreatedPixelShader(bytecode, *shader));
    return result;
}

bool ScenePassRenderer::ObserveCreatedPixelShader(std::span<const std::byte> bytecode,
    ID3D11PixelShader* shader) noexcept
{
    if (!CanObserveShader(shader)) return false;
    if (!SameDevice(shader, device_.Get())) return false;
    const auto* descriptor = FindRenoDxFallout4Shader(ComputeRenoDxShaderCrc32(bytecode));
    if (descriptor == nullptr) return false;
    const auto end = observed_.begin() + observedCount_;
    const auto found = std::find_if(observed_.begin(), end,
        [shader](const ObservedShader& observed) { return SameIdentity(observed.original.Get(), shader); });
    if (found != end) return found->descriptor == descriptor;
    if (observedCount_ == observed_.size()) return false;
    observed_[observedCount_++] = {shader, descriptor};
    return true;
}

HRESULT ScenePassRenderer::CreateSceneTexture(const D3D11_TEXTURE2D_DESC& original,
    const D3D11_SUBRESOURCE_DATA* initialData, SceneTextureHandle& handle) noexcept
{
    handle = {};
    const auto registration = SceneRegistrationStatus(original, initialData);
    if (FAILED(registration)) return registration;
    auto promoted = original;
    promoted.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<ID3D11Texture2D> texture;
    const auto result = device_->CreateTexture2D(&promoted, nullptr, &texture);
    if (result != S_OK) return result;
    if (!texture) return E_FAIL;
    return AdoptSceneTexture(original, texture.Get(), handle);
}

HRESULT ScenePassRenderer::AdoptSceneTexture(const D3D11_TEXTURE2D_DESC& original,
    ID3D11Texture2D* texture, SceneTextureHandle& handle) noexcept
{
    handle = {};
    const auto registration = SceneRegistrationStatus(original);
    if (FAILED(registration)) return registration;
    if (!QualifiedSceneTexture(original, texture)) return E_INVALIDARG;
    SceneTexture prepared;
    prepared.texture = texture;
    const auto result = PrepareSceneViews(device_.Get(), texture,
        prepared.shaderResource, prepared.renderTarget);
    if (FAILED(result)) return result;
    const auto slot = sceneCount_++;
    scenes_[slot] = std::move(prepared);
    handle.owner_ = this;
    handle.epoch_ = lifetime_.CurrentEpoch();
    handle.slot_ = slot;
    return S_OK;
}

HRESULT ScenePassRenderer::SceneRegistrationStatus(const D3D11_TEXTURE2D_DESC& original,
    const D3D11_SUBRESOURCE_DATA* initialData) const noexcept
{
    if (!Active() || invoking_) return E_UNEXPECTED;
    if (initialData != nullptr) return E_INVALIDARG;
    if (!PromotableScene(original)) return E_INVALIDARG;
    return sceneCount_ == scenes_.size() ? E_OUTOFMEMORY : S_OK;
}

bool ScenePassRenderer::PromotableScene(const D3D11_TEXTURE2D_DESC& original) const noexcept
{
    if (!SupportedSceneLayout(original)) return false;
    const SceneResourceDescription description{
        original.Format == DXGI_FORMAT_R11G11B10_FLOAT
            ? SceneResourceFormat::r11g11b10Float : SceneResourceFormat::other,
        original.Width, original.Height};
    return ShouldPromoteSceneResourceToFp16(description, width_, height_);
}

bool ScenePassRenderer::QualifiedSceneTexture(const D3D11_TEXTURE2D_DESC& original,
    ID3D11Texture2D* texture) const noexcept
{
    if (texture == nullptr) return false;
    if (!SameDevice(texture, device_.Get())) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (!SupportedSceneLayout(description)) return false;
    return description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        description.Width == original.Width && description.Height == original.Height;
}

}  // namespace fo4hdr
