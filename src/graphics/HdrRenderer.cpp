// SPDX-License-Identifier: GPL-3.0-or-later

#include "HdrRenderer.hpp"
#include "RenoDxShaderIdentity.hpp"
#include "resources/HdrResourcePolicy.hpp"

#include <fo4hdr-shaders/ToneMapA.hpp>
#include <fo4hdr-shaders/ToneMapB.hpp>
#include <fo4hdr-shaders/ToneMapNewA.hpp>
#include <fo4hdr-shaders/ToneMapNewB.hpp>
#include <fo4hdr-shaders/CreativeGradeLut.hpp>

#include <exception>
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
    {kCreativeGradeLutBytecode, sizeof(kCreativeGradeLutBytecode)}
}};

std::size_t ProgramIndex(RenoDxShaderPass pass) noexcept
{
    switch (pass)
    {
    case RenoDxShaderPass::toneMapA: return 0;
    case RenoDxShaderPass::toneMapB: return 1;
    case RenoDxShaderPass::toneMapNewA: return 2;
    case RenoDxShaderPass::toneMapNewB: return 3;
    case RenoDxShaderPass::lut: return 4;
    default: return ScenePassRenderer::replacementProgramCount;
    }
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
    ID3D11DeviceContext* context, UINT width, UINT height) noexcept
{
    if (device == nullptr || context == nullptr) return false;
    return width != 0 && height != 0;
}

bool CompatibleDevice(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return false;
    return AssociatedImmediateContext(device, context) && Fp16SceneSupported(device);
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

ScenePassRenderer::~ScenePassRenderer() noexcept
{
    // Destruction on another thread cannot safely clear the exclusive context.
    if (!Retire()) std::terminate();
}

bool ScenePassRenderer::Initialize(ID3D11Device* device,
    ID3D11DeviceContext* immediateContext, UINT width, UINT height) noexcept
{
    if (lifetime_.CurrentEpoch() != 0) return false;
    if (!ValidInitializationArguments(device, immediateContext, width, height)) return false;
    if (!CompatibleDevice(device, immediateContext)) return false;

    std::array<ComPtr<ID3D11PixelShader>, replacementProgramCount> prepared;
    if (!PreparePrograms(device, prepared)) return false;
    if (!lifetime_.Prepare(2, 4)) return false;
    ownerThread_ = std::this_thread::get_id();
    device_ = device;
    context_ = immediateContext;
    width_ = width;
    height_ = height;
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

bool ScenePassRenderer::Retire() noexcept
{
    if (context_ && !OnOwnerThread()) return false;
    if (invoking_) return false;
    lifetime_.Retire();
    // ClearState is permitted only because this context is exclusively caller-owned.
    if (context_) context_->ClearState();
    for (auto& scene : scenes_) scene = {};
    for (auto& shader : observed_) shader = {};
    for (auto& shader : replacements_) shader.Reset();
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
    if (!BoundSceneViewsMatch(*descriptor, *source, *target)) return nullptr;
    return replacements_[program].Get();
}

bool ScenePassRenderer::BoundSceneViewsMatch(const RenoDxShaderDescriptor& descriptor,
    const SceneTexture& source, const SceneTexture& target) const noexcept
{
    ComPtr<ID3D11ShaderResourceView> boundInput;
    ComPtr<ID3D11RenderTargetView> boundTarget;
    context_->PSGetShaderResources(descriptor.sceneInputSlot, 1, &boundInput);
    context_->OMGetRenderTargets(1, &boundTarget, nullptr);
    return boundInput.Get() == source.shaderResource.Get() &&
        boundTarget.Get() == target.renderTarget.Get();
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
    ScopedPixelShader pixelScope{context_.Get(), replacement};
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
