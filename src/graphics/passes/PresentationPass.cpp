// SPDX-License-Identifier: GPL-3.0-or-later

#include "PresentationPass.hpp"
#include "../resources/NativePresentationProxy.hpp"
#include "../d3d11/ScopedD3D11ComputeConstantBuffer.hpp"
#include "ObservedDraw.hpp"

#include "Fallout4HDR/ui/SettingsWindow.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"

#include <fo4hdr-shaders/PresentPixels.hpp>
#include <fo4hdr-shaders/CopySdrCodes.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT outputCapacity = 8;
constexpr UINT classCapacity = 256;
constexpr std::array supportedSceneDomains{
    SceneColorDomain::bt709Scene, SceneColorDomain::bt2020UiRelative};
constexpr std::array<UINT, 6> calibrationModes{0, 10, 11, 12, 13, 14};
constexpr std::size_t variantsPerCalibration = supportedSceneDomains.size() * 2;
constexpr std::array codedLayerFormats{DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
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
    UINT settingsPresent{};
    UINT calibrationMode{};
    UINT reserved{};
};
static_assert(sizeof(PresentConstants) == 32);
static_assert(offsetof(PresentConstants, calibrationMode) == 24);

std::size_t CalibrationIndex(std::uint32_t mode) noexcept
{
    if (mode == 0) return 0;
    if (mode >= 10 && mode <= 14) return mode - 9;
    return calibrationModes.size();
}

bool SdrPacketCurrent(const CompletedNativeSdrFrame* packet, std::uint64_t frame,
    const ScenePassRenderer* scenes) noexcept
{
    return packet == nullptr || packet->ReadyFor(frame, scenes);
}

bool CurrentSettingsPublication(const SettingsWindowOverlay& overlay,
    std::uint64_t frame, const PresentationConfiguration& configuration) noexcept
{
    if (CalibrationIndex(overlay.calibrationMode) >= calibrationModes.size()) return false;
    if (overlay.source == nullptr && overlay.calibrationMode == 0) return true;
    if (overlay.frame != frame) return false;
    if (overlay.width != configuration.width) return false;
    return overlay.height == configuration.height;
}

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
        : context_(context), outputs_(context), constants_(context)
    {
        context_->CSGetShader(&shader_, classes_.data(), &classCount_);
        constants_.Capture();
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
        if (!constants_.Ready()) return false;
        return outputs_.Valid();
    }

    void PrepareOutputs() noexcept
    {
        mutated_ = true;
        context_->SetPredication(nullptr, FALSE);
        outputs_.Unbind(context_);
    }

    void PrepareCopy() noexcept
    {
        PrepareOutputs();
        std::array<ID3D11ShaderResourceView*, 4> emptyInputs{};
        context_->CSSetShaderResources(0, static_cast<UINT>(emptyInputs.size()), emptyInputs.data());
    }

    void Bind(ID3D11ComputeShader* shader, ID3D11Buffer* constants,
        const std::array<ID3D11ShaderResourceView*, 4>& inputs,
        ID3D11UnorderedAccessView* target) noexcept
    {
        PrepareOutputs();
        context_->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.data());
        context_->CSSetUnorderedAccessViews(0, 1, &target, preserveCounters.data());
        constants_.Bind(constants);
        context_->CSSetShader(shader, nullptr, 0);
    }

    ScopedComputeState(const ScopedComputeState&) = delete;
    ScopedComputeState& operator=(const ScopedComputeState&) = delete;

private:
    void Restore() noexcept
    {
        std::array<ID3D11ShaderResourceView*, 4> emptyInputs{};
        ID3D11UnorderedAccessView* emptyTarget{};
        context_->CSSetShaderResources(0, static_cast<UINT>(emptyInputs.size()), emptyInputs.data());
        context_->CSSetUnorderedAccessViews(0, 1, &emptyTarget, preserveCounters.data());
        outputs_.Restore(context_);
        context_->CSSetShaderResources(0, static_cast<UINT>(inputs_.size()), inputs_.data());
        constants_.Restore();
        context_->CSSetShader(shader_.Get(), classes_.data(), classCount_);
        context_->SetPredication(predicate_.Get(), predicateValue_);
    }

    ID3D11DeviceContext1* context_;
    OutputBindings outputs_;
    ComPtr<ID3D11ComputeShader> shader_;
    ScopedD3D11ComputeConstantBuffer constants_;
    std::array<ID3D11ShaderResourceView*, 4> inputs_{};
    ComPtr<ID3D11Predicate> predicate_;
    std::array<ID3D11ClassInstance*, classCapacity> classes_{};
    UINT classCount_{classCapacity};
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
    std::array<ComPtr<ID3D11Buffer>, variantsPerCalibration * calibrationModes.size()> constants;
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
    for (std::size_t index = 0; index < prepared.constants.size(); ++index)
    {
        const auto domain = supportedSceneDomains[index % supportedSceneDomains.size()];
        const PresentConstants values{
            configuration.width, configuration.height, configuration.displayPeakNits,
            static_cast<UINT>(domain), configuration.whites.uiWhiteNits,
            (index % variantsPerCalibration) >= supportedSceneDomains.size() ? 1U : 0U,
            calibrationModes[index / variantsPerCalibration]};
        if (FAILED(CreateConstants(device, values, &prepared.constants[index]))) return false;
        if (!prepared.constants[index]) return false;
    }
    return true;
}

bool PrepareSdrProgram(ID3D11Device* device, PreparedPresentation& prepared) noexcept
{
    if (FAILED(device->CreateComputeShader(kCopySdrCodesBytecode,
            sizeof(kCopySdrCodesBytecode), nullptr, &prepared.sdrShader))) return false;
    return prepared.sdrShader != nullptr;
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
    return PrepareSdrProgram(device, prepared);
}

bool TerminalLayout(const D3D11_TEXTURE2D_DESC& value) noexcept
{
    if (value.MipLevels != 1 || value.ArraySize != 1) return false;
    return value.SampleDesc.Count == 1 && value.SampleDesc.Quality == 0;
}

bool PresentationFootprint(const D3D11_TEXTURE2D_DESC& value,
    const PresentationConfiguration& configuration) noexcept
{
    return value.Width == configuration.width && value.Height == configuration.height &&
        TerminalLayout(value);
}

ComPtr<ID3D11Texture2D> DeviceTexture(ID3D11View* view, ID3D11Device* device) noexcept
{
    const auto resource = Resource(view);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource) return texture;
    if (FAILED(resource.As(&texture))) return {};
    if (!texture) return {};
    ComPtr<ID3D11Device> actualDevice;
    texture->GetDevice(&actualDevice);
    return SameObject(actualDevice.Get(), device) ? texture : ComPtr<ID3D11Texture2D>{};
}

bool DefaultLayerStorage(const D3D11_TEXTURE2D_DESC& value,
    const PresentationConfiguration& configuration) noexcept
{
    if (!PresentationFootprint(value, configuration)) return false;
    return value.Usage == D3D11_USAGE_DEFAULT && value.CPUAccessFlags == 0 && value.MiscFlags == 0;
}

bool TerminalDescription(const D3D11_TEXTURE2D_DESC& value,
    const PresentationConfiguration& configuration) noexcept
{
    if (value.Format != DXGI_FORMAT_R10G10B10A2_UNORM) return false;
    if (!PresentationFootprint(value, configuration)) return false;
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
    texture = DeviceTexture(target, device);
    if (!texture) return false;
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
    const PresentationConfiguration& configuration, DXGI_FORMAT format) noexcept
{
    if (value.Format != format) return false;
    if (!DefaultLayerStorage(value, configuration)) return false;
    return (value.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
}

bool CodedLayerView(const D3D11_SHADER_RESOURCE_VIEW_DESC& view, DXGI_FORMAT format) noexcept
{
    if (view.Format != format) return false;
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    return view.Texture2D.MostDetailedMip == 0 && view.Texture2D.MipLevels == 1;
}

bool CodedLayerSource(ID3D11ShaderResourceView* source, ID3D11Device* device,
    const PresentationConfiguration& configuration,
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) noexcept
{
    if (source == nullptr) return true;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    source->GetDesc(&view);
    if (!CodedLayerView(view, format)) return false;
    const auto texture = DeviceTexture(source, device);
    if (!texture) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return CodedLayerDescription(description, configuration, format);
}

bool SettingsViewLayout(const D3D11_SHADER_RESOURCE_VIEW_DESC& view) noexcept
{
    if (view.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    return view.Texture2D.MostDetailedMip == 0 && view.Texture2D.MipLevels == 1;
}

bool SettingsDescription(const D3D11_TEXTURE2D_DESC& value,
    const PresentationConfiguration& configuration) noexcept
{
    if (value.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
    if (!DefaultLayerStorage(value, configuration)) return false;
    return value.BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
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

bool PresentationPass::PrepareCodedLayer(ID3D11Device* device,
    const PresentationConfiguration& configuration, DXGI_FORMAT format, CodedLayer& prepared) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = configuration.width;
    description.Height = configuration.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &prepared.texture))) return false;
    if (!prepared.texture) return false;
    if (FAILED(device->CreateShaderResourceView(prepared.texture.Get(), nullptr, &prepared.source)))
        return false;
    return prepared.source != nullptr;
}

bool PresentationPass::Initialize(ID3D11Device* device,
    ID3D11DeviceContext* immediateContext, PresentationConfiguration configuration) noexcept
{
    if (lifetime_.CurrentEpoch() != 0) return false;
    if (!ValidInitialization(device, immediateContext, configuration)) return false;
    PreparedPresentation prepared;
    if (!PreparePresentation(device, immediateContext, configuration, prepared)) return false;
    std::array<CodedLayer, 3> preparedLayers;
    for (std::size_t index = 0; index < preparedLayers.size(); ++index)
    {
        if (!PrepareCodedLayer(device, configuration, codedLayerFormats[index], preparedLayers[index])) return false;
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
    layers_ = std::move(preparedLayers);
    (void)performance_.Initialize(device, immediateContext, configuration.measurePerformance);
    lifetime_.PublishInitial();
    return true;
}

LutProfileDrawSnapshot PresentationPass::CopyLutInspector() const noexcept
{
    if (!Active() || !configuration_.scenes) return {};
    return configuration_.scenes->CopyLutInspector();
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

GpuPerformanceSnapshot PresentationPass::Performance() const noexcept
{
    if (!Active() || executing_) return {};
    return performance_.Snapshot();
}

void PresentationPass::PollPerformance(PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return;
    performance_.Poll();
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

bool PresentationPass::QualifiedSettingsSource(ID3D11ShaderResourceView* source,
    ID3D11UnorderedAccessView* terminal) const noexcept
{
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    source->GetDesc(&view);
    if (!SettingsViewLayout(view)) return false;
    const auto texture = DeviceTexture(source, device_.Get());
    if (!texture) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (!SettingsDescription(description, configuration_)) return false;
    const auto sourceIdentity = Identity(texture.Get());
    const auto targetIdentity = Identity(Resource(terminal).Get());
    return sourceIdentity && targetIdentity && sourceIdentity.Get() != targetIdentity.Get();
}

bool PresentationPass::ReadSettingsOverlay(std::uint64_t frame,
    ID3D11UnorderedAccessView* terminal, SettingsWindowOverlay& overlay) noexcept
{
    if (configuration_.settingsWindow == nullptr) return true;
    overlay = configuration_.settingsWindow->Draw(frame, SettingsWindowScope::qualifiedExclusive);
    if (!CurrentSettingsPublication(overlay, frame, configuration_)) return false;
    return overlay.source == nullptr || QualifiedSettingsSource(overlay.source, terminal);
}

bool PresentationPass::QualifiedLayerSource(ID3D11ShaderResourceView* source,
    ComPtr<IUnknown>& identity, DXGI_FORMAT format) const noexcept
{
    if (!CodedLayerSource(source, device_.Get(), configuration_, format)) return false;
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
    ID3D11ShaderResourceView* hud, DXGI_FORMAT backdropFormat) const noexcept
{
    std::array<ComPtr<IUnknown>, 2> identities;
    if (!QualifiedLayerSource(backdrop, identities[0], backdropFormat)) return false;
    if (!QualifiedLayerSource(hud, identities[1], DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    if (!identities[0] || !identities[1]) return true;
    return identities[0].Get() != identities[1].Get();
}

void PresentationPass::ClearLayers() noexcept
{
    selectedLayers_ = {};
}

bool PresentationPass::PrepareLayers(ID3D11ShaderResourceView* backdrop,
    ID3D11ShaderResourceView* hud, PresentationScope scope) noexcept
{
    return PrepareLayerSources(backdrop, hud, scope, DXGI_FORMAT_R8G8B8A8_UNORM);
}

bool PresentationPass::PrepareNativeLayers(const CompletedNativeUiLayers& layers,
    PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return false;
    const auto frame = configuration_.scenes->FrameGeneration();
    if (!layers.ReadyFor(frame, configuration_.scenes))
    {
        layerFrame_ = 0;
        layerFailureFrame_ = frame;
        return false;
    }
    return PrepareLayerSources(layers.Backdrop(), layers.Hud(), scope, DXGI_FORMAT_R16G16B16A16_FLOAT);
}

bool PresentationPass::PrepareLayerSources(ID3D11ShaderResourceView* backdrop,
    ID3D11ShaderResourceView* hud, PresentationScope scope, DXGI_FORMAT backdropFormat) noexcept
{
    if (!CanExecute(scope)) return false;
    const auto frame = configuration_.scenes->FrameGeneration();
    if (frame == 0) return false;
    layerFrame_ = 0;
    layerFailureFrame_ = frame;
    if (!QualifiedLayerSources(backdrop, hud, backdropFormat)) return false;
    ExecutionScope executing{executing_};
    if (!CopyLayerSources(backdrop, hud, backdropFormat)) return false;
    layerFrame_ = frame;
    layerFailureFrame_ = 0;
    return true;
}

bool PresentationPass::CopyLayerSources(ID3D11ShaderResourceView* backdrop,
    ID3D11ShaderResourceView* hud, DXGI_FORMAT backdropFormat) noexcept
{
    const std::array textures{DeviceTexture(backdrop, device_.Get()), DeviceTexture(hud, device_.Get())};
    if ((textures[0].Get() != nullptr) != (backdrop != nullptr)) return false;
    if ((textures[1].Get() != nullptr) != (hud != nullptr)) return false;
    const std::array<std::size_t, 2> destinations{
        backdropFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? 2U : 0U, 1U};
    std::array<ID3D11ShaderResourceView*, 2> selected{};
    {
        ScopedComputeState state{context_.Get()};
        if (!state.Ready()) return false;
        state.PrepareCopy();
        for (std::size_t index = 0; index < textures.size(); ++index)
        {
            if (!textures[index]) continue;
            const auto& layer = layers_[destinations[index]];
            // Same-format copies snapshot codes before the caller can mutate its source.
            context_->CopyResource(layer.texture.Get(), textures[index].Get());
            selected[index] = layer.source.Get();
        }
    }
    // No caller view is retained; publish only after borrowed state restoration.
    selectedLayers_ = selected;
    return true;
}

bool PresentationPass::CopySdrCodes(ID3D11ShaderResourceView* source,
    ID3D11UnorderedAccessView* target) noexcept
{
    ScopedComputeState state{context_.Get()};
    if (!state.Ready()) return false;
    const std::array<ID3D11ShaderResourceView*, 4> inputs{source, nullptr, nullptr, nullptr};
    state.Bind(sdrShader_.Get(), constants_[0].Get(), inputs, target);
    context_->Dispatch((configuration_.width + 7) / 8, (configuration_.height + 7) / 8, 1);
    return true;
}

bool PresentationPass::PrepareSdrSource(ID3D11ShaderResourceView* source,
    PresentationScope scope, DXGI_FORMAT format, const CompletedNativeSdrFrame* packet) noexcept
{
    if (!CanExecute(scope)) return false;
    sdrFrame_ = 0;
    const auto frame = configuration_.scenes->FrameGeneration();
    if (!framePrepared_ || frame == 0) return false;
    if (source == nullptr) return false;
    if (!CodedLayerSource(source, device_.Get(), configuration_, format)) return false;
    ExecutionScope executing{executing_};
    if (!CopySdrCodes(source, sdrProxy_.target.Get())) return false;
    if (!SdrPacketCurrent(packet, frame, configuration_.scenes)) return false;
    // The private code-domain image is published only after borrowed state is restored.
    sdrFrame_ = frame;
    return true;
}

bool PresentationPass::PrepareSdrHandoff(ID3D11ShaderResourceView* completedAuthoredCodes,
    PresentationScope scope) noexcept
{
    return PrepareSdrSource(completedAuthoredCodes, scope, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr);
}

bool PresentationPass::PrepareNativeSdrHandoff(const CompletedNativeSdrFrame& packet,
    PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return false;
    sdrFrame_ = 0;
    const auto frame = configuration_.scenes->FrameGeneration();
    if (!packet.ReadyFor(frame, configuration_.scenes)) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    packet.source_->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
    return PrepareSdrSource(packet.source_, scope, description.Format, &packet);
}

bool PresentationPass::BeginFrame(PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return false;
    framePrepared_ = false;
    sdrFrame_ = 0;
    layerFrame_ = 0;
    layerFailureFrame_ = 0;
    ClearLayers();
    if (!configuration_.scenes->EndFrame()) return false;
    auto* target = CurrentTarget();
    if (target == nullptr) return false;
    ExecutionScope executing{executing_};
    if (!ClearBlack(context_.Get(), target)) return false;
    if (!configuration_.scenes->BeginFrame()) return false;
    framePrepared_ = true;
    return true;
}

bool PresentationPass::ClearTerminal(ID3D11UnorderedAccessView* target) noexcept
{
    ExecutionScope executing{executing_};
    return ClearBlack(context_.Get(), target);
}

bool PresentationPass::ComposeNativeFallback(ID3D11UnorderedAccessView* target) noexcept
{
    ExecutionScope executing{executing_};
    if (ComposeSdrChain(target)) return true;
    if (!SetHdrColorSpace(false)) return false;
    return ClearBlack(context_.Get(), target);
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
    performance_.Poll();
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
    if (domain >= supportedSceneDomains.size()) return false;
    const auto frame = configuration_.scenes->FrameGeneration();
    SettingsWindowOverlay overlay;
    if (!ReadSettingsOverlay(frame, terminal, overlay)) return false;
    ScopedComputeState state{context_.Get()};
    if (!state.Ready()) return false;
    if (layerFrame_ != frame || frame == 0)
    {
        ClearLayers();
    }
    const std::array inputs{source.shaderResource, selectedLayers_[0],
        selectedLayers_[1], overlay.source};
    const auto index = CalibrationIndex(overlay.calibrationMode) * variantsPerCalibration +
        domain + (overlay.source != nullptr ? supportedSceneDomains.size() : 0);
    state.Bind(shader_.Get(), constants_[index].Get(), inputs, terminal);
    const bool measured = performance_.Begin(frame);
    context_->Dispatch((configuration_.width + 7) / 8, (configuration_.height + 7) / 8, 1);
    if (measured) performance_.End(true);
    return true;
}

bool PresentationPass::Retire() noexcept
{
    if (!CanRetire()) return false;
    if (!performance_.Retire()) return false;
    lifetime_.Retire();
    CloseFrame();
    if (!UnbindChainBuffer()) return false;
    buffer_ = {};
    chain_.Reset();
    constants_ = {};
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
    ClearLayers();
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
