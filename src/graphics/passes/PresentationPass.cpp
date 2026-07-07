// SPDX-License-Identifier: GPL-3.0-or-later

#include "PresentationPass.hpp"

#include <fo4hdr-shaders/PresentPixels.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT outputCapacity = 8;
constexpr UINT classCapacity = 256;
constexpr std::array<UINT, outputCapacity> preserveCounters{
    UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1)};

struct PresentConstants
{
    UINT width;
    UINT height;
    float peak;
    float reserved;
};
static_assert(sizeof(PresentConstants) == 16);

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
        context_->CSGetShaderResources(0, 1, &input_);
        context_->GetPredication(&predicate_, &predicateValue_);
    }

    ~ScopedComputeState() noexcept
    {
        if (mutated_) Restore();
        const auto captured = classCount_ < classCapacity ? classCount_ : classCapacity;
        for (UINT index = 0; index < captured; ++index)
        {
            if (classes_[index] != nullptr) classes_[index]->Release();
        }
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
        ID3D11ShaderResourceView* input, ID3D11UnorderedAccessView* target) noexcept
    {
        PrepareOutputs();
        context_->CSSetShaderResources(0, 1, &input);
        context_->CSSetUnorderedAccessViews(0, 1, &target, preserveCounters.data());
        context_->CSSetConstantBuffers(0, 1, &constants);
        context_->CSSetShader(shader, nullptr, 0);
    }

    ScopedComputeState(const ScopedComputeState&) = delete;
    ScopedComputeState& operator=(const ScopedComputeState&) = delete;

private:
    void Restore() noexcept
    {
        ID3D11ShaderResourceView* emptyInput{};
        ID3D11UnorderedAccessView* emptyTarget{};
        context_->CSSetShaderResources(0, 1, &emptyInput);
        context_->CSSetUnorderedAccessViews(0, 1, &emptyTarget, preserveCounters.data());
        outputs_.Restore(context_);
        auto* input = input_.Get();
        auto* constants = constants_.Get();
        context_->CSSetShaderResources(0, 1, &input);
        context_->CSSetConstantBuffers1(0, 1, &constants, &firstConstant_, &constantCount_);
        context_->CSSetShader(shader_.Get(), classes_.data(), classCount_);
        context_->SetPredication(predicate_.Get(), predicateValue_);
    }

    ID3D11DeviceContext1* context_;
    OutputBindings outputs_;
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11ShaderResourceView> input_;
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

bool TypedStoreSupported(ID3D11Device* device) noexcept
{
    UINT support{};
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UNORM, &support))) return false;
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if ((support & required) != required) return false;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 details{DXGI_FORMAT_R10G10B10A2_UNORM, 0};
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
    return options.ConstantBufferOffsetting && TypedStoreSupported(device);
}

HRESULT CreateConstants(ID3D11Device* device, const PresentationConfiguration& configuration,
    ID3D11Buffer** output) noexcept
{
    const PresentConstants values{
        configuration.width, configuration.height, configuration.displayPeakNits, 0.0F};
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
    ComPtr<ID3D11Buffer> constants;
};

bool ValidInitialization(ID3D11Device* device, ID3D11DeviceContext* context,
    const PresentationConfiguration& configuration) noexcept
{
    if (device == nullptr || context == nullptr) return false;
    if (!ValidConfiguration(configuration)) return false;
    return configuration.scenes->MatchesContext(device, context);
}

bool PrepareContext(ID3D11Device* device, ID3D11DeviceContext* context,
    ComPtr<ID3D11DeviceContext1>& prepared) noexcept
{
    if (!PresentationDevice(device, context)) return false;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&prepared)))) return false;
    return prepared != nullptr;
}

bool PreparePresentation(ID3D11Device* device, ID3D11DeviceContext* context,
    const PresentationConfiguration& configuration, PreparedPresentation& prepared) noexcept
{
    if (!PrepareContext(device, context, prepared.context)) return false;
    if (FAILED(device->CreateComputeShader(kPresentPixelsBytecode,
            sizeof(kPresentPixelsBytecode), nullptr, &prepared.shader))) return false;
    if (!prepared.shader) return false;
    if (FAILED(CreateConstants(device, configuration, &prepared.constants))) return false;
    return prepared.constants != nullptr;
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

HRESULT ApplyHdrColorSpace(IDXGISwapChain3* chain) noexcept
{
    UINT support{};
    constexpr auto colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
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

bool PresentationPass::Initialize(ID3D11Device* device,
    ID3D11DeviceContext* immediateContext, PresentationConfiguration configuration) noexcept
{
    if (lifetime_.CurrentEpoch() != 0) return false;
    if (!ValidInitialization(device, immediateContext, configuration)) return false;
    PreparedPresentation prepared;
    if (!PreparePresentation(device, immediateContext, configuration, prepared)) return false;
    if (!lifetime_.Prepare(2, 4)) return false;
    ownerThread_ = std::this_thread::get_id();
    configuration_ = configuration;
    device_ = device;
    context_ = std::move(prepared.context);
    shader_ = std::move(prepared.shader);
    constants_ = std::move(prepared.constants);
    lifetime_.PublishInitial();
    return true;
}

bool PresentationPass::OnOwnerThread() const noexcept
{
    return ownerThread_ == std::this_thread::get_id();
}

bool PresentationPass::Active() const noexcept
{
    return OnOwnerThread() && lifetime_.CurrentEpoch() == 3;
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
    result = ApplyHdrColorSpace(candidate.Get());
    if (FAILED(result)) return result;
    buffer_ = std::move(prepared);
    chain_ = std::move(candidate);
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

bool PresentationPass::BeginFrame(PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return false;
    framePrepared_ = false;
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
    ExecutionScope executing{executing_};
    ScopedComputeState state{context_.Get()};
    if (!state.Ready()) return false;
    if (source.texture == nullptr)
    {
        state.PrepareOutputs();
        const float black[]{0.0F, 0.0F, 0.0F, 1.0F};
        context_->ClearUnorderedAccessViewFloat(terminal, black);
        return false;
    }
    state.Bind(shader_.Get(), constants_.Get(), source.shaderResource, terminal);
    context_->Dispatch((configuration_.width + 7) / 8, (configuration_.height + 7) / 8, 1);
    return true;
}

void PresentationPass::ComposeChain(PresentationScope scope) noexcept
{
    if (!CanExecute(scope)) return;
    auto* target = CurrentTarget();
    if (target == nullptr) return;
    if (framePrepared_)
        static_cast<void>(ExecuteCurrentScene(target, scope));
    else
        ClearBlack(context_.Get(), target);
}

HRESULT PresentationPass::Present(IDXGISwapChain* chain, UINT interval, UINT flags,
    PresentationScope scope) noexcept
{
    if (chain == nullptr) return E_INVALIDARG;
    const bool owned = Active() && !executing_ && SameChain(chain);
    const bool test = (flags & DXGI_PRESENT_TEST) != 0;
    if (owned && !test)
    {
        ComposeChain(scope);
        CloseFrame();
    }
    // Transient state and references are gone before native Present changes bindings.
    const auto result = chain->Present(interval, flags);
    if (owned && DeviceLost(result))
    {
        lifetime_.Retire();
        CloseFrame();
    }
    return result;
}

bool PresentationPass::Retire() noexcept
{
    if (!CanRetire()) return false;
    lifetime_.Retire();
    CloseFrame();
    if (!UnbindChainBuffer()) return false;
    buffer_ = {};
    chain_.Reset();
    constants_.Reset();
    shader_.Reset();
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
