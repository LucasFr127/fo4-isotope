// SPDX-License-Identifier: GPL-3.0-or-later

#include "HdrRenderer.hpp"
#include "RenoDxShaderIdentity.hpp"
#include "resources/HdrResourcePolicy.hpp"

#include <fo4hdr-shaders/ToneMapA.hpp>
#include <fo4hdr-shaders/ToneMapB.hpp>
#include <fo4hdr-shaders/ToneMapNewA.hpp>
#include <fo4hdr-shaders/ToneMapNewB.hpp>
#include <fo4hdr-shaders/CreativeGradeLut.hpp>
#include <fo4hdr-shaders/TemporalAa.hpp>
#include <fo4hdr-shaders/TemporalAa2.hpp>
#include <fo4hdr-shaders/Fxaa.hpp>
#include <fo4hdr-shaders/Video.hpp>

#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;

struct ReplacementBytecode
{
    const BYTE* data;
    std::size_t size;
};

constexpr std::array<ReplacementBytecode, ScenePassRenderer::replacementProgramCount> programs{{
    {kToneMapABytecode, sizeof(kToneMapABytecode)},
    {kToneMapBBytecode, sizeof(kToneMapBBytecode)},
    {kToneMapNewABytecode, sizeof(kToneMapNewABytecode)},
    {kToneMapNewBBytecode, sizeof(kToneMapNewBBytecode)},
    {kCreativeGradeLutBytecode, sizeof(kCreativeGradeLutBytecode)},
    {kTemporalAaBytecode, sizeof(kTemporalAaBytecode)},
    {kTemporalAa2Bytecode, sizeof(kTemporalAa2Bytecode)},
    {kFxaaBytecode, sizeof(kFxaaBytecode)},
    {kVideoBytecode, sizeof(kVideoBytecode)}
}};

std::size_t ProgramIndex(RenoDxShaderPass pass) noexcept
{
    constexpr std::array supported{
        RenoDxShaderPass::toneMapA, RenoDxShaderPass::toneMapB,
        RenoDxShaderPass::toneMapNewA, RenoDxShaderPass::toneMapNewB,
        RenoDxShaderPass::lut, RenoDxShaderPass::taa, RenoDxShaderPass::taa2,
        RenoDxShaderPass::fxaa, RenoDxShaderPass::video};
    for (std::size_t index = 0; index < supported.size(); ++index)
    {
        if (supported[index] == pass) return index;
    }
    return ScenePassRenderer::replacementProgramCount;
}

bool SameIdentity(IUnknown* left, IUnknown* right) noexcept
{
    if (left == nullptr || right == nullptr) return false;
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    if (FAILED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity)))) return false;
    if (FAILED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity)))) return false;
    return leftIdentity && rightIdentity && leftIdentity.Get() == rightIdentity.Get();
}

bool TemporalPass(RenoDxShaderPass pass) noexcept
{
    return pass == RenoDxShaderPass::taa || pass == RenoDxShaderPass::taa2;
}

bool SupportedInputDomain(RenoDxShaderPass pass, SceneColorDomain domain) noexcept
{
    if (domain == SceneColorDomain::bt709Scene) return true;
    return pass == RenoDxShaderPass::fxaa;
}

ComPtr<IUnknown> CanonicalIdentity(IUnknown* object) noexcept
{
    ComPtr<IUnknown> identity;
    if (object == nullptr) return identity;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) identity.Reset();
    return identity;
}

ComPtr<ID3D11Resource> ViewResource(ID3D11View* view) noexcept
{
    ComPtr<ID3D11Resource> resource;
    if (view != nullptr) view->GetResource(&resource);
    return resource;
}

bool CompatibleTextureFootprint(ID3D11View* view, ID3D11Texture2D* color) noexcept
{
    const auto resource = ViewResource(view);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC candidate{};
    D3D11_TEXTURE2D_DESC destination{};
    texture->GetDesc(&candidate);
    color->GetDesc(&destination);
    if (candidate.Width != destination.Width || candidate.Height != destination.Height) return false;
    if (candidate.ArraySize != 1) return false;
    return candidate.SampleDesc.Count == destination.SampleDesc.Count &&
        candidate.SampleDesc.Quality == destination.SampleDesc.Quality;
}

bool Float4TemporalFormat(DXGI_FORMAT format) noexcept
{
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        format == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

bool CompatibleMetadataTarget(ID3D11RenderTargetView* view,
    ID3D11Texture2D* color) noexcept
{
    if (view == nullptr) return false;
    D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
    view->GetDesc(&viewDescription);
    if (!Float4TemporalFormat(viewDescription.Format)) return false;
    if (viewDescription.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return false;
    if (viewDescription.Texture2D.MipSlice != 0) return false;
    return CompatibleTextureFootprint(view, color);
}

bool CompatibleHistoryView(ID3D11ShaderResourceView* view,
    ID3D11Texture2D* scene) noexcept
{
    if (view == nullptr) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    view->GetDesc(&viewDescription);
    if (!Float4TemporalFormat(viewDescription.Format)) return false;
    if (viewDescription.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    if (viewDescription.Texture2D.MostDetailedMip != 0) return false;
    if (viewDescription.Texture2D.MipLevels != 1) return false;
    return CompatibleTextureFootprint(view, scene);
}

std::array<ComPtr<ID3D11ShaderResourceView>, 5> TemporalInputs(
    ID3D11DeviceContext* context, UINT count) noexcept
{
    std::array<ID3D11ShaderResourceView*, 5> raw{};
    std::array<ComPtr<ID3D11ShaderResourceView>, 5> owned;
    context->PSGetShaderResources(0, count, raw.data());
    for (std::size_t index = 0; index < count; ++index) owned[index].Attach(raw[index]);
    return owned;
}

bool DistinctTemporalIdentities(const std::array<ComPtr<IUnknown>, 3>& identities) noexcept
{
    for (const auto& identity : identities)
    {
        if (!identity) return false;
    }
    return identities[0].Get() != identities[1].Get() &&
        identities[0].Get() != identities[2].Get() &&
        identities[1].Get() != identities[2].Get();
}

bool InputAvoidsTemporalOutputs(ID3D11ShaderResourceView* input,
    const std::array<ComPtr<IUnknown>, 3>& identities, bool history) noexcept
{
    const auto identity = CanonicalIdentity(ViewResource(input).Get());
    if (!identity) return false;
    if (history && identity.Get() == identities[0].Get()) return false;
    return identity.Get() != identities[1].Get() && identity.Get() != identities[2].Get();
}

bool InputsAvoidTemporalOutputs(
    const std::array<ComPtr<ID3D11ShaderResourceView>, 5>& inputs,
    ID3D11Resource* scene, ID3D11Resource* metadata, ID3D11Resource* color) noexcept
{
    const std::array identities{
        CanonicalIdentity(scene), CanonicalIdentity(metadata), CanonicalIdentity(color)};
    if (!DistinctTemporalIdentities(identities)) return false;
    if (!inputs[1]) return false;
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        if (!inputs[index]) continue;
        if (!InputAvoidsTemporalOutputs(inputs[index].Get(), identities, index == 1)) return false;
    }
    return true;
}

bool AssociatedImmediateContext(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11Device> contextDevice;
    device->GetImmediateContext(&immediate);
    context->GetDevice(&contextDevice);
    return SameIdentity(immediate.Get(), context) && SameIdentity(contextDevice.Get(), device);
}

bool Fp16SceneSupported(ID3D11Device* device) noexcept
{
    UINT support{};
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R16G16B16A16_FLOAT, &support)))
        return false;
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    return (support & required) == required;
}

bool ValidInitializationArguments(ID3D11Device* device,
    ID3D11DeviceContext* context, const ScenePassConfiguration& configuration) noexcept
{
    if (device == nullptr || context == nullptr) return false;
    return configuration.width != 0 && configuration.height != 0 && configuration.whites.Valid();
}

bool CompatibleDevice(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return false;
    return AssociatedImmediateContext(device, context) && Fp16SceneSupported(device);
}

bool ValidWhite(float value) noexcept
{
    if (!std::isfinite(value)) return false;
    return value >= kMinimumUiPaperWhiteNits && value <= kMaximumUiPaperWhiteNits;
}

bool PreparePixelContext(ID3D11Device* device, ID3D11DeviceContext* context,
    ComPtr<ID3D11DeviceContext1>& prepared) noexcept
{
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,
            &options, sizeof(options)))) return false;
    if (!options.ConstantBufferOffsetting) return false;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&prepared)))) return false;
    return prepared != nullptr;
}

bool PrepareVideoConstants(ID3D11Device* device, LayerWhiteConfiguration whites,
    ComPtr<ID3D11Buffer>& prepared) noexcept
{
    const std::array<float, 4> values{whites.videoWhiteNits / whites.uiWhiteNits, 0.0F, 0.0F, 0.0F};
    static_assert(sizeof(values) == 16);
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(values);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA data{values.data(), 0, 0};
    if (FAILED(device->CreateBuffer(&description, &data, &prepared))) return false;
    return prepared != nullptr;
}

bool PreparePrograms(ID3D11Device* device,
    std::array<ComPtr<ID3D11PixelShader>, ScenePassRenderer::replacementProgramCount>& prepared) noexcept
{
    for (std::size_t index = 0; index < programs.size(); ++index)
    {
        const auto& program = programs[index];
        const auto result = device->CreatePixelShader(
            program.data, program.size, nullptr, &prepared[index]);
        if (FAILED(result) || !prepared[index]) return false;
    }
    return true;
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

std::size_t CanonicalProgramIndex(const RenoDxShaderDescriptor* descriptor) noexcept
{
    if (descriptor == nullptr) return ScenePassRenderer::replacementProgramCount;
    if (FindRenoDxFallout4Shader(descriptor->crc32) != descriptor)
        return ScenePassRenderer::replacementProgramCount;
    return ProgramIndex(descriptor->pass);
}

bool SupportedSceneLayout(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    if (description.MipLevels != 1 || description.ArraySize != 1) return false;
    if (description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0) return false;
    if (description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0) return false;
    if (description.MiscFlags != 0) return false;
    return description.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
}

// Class-instance tuples support 256 entries; the SDK interface limit is separate.
constexpr UINT pixelClassInstanceCapacity = 256;

class ScopedPixelShader
{
public:
    ScopedPixelShader(ID3D11DeviceContext* context, ID3D11PixelShader* replacement) noexcept
        : context_(context)
    {
        context_->PSGetShader(&previous_, classes_.data(), &classCount_);
        if (classCount_ > pixelClassInstanceCapacity) return;
        if (replacement != nullptr)
        {
            context_->PSSetShader(replacement, nullptr, 0);
            replaced_ = true;
        }
    }

    ~ScopedPixelShader() noexcept
    {
        if (classCount_ <= pixelClassInstanceCapacity)
            context_->PSSetShader(previous_.Get(), classes_.data(), classCount_);
        const auto capturedCount = classCount_ < pixelClassInstanceCapacity
            ? classCount_ : pixelClassInstanceCapacity;
        for (UINT index = 0; index < capturedCount; ++index)
        {
            if (classes_[index] != nullptr) classes_[index]->Release();
        }
    }

    [[nodiscard]] bool Replaced() const noexcept { return replaced_; }
    ScopedPixelShader(const ScopedPixelShader&) = delete;
    ScopedPixelShader& operator=(const ScopedPixelShader&) = delete;

private:
    ID3D11DeviceContext* context_;
    ComPtr<ID3D11PixelShader> previous_;
    std::array<ID3D11ClassInstance*, pixelClassInstanceCapacity> classes_{};
    UINT classCount_{pixelClassInstanceCapacity};
    bool replaced_{};
};

class ScopedVideoConstants
{
public:
    ScopedVideoConstants(ID3D11DeviceContext1* context, ID3D11Buffer* replacement) noexcept
        : context_(context), replacement_(replacement)
    {
        if (replacement_ == nullptr) return;
        // The base getter owns the reference; Context1 supplies only the exact window.
        context_->PSGetConstantBuffers(0, 1, &previous_);
        context_->PSGetConstantBuffers1(0, 1, nullptr, &firstConstant_, &constantCount_);
    }

    ~ScopedVideoConstants() noexcept
    {
        if (!bound_) return;
        auto* previous = previous_.Get();
        context_->PSSetConstantBuffers1(0, 1, &previous, &firstConstant_, &constantCount_);
    }

    [[nodiscard]] bool Ready() const noexcept
    {
        if (replacement_ == nullptr) return true;
        // Bound empty windows are legal, but cannot be distinguished here from
        // an unqualified legacy default, so this path leaves that draw unchanged.
        if (previous_ && constantCount_ == 0) return false;
        if (firstConstant_ % 16 != 0 || constantCount_ % 16 != 0) return false;
        return constantCount_ <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
    }

    void Bind() noexcept
    {
        if (replacement_ == nullptr) return;
        context_->PSSetConstantBuffers(0, 1, &replacement_);
        bound_ = true;
    }

    ScopedVideoConstants(const ScopedVideoConstants&) = delete;
    ScopedVideoConstants& operator=(const ScopedVideoConstants&) = delete;

private:
    ID3D11DeviceContext1* context_;
    ID3D11Buffer* replacement_;
    ComPtr<ID3D11Buffer> previous_;
    UINT firstConstant_{};
    UINT constantCount_{};
    bool bound_{};
};

class DrawScope
{
public:
    explicit DrawScope(bool& invoking) noexcept : invoking_(invoking) { invoking_ = true; }
    ~DrawScope() noexcept { invoking_ = false; }
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;

private:
    bool& invoking_;
};

}  // namespace

bool LayerWhiteConfiguration::Valid() const noexcept
{
    return ValidWhite(uiWhiteNits) && ValidWhite(videoWhiteNits);
}

ScenePassRenderer::~ScenePassRenderer() noexcept
{
    // Destruction on another thread cannot safely clear the exclusive context.
    if (!Retire()) std::terminate();
}

bool ScenePassRenderer::Initialize(ID3D11Device* device,
    ID3D11DeviceContext* immediateContext, UINT width, UINT height) noexcept
{
    return Initialize(device, immediateContext, ScenePassConfiguration{width, height, {}});
}

bool ScenePassRenderer::Initialize(ID3D11Device* device,
    ID3D11DeviceContext* immediateContext, ScenePassConfiguration configuration) noexcept
{
    if (lifetime_.CurrentEpoch() != 0) return false;
    if (!ValidInitializationArguments(device, immediateContext, configuration))
        return false;
    if (!CompatibleDevice(device, immediateContext)) return false;

    ComPtr<ID3D11DeviceContext1> preparedContext;
    if (!PreparePixelContext(device, immediateContext, preparedContext)) return false;
    ComPtr<ID3D11Buffer> preparedVideoConstants;
    if (!PrepareVideoConstants(device, configuration.whites, preparedVideoConstants)) return false;

    std::array<ComPtr<ID3D11PixelShader>, replacementProgramCount> prepared;
    if (!PreparePrograms(device, prepared)) return false;
    if (!lifetime_.Prepare(2, 4)) return false;
    ownerThread_ = std::this_thread::get_id();
    device_ = device;
    context_ = std::move(preparedContext);
    videoConstants_ = std::move(preparedVideoConstants);
    whites_ = configuration.whites;
    width_ = configuration.width;
    height_ = configuration.height;
    replacements_ = std::move(prepared);
    lifetime_.PublishInitial();
    return true;
}

bool ScenePassRenderer::OnOwnerThread() const noexcept
{
    return ownerThread_ == std::this_thread::get_id();
}

bool ScenePassRenderer::Active() const noexcept
{
    return OnOwnerThread() && lifetime_.CurrentEpoch() == 3;
}

bool ScenePassRenderer::MatchesContext(ID3D11Device* device,
    ID3D11DeviceContext* immediateContext) const noexcept
{
    if (!Active() || invoking_) return false;
    return SameIdentity(device, device_.Get()) && SameIdentity(immediateContext, context_.Get());
}

bool ScenePassRenderer::MatchesLayerWhites(LayerWhiteConfiguration whites) const noexcept
{
    if (!Active() || invoking_) return false;
    return whites.uiWhiteNits == whites_.uiWhiteNits && whites.videoWhiteNits == whites_.videoWhiteNits;
}

bool ScenePassRenderer::Retire() noexcept
{
    if (context_ && !OnOwnerThread()) return false;
    if (invoking_) return false;
    ClearLineage();
    frameOpen_ = false;
    lifetime_.Retire();
    // ClearState is permitted only because this context is exclusively caller-owned.
    if (context_) context_->ClearState();
    for (auto& scene : scenes_) scene = {};
    for (auto& shader : observed_) shader = {};
    for (auto& shader : replacements_) shader.Reset();
    videoConstants_.Reset();
    context_.Reset();
    device_.Reset();
    sceneCount_ = 0;
    observedCount_ = 0;
    return true;
}

HRESULT ScenePassRenderer::CreateObservedPixelShader(std::span<const std::byte> bytecode,
    ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) noexcept
{
    if (!Active() || invoking_) return E_UNEXPECTED;
    const auto result = device_->CreatePixelShader(
        bytecode.data(), bytecode.size(), linkage, shader);
    // Validation-only S_FALSE/null outputs are not created shader associations.
    if (FAILED(result)) return result;
    if (shader == nullptr) return result;
    if (*shader == nullptr) return result;
    const auto* descriptor = FindRenoDxFallout4Shader(ComputeRenoDxShaderCrc32(bytecode));
    if (descriptor == nullptr || observedCount_ == observed_.size()) return result;
    observed_[observedCount_++] = {*shader, descriptor};
    return result;
}

HRESULT ScenePassRenderer::CreateSceneTexture(const D3D11_TEXTURE2D_DESC& original,
    const D3D11_SUBRESOURCE_DATA* initialData, SceneTextureHandle& handle) noexcept
{
    handle = {};
    if (!Active() || invoking_) return E_UNEXPECTED;
    if (initialData != nullptr) return E_INVALIDARG;
    if (!PromotableScene(original)) return E_INVALIDARG;
    if (sceneCount_ == scenes_.size()) return E_OUTOFMEMORY;
    SceneTexture prepared;
    const auto result = PrepareSceneTexture(original, prepared);
    if (FAILED(result)) return result;
    const auto slot = sceneCount_++;
    scenes_[slot] = std::move(prepared);
    handle.owner_ = this;
    handle.epoch_ = lifetime_.CurrentEpoch();
    handle.slot_ = slot;
    return S_OK;
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

HRESULT ScenePassRenderer::PrepareSceneTexture(const D3D11_TEXTURE2D_DESC& original,
    SceneTexture& prepared) noexcept
{
    auto promoted = original;
    promoted.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto result = device_->CreateTexture2D(&promoted, nullptr, &prepared.texture);
    if (FAILED(result)) return result;
    if (!prepared.texture) return E_FAIL;
    return PrepareSceneViews(device_.Get(), prepared.texture.Get(),
        prepared.shaderResource, prepared.renderTarget);
}

const ScenePassRenderer::SceneTexture* ScenePassRenderer::Resolve(
    SceneTextureHandle handle) const noexcept
{
    if (!Active() || handle.owner_ != this) return nullptr;
    if (handle.epoch_ != lifetime_.CurrentEpoch() || handle.slot_ >= sceneCount_) return nullptr;
    return &scenes_[handle.slot_];
}

SceneTextureViews ScenePassRenderer::Views(SceneTextureHandle handle) const noexcept
{
    const auto* scene = Resolve(handle);
    if (scene == nullptr) return {};
    return {scene->texture.Get(), scene->shaderResource.Get(), scene->renderTarget.Get()};
}

void ScenePassRenderer::ClearLineage() noexcept
{
    selectedScene_ = {};
    for (auto& scene : scenes_) scene.toneProduced = false;
}

bool ScenePassRenderer::BeginFrame() noexcept
{
    if (!Active() || invoking_) return false;
    ClearLineage();
    frameOpen_ = false;
    if (frameGeneration_ == std::numeric_limits<std::uint64_t>::max()) return false;
    ++frameGeneration_;
    frameOpen_ = true;
    return true;
}

bool ScenePassRenderer::EndFrame() noexcept
{
    if (!Active() || invoking_) return false;
    ClearLineage();
    frameOpen_ = false;
    return true;
}

SceneTextureHandle ScenePassRenderer::CurrentScene() const noexcept
{
    if (invoking_) return {};
    if (!frameOpen_) return {};
    if (Resolve(selectedScene_) == nullptr) return {};
    return selectedScene_;
}

SceneColorDomain ScenePassRenderer::CurrentSceneDomain() const noexcept
{
    const auto* scene = Resolve(CurrentScene());
    return scene != nullptr ? scene->domain : SceneColorDomain::bt709Scene;
}

std::uint64_t ScenePassRenderer::FrameGeneration() const noexcept
{
    if (!Active() || invoking_) return 0;
    return frameOpen_ ? frameGeneration_ : 0;
}

void ScenePassRenderer::InvalidateSlot(std::size_t slot) noexcept
{
    scenes_[slot].toneProduced = false;
    scenes_[slot].domain = SceneColorDomain::bt709Scene;
    if (selectedScene_.owner_ == this && selectedScene_.slot_ == slot) selectedScene_ = {};
}

bool ScenePassRenderer::InvalidateScene(SceneTextureHandle handle) noexcept
{
    if (!Active() || invoking_) return false;
    if (Resolve(handle) == nullptr) return false;
    InvalidateSlot(handle.slot_);
    return true;
}

std::uint8_t ScenePassRenderer::WriteMaskForResource(ID3D11Resource* resource) const noexcept
{
    constexpr std::uint8_t allScenes = 0xff;
    const auto identity = CanonicalIdentity(resource);
    if (!identity) return allScenes;
    std::uint8_t writes{};
    for (std::size_t index = 0; index < sceneCount_; ++index)
    {
        const auto owned = CanonicalIdentity(scenes_[index].texture.Get());
        if (!owned) return allScenes;
        if (identity.Get() == owned.Get()) writes |= static_cast<std::uint8_t>(1u << index);
    }
    return writes;
}

std::uint8_t ScenePassRenderer::BoundWriteMask() const noexcept
{
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> raw{};
    context_->OMGetRenderTargets(static_cast<UINT>(raw.size()), raw.data(), nullptr);
    std::uint8_t writes{};
    for (auto* view : raw)
    {
        if (view == nullptr) continue;
        ComPtr<ID3D11RenderTargetView> owned;
        owned.Attach(view);
        writes |= WriteMaskForResource(ViewResource(owned.Get()).Get());
    }
    return writes;
}

void ScenePassRenderer::InvalidateWrites(std::uint8_t writes) noexcept
{
    for (std::size_t index = 0; index < sceneCount_; ++index)
    {
        if ((writes & (1u << index)) != 0) InvalidateSlot(index);
    }
}

ScenePassRenderer::DrawLineage ScenePassRenderer::CaptureLineage(
    SceneTextureHandle input) const noexcept
{
    if (!frameOpen_) return {};
    const auto* source = Resolve(input);
    const auto* selected = Resolve(selectedScene_);
    return {source != nullptr && source == selected,
        source != nullptr && source->toneProduced, selected != nullptr, BoundWriteMask(),
        source != nullptr ? source->domain : SceneColorDomain::bt709Scene};
}

void ScenePassRenderer::PublishLineage(const RenoDxShaderDescriptor& descriptor,
    SceneTextureHandle destination, const DrawLineage& lineage) noexcept
{
    if (descriptor.group == PipelinePassGroup::toneMapFamily)
    {
        scenes_[destination.slot_].toneProduced = true;
        scenes_[destination.slot_].domain = SceneColorDomain::bt709Scene;
        return;
    }
    if (descriptor.pass == RenoDxShaderPass::video)
    {
        scenes_[destination.slot_].domain = SceneColorDomain::bt2020UiRelative;
        selectedScene_ = destination;
        return;
    }
    if (lineage.consumesSelected)
    {
        scenes_[destination.slot_].domain = lineage.domain;
        selectedScene_ = destination;
        return;
    }
    if (lineage.selectionExisted) return;
    if (descriptor.pass == RenoDxShaderPass::lut && lineage.toneInput)
    {
        scenes_[destination.slot_].domain = SceneColorDomain::bt709Scene;
        selectedScene_ = destination;
    }
}

void ScenePassRenderer::CompleteLineage(const RenoDxShaderDescriptor* descriptor,
    SceneTextureHandle destination, SceneDrawOutcome outcome,
    const DrawLineage& lineage) noexcept
{
    if (!frameOpen_) return;
    InvalidateWrites(lineage.writes);
    if (outcome == SceneDrawOutcome::callbackFailed)
    {
        selectedScene_ = {};
        return;
    }
    if (outcome != SceneDrawOutcome::replaced)
    {
        if (lineage.consumesSelected) selectedScene_ = {};
        return;
    }
    PublishLineage(*descriptor, destination, lineage);
}

const RenoDxShaderDescriptor* ScenePassRenderer::BoundDescriptor() const noexcept
{
    ComPtr<ID3D11PixelShader> actual;
    context_->PSGetShader(&actual, nullptr, nullptr);
    for (std::size_t index = 0; index < observedCount_; ++index)
    {
        if (observed_[index].original.Get() == actual.Get())
            return observed_[index].descriptor;
    }
    return nullptr;
}

ID3D11PixelShader* ScenePassRenderer::QualifiedReplacement(
    const RenoDxShaderDescriptor* descriptor, SceneTextureHandle input,
    SceneTextureHandle destination) const noexcept
{
    const auto program = CanonicalProgramIndex(descriptor);
    if (program == replacements_.size()) return nullptr;
    const auto* source = Resolve(input);
    const auto* target = Resolve(destination);
    if (source == nullptr) return nullptr;
    if (target == nullptr) return nullptr;
    if (source == target) return nullptr;
    if (!SupportedInputDomain(descriptor->pass, source->domain)) return nullptr;
    if (!BoundSceneViewsMatch(*descriptor, *source, *target)) return nullptr;
    return replacements_[program].Get();
}

bool ScenePassRenderer::BoundSceneViewsMatch(const RenoDxShaderDescriptor& descriptor,
    const SceneTexture& source, const SceneTexture& target) const noexcept
{
    if (TemporalPass(descriptor.pass)) return BoundTemporalViewsMatch(descriptor, source, target);
    ComPtr<ID3D11ShaderResourceView> boundInput;
    ComPtr<ID3D11RenderTargetView> boundTarget;
    context_->PSGetShaderResources(descriptor.sceneInputSlot, 1, &boundInput);
    context_->OMGetRenderTargets(1, &boundTarget, nullptr);
    return boundInput.Get() == source.shaderResource.Get() &&
        boundTarget.Get() == target.renderTarget.Get();
}

bool ScenePassRenderer::BoundTemporalViewsMatch(const RenoDxShaderDescriptor& descriptor,
    const SceneTexture& source, const SceneTexture& target) const noexcept
{
    std::array<ID3D11RenderTargetView*, 2> raw{};
    context_->OMGetRenderTargets(static_cast<UINT>(raw.size()), raw.data(), nullptr);
    ComPtr<ID3D11RenderTargetView> metadata;
    ComPtr<ID3D11RenderTargetView> color;
    metadata.Attach(raw[0]);
    color.Attach(raw[1]);
    if (color.Get() != target.renderTarget.Get()) return false;
    if (!CompatibleMetadataTarget(metadata.Get(), target.texture.Get())) return false;
    const auto metadataResource = ViewResource(metadata.Get());
    const auto inputs = TemporalInputs(context_.Get(), descriptor.pass == RenoDxShaderPass::taa ? 4 : 5);
    if (inputs[0].Get() != source.shaderResource.Get()) return false;
    if (!CompatibleHistoryView(inputs[1].Get(), source.texture.Get())) return false;
    return InputsAvoidTemporalOutputs(inputs, source.texture.Get(),
        metadataResource.Get(), target.texture.Get());
}

bool ScenePassRenderer::CanInvoke(FiniteDrawInvocation invocation) const noexcept
{
    if (!Active()) return false;
    if (invoking_) return false;
    return invocation.draw != nullptr;
}

SceneDrawOutcome ScenePassRenderer::Invoke(const RenoDxShaderDescriptor* descriptor,
    SceneTextureHandle input, SceneTextureHandle destination,
    FiniteDrawInvocation invocation) noexcept
{
    DrawScope drawScope{invoking_};
    auto* replacement = QualifiedReplacement(descriptor, input, destination);
    const auto lineage = CaptureLineage(input);
    auto* videoConstants = replacement != nullptr && descriptor->pass == RenoDxShaderPass::video
        ? videoConstants_.Get() : nullptr;
    const auto outcome = InvokeDraw(replacement, invocation, videoConstants);
    // Publish only after the borrowed pixel shader and class tuple are restored.
    CompleteLineage(descriptor, destination, outcome, lineage);
    return outcome;
}

SceneDrawOutcome ScenePassRenderer::InvokeDraw(ID3D11PixelShader* replacement,
    FiniteDrawInvocation invocation, ID3D11Buffer* videoConstants) noexcept
{
    ScopedVideoConstants constantsScope{context_.Get(), videoConstants};
    ScopedPixelShader pixelScope{context_.Get(), constantsScope.Ready() ? replacement : nullptr};
    if (pixelScope.Replaced()) constantsScope.Bind();
    try
    {
        invocation.draw(context_.Get(), invocation.argument);
    }
    catch (...)
    {
        return SceneDrawOutcome::callbackFailed;
    }
    return pixelScope.Replaced() ? SceneDrawOutcome::replaced : SceneDrawOutcome::forwarded;
}

SceneDrawOutcome ScenePassRenderer::Execute(const RenoDxShaderDescriptor& descriptor,
    SceneTextureHandle input, SceneTextureHandle destination,
    FiniteDrawInvocation invocation) noexcept
{
    if (!CanInvoke(invocation)) return SceneDrawOutcome::invalidInvocation;
    return Invoke(&descriptor, input, destination, invocation);
}

SceneDrawOutcome ScenePassRenderer::ExecuteObserved(SceneTextureHandle input,
    SceneTextureHandle destination, FiniteDrawInvocation invocation) noexcept
{
    if (!CanInvoke(invocation)) return SceneDrawOutcome::invalidInvocation;
    return Invoke(BoundDescriptor(), input, destination, invocation);
}

}  // namespace fo4hdr
