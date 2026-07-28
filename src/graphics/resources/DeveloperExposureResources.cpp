// SPDX-License-Identifier: GPL-3.0-or-later
#include "DeveloperExposureResources.hpp"

#include <array>
#include <cstdint>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;

struct ExposureSelector
{
    UINT enabled{};
    std::array<UINT, 3> reserved{};
};
static_assert(sizeof(ExposureSelector) == 16);

bool CreateSelector(ID3D11Device* device, UINT enabled, ID3D11Buffer** buffer) noexcept
{
    const ExposureSelector selector{enabled};
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(selector);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial{&selector};
    return SUCCEEDED(device->CreateBuffer(&description, &initial, buffer));
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

template <class T, std::size_t Count>
bool AnyBoundAndRelease(const std::array<T*, Count>& objects) noexcept
{
    bool bound{};
    for (auto* object : objects)
    {
        if (object == nullptr) continue;
        bound = true;
        object->Release();
    }
    return bound;
}

bool ComputeShaderIdle(ID3D11DeviceContext* context) noexcept
{
    ComPtr<ID3D11ComputeShader> shader;
    std::array<ID3D11ClassInstance*, 256> classes{};
    UINT count = static_cast<UINT>(classes.size());
    context->CSGetShader(&shader, classes.data(), &count);
    const bool boundClasses = AnyBoundAndRelease(classes);
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue{};
    context->GetPredication(&predicate, &predicateValue);
    return !shader && count == 0 && !boundClasses && !predicate;
}

bool UnorderedViewsIdle(ID3D11DeviceContext* context) noexcept
{
    constexpr UINT count = D3D11_PS_CS_UAV_REGISTER_COUNT;
    std::array<ID3D11UnorderedAccessView*, count> compute{};
    std::array<ID3D11UnorderedAccessView*, count> output{};
    context->CSGetUnorderedAccessViews(0, count, compute.data());
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
        0, count, output.data());
    const bool computeBound = AnyBoundAndRelease(compute);
    const bool outputBound = AnyBoundAndRelease(output);
    return !computeBound && !outputBound;
}

bool ExposureViewDescriptionQualified(const D3D11_SHADER_RESOURCE_VIEW_DESC& description) noexcept
{
    return description.Format == DXGI_FORMAT_R32_FLOAT &&
        description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
        description.Texture2D.MostDetailedMip == 0 && description.Texture2D.MipLevels == 1;
}

bool ExposureTextureQualified(ID3D11Texture2D* texture) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return description.Format == DXGI_FORMAT_R32_FLOAT &&
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
    UINT previousComputeFirst{};
    UINT previousComputeCount{};
};

bool ComputeInputsIdle(ID3D11DeviceContext1* context, ExposureInputs& inputs) noexcept
{
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11Buffer> constants;
    context->CSGetShaderResources(0, 1, &source);
    context->CSGetConstantBuffers(2, 1, &constants);
    if (source || constants) return false;
    context->CSGetConstantBuffers1(2, 1, nullptr,
        &inputs.previousComputeFirst, &inputs.previousComputeCount);
    return inputs.previousComputeFirst % 16 == 0 &&
        inputs.previousComputeCount % 16 == 0 && inputs.previousComputeCount <= 4096;
}

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

void DispatchCapture(ID3D11DeviceContext1* context, const ExposureInputs& inputs,
    ID3D11ComputeShader* shader, ID3D11UnorderedAccessView* target) noexcept
{
    auto* source = inputs.source.Get();
    auto* constants = inputs.constants.Get();
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetShaderResources(0, 1, &source);
    context->CSSetConstantBuffers1(2, 1, &constants, &inputs.first, &inputs.count);
    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    context->Dispatch(1, 1, 1);
    ID3D11UnorderedAccessView* noTarget{};
    ID3D11ShaderResourceView* noSource{};
    ID3D11Buffer* noConstants{};
    context->CSSetUnorderedAccessViews(0, 1, &noTarget, nullptr);
    context->CSSetShaderResources(0, 1, &noSource);
    context->CSSetConstantBuffers1(2, 1, &noConstants,
        &inputs.previousComputeFirst, &inputs.previousComputeCount);
    context->CSSetShader(nullptr, nullptr, 0);
}

struct CaptureBundle
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11UnorderedAccessView> target;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> enabled;
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
    return CreateSelector(device, 1, &bundle.enabled);
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

bool CaptureComputeIdle(ID3D11DeviceContext1* context, ExposureInputs& inputs) noexcept
{
    return ComputeShaderIdle(context) && ComputeInputsIdle(context, inputs) &&
        UnorderedViewsIdle(context);
}

}  // namespace

bool DeveloperExposureResources::Prepare(ID3D11Device* device,
    std::span<const std::byte> captureBytecode) noexcept
{
    Reset();
    if (device == nullptr) return false;
    if (!CreateSelector(device, 0, &selectors_[0])) return false;
    if (!CapturePreparationSupported(device, captureBytecode)) return true;
    CaptureBundle bundle;
    if (!CreateCaptureBundle(device, captureBytecode, bundle)) return true;
    device_ = device;
    texture_ = std::move(bundle.texture);
    source_ = std::move(bundle.source);
    target_ = std::move(bundle.target);
    shader_ = std::move(bundle.shader);
    selectors_[1] = std::move(bundle.enabled);
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
    if (!CaptureComputeIdle(context1.Get(), inputs)) return false;
    if (!ReadExposureInputs(device_.Get(), context1.Get(), inputs)) return false;
    DispatchCapture(context1.Get(), inputs, shader_.Get(), target_.Get());
    return SUCCEEDED(device_->GetDeviceRemovedReason());
}

}  // namespace fo4hdr
