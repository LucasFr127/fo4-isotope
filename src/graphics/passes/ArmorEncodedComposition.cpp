// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArmorEncodedComposition.hpp"

#include <fo4hdr-shaders/ArmorEncodedComposition.hpp>

#include <array>
#include <cmath>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT outputCapacity = 8;
constexpr UINT classCapacity = 256;
constexpr UINT inputCapacity = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr UINT constantSlot = 13;
constexpr std::array sourceGetters{
    &ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::HSGetShaderResources,
    &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::GSGetShaderResources,
    &ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::CSGetShaderResources};
constexpr std::array sourceSetters{
    &ID3D11DeviceContext::VSSetShaderResources, &ID3D11DeviceContext::HSSetShaderResources,
    &ID3D11DeviceContext::DSSetShaderResources, &ID3D11DeviceContext::GSSetShaderResources,
    &ID3D11DeviceContext::PSSetShaderResources, &ID3D11DeviceContext::CSSetShaderResources};

struct EncodedConstants
{
    UINT encode;
    float peakUiRelative;
    std::array<UINT, 2> reserved{};
};
static_assert(sizeof(EncodedConstants) == 16);

template<class Interface, std::size_t count>
void ReleaseReferences(const std::array<Interface*, count>& references) noexcept
{
    for (auto* reference : references)
    {
        if (reference != nullptr) reference->Release();
    }
}

bool EmptyUavs(ID3D11DeviceContext* context) noexcept
{
    std::array<ID3D11UnorderedAccessView*, outputCapacity> compute{}, output{};
    context->CSGetUnorderedAccessViews(0, outputCapacity, compute.data());
    context->OMGetRenderTargetsAndUnorderedAccessViews(
        0, nullptr, nullptr, 0, outputCapacity, output.data());
    bool empty = true;
    for (UINT index = 0; index < outputCapacity; ++index)
    {
        empty &= compute[index] == nullptr && output[index] == nullptr;
    }
    ReleaseReferences(compute);
    ReleaseReferences(output);
    return empty;
}

class ScopedEncodedState
{
public:
    explicit ScopedEncodedState(ID3D11DeviceContext1* context) noexcept : context_(context)
    {
        CaptureSources();
        context_->OMGetRenderTargets(outputCapacity, renderTargets_.data(), &depth_);
        CaptureCompute();
        context_->GetPredication(&predicate_, &predicateValue_);
    }

    ~ScopedEncodedState() noexcept
    {
        if (mutated_) Restore();
        for (const auto& stage : sources_) ReleaseReferences(stage);
        ReleaseReferences(renderTargets_);
        ReleaseReferences(classes_);
    }

    [[nodiscard]] bool Ready() const noexcept
    {
        if (classCount_ > classCapacity) return false;
        if (constants_ && count_ == 0) return false;
        if (first_ % 16 != 0 || count_ % 16 != 0) return false;
        return count_ <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
    }

    void Bind(ID3D11ComputeShader* shader, ID3D11Buffer* constants,
        ID3D11UnorderedAccessView* target) noexcept
    {
        mutated_ = true;
        // Binding a UAV can evict aliases from every stage, including slot 127.
        const std::array<ID3D11ShaderResourceView*, inputCapacity> empty{};
        for (const auto setter : sourceSetters)
        {
            (context_->*setter)(0, inputCapacity, empty.data());
        }
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->SetPredication(nullptr, FALSE);
        context_->CSSetShader(shader, nullptr, 0);
        context_->CSSetConstantBuffers(constantSlot, 1, &constants);
        context_->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    }

    ScopedEncodedState(const ScopedEncodedState&) = delete;
    ScopedEncodedState& operator=(const ScopedEncodedState&) = delete;

private:
    void CaptureCompute() noexcept
    {
        context_->CSGetShader(&shader_, classes_.data(), &classCount_);
        context_->CSGetConstantBuffers(constantSlot, 1, &constants_);
        context_->CSGetConstantBuffers1(constantSlot, 1, nullptr, &first_, &count_);
    }

    void CaptureSources() noexcept
    {
        for (std::size_t stage = 0; stage < sources_.size(); ++stage)
        {
            (context_->*sourceGetters[stage])(0, inputCapacity, sources_[stage].data());
        }
    }

    void RestoreSources() noexcept
    {
        for (std::size_t stage = 0; stage < sources_.size(); ++stage)
        {
            (context_->*sourceSetters[stage])(0, inputCapacity, sources_[stage].data());
        }
    }

    void RestoreCompute() noexcept
    {
        auto* constants = constants_.Get();
        context_->CSSetConstantBuffers1(constantSlot, 1, &constants, &first_, &count_);
        context_->CSSetShader(shader_.Get(), classes_.data(), classCount_);
    }

    void Restore() noexcept
    {
        ID3D11UnorderedAccessView* empty{};
        context_->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
        context_->OMSetRenderTargets(outputCapacity, renderTargets_.data(), depth_.Get());
        RestoreSources();
        RestoreCompute();
        context_->SetPredication(predicate_.Get(), predicateValue_);
    }

    ID3D11DeviceContext1* context_;
    std::array<std::array<ID3D11ShaderResourceView*, inputCapacity>, 6> sources_{};
    std::array<ID3D11RenderTargetView*, outputCapacity> renderTargets_{};
    std::array<ID3D11ClassInstance*, classCapacity> classes_{};
    ComPtr<ID3D11DepthStencilView> depth_;
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11Predicate> predicate_;
    UINT classCount_{classCapacity};
    UINT first_{};
    UINT count_{};
    BOOL predicateValue_{};
    bool mutated_{};
};

ComPtr<IUnknown> Identity(IUnknown* object) noexcept
{
    ComPtr<IUnknown> identity;
    if (object != nullptr && FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))))
        identity.Reset();
    return identity;
}

bool SameObject(IUnknown* left, IUnknown* right) noexcept
{
    const auto first = Identity(left);
    const auto second = Identity(right);
    return first && second && first.Get() == second.Get();
}

bool SupportedDevice(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (device->GetFeatureLevel() != D3D_FEATURE_LEVEL_11_0) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Device> owner;
    context->GetDevice(&owner);
    if (!SameObject(device, owner.Get())) return false;
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,
            &options, sizeof(options)))) return false;
    return options.ConstantBufferOffsetting;
}

bool TypedReadWrite(ID3D11Device* device) noexcept
{
    UINT support{};
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R16G16B16A16_FLOAT, &support)))
        return false;
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if ((support & required) != required) return false;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 details{DXGI_FORMAT_R16G16B16A16_FLOAT, 0};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2,
            &details, sizeof(details)))) return false;
    constexpr UINT operations = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
        D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    return (details.OutFormatSupport2 & operations) == operations;
}

bool ViewLayouts(const ArmorEncodedConfiguration& configuration) noexcept
{
    if (!configuration.target || !configuration.renderTarget) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC target{};
    D3D11_RENDER_TARGET_VIEW_DESC renderTarget{};
    configuration.target->GetDesc(&target);
    configuration.renderTarget->GetDesc(&renderTarget);
    if (target.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
    if (target.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D) return false;
    if (target.Texture2D.MipSlice != 0) return false;
    return renderTarget.Format == target.Format &&
        renderTarget.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D &&
        renderTarget.Texture2D.MipSlice == 0;
}

bool SingleImage(const D3D11_TEXTURE2D_DESC& value) noexcept
{
    if (value.Width == 0 || value.Height == 0) return false;
    if (value.MipLevels != 1 || value.ArraySize != 1) return false;
    return value.SampleDesc.Count == 1 && value.SampleDesc.Quality == 0;
}

bool TextureLayout(const D3D11_TEXTURE2D_DESC& value) noexcept
{
    if (value.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
    if (!SingleImage(value)) return false;
    if (value.Usage != D3D11_USAGE_DEFAULT || value.CPUAccessFlags != 0) return false;
    constexpr UINT required = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    return (value.BindFlags & required) == required;
}

bool QualifiedTexture(ID3D11Device* device, const ArmorEncodedConfiguration& configuration,
    ComPtr<ID3D11Texture2D>& texture) noexcept
{
    if (!ViewLayouts(configuration)) return false;
    ComPtr<ID3D11Resource> output, native;
    configuration.target->GetResource(&output);
    configuration.renderTarget->GetResource(&native);
    if (!SameObject(output.Get(), native.Get())) return false;
    if (FAILED(output.As(&texture)) || !texture) return false;
    ComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (!SameObject(device, owner.Get())) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return TextureLayout(description);
}

bool PrepareConstants(ID3D11Device* device, float peakUiRelative,
    std::array<ComPtr<ID3D11Buffer>, 2>& buffers) noexcept
{
    const std::array<EncodedConstants, 2> values{{{1, 0}, {0, peakUiRelative}}};
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(EncodedConstants);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (std::size_t index = 0; index < buffers.size(); ++index)
    {
        const D3D11_SUBRESOURCE_DATA data{&values[index], 0, 0};
        if (FAILED(device->CreateBuffer(&description, &data, &buffers[index]))) return false;
        if (!buffers[index]) return false;
    }
    return true;
}

bool ValidConfiguration(const ArmorEncodedConfiguration& configuration) noexcept
{
    if (configuration.resourceGeneration == 0) return false;
    if (configuration.domain != ArmorCompositionDomain::linearBt2020UiRelative) return false;
    return std::isfinite(configuration.peakUiRelative) && configuration.peakUiRelative > 0;
}

struct PreparedEncoded
{
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Texture2D> texture;
    std::array<ComPtr<ID3D11Buffer>, 2> constants;
};

bool ValidInitialization(ID3D11Device* device, ID3D11DeviceContext* context,
    const ArmorEncodedConfiguration& configuration) noexcept
{
    if (device == nullptr || context == nullptr) return false;
    if (!ValidConfiguration(configuration)) return false;
    return SupportedDevice(device, context) && TypedReadWrite(device);
}

bool PrepareEncoded(ID3D11Device* device, ID3D11DeviceContext* context,
    const ArmorEncodedConfiguration& configuration, PreparedEncoded& prepared) noexcept
{
    if (!QualifiedTexture(device, configuration, prepared.texture)) return false;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&prepared.context)))) return false;
    if (!prepared.context) return false;
    if (FAILED(device->CreateComputeShader(kArmorEncodedCompositionBytecode,
            sizeof(kArmorEncodedCompositionBytecode), nullptr, &prepared.shader))) return false;
    if (!prepared.shader) return false;
    return PrepareConstants(device, configuration.peakUiRelative, prepared.constants);
}

}  // namespace

bool ArmorEncodedComposition::Initialize(ID3D11Device* device,
    ID3D11DeviceContext* context, ArmorEncodedConfiguration configuration) noexcept
{
    if (context_) return false;
    if (!ValidInitialization(device, context, configuration)) return false;
    PreparedEncoded prepared;
    if (!PrepareEncoded(device, context, configuration, prepared)) return false;
    D3D11_TEXTURE2D_DESC description{};
    prepared.texture->GetDesc(&description);
    width_ = description.Width;
    height_ = description.Height;
    ownerThread_ = std::this_thread::get_id();
    device_ = device;
    context_ = std::move(prepared.context);
    shader_ = std::move(prepared.shader);
    texture_ = std::move(prepared.texture);
    renderTarget_ = configuration.renderTarget;
    target_ = configuration.target;
    constants_ = std::move(prepared.constants);
    configuration_ = configuration;
    domain_ = configuration.domain;
    return true;
}

bool ArmorEncodedComposition::OnOwnerThread() const noexcept
{
    return ownerThread_ == std::this_thread::get_id();
}

bool ArmorEncodedComposition::Qualified(const ArmorEncodedToken& token) const noexcept
{
    if (token.scope != ArmorCompositionScope::qualifiedExclusive) return false;
    if (token.owner != ArmorCompositionOwner::powerArmor || !token.postAa) return false;
    if (token.ownerThread != ownerThread_) return false;
    if (token.resourceGeneration != configuration_.resourceGeneration) return false;
    return token.frameGeneration != 0 && token.serial != 0;
}

bool ArmorEncodedComposition::Dispatch(std::size_t direction) noexcept
{
    if (device_->GetDeviceRemovedReason() != S_OK) return false;
    // Hidden append/counter state cannot be reconstructed from a Get call.
    if (!EmptyUavs(context_.Get())) return false;
    {
        ScopedEncodedState state{context_.Get()};
        if (!state.Ready()) return false;
        state.Bind(shader_.Get(), constants_[direction].Get(), target_.Get());
        context_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
    }
    return device_->GetDeviceRemovedReason() == S_OK;
}

bool ArmorEncodedComposition::Begin(const ArmorEncodedToken& token) noexcept
{
    if (!OnOwnerThread() || !context_) return false;
    if (domain_ == ArmorCompositionDomain::encodedBt709)
    {
        Invalidate();
        return false;
    }
    if (domain_ != ArmorCompositionDomain::linearBt2020UiRelative) return false;
    if (!Qualified(token)) return false;
    if (!Dispatch(0))
    {
        Invalidate();
        return false;
    }
    activeToken_ = token;
    domain_ = ArmorCompositionDomain::encodedBt709;
    return true;
}

bool ArmorEncodedComposition::End(const ArmorEncodedToken& token) noexcept
{
    if (!OnOwnerThread() || !context_) return false;
    if (domain_ != ArmorCompositionDomain::encodedBt709) return false;
    if (!CanDecode(token) || !Dispatch(1))
    {
        Invalidate();
        return false;
    }
    activeToken_ = {};
    // Publish linear only after decode submission and exact binding restoration.
    domain_ = ArmorCompositionDomain::linearBt2020UiRelative;
    return true;
}

bool ArmorEncodedComposition::CanDecode(const ArmorEncodedToken& token) const noexcept
{
    return Qualified(token) && token == activeToken_;
}

void ArmorEncodedComposition::Invalidate() noexcept
{
    if (!OnOwnerThread()) return;
    activeToken_ = {};
    domain_ = ArmorCompositionDomain::unknown;
}

bool ArmorEncodedComposition::Retire() noexcept
{
    if (context_ && !OnOwnerThread()) return false;
    activeToken_ = {};
    domain_ = ArmorCompositionDomain::unknown;
    constants_ = {};
    target_.Reset();
    renderTarget_.Reset();
    texture_.Reset();
    shader_.Reset();
    context_.Reset();
    device_.Reset();
    configuration_ = {};
    width_ = height_ = 0;
    return true;
}

ArmorCompositionDomain ArmorEncodedComposition::Domain() const noexcept
{
    if (!OnOwnerThread()) return ArmorCompositionDomain::unknown;
    return domain_;
}

}  // namespace fo4hdr
