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

bool PipboyTextureMatches(const D3D11_TEXTURE2D_DESC& value) noexcept
{
    const std::array layout{value.Width, value.Height, value.MipLevels,
        value.ArraySize, value.SampleDesc.Count, value.SampleDesc.Quality};
    constexpr std::array expected{876U, 700U, 1U, 1U, 1U, 0U};
    return layout == expected && value.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
}

ComPtr<IUnknown> PipboyResourceIdentity(ID3D11Resource* resource) noexcept
{
    if (resource == nullptr) return {};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) || !texture) return {};
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return PipboyTextureMatches(description) ? CanonicalIdentity(resource) : ComPtr<IUnknown>{};
}

bool PipboyInputViewMatches(const D3D11_SHADER_RESOURCE_VIEW_DESC& value) noexcept
{
    if (value.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    if (value.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    if (value.Texture2D.MostDetailedMip != 0) return false;
    return value.Texture2D.MipLevels == 1 || value.Texture2D.MipLevels == UINT(-1);
}

ComPtr<IUnknown> PipboyInputIdentity(ID3D11DeviceContext* context,
    ID3D11Device* device, UINT slot) noexcept
{
    ComPtr<ID3D11ShaderResourceView> view;
    context->PSGetShaderResources(slot, 1, &view);
    if (!view || !SameDevice(view.Get(), device)) return {};
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    view->GetDesc(&description);
    if (!PipboyInputViewMatches(description)) return {};
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    return PipboyResourceIdentity(resource.Get());
}

ComPtr<IUnknown> PipboyOutputIdentity(ID3D11DeviceContext* context, ID3D11Device* device) noexcept
{
    ComPtr<ID3D11Predicate> predicate;
    BOOL value{};
    context->GetPredication(&predicate, &value);
    if (predicate) return {};
    ComPtr<ID3D11RenderTargetView> view;
    context->OMGetRenderTargets(1, &view, nullptr);
    if (!view || !SameDevice(view.Get(), device)) return {};
    D3D11_RENDER_TARGET_VIEW_DESC description{};
    view->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        description.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return {};
    if (description.Texture2D.MipSlice != 0) return {};
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    return PipboyResourceIdentity(resource.Get());
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
    const auto crc32 = ComputeRenoDxShaderCrc32(bytecode);
    const auto* descriptor = FindRenoDxFallout4Shader(crc32);
    const auto pipboyRole = MatchPipboyScreenBytecode(bytecode, crc32);
    if (descriptor == nullptr && pipboyRole == PipboyScreenShader::unknown) return false;
    const auto end = observed_.begin() + observedCount_;
    const auto found = std::find_if(observed_.begin(), end,
        [shader](const ObservedShader& observed) { return SameIdentity(observed.original.Get(), shader); });
    if (found != end)
        return found->descriptor == descriptor && found->pipboyRole == pipboyRole;
    if (observedCount_ == observed_.size()) return false;
    observed_[observedCount_++] = {shader, descriptor, pipboyRole};
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

bool ScenePassRenderer::StartPipboyScreenObservation() noexcept
{
    if (!Active() || invoking_) return false;
    pipboyProducerIdentity_.Reset();
    const auto epoch = lifetime_.CurrentEpoch();
    pipboyObservation_.Begin(epoch, epoch);
    return true;
}

bool ScenePassRenderer::StopPipboyScreenObservation() noexcept
{
    if (!OnOwnerThread() || invoking_) return false;
    RetirePipboyScreenObservation();
    return true;
}

PipboyScreenObservation ScenePassRenderer::PipboyScreenSnapshot() const noexcept
{
    if (!OnOwnerThread() || invoking_) return {};
    return pipboyObservation_;
}

void ScenePassRenderer::RetirePipboyScreenObservation() noexcept
{
    pipboyObservation_.Stop();
    pipboyProducerIdentity_.Reset();
}

PipboyScreenShader ScenePassRenderer::BoundPipboyScreenRole() const noexcept
{
    ComPtr<ID3D11PixelShader> shader;
    context_->PSGetShader(&shader, nullptr, nullptr);
    if (!shader) return PipboyScreenShader::unknown;
    const auto end = observed_.begin() + observedCount_;
    const auto found = std::find_if(observed_.begin(), end, [&shader](const ObservedShader& entry) {
        return SameIdentity(entry.original.Get(), shader.Get());
    });
    return found == end ? PipboyScreenShader::unknown : found->pipboyRole;
}

void ScenePassRenderer::ObservePipboyScreenDraw(SceneDrawOutcome outcome) noexcept
{
    if (!pipboyObservation_.requested) return;
    const auto epoch = lifetime_.CurrentEpoch();
    const bool continues = frameOpen_ && pipboyObservation_.Matches(epoch, epoch);
    if (!continues || outcome == SceneDrawOutcome::callbackFailed)
    {
        RetirePipboyScreenObservation();
        return;
    }
    if (outcome != SceneDrawOutcome::forwarded) return;
    const auto role = BoundPipboyScreenRole();
    if (role != PipboyScreenShader::unknown) ObservePipboyScreenRole(role);
}

void ScenePassRenderer::ObservePipboyScreenRole(PipboyScreenShader role) noexcept
{
    const bool producer = role == PipboyScreenShader::producer;
    auto input = PipboyInputIdentity(context_.Get(), device_.Get(), producer ? 0 : 4);
    if (!input)
    {
        RejectPipboyScreenDraw(producer);
        return;
    }
    if (producer)
    {
        if (!CompletePipboyProducer(input.Get())) RejectPipboyScreenDraw(true);
    }
    else
    {
        pipboyObservation_.Consumer(frameGeneration_,
            pipboyProducerIdentity_ && pipboyProducerIdentity_.Get() == input.Get(),
            pipboyProducerIdentity_ != nullptr);
    }
}

void ScenePassRenderer::RejectPipboyScreenDraw(bool producer) noexcept
{
    ++pipboyObservation_.rejectedDraws;
    if (producer) pipboyProducerIdentity_.Reset();
}

bool ScenePassRenderer::CompletePipboyProducer(IUnknown* input) noexcept
{
    auto output = PipboyOutputIdentity(context_.Get(), device_.Get());
    if (!output) return false;
    if (output.Get() == input) return false;
    pipboyProducerIdentity_ = std::move(output);
    pipboyObservation_.Producer(frameGeneration_);
    return true;
}

}  // namespace fo4hdr
