// SPDX-License-Identifier: GPL-3.0-or-later

#include "PresentationPass.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"

#include <fo4hdr-shaders/PresentPixels.hpp>
#include <fo4hdr-shaders/PrepareBackdrop.hpp>
#include <fo4hdr-shaders/PrepareHud.hpp>
#include <fo4hdr-shaders/CopySdrCodes.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT outputCapacity = 8;
constexpr UINT classCapacity = 256;
constexpr std::array supportedSceneDomains{
    SceneColorDomain::bt709Scene, SceneColorDomain::bt2020UiRelative};
constexpr std::array<UINT, outputCapacity> preserveCounters{
    UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1)};

template<class Interface, std::size_t count>
void ReleaseReferences(const std::array<Interface*, count>& references, UINT captured) noexcept
{
    for (UINT index = 0; index < captured; ++index)
    {
        if (references[index] != nullptr) references[index]->Release();
    }
}

struct PresentConstants
{
    UINT width;
    UINT height;
    float peak;
    UINT sceneDomain;
    float uiWhite;
    std::array<float, 3> reserved{};
};
static_assert(sizeof(PresentConstants) == 32);

ComPtr<IUnknown> Identity(IUnknown* object) noexcept
{
    ComPtr<IUnknown> identity;
    if (object == nullptr) return identity;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) identity.Reset();
    return identity;
}

bool SameObject(IUnknown* left, IUnknown* right) noexcept
{
    const auto first = Identity(left);
    const auto second = Identity(right);
    return first && second && first.Get() == second.Get();
}

ComPtr<ID3D11Resource> Resource(ID3D11View* view) noexcept
{
    ComPtr<ID3D11Resource> resource;
    if (view != nullptr) view->GetResource(&resource);
    return resource;
}

bool DropDecision(ID3D11View* view,
    IUnknown* owned, bool& drop) noexcept
{
    drop = false;
    if (view == nullptr) return true;
    const auto identity = Identity(Resource(view).Get());
    if (!identity) return false;
    drop = identity.Get() == owned;
    return true;
}

template<class View>
bool DropOwned(View*& view, IUnknown* owned) noexcept
{
    bool drop{};
    if (!DropDecision(view, owned, drop)) return false;
    if (drop)
    {
        view->Release();
        view = nullptr;
    }
    return true;
}

class OutputBindings
{
public:
    explicit OutputBindings(ID3D11DeviceContext* context) noexcept
    {
        context->OMGetRenderTargets(outputCapacity, renderTargets_.data(), &depth_);
        context->OMGetRenderTargetsAndUnorderedAccessViews(
            0, nullptr, nullptr, 0, outputCapacity, omTargets_.data());
        context->CSGetUnorderedAccessViews(0, outputCapacity, csTargets_.data());
    }

    ~OutputBindings() noexcept
    {
        for (UINT index = 0; index < outputCapacity; ++index)
        {
            if (renderTargets_[index] != nullptr) renderTargets_[index]->Release();
            if (omTargets_[index] != nullptr) omTargets_[index]->Release();
            if (csTargets_[index] != nullptr) csTargets_[index]->Release();
        }
    }

    [[nodiscard]] UINT RenderTargetCount() const noexcept
    {
        UINT count{};
        for (UINT index = 0; index < outputCapacity; ++index)
        {
            if (renderTargets_[index] != nullptr) count = index + 1;
        }
        return count;
    }

    [[nodiscard]] bool Valid() const noexcept
    {
        const auto count = RenderTargetCount();
        for (UINT index = 0; index < count; ++index)
        {
            if (omTargets_[index] != nullptr) return false;
        }
        return true;
    }

    void Unbind(ID3D11DeviceContext* context) const noexcept
    {
        std::array<ID3D11UnorderedAccessView*, outputCapacity> empty{};
        context->OMSetRenderTargetsAndUnorderedAccessViews(
            0, nullptr, nullptr, 0, outputCapacity, empty.data(), preserveCounters.data());
        context->CSSetUnorderedAccessViews(0, outputCapacity, empty.data(), preserveCounters.data());
    }

    void Restore(ID3D11DeviceContext* context) const noexcept
    {
        const auto count = RenderTargetCount();
        if (count == outputCapacity)
            context->OMSetRenderTargets(outputCapacity, renderTargets_.data(), depth_.Get());
        else
            context->OMSetRenderTargetsAndUnorderedAccessViews(count, renderTargets_.data(),
                depth_.Get(), count, outputCapacity - count,
                omTargets_.data() + count, preserveCounters.data() + count);
        context->CSSetUnorderedAccessViews(
            0, outputCapacity, csTargets_.data(), preserveCounters.data());
    }

    [[nodiscard]] bool Exclude(ID3D11Texture2D* texture) noexcept
    {
        const auto owned = Identity(texture);
        if (!owned) return false;
        for (UINT index = 0; index < outputCapacity; ++index)
        {
            if (!DropOwned(renderTargets_[index], owned.Get())) return false;
            if (!DropOwned(omTargets_[index], owned.Get())) return false;
            if (!DropOwned(csTargets_[index], owned.Get())) return false;
        }
        return true;
    }

    OutputBindings(const OutputBindings&) = delete;
    OutputBindings& operator=(const OutputBindings&) = delete;

private:
    std::array<ID3D11RenderTargetView*, outputCapacity> renderTargets_{};
    std::array<ID3D11UnorderedAccessView*, outputCapacity> omTargets_{};
    std::array<ID3D11UnorderedAccessView*, outputCapacity> csTargets_{};
    ComPtr<ID3D11DepthStencilView> depth_;
};

class ScopedComputeState
{
public:
    explicit ScopedComputeState(ID3D11DeviceContext1* context) noexcept
        : context_(context), outputs_(context)
    {
        context_->CSGetShader(&shader_, classes_.data(), &classCount_);
        context_->CSGetConstantBuffers1(0, 1, &constants_, &firstConstant_, &constantCount_);
        context_->CSGetShaderResources(0, static_cast<UINT>(inputs_.size()), inputs_.data());
        context_->GetPredication(&predicate_, &predicateValue_);
    }

    ~ScopedComputeState() noexcept
    {
        if (mutated_) Restore();
        const auto captured = classCount_ < classCapacity ? classCount_ : classCapacity;
        ReleaseReferences(classes_, captured);
        ReleaseReferences(inputs_, static_cast<UINT>(inputs_.size()));
    }

    [[nodiscard]] bool Ready() const noexcept
    {
        if (classCount_ > classCapacity) return false;
        if (firstConstant_ % 16 != 0 || constantCount_ % 16 != 0) return false;
        if (constantCount_ > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT) return false;
        return outputs_.Valid();
    }

    void PrepareOutputs() noexcept
    {
        mutated_ = true;
        context_->SetPredication(nullptr, FALSE);
        outputs_.Unbind(context_);
    }

    void Bind(ID3D11ComputeShader* shader, ID3D11Buffer* constants,
        const std::array<ID3D11ShaderResourceView*, 3>& inputs,
        ID3D11UnorderedAccessView* target) noexcept
    {
        PrepareOutputs();
        context_->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
        context_->CSSetUnorderedAccessViews(0, 1, &target, preserveCounters.data());
        context_->CSSetConstantBuffers(0, 1, &constants);
        context_->CSSetShader(shader, nullptr, 0);
    }

    ScopedComputeState(const ScopedComputeState&) = delete;
    ScopedComputeState& operator=(const ScopedComputeState&) = delete;

private:
    void Restore() noexcept
    {
        std::array<ID3D11ShaderResourceView*, 3> emptyInputs{};
        ID3D11UnorderedAccessView* emptyTarget{};
        context_->CSSetShaderResources(0, static_cast<UINT>(emptyInputs.size()), emptyInputs.data());
        context_->CSSetUnorderedAccessViews(0, 1, &emptyTarget, preserveCounters.data());
        outputs_.Restore(context_);
        auto* constants = constants_.Get();
        context_->CSSetShaderResources(0, static_cast<UINT>(inputs_.size()), inputs_.data());
        context_->CSSetConstantBuffers1(0, 1, &constants, &firstConstant_, &constantCount_);
        context_->CSSetShader(shader_.Get(), classes_.data(), classCount_);
        context_->SetPredication(predicate_.Get(), predicateValue_);
    }

    ID3D11DeviceContext1* context_;
    OutputBindings outputs_;
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> constants_;
    std::array<ID3D11ShaderResourceView*, 3> inputs_{};
    ComPtr<ID3D11Predicate> predicate_;
    std::array<ID3D11ClassInstance*, classCapacity> classes_{};
    UINT classCount_{classCapacity};
    UINT firstConstant_{};
    UINT constantCount_{};
    BOOL predicateValue_{};
    bool mutated_{};
};

class ExecutionScope
{
public:
    explicit ExecutionScope(bool& executing) noexcept : executing_(executing) { executing_ = true; }
    ~ExecutionScope() noexcept { executing_ = false; }
    ExecutionScope(const ExecutionScope&) = delete;
    ExecutionScope& operator=(const ExecutionScope&) = delete;

private:
    bool& executing_;
};

bool ValidConfiguration(const PresentationConfiguration& value) noexcept
{
    if (value.scenes == nullptr) return false;
    if (value.width == 0 || value.height == 0) return false;
    if (value.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return false;
    if (value.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return false;
    return std::isfinite(value.displayPeakNits) && value.displayPeakNits > 0.0F &&
        value.displayPeakNits <= 10000.0F;
}

bool TypedStoreSupported(ID3D11Device* device, DXGI_FORMAT format, UINT required) noexcept
{
    UINT support{};
    if (FAILED(device->CheckFormatSupport(format, &support))) return false;
    if ((support & required) != required) return false;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 details{format, 0};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2,
            &details, sizeof(details)))) return false;
    return (details.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

bool PresentationDevice(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (device->GetFeatureLevel() != D3D_FEATURE_LEVEL_11_0) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,
            &options, sizeof(options)))) return false;
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    return options.ConstantBufferOffsetting &&
        TypedStoreSupported(device, DXGI_FORMAT_R10G10B10A2_UNORM, required);
}

bool LayerFormatsSupported(ID3D11Device* device) noexcept
{
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW | D3D11_FORMAT_SUPPORT_SHADER_LOAD;
    if (!TypedStoreSupported(device, DXGI_FORMAT_R16G16B16A16_FLOAT, required)) return false;
    UINT support{};
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM, &support))) return false;
    constexpr UINT codedRequired = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD;
    return (support & codedRequired) == codedRequired;
}

HRESULT CreateConstants(ID3D11Device* device, const PresentConstants& values,
    ID3D11Buffer** output) noexcept
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(values);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA data{&values, 0, 0};
    return device->CreateBuffer(&description, &data, output);
}

struct PreparedPresentation
{
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11ComputeShader> sdrShader;
    std::array<ComPtr<ID3D11Buffer>, 2> constants;
    std::array<ComPtr<ID3D11ComputeShader>, 2> layerShaders;
};

bool ValidInitialization(ID3D11Device* device, ID3D11DeviceContext* context,
    const PresentationConfiguration& configuration) noexcept
{
    if (device == nullptr || context == nullptr) return false;
    if (!ValidConfiguration(configuration)) return false;
    if (!configuration.whites.Valid()) return false;
    if (!configuration.scenes->MatchesLayerWhites(configuration.whites)) return false;
    return configuration.scenes->MatchesContext(device, context);
}

bool PrepareContext(ID3D11Device* device, ID3D11DeviceContext* context,
    ComPtr<ID3D11DeviceContext1>& prepared) noexcept
{
    if (!PresentationDevice(device, context)) return false;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&prepared)))) return false;
    return prepared != nullptr;
}

bool PrepareConstantVariants(ID3D11Device* device,
    const PresentationConfiguration& configuration, PreparedPresentation& prepared) noexcept
{
    for (const auto domain : supportedSceneDomains)
    {
        const auto index = static_cast<std::size_t>(domain);
        if (index >= prepared.constants.size()) return false;
        const PresentConstants values{
            configuration.width, configuration.height, configuration.displayPeakNits,
            static_cast<UINT>(domain), configuration.whites.uiWhiteNits};
        if (FAILED(CreateConstants(device, values, &prepared.constants[index]))) return false;
        if (!prepared.constants[index]) return false;
    }
    return true;
}

bool PrepareLayerPrograms(ID3D11Device* device, PreparedPresentation& prepared) noexcept
{
    if (FAILED(device->CreateComputeShader(kPrepareBackdropBytecode,
            sizeof(kPrepareBackdropBytecode), nullptr, &prepared.layerShaders[0]))) return false;
    if (FAILED(device->CreateComputeShader(kPrepareHudBytecode,
            sizeof(kPrepareHudBytecode), nullptr, &prepared.layerShaders[1]))) return false;
    if (FAILED(device->CreateComputeShader(kCopySdrCodesBytecode,
            sizeof(kCopySdrCodesBytecode), nullptr, &prepared.sdrShader))) return false;
    return prepared.layerShaders[0] && prepared.layerShaders[1] && prepared.sdrShader;
}

bool PreparePresentation(ID3D11Device* device, ID3D11DeviceContext* context,
    const PresentationConfiguration& configuration, PreparedPresentation& prepared) noexcept
{
    if (!PrepareContext(device, context, prepared.context)) return false;
    if (!LayerFormatsSupported(device)) return false;
    if (FAILED(device->CreateComputeShader(kPresentPixelsBytecode,
            sizeof(kPresentPixelsBytecode), nullptr, &prepared.shader))) return false;
    if (!prepared.shader) return false;
    if (!PrepareConstantVariants(device, configuration, prepared)) return false;
    return PrepareLayerPrograms(device, prepared);
}

bool TerminalLayout(const D3D11_TEXTURE2D_DESC& value) noexcept
{
    if (value.MipLevels != 1 || value.ArraySize != 1) return false;
    return value.SampleDesc.Count == 1 && value.SampleDesc.Quality == 0;
}

bool TerminalDescription(const D3D11_TEXTURE2D_DESC& value,
    const PresentationConfiguration& configuration) noexcept
{
    if (value.Format != DXGI_FORMAT_R10G10B10A2_UNORM) return false;
    if (value.Width != configuration.width || value.Height != configuration.height) return false;
    if (!TerminalLayout(value)) return false;
    if ((value.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0) return false;
    return (value.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
}

bool TerminalViewLayout(const D3D11_UNORDERED_ACCESS_VIEW_DESC& view) noexcept
{
    if (view.Format != DXGI_FORMAT_R10G10B10A2_UNORM) return false;
    return view.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D && view.Texture2D.MipSlice == 0;
}

bool TerminalView(ID3D11UnorderedAccessView* target, ID3D11Device* device,
    const PresentationConfiguration& configuration, ComPtr<ID3D11Texture2D>& texture) noexcept
{
    if (target == nullptr) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    target->GetDesc(&view);
    if (!TerminalViewLayout(view)) return false;
    const auto resource = Resource(target);
    if (!resource) return false;
    if (FAILED(resource.As(&texture))) return false;
    if (!texture) return false;
    ComPtr<ID3D11Device> actualDevice;
    texture->GetDevice(&actualDevice);
    if (!SameObject(actualDevice.Get(), device)) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return TerminalDescription(description, configuration);
}

bool SelectedTexture(ID3D11Texture2D* scene,
    const PresentationConfiguration& configuration) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    scene->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
    return description.Width == configuration.width && description.Height == configuration.height;
}

bool CodedLayerDescription(const D3D11_TEXTURE2D_DESC& value,
    const PresentationConfiguration& configuration) noexcept
{
    if (value.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    if (value.Width != configuration.width || value.Height != configuration.height) return false;
    if (!TerminalLayout(value)) return false;
    if (value.Usage != D3D11_USAGE_DEFAULT) return false;
    if (value.CPUAccessFlags != 0 || value.MiscFlags != 0) return false;
    return (value.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
}

bool CodedLayerView(const D3D11_SHADER_RESOURCE_VIEW_DESC& view) noexcept
{
    if (view.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    return view.Texture2D.MostDetailedMip == 0 && view.Texture2D.MipLevels == 1;
}

bool CodedLayerSource(ID3D11ShaderResourceView* source, ID3D11Device* device,
    const PresentationConfiguration& configuration) noexcept
{
    if (source == nullptr) return true;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    source->GetDesc(&view);
    if (!CodedLayerView(view)) return false;
    const auto resource = Resource(source);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) return false;
    if (!texture) return false;
    ComPtr<ID3D11Device> actualDevice;
    texture->GetDevice(&actualDevice);
    if (!SameObject(actualDevice.Get(), device)) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return CodedLayerDescription(description, configuration);
}

bool FlipChainLayout(const DXGI_SWAP_CHAIN_DESC1& value) noexcept
{
    if (value.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) return false;
    if (value.BufferCount != 2 || value.Stereo) return false;
    return value.SampleDesc.Count == 1 && value.SampleDesc.Quality == 0;
}

bool ChainDescription(const DXGI_SWAP_CHAIN_DESC1& value,
    const PresentationConfiguration& configuration) noexcept
{
    if (!FlipChainLayout(value)) return false;
    if (value.Format != DXGI_FORMAT_R10G10B10A2_UNORM) return false;
    if (value.Width != configuration.width || value.Height != configuration.height) return false;
    if ((value.BufferUsage & DXGI_USAGE_SHADER_INPUT) != 0) return false;
    return (value.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) != 0;
}

bool DisplayFormatSupported(ID3D11Device* device) noexcept
{
    UINT support{};
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UNORM, &support))) return false;
    return (support & D3D11_FORMAT_SUPPORT_DISPLAY) != 0;
}

HRESULT ChainDeviceQualification(IDXGISwapChain3* chain, ID3D11Device* device) noexcept
{
    ComPtr<ID3D11Device> actual;
    const auto result = chain->GetDevice(IID_PPV_ARGS(&actual));
    if (FAILED(result)) return result;
    if (!SameObject(actual.Get(), device)) return E_INVALIDARG;
    return DisplayFormatSupported(device) ? S_OK : DXGI_ERROR_UNSUPPORTED;
}

HRESULT CheckedChain(IDXGISwapChain* chain, ID3D11Device* device,
    const PresentationConfiguration& configuration, ComPtr<IDXGISwapChain3>& candidate) noexcept
{
    if (chain == nullptr) return E_INVALIDARG;
    auto result = chain->QueryInterface(IID_PPV_ARGS(&candidate));
    if (FAILED(result)) return result;
    if (!candidate) return E_NOINTERFACE;
    DXGI_SWAP_CHAIN_DESC1 description{};
    result = candidate->GetDesc1(&description);
    if (FAILED(result)) return result;
    if (!ChainDescription(description, configuration)) return E_INVALIDARG;
    return ChainDeviceQualification(candidate.Get(), device);
}

HRESULT ApplyColorSpace(IDXGISwapChain3* chain, bool hdr) noexcept
{
    UINT support{};
    const auto colorSpace = hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const auto result = chain->CheckColorSpaceSupport(colorSpace, &support);
    if (FAILED(result)) return result;
    if ((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) return DXGI_ERROR_UNSUPPORTED;
    return chain->SetColorSpace1(colorSpace);
}

bool DeviceLost(HRESULT result) noexcept
{
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET;
}

bool ClearBlack(ID3D11DeviceContext1* context, ID3D11UnorderedAccessView* target) noexcept
{
    ScopedComputeState state{context};
    if (!state.Ready()) return false;
    state.PrepareOutputs();
    const float black[]{0.0F, 0.0F, 0.0F, 1.0F};
    context->ClearUnorderedAccessViewFloat(target, black);
    return true;
}

}  // namespace

PresentationPass::~PresentationPass() noexcept
{
    if (!Retire()) std::terminate();
}

bool PresentationPass::PrepareLayer(ID3D11Device* device,
    const PresentationConfiguration& configuration, LinearLayer& prepared) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = configuration.width;
    description.Height = configuration.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &prepared.texture))) return false;
    if (!prepared.texture) return false;
    if (FAILED(device->CreateShaderResourceView(prepared.texture.Get(), nullptr, &prepared.source)))
        return false;
    if (!prepared.source) return false;
    if (FAILED(device->CreateUnorderedAccessView(prepared.texture.Get(), nullptr, &prepared.target)))
        return false;
    return prepared.target != nullptr;
}

bool PresentationPass::Initialize(ID3D11Device* device,
    ID3D11DeviceContext* immediateContext, PresentationConfiguration configuration) noexcept
{
    if (lifetime_.CurrentEpoch() != 0) return false;
    if (!ValidInitialization(device, immediateContext, configuration)) return false;
    PreparedPresentation prepared;
    if (!PreparePresentation(device, immediateContext, configuration, prepared)) return false;
    std::array<LinearLayer, 2> preparedLayers;
    for (auto& layer : preparedLayers)
    {
        if (!PrepareLayer(device, configuration, layer)) return false;
    }
    LinearLayer preparedSdr;
    if (!PrepareLayer(device, configuration, preparedSdr)) return false;
    if (!lifetime_.Prepare(2, 4)) return false;
    ownerThread_ = std::this_thread::get_id();
    configuration_ = configuration;
    device_ = device;
    context_ = std::move(prepared.context);
    shader_ = std::move(prepared.shader);
    sdrShader_ = std::move(prepared.sdrShader);
    sdrProxy_ = std::move(preparedSdr);
    constants_ = std::move(prepared.constants);
    layerShaders_ = std::move(prepared.layerShaders);
    layers_ = std::move(preparedLayers);
    lifetime_.PublishInitial();
    return true;
}

bool PresentationPass::OnOwnerThread() const noexcept
{
    return ownerThread_ == std::this_thread::get_id();
}

bool PresentationPass::ApplySettings(const OperatorLutData& table,
    LayerWhiteConfiguration whites, PresentationScope scope,
    SceneGradeConfiguration sceneGrade) noexcept
{
    if (!CanApplySettings(scope)) return false;
    if (!table.IsValid() || !whites.Valid()) return false;
    if (!sceneGrade.Valid()) return false;
    auto configuration = configuration_;
    configuration.displayPeakNits = static_cast<float>(table.displayPeakNits);
    configuration.whites = whites;
    if (!ValidConfiguration(configuration)) return false;
    PreparedPresentation prepared;
    if (!PrepareConstantVariants(device_.Get(), configuration, prepared)) return false;
    if (!configuration_.scenes->ApplySettings(table, whites, sceneGrade)) return false;
    constants_ = std::move(prepared.constants);
    configuration_ = configuration;
    layerFrame_ = 0;
    layerFailureFrame_ = 0;
    return true;
}

bool PresentationPass::Active() const noexcept
{
    return OnOwnerThread() && lifetime_.CurrentEpoch() == 3;
}

bool PresentationPass::CanApplySettings(PresentationScope scope) const noexcept
{
    if (!CanExecute(scope)) return false;
    if (framePrepared_) return false;
    return configuration_.scenes->FrameGeneration() == 0;
}

bool PresentationPass::CanExecute(PresentationScope scope) const noexcept
{
    if (scope != PresentationScope::qualifiedExclusive) return false;
    if (!Active() || executing_) return false;
    return configuration_.scenes->MatchesContext(device_.Get(), context_.Get());
}

HRESULT PresentationPass::PrepareChain(IDXGISwapChain* chain) noexcept
{
    if (!CanPrepareChain()) return E_UNEXPECTED;
    ComPtr<IDXGISwapChain3> candidate;
    auto result = CheckedChain(chain, device_.Get(), configuration_, candidate);
    if (FAILED(result)) return result;
    ChainBuffer prepared;
    result = PrepareBuffer(candidate.Get(), prepared);
    if (FAILED(result)) return result;
    result = ApplyColorSpace(candidate.Get(), true);
    if (FAILED(result)) return result;
    buffer_ = std::move(prepared);
    chain_ = std::move(candidate);
    hdrColorSpace_ = true;
    return S_OK;
}

bool PresentationPass::CanPrepareChain() const noexcept
{
    if (!Active() || executing_) return false;
    if (chain_) return false;
    return configuration_.scenes->MatchesContext(device_.Get(), context_.Get());
}

HRESULT PresentationPass::PrepareBuffer(IDXGISwapChain3* chain,
    ChainBuffer& prepared) const noexcept
{
    // DXGI keeps the D3D11 buffer-zero view current across Present. Other indices
    // need not support writable views, even when their texture bind flags match.
    auto result = chain->GetBuffer(0, IID_PPV_ARGS(&prepared.texture));
    if (FAILED(result)) return result;
    if (!prepared.texture) return E_FAIL;
    result = device_->CreateUnorderedAccessView(prepared.texture.Get(), nullptr, &prepared.target);
    if (FAILED(result)) return result;
    if (!prepared.target) return E_FAIL;
    ComPtr<ID3D11Texture2D> checked;
    return TerminalView(prepared.target.Get(), device_.Get(), configuration_, checked)
        ? S_OK : E_INVALIDARG;
}

bool PresentationPass::SameChain(IDXGISwapChain* chain) const noexcept
{
    return chain_ && SameObject(chain, chain_.Get());
}

ID3D11UnorderedAccessView* PresentationPass::CurrentTarget() const noexcept
{
    return chain_ ? buffer_.target.Get() : nullptr;
}

bool PresentationPass::QualifiedTarget(ID3D11UnorderedAccessView* terminal,
    ID3D11Texture2D* scene) const noexcept
{
    ComPtr<ID3D11Texture2D> target;
    if (!TerminalView(terminal, device_.Get(), configuration_, target)) return false;
    if (scene == nullptr) return true;
    if (!SelectedTexture(scene, configuration_)) return false;
    const auto sourceIdentity = Identity(scene);
    const auto targetIdentity = Identity(target.Get());
    return sourceIdentity && targetIdentity && sourceIdentity.Get() != targetIdentity.Get();
}

bool PresentationPass::QualifiedLayerSource(ID3D11ShaderResourceView* source,
    ComPtr<IUnknown>& identity) const noexcept
{
    if (!CodedLayerSource(source, device_.Get(), configuration_)) return false;
    if (source == nullptr) return true;
    identity = Identity(Resource(source).Get());
    if (!identity) return false;
    for (const auto& layer : layers_)
    {
        const auto owned = Identity(layer.texture.Get());
        if (!owned || identity.Get() == owned.Get()) return false;
    }
    return true;
}

bool PresentationPass::QualifiedLayerSources(ID3D11ShaderResourceView* backdrop,
    ID3D11ShaderResourceView* hud) const noexcept
{
    std::array<ComPtr<IUnknown>, 2> identities;
    if (!QualifiedLayerSource(backdrop, identities[0])) return false;
    if (!QualifiedLayerSource(hud, identities[1])) return false;
    if (!identities[0] || !identities[1]) return true;
    return identities[0].Get() != identities[1].Get();
}

void PresentationPass::ClearLayers() noexcept
{
    const float transparent[4]{};
    for (const auto& layer : layers_)
        context_->ClearUnorderedAccessViewFloat(layer.target.Get(), transparent);
}

bool PresentationPass::PrepareLayers(ID3D11ShaderResourceView* backdrop,
    ID3D11ShaderResourceView* hud, PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return false;
    const auto frame = configuration_.scenes->FrameGeneration();
    if (frame == 0) return false;
    layerFrame_ = 0;
    layerFailureFrame_ = frame;
    if (!QualifiedLayerSources(backdrop, hud)) return false;
    ExecutionScope executing{executing_};
    {
        ScopedComputeState state{context_.Get()};
        if (!state.Ready()) return false;
        state.PrepareOutputs();
        const std::array sources{backdrop, hud};
        for (std::size_t index = 0; index < sources.size(); ++index)
        {
            if (sources[index] == nullptr)
            {
                const float transparent[4]{};
                context_->ClearUnorderedAccessViewFloat(layers_[index].target.Get(), transparent);
                continue;
            }
            const std::array<ID3D11ShaderResourceView*, 3> inputs{sources[index], nullptr, nullptr};
            state.Bind(layerShaders_[index].Get(), constants_[0].Get(), inputs,
                layers_[index].target.Get());
            context_->Dispatch((configuration_.width + 7) / 8, (configuration_.height + 7) / 8, 1);
        }
    }
    // Both outputs become usable only after borrowed bindings have been restored.
    layerFrame_ = frame;
    layerFailureFrame_ = 0;
    return true;
}

bool PresentationPass::CopySdrCodes(ID3D11ShaderResourceView* source,
    ID3D11UnorderedAccessView* target) noexcept
{
    ScopedComputeState state{context_.Get()};
    if (!state.Ready()) return false;
    const std::array<ID3D11ShaderResourceView*, 3> inputs{source, nullptr, nullptr};
    state.Bind(sdrShader_.Get(), constants_[0].Get(), inputs, target);
    context_->Dispatch((configuration_.width + 7) / 8, (configuration_.height + 7) / 8, 1);
    return true;
}

bool PresentationPass::PrepareSdrHandoff(ID3D11ShaderResourceView* completedAuthoredCodes,
    PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return false;
    sdrFrame_ = 0;
    const auto frame = configuration_.scenes->FrameGeneration();
    if (!framePrepared_ || frame == 0) return false;
    if (completedAuthoredCodes == nullptr) return false;
    if (!CodedLayerSource(completedAuthoredCodes, device_.Get(), configuration_)) return false;
    ExecutionScope executing{executing_};
    if (!CopySdrCodes(completedAuthoredCodes, sdrProxy_.target.Get())) return false;
    // The private code-domain image is published only after borrowed state is restored.
    sdrFrame_ = frame;
    return true;
}

bool PresentationPass::BeginFrame(PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return false;
    framePrepared_ = false;
    sdrFrame_ = 0;
    layerFrame_ = 0;
    layerFailureFrame_ = 0;
    if (!configuration_.scenes->EndFrame()) return false;
    auto* target = CurrentTarget();
    if (target == nullptr) return false;
    ExecutionScope executing{executing_};
    if (!ClearBlack(context_.Get(), target)) return false;
    if (!configuration_.scenes->BeginFrame()) return false;
    framePrepared_ = true;
    return true;
}

bool PresentationPass::ExecuteCurrentScene(ID3D11UnorderedAccessView* terminal,
    PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return false;
    const auto selected = configuration_.scenes->CurrentScene();
    const auto source = configuration_.scenes->Views(selected);
    if (!QualifiedTarget(terminal, source.texture)) return false;
    const auto frame = configuration_.scenes->FrameGeneration();
    if (frame != 0 && layerFailureFrame_ == frame) return false;
    ExecutionScope executing{executing_};
    if (source.texture == nullptr)
    {
        ClearBlack(context_.Get(), terminal);
        return false;
    }
    return ComposeSelected(source, terminal);
}

bool PresentationPass::ComposeSelected(SceneTextureViews source,
    ID3D11UnorderedAccessView* terminal) noexcept
{
    const auto domain = static_cast<std::size_t>(configuration_.scenes->CurrentSceneDomain());
    if (domain >= constants_.size()) return false;
    ScopedComputeState state{context_.Get()};
    if (!state.Ready()) return false;
    const auto frame = configuration_.scenes->FrameGeneration();
    if (layerFrame_ != frame || frame == 0)
    {
        state.PrepareOutputs();
        ClearLayers();
    }
    const std::array inputs{source.shaderResource, layers_[0].source.Get(), layers_[1].source.Get()};
    state.Bind(shader_.Get(), constants_[domain].Get(), inputs, terminal);
    context_->Dispatch((configuration_.width + 7) / 8, (configuration_.height + 7) / 8, 1);
    return true;
}

bool PresentationPass::ComposeHdrChain(ID3D11UnorderedAccessView* target,
    PresentationScope scope) noexcept
{
    if (!framePrepared_) return false;
    if (!SetHdrColorSpace(true)) return false;
    return ExecuteCurrentScene(target, scope);
}

bool PresentationPass::ComposeSdrChain(ID3D11UnorderedAccessView* target) noexcept
{
    if (!framePrepared_) return false;
    const auto frame = configuration_.scenes->FrameGeneration();
    if (frame == 0 || sdrFrame_ != frame) return false;
    if (!SetHdrColorSpace(false)) return false;
    return CopySdrCodes(sdrProxy_.source.Get(), target);
}

void PresentationPass::ComposeChain(PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return;
    auto* target = CurrentTarget();
    if (target == nullptr) return;
    if (ComposeHdrChain(target, scope)) return;
    ExecutionScope executing{executing_};
    if (!ComposeSdrChain(target)) ClearBlack(context_.Get(), target);
}

bool PresentationPass::SetHdrColorSpace(bool hdr) noexcept
{
    if (!chain_) return false;
    if (hdr == hdrColorSpace_) return true;
    if (FAILED(ApplyColorSpace(chain_.Get(), hdr))) return false;
    hdrColorSpace_ = hdr;
    return true;
}

HRESULT PresentationPass::Present(IDXGISwapChain* chain, UINT interval, UINT flags,
    PresentationScope scope) noexcept
{
    if (chain == nullptr) return E_INVALIDARG;
    PrepareNativePresent(chain, flags, scope);
    const auto result = chain->Present(interval, flags);
    CompleteNativePresent(chain, result);
    return result;
}

void PresentationPass::PrepareNativePresent(IDXGISwapChain* chain, UINT flags,
    PresentationScope scope) noexcept
{
    if (chain == nullptr) return;
    const bool owned = Active() && !executing_ && SameChain(chain);
    const bool test = (flags & DXGI_PRESENT_TEST) != 0;
    if (owned && !test)
    {
        ComposeChain(scope);
        CloseFrame();
    }
}

void PresentationPass::CompleteNativePresent(IDXGISwapChain* chain, HRESULT nativeResult) noexcept
{
    if (chain == nullptr) return;
    const bool owned = Active() && !executing_ && SameChain(chain);
    if (owned && DeviceLost(nativeResult))
    {
        lifetime_.Retire();
        CloseFrame();
    }
}

bool PresentationPass::Retire() noexcept
{
    if (!CanRetire()) return false;
    lifetime_.Retire();
    CloseFrame();
    if (!UnbindChainBuffer()) return false;
    buffer_ = {};
    chain_.Reset();
    constants_ = {};
    layerShaders_ = {};
    layers_ = {};
    shader_.Reset();
    sdrShader_.Reset();
    sdrProxy_ = {};
    hdrColorSpace_ = false;
    context_.Reset();
    device_.Reset();
    configuration_ = {};
    return true;
}

bool PresentationPass::CanRetire() const noexcept
{
    if (context_ && !OnOwnerThread()) return false;
    return !executing_;
}

void PresentationPass::CloseFrame() noexcept
{
    framePrepared_ = false;
    sdrFrame_ = 0;
    layerFrame_ = 0;
    layerFailureFrame_ = 0;
    if (configuration_.scenes != nullptr)
        static_cast<void>(configuration_.scenes->EndFrame());
}

bool PresentationPass::UnbindChainBuffer() noexcept
{
    if (!chain_) return true;
    OutputBindings outputs{context_.Get()};
    if (!outputs.Valid()) return false;
    if (!outputs.Exclude(buffer_.texture.Get())) return false;
    outputs.Unbind(context_.Get());
    outputs.Restore(context_.Get());
    return true;
}

}  // namespace fo4hdr
