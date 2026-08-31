// SPDX-License-Identifier: GPL-3.0-or-later
#include "DeveloperExposureResources.hpp"
#include "../d3d11/ScopedD3D11ComputeConstantBuffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;

struct ExposureSelector
{
    UINT enabled{};
    float sceneExposure{1.0F};
    float bloom{1.0F};
    UINT reserved{};
};
static_assert(sizeof(ExposureSelector) == 16);
static_assert(offsetof(ExposureSelector, sceneExposure) == 4);
static_assert(offsetof(ExposureSelector, bloom) == 8);

bool CreateSelector(ID3D11Device* device, const ExposureSelector& selector, ID3D11Buffer** buffer) noexcept
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(selector);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial{&selector};
    return SUCCEEDED(device->CreateBuffer(&description, &initial, buffer));
}

bool ControlsValid(float sceneExposure, float bloom) noexcept
{
    const std::array<std::array<float, 3>, 2> ranges{{
        {sceneExposure, 0.0625F, 16.0F}, {bloom, 0.0F, 2.0F}}};
    return std::all_of(ranges.begin(), ranges.end(), [](const auto& range) {
        return std::isfinite(range[0]) && range[0] >= range[1] && range[0] <= range[2];
    });
}

bool CaptureFormatSupported(ID3D11Device* device) noexcept
{
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    UINT support{};
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R32G32_FLOAT, &support)) ||
        (support & required) != required) return false;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 details{DXGI_FORMAT_R32G32_FLOAT};
    return SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2,
               &details, sizeof(details))) &&
        (details.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

bool CreateSnapshot(ID3D11Device* device, ID3D11Texture2D** texture) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 1;
    description.Height = 1;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R32G32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    const std::array<float, 2> invalidPair{};
    const D3D11_SUBRESOURCE_DATA initial{invalidPair.data(), sizeof(invalidPair)};
    return SUCCEEDED(device->CreateTexture2D(&description, &initial, texture));
}

bool NativeExposureFormat(DXGI_FORMAT format) noexcept
{
    constexpr std::array formats{DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R16_FLOAT,
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
    return std::find(formats.begin(), formats.end(), format) != formats.end();
}
bool ExposureViewDescriptionQualified(const D3D11_SHADER_RESOURCE_VIEW_DESC& description) noexcept
{
    return NativeExposureFormat(description.Format) &&
        description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
        description.Texture2D.MostDetailedMip == 0 && description.Texture2D.MipLevels == 1;
}

bool ExposureTextureQualified(ID3D11Texture2D* texture) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return NativeExposureFormat(description.Format) &&
        description.Width == 1 && description.Height == 1 && description.MipLevels == 1 &&
        description.ArraySize == 1 && description.SampleDesc.Count == 1;
}

bool ExposureViewQualified(ID3D11Device* device, ID3D11ShaderResourceView* view) noexcept
{
    if (view == nullptr) return false;
    ComPtr<ID3D11Device> viewDevice;
    view->GetDevice(&viewDevice);
    if (viewDevice.Get() != device) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    view->GetDesc(&viewDescription);
    if (!ExposureViewDescriptionQualified(viewDescription)) return false;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) return false;
    return ExposureTextureQualified(texture.Get());
}

bool ExposureSamplerQualified(ID3D11Device* device, ID3D11SamplerState* sampler) noexcept
{
    if (sampler == nullptr) return false;
    ComPtr<ID3D11Device> samplerDevice;
    sampler->GetDevice(&samplerDevice);
    if (samplerDevice.Get() != device) return false;
    D3D11_SAMPLER_DESC description{};
    sampler->GetDesc(&description);
    const bool ordinary = description.Filter == D3D11_FILTER_MIN_MAG_MIP_POINT ||
        description.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    return ordinary && description.AddressU == D3D11_TEXTURE_ADDRESS_CLAMP &&
        description.AddressV == D3D11_TEXTURE_ADDRESS_CLAMP &&
        description.AddressW == D3D11_TEXTURE_ADDRESS_CLAMP;
}

struct ExposureInputs
{
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants;
    UINT first{};
    UINT count{};
};

bool ExposureRangeQualified(const ExposureInputs& inputs) noexcept
{
    return inputs.count != 0 && inputs.count <= 4096 &&
        inputs.first % 16 == 0 && inputs.count % 16 == 0;
}

bool ExposureWindowQualified(ID3D11Device* device, const ExposureInputs& inputs) noexcept
{
    if (!inputs.constants) return false;
    if (!ExposureRangeQualified(inputs)) return false;
    ComPtr<ID3D11Device> bufferDevice;
    inputs.constants->GetDevice(&bufferDevice);
    if (bufferDevice.Get() != device) return false;
    D3D11_BUFFER_DESC description{};
    inputs.constants->GetDesc(&description);
    // Only the two capture float4s must lie in storage. A reported legacy
    // 4096-constant window does not imply a 64 KiB allocation.
    const auto firstByte = static_cast<std::uint64_t>(inputs.first) * 16;
    return (description.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0 &&
        firstByte + 32 <= description.ByteWidth;
}

bool ReadExposureInputs(ID3D11Device* device, ID3D11DeviceContext1* context,
    ExposureInputs& inputs) noexcept
{
    context->PSGetShaderResources(2, 1, &inputs.source);
    context->PSGetSamplers(2, 1, &inputs.sampler);
    // The base getter owns the reference; the range getter supplies only bounds.
    context->PSGetConstantBuffers(2, 1, &inputs.constants);
    context->PSGetConstantBuffers1(2, 1, nullptr, &inputs.first, &inputs.count);
    return ExposureViewQualified(device, inputs.source.Get()) &&
        ExposureSamplerQualified(device, inputs.sampler.Get()) &&
        ExposureWindowQualified(device, inputs);
}

class ScopedExposureCaptureState
{
public:
    explicit ScopedExposureCaptureState(ID3D11DeviceContext1* context) noexcept
        : context_(context), constants_(context, 2)
    {
        std::array<ID3D11ClassInstance*, 256> instances{};
        context_->CSGetShader(&shader_, instances.data(), &instanceCount_);
        for (UINT i = 0; i < std::min(instanceCount_, UINT(instances.size())); ++i)
            instances_[i].Attach(instances[i]);
        context_->CSGetShaderResources(0, 1, &source_);
        context_->CSGetUnorderedAccessViews(0, 1, &target_);
        constants_.Capture();
    }
    ~ScopedExposureCaptureState() noexcept { Restore(); }
    [[nodiscard]] bool Ready() const noexcept
    {
        return instanceCount_ <= instances_.size() && constants_.Ready();
    }
    void Bind(const ExposureInputs& inputs, ID3D11ComputeShader* shader,
        ID3D11UnorderedAccessView* target) noexcept
    {
        bound_ = true;
        auto* source = inputs.source.Get();
        constants_.BindRange(inputs.constants.Get(), inputs.first, inputs.count);
        context_->CSSetShader(shader, nullptr, 0);
        context_->CSSetShaderResources(0, 1, &source);
        constexpr UINT preserveCounter = UINT(-1);
        context_->CSSetUnorderedAccessViews(0, 1, &target, &preserveCounter);
    }
    void Restore() noexcept
    {
        if (!bound_) return;
        ID3D11ShaderResourceView* noSource{};
        ID3D11UnorderedAccessView* noTarget{};
        context_->CSSetShaderResources(0, 1, &noSource);
        constexpr UINT preserveCounter = UINT(-1);
        context_->CSSetUnorderedAccessViews(0, 1, &noTarget, &preserveCounter);
        auto* source = source_.Get();
        auto* target = target_.Get();
        context_->CSSetShaderResources(0, 1, &source);
        context_->CSSetUnorderedAccessViews(0, 1, &target, &preserveCounter);
        constants_.Restore();
        std::array<ID3D11ClassInstance*, 256> instances{};
        for (UINT i = 0; i < instanceCount_; ++i) instances[i] = instances_[i].Get();
        context_->CSSetShader(shader_.Get(), instanceCount_ ? instances.data() : nullptr, instanceCount_);
        bound_ = false;
    }
    ScopedExposureCaptureState(const ScopedExposureCaptureState&) = delete;
    ScopedExposureCaptureState& operator=(const ScopedExposureCaptureState&) = delete;
private:
    ID3D11DeviceContext1* context_;
    ComPtr<ID3D11ComputeShader> shader_;
    std::array<ComPtr<ID3D11ClassInstance>, 256> instances_{};
    UINT instanceCount_{256};
    ComPtr<ID3D11ShaderResourceView> source_;
    ComPtr<ID3D11UnorderedAccessView> target_;
    ScopedD3D11ComputeConstantBuffer constants_;
    bool bound_{};
};

bool DispatchCapture(ID3D11DeviceContext1* context, const ExposureInputs& inputs,
    ID3D11ComputeShader* shader, ID3D11UnorderedAccessView* target) noexcept
{
    ScopedExposureCaptureState previous(context);
    if (!previous.Ready()) return false;
    // The actual nonnull PS exposure source is disjoint from writable views.
    // Caller excludes snapshot aliases; only the owned UAV is bound here.
    previous.Bind(inputs, shader, target);
    context->Dispatch(1, 1, 1);
    previous.Restore();
    return true;
}
struct CaptureBundle
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11UnorderedAccessView> target;
    ComPtr<ID3D11ComputeShader> shader;
};

bool CreateCaptureBundle(ID3D11Device* device, std::span<const std::byte> bytecode,
    CaptureBundle& bundle) noexcept
{
    if (!CreateSnapshot(device, &bundle.texture)) return false;
    if (FAILED(device->CreateShaderResourceView(bundle.texture.Get(), nullptr, &bundle.source)))
        return false;
    if (FAILED(device->CreateUnorderedAccessView(bundle.texture.Get(), nullptr, &bundle.target)))
        return false;
    if (FAILED(device->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &bundle.shader)))
        return false;
    return true;
}

bool CapturePreparationSupported(ID3D11Device* device, std::span<const std::byte> bytecode) noexcept
{
    if (bytecode.empty()) return false;
    if (device->GetFeatureLevel() != D3D_FEATURE_LEVEL_11_0) return false;
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,
            &options, sizeof(options)))) return false;
    return options.ConstantBufferOffsetting && CaptureFormatSupported(device);
}

bool CaptureContextQualified(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (context == nullptr) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    return contextDevice.Get() == device;
}

bool CapturePredicateClear(ID3D11DeviceContext* context) noexcept
{
    ComPtr<ID3D11Predicate> predicate;
    BOOL value{};
    context->GetPredication(&predicate, &value);
    return !predicate;
}
}  // namespace

bool DeveloperExposureResources::Prepare(ID3D11Device* device,
    std::span<const std::byte> captureBytecode, float sceneExposure, float bloom) noexcept
{
    Reset();
    if (!SetControls(device, sceneExposure, bloom)) return false;
    if (!CapturePreparationSupported(device, captureBytecode)) return true;
    CaptureBundle bundle;
    if (!CreateCaptureBundle(device, captureBytecode, bundle)) return true;
    device_ = device;
    texture_ = std::move(bundle.texture);
    source_ = std::move(bundle.source);
    target_ = std::move(bundle.target);
    shader_ = std::move(bundle.shader);
    return true;
}

bool DeveloperExposureResources::SetControls(ID3D11Device* device, float sceneExposure, float bloom) noexcept
{
    if (device == nullptr) return false;
    if (device_ && device_.Get() != device) return false;
    if (!ControlsValid(sceneExposure, bloom)) return false;
    std::array<ComPtr<ID3D11Buffer>, 2> prepared;
    for (UINT enabled = 0; enabled < prepared.size(); ++enabled)
    {
        const ExposureSelector values{enabled, sceneExposure, bloom};
        if (!CreateSelector(device, values, &prepared[enabled])) return false;
    }
    selectors_ = std::move(prepared);
    return true;
}

void DeveloperExposureResources::Reset() noexcept
{
    shader_.Reset();
    target_.Reset();
    source_.Reset();
    texture_.Reset();
    selectors_ = {};
    device_.Reset();
}

bool DeveloperExposureResources::Ready() const noexcept
{
    return device_ && texture_ && source_ && target_ && shader_ && selectors_[1];
}

ID3D11Buffer* DeveloperExposureResources::Selector(bool enabled) const noexcept
{
    return selectors_[enabled && Ready() ? 1 : 0].Get();
}

ID3D11ShaderResourceView* DeveloperExposureResources::Snapshot() const noexcept
{
    return source_.Get();
}

bool DeveloperExposureResources::CanReplay(ID3D11DeviceContext* context) const noexcept
{
    if (!Ready()) return false;
    if (!CaptureContextQualified(device_.Get(), context)) return false;
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) return false;
    ExposureInputs inputs;
    return ReadExposureInputs(device_.Get(), context1.Get(), inputs);
}

bool DeveloperExposureResources::TryCapture(ID3D11DeviceContext* context) noexcept
{
    if (!Ready()) return false;
    if (!CaptureContextQualified(device_.Get(), context)) return false;
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) return false;
    ExposureInputs inputs;
    if (!CapturePredicateClear(context1.Get())) return false;
    if (!ReadExposureInputs(device_.Get(), context1.Get(), inputs)) return false;
    if (!DispatchCapture(context1.Get(), inputs, shader_.Get(), target_.Get())) return false;
    return SUCCEEDED(device_->GetDeviceRemovedReason());
}

ScopedDeveloperExposureBindings::ScopedDeveloperExposureBindings(
    ID3D11DeviceContext1* context, bool tone) noexcept : context_(context), tone_(tone)
{
    if (!tone_) return;
    context_->PSGetConstantBuffers(13, 1, &previousConstants_);
    context_->PSGetConstantBuffers1(13, 1, nullptr, &firstConstant_, &constantCount_);
    context_->PSGetShaderResources(17, 1, &previousSource_);
}

ScopedDeveloperExposureBindings::~ScopedDeveloperExposureBindings() noexcept
{
    if (!bound_) return;
    auto* source = previousSource_.Get();
    auto* constants = previousConstants_.Get();
    context_->PSSetShaderResources(17, 1, &source);
    context_->PSSetConstantBuffers1(13, 1, &constants, &firstConstant_, &constantCount_);
}

bool ScopedDeveloperExposureBindings::Ready() const noexcept
{
    if (!tone_) return true;
    // Bound empty windows cannot qualify an unspecified legacy default.
    if (previousConstants_ && constantCount_ == 0) return false;
    if (firstConstant_ % 16 != 0 || constantCount_ % 16 != 0) return false;
    return constantCount_ <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
}

void ScopedDeveloperExposureBindings::Bind(ID3D11Buffer* selector,
    ID3D11ShaderResourceView* snapshot) noexcept
{
    if (!tone_) return;
    context_->PSSetConstantBuffers(13, 1, &selector);
    context_->PSSetShaderResources(17, 1, &snapshot);
    bound_ = true;
}

}  // namespace fo4hdr
