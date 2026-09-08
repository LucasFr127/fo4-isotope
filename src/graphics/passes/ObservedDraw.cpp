// SPDX-License-Identifier: GPL-3.0-or-later

#include "ObservedDraw.hpp"
#include "../resources/NativePresentationProxy.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;
constexpr UINT sourceSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr UINT uavSlots = D3D11_PS_CS_UAV_REGISTER_COUNT;

ComPtr<IUnknown> Identity(IUnknown* object) noexcept
{
    ComPtr<IUnknown> identity;
    if (object != nullptr) static_cast<void>(object->QueryInterface(IID_PPV_ARGS(&identity)));
    return identity;
}

bool SameObject(IUnknown* left, IUnknown* right) noexcept
{
    const auto first = Identity(left);
    const auto second = Identity(right);
    return first && second && first.Get() == second.Get();
}

bool SameDevice(ID3D11DeviceChild* child, ID3D11Device* device) noexcept
{
    if (child == nullptr) return false;
    ComPtr<ID3D11Device> actual;
    child->GetDevice(&actual);
    return SameObject(actual.Get(), device);
}

bool ColdContext(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (device == nullptr) return false;
    if (context == nullptr) return false;
    if (device->GetFeatureLevel() != D3D_FEATURE_LEVEL_11_0) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    if (!SameDevice(context, device)) return false;
    ComPtr<ID3D11DeviceContext> canonical;
    device->GetImmediateContext(&canonical);
    return SameObject(canonical.Get(), context);
}

ComPtr<ID3D11Texture2D> Texture(ID3D11View* view) noexcept
{
    ComPtr<ID3D11Resource> resource;
    if (view != nullptr) view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource) static_cast<void>(resource.As(&texture));
    return texture;
}

bool SingleImage(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    if (description.MipLevels != 1 || description.ArraySize != 1) return false;
    return description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0;
}

bool ImageExtent(const D3D11_TEXTURE2D_DESC& description,
    const ScenePassConfiguration& configuration) noexcept
{
    return description.Width == configuration.width && description.Height == configuration.height;
}

bool ImageDimensions(const ScenePassConfiguration& configuration) noexcept
{
    return configuration.width != 0 && configuration.height != 0;
}

bool NonemptyImage(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    return description.Width != 0 && description.Height != 0;
}

D3D11_TEXTURE2D_DESC LayerDescription(ScenePassConfiguration configuration, DXGI_FORMAT format) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = configuration.width;
    description.Height = configuration.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return description;
}

template <class Layer>
bool PrepareLayer(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& description, Layer& layer) noexcept
{
    if (FAILED(device->CreateTexture2D(&description, nullptr, &layer.texture))) return false;
    if (FAILED(device->CreateRenderTargetView(layer.texture.Get(), nullptr, &layer.target))) return false;
    if (FAILED(device->CreateShaderResourceView(layer.texture.Get(), nullptr, &layer.source))) return false;
    return layer.texture && layer.target && layer.source;
}

template <class Layers>
bool PrepareImages(ID3D11Device* device, ScenePassConfiguration configuration, Layers& layers) noexcept
{
    const auto backdrop = LayerDescription(configuration, DXGI_FORMAT_R16G16B16A16_FLOAT);
    const auto hud = LayerDescription(configuration, DXGI_FORMAT_R8G8B8A8_UNORM);
    return PrepareLayer(device, backdrop, layers[0]) && PrepareLayer(device, hud, layers[1]);
}

bool PrepareBlend(ID3D11Device* device, ComPtr<ID3D11BlendState>& blend) noexcept
{
    D3D11_BLEND_DESC description{};
    auto& target = description.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device->CreateBlendState(&description, &blend)) && blend;
}

std::size_t LayerIndex(NativeUiShaderRole role) noexcept
{
    return role == NativeUiShaderRole::gammaBackdrop ? 0 : 1;
}

NativeUiShaderRole BytecodeRole(std::span<const std::byte> bytecode) noexcept
{
    if (bytecode.size() < 32) return NativeUiShaderRole::unknown;
    if (std::memcmp(bytecode.data(), "DXBC", 4) != 0) return NativeUiShaderRole::unknown;
    std::array<std::uint32_t, 4> hash{};
    std::memcpy(hash.data(), bytecode.data() + 4, sizeof(hash));
    return FindNativeUiShaderRole(hash);
}

void ReleaseReference(IUnknown* object) noexcept
{
    if (object != nullptr) object->Release();
}

bool PixelShaderMatches(ID3D11DeviceContext* context, ID3D11PixelShader* expected) noexcept
{
    ComPtr<ID3D11PixelShader> actual;
    std::array<ID3D11ClassInstance*, 256> classes{};
    UINT count = static_cast<UINT>(classes.size());
    context->PSGetShader(&actual, classes.data(), &count);
    for (auto* instance : classes) ReleaseReference(instance);
    return count <= classes.size() && SameObject(actual.Get(), expected);
}

bool MipZeroSource(const D3D11_SHADER_RESOURCE_VIEW_DESC& view) noexcept
{
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    return view.Texture2D.MostDetailedMip == 0 && view.Texture2D.MipLevels == 1;
}

bool CodedHudImage(const D3D11_SHADER_RESOURCE_VIEW_DESC& view,
    const D3D11_TEXTURE2D_DESC& description, const ScenePassConfiguration& configuration) noexcept
{
    if (view.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    return ImageExtent(description, configuration);
}

bool SourceView(ID3D11ShaderResourceView* source, NativeUiShaderRole role,
    const ScenePassConfiguration& configuration) noexcept
{
    if (source == nullptr) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    source->GetDesc(&view);
    if (!MipZeroSource(view)) return false;
    const auto texture = Texture(source);
    if (!texture) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (!SingleImage(description)) return false;
    if (!NonemptyImage(description)) return false;
    if (role == NativeUiShaderRole::gammaBackdrop) return true;
    return CodedHudImage(view, description, configuration);
}

template <class Views>
bool ReleaseAndOccupied(const Views& views) noexcept
{
    const bool occupied = std::any_of(views.begin(), views.end(), [](auto* view) { return view != nullptr; });
    for (auto* view : views) ReleaseReference(view);
    return occupied;
}

bool OccupiedUavs(ID3D11DeviceContext* context) noexcept
{
    std::array<ID3D11UnorderedAccessView*, uavSlots> om{};
    std::array<ID3D11UnorderedAccessView*, uavSlots> cs{};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavSlots, om.data());
    context->CSGetUnorderedAccessViews(0, uavSlots, cs.data());
    const bool omOccupied = ReleaseAndOccupied(om);
    const bool csOccupied = ReleaseAndOccupied(cs);
    return omOccupied || csOccupied;
}

bool BoundTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* expected,
    ID3D11DepthStencilView* expectedDepth) noexcept
{
    std::array<ID3D11RenderTargetView*, 8> targets{};
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), &depth);
    const bool first = SameObject(targets[0], expected);
    const bool others = std::all_of(targets.begin() + 1, targets.end(), [](auto* view) { return view == nullptr; });
    for (auto* target : targets) ReleaseReference(target);
    const bool depthMatches = depth.Get() == expectedDepth || SameObject(depth.Get(), expectedDepth);
    return first && others && depthMatches;
}

bool BoundBlend(ID3D11DeviceContext* context, ID3D11BlendState* expected, UINT expectedMask) noexcept
{
    ComPtr<ID3D11BlendState> blend;
    std::array<float, 4> factors{};
    UINT mask{};
    context->OMGetBlendState(&blend, factors.data(), &mask);
    constexpr std::array<float, 4> defaults{1, 1, 1, 1};
    if (!SameObject(blend.Get(), expected)) return false;
    return factors == defaults && mask == expectedMask;
}

using ReadSources = void (*)(ID3D11DeviceContext*, ID3D11ShaderResourceView**);
using WriteSource = void (*)(ID3D11DeviceContext*, UINT, ID3D11ShaderResourceView*);
constexpr std::array<ReadSources, 6> readSources{
    [](auto* context, auto** views) { context->VSGetShaderResources(0, sourceSlots, views); },
    [](auto* context, auto** views) { context->HSGetShaderResources(0, sourceSlots, views); },
    [](auto* context, auto** views) { context->DSGetShaderResources(0, sourceSlots, views); },
    [](auto* context, auto** views) { context->GSGetShaderResources(0, sourceSlots, views); },
    [](auto* context, auto** views) { context->PSGetShaderResources(0, sourceSlots, views); },
    [](auto* context, auto** views) { context->CSGetShaderResources(0, sourceSlots, views); }};
constexpr std::array<WriteSource, 6> writeSource{
    [](auto* context, UINT slot, auto* view) { context->VSSetShaderResources(slot, 1, &view); },
    [](auto* context, UINT slot, auto* view) { context->HSSetShaderResources(slot, 1, &view); },
    [](auto* context, UINT slot, auto* view) { context->DSSetShaderResources(slot, 1, &view); },
    [](auto* context, UINT slot, auto* view) { context->GSSetShaderResources(slot, 1, &view); },
    [](auto* context, UINT slot, auto* view) { context->PSSetShaderResources(slot, 1, &view); },
    [](auto* context, UINT slot, auto* view) { context->CSSetShaderResources(slot, 1, &view); }};

class ScopedUiState
{
public:
    ScopedUiState(ID3D11DeviceContext* context, ID3D11RenderTargetView* expected,
        ID3D11RenderTargetView* capture) noexcept : context_(context), capture_(capture)
    {
        std::array<ID3D11RenderTargetView*, 8> targets{};
        ID3D11DepthStencilView* depth{};
        context_->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), &depth);
        for (std::size_t index = 0; index < targets.size(); ++index) targets_[index].Attach(targets[index]);
        depth_.Attach(depth);
        if (!SameObject(targets_[0].Get(), expected)) return;
        if (std::any_of(targets_.begin() + 1, targets_.end(), [](const auto& view) { return view != nullptr; })) return;
        if (OccupiedUavs(context_)) return;
        ComPtr<ID3D11Predicate> predicate;
        BOOL value{};
        context_->GetPredication(&predicate, &value);
        if (predicate) return;
        context_->OMGetBlendState(&blend_, factors_.data(), &mask_);
        CaptureAliases();
        ready_ = true;
    }
    ~ScopedUiState() noexcept { Restore(); }
    [[nodiscard]] bool Ready() const noexcept { return ready_; }
    [[nodiscard]] bool DepthDetached() const noexcept { return !depth_; }
    [[nodiscard]] bool Bind(ID3D11BlendState* blend, bool clear) noexcept
    {
        mutated_ = true;
        for (std::size_t stage = 0; stage < aliases_.size(); ++stage)
            for (UINT slot = 0; slot < sourceSlots; ++slot)
                if (aliases_[stage][slot]) writeSource[stage](context_, slot, nullptr);
        if (clear) { const float transparent[4]{}; context_->ClearRenderTargetView(capture_, transparent); }
        context_->OMSetRenderTargets(1, &capture_, depth_.Get());
        context_->OMSetBlendState(blend, nullptr, mask_);
        return BoundTarget(context_, capture_, depth_.Get()) && BoundBlend(context_, blend, mask_);
    }

private:
    void CaptureAliases() noexcept
    {
        const auto captureTexture = Texture(capture_);
        for (std::size_t stage = 0; stage < aliases_.size(); ++stage)
        {
            std::array<ID3D11ShaderResourceView*, sourceSlots> views{};
            readSources[stage](context_, views.data());
            for (UINT slot = 0; slot < sourceSlots; ++slot)
            {
                sources_[stage][slot].Attach(views[slot]);
                aliases_[stage][slot] = SameObject(Texture(views[slot]).Get(), captureTexture.Get());
            }
        }
    }
    void Restore() noexcept
    {
        if (!mutated_) return;
        context_->OMSetBlendState(blend_.Get(), factors_.data(), mask_);
        std::array<ID3D11RenderTargetView*, 8> targets{};
        for (std::size_t index = 0; index < targets.size(); ++index) targets[index] = targets_[index].Get();
        context_->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), depth_.Get());
        for (std::size_t stage = 0; stage < aliases_.size(); ++stage)
            for (UINT slot = 0; slot < sourceSlots; ++slot)
                if (aliases_[stage][slot]) writeSource[stage](context_, slot, sources_[stage][slot].Get());
    }

    ID3D11DeviceContext* context_;
    ID3D11RenderTargetView* capture_;
    std::array<ComPtr<ID3D11RenderTargetView>, 8> targets_;
    ComPtr<ID3D11DepthStencilView> depth_;
    ComPtr<ID3D11BlendState> blend_;
    std::array<float, 4> factors_{};
    UINT mask_{};
    std::array<std::array<ComPtr<ID3D11ShaderResourceView>, sourceSlots>, 6> sources_;
    std::array<std::array<bool, sourceSlots>, 6> aliases_{};
    bool ready_{};
    bool mutated_{};
};

class InvocationScope
{
public:
    explicit InvocationScope(bool& invoking) noexcept : invoking_(invoking) { invoking_ = true; }
    ~InvocationScope() noexcept { invoking_ = false; }
private:
    bool& invoking_;
};

bool Invoke(FiniteDrawInvocation invocation, ID3D11DeviceContext* context) noexcept
{
    try { invocation.draw(context, invocation.argument); return true; }
    catch (...) { return false; }
}

bool TargetView(const D3D11_RENDER_TARGET_VIEW_DESC& view) noexcept
{
    if (view.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    return view.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && view.Texture2D.MipSlice == 0;
}

bool TargetImage(const D3D11_TEXTURE2D_DESC& description,
    const ScenePassConfiguration& configuration) noexcept
{
    if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    return SingleImage(description) && ImageExtent(description, configuration);
}

bool FrameScene(const NativeUiFrame& frame, ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (frame.scenes == nullptr) return false;
    if (!frame.scenes->MatchesContext(device, context)) return false;
    if (frame.scenes->FrameGeneration() != frame.frame) return false;
    return frame.scenes->Views(frame.scenes->CurrentScene()).texture != nullptr;
}

bool KnownUiRole(NativeUiShaderRole role) noexcept
{
    return role == NativeUiShaderRole::hudComposite || role == NativeUiShaderRole::gammaBackdrop;
}

bool QualifiedRequirements(const NativeUiFrame& frame) noexcept
{
    return std::all_of(frame.requirements.begin(), frame.requirements.end(), [](auto requirement) {
        return requirement == NativeUiLayerRequirement::required ||
            requirement == NativeUiLayerRequirement::qualifiedAbsent;
    });
}

}  // namespace

bool NativeUiCapture::CanUse() const noexcept
{
    return thread_ == std::this_thread::get_id() && device_ && context_ && !invoking_;
}

bool NativeUiCapture::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
    ScenePassConfiguration configuration) noexcept
{
    if (device_) return false;
    if (!ColdContext(device, context)) return false;
    if (!ImageDimensions(configuration)) return false;
    decltype(layers_) layers;
    ComPtr<ID3D11BlendState> blend;
    if (!PrepareImages(device, configuration, layers)) return false;
    if (!PrepareBlend(device, blend)) return false;
    device_ = device;
    context_ = context;
    layers_ = std::move(layers);
    blend_ = std::move(blend);
    configuration_ = configuration;
    thread_ = std::this_thread::get_id();
    ++epoch_;
    return true;
}

bool NativeUiCapture::Retire() noexcept
{
    if (!device_) return true;
    if (!CanUse()) return false;
    CloseFrame();
    observed_ = {};
    observedCount_ = 0;
    layers_ = {};
    blend_.Reset();
    context_.Reset();
    device_.Reset();
    return true;
}

bool NativeUiCapture::ObserveCreatedPixelShader(std::span<const std::byte> bytecode,
    ID3D11PixelShader* shader) noexcept
{
    if (!CanUse() || !SameDevice(shader, device_.Get())) return false;
    const auto role = BytecodeRole(bytecode);
    if (role == NativeUiShaderRole::unknown) return false;
    const auto previous = ObservedRole(shader);
    if (previous != NativeUiShaderRole::unknown) return previous == role;
    if (observedCount_ == observed_.size()) return false;
    observed_[observedCount_++] = {shader, role};
    return true;
}

NativeUiShaderRole NativeUiCapture::ObservedRole(ID3D11PixelShader* shader) const noexcept
{
    for (std::size_t index = 0; index < observedCount_; ++index)
        if (SameObject(observed_[index].shader.Get(), shader)) return observed_[index].role;
    return NativeUiShaderRole::unknown;
}

void NativeUiCapture::AdvanceFrame(std::uint64_t frame) noexcept
{
    if (!CanUse()) return;
    if (frame == 0 || frame <= lastFrame_) return;
    CloseFrame();
    frame_ = frame;
    lastFrame_ = frame;
    requirements_.fill(NativeUiLayerRequirement::required);
}

void NativeUiCapture::CloseFrame() noexcept
{
    if (!CanUse()) return;
    presentationTarget_.Reset();
    scenes_ = nullptr;
    presentationProxy_ = nullptr;
    frame_ = 0;
    captured_ = {};
    failed_ = false;
    ++epoch_;
}

bool NativeUiCapture::QualifiedTarget(ID3D11RenderTargetView* target) const noexcept
{
    if (!SameDevice(target, device_.Get())) return false;
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    target->GetDesc(&view);
    if (!TargetView(view)) return false;
    const auto texture = Texture(target);
    if (!texture) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (!TargetImage(description, configuration_)) return false;
    return !OwnedLayerAlias(texture.Get());
}

bool NativeUiCapture::OwnedLayerAlias(ID3D11Texture2D* texture) const noexcept
{
    return std::any_of(layers_.begin(), layers_.end(),
        [texture](const Layer& layer) { return SameObject(texture, layer.texture.Get()); });
}

bool NativeUiCapture::FrameAdmissionCurrent(const NativeUiFrame& frame) const noexcept
{
    if (frame.frame == 0 || frame.frame < lastFrame_) return false;
    return frame.frame != lastFrame_ || frame_ != 0;
}

bool NativeUiCapture::CanBeginFrame(const NativeUiFrame& frame, NativeUiCaptureScope scope) const noexcept
{
    if (!CanUse()) return false;
    if (scope != NativeUiCaptureScope::qualifiedExclusive) return false;
    if (!FrameAdmissionCurrent(frame)) return false;
    if (presentationTarget_) return false;
    if (!QualifiedRequirements(frame)) return false;
    return FrameScene(frame, device_.Get(), context_.Get());
}

bool NativeUiCapture::BeginFrame(NativeUiFrame frame, NativeUiCaptureScope scope) noexcept
{
    if (!CanBeginFrame(frame, scope)) return false;
    if (!QualifiedTarget(frame.presentationTarget)) return false;
    return CommitFrame(frame);
}

bool NativeUiCapture::BeginPresentationFrame(NativePresentationProxy& proxy,
    ScenePassRenderer& scenes, NativeUiCaptureScope scope) noexcept
{
    const auto owned = proxy.SceneViews();
    const auto selected = scenes.Views(scenes.CurrentScene());
    if (!scenes.CurrentSceneOperatorProduced()) return false;
    if (!SameObject(owned.texture, selected.texture)) return false;
    if (!SameDevice(owned.renderTarget, device_.Get())) return false;
    const NativeUiFrame frame{scenes.FrameGeneration(), owned.renderTarget, &scenes};
    if (!CanBeginFrame(frame, scope)) return false;
    if (!CommitFrame(frame)) return false;
    presentationProxy_ = &proxy;
    return PresentationSceneCurrent();
}

bool NativeUiCapture::CommitFrame(const NativeUiFrame& frame) noexcept
{
    if (frame.frame != frame_) AdvanceFrame(frame.frame);
    if (frame.frame != frame_) return false;
    presentationTarget_ = frame.presentationTarget;
    scenes_ = frame.scenes;
    requirements_ = frame.requirements;
    return true;
}

void NativeUiCapture::WithdrawLostPresentationScene() noexcept
{
    if (CanUse() && !PresentationSceneCurrent()) CloseFrame();
}

bool NativeUiCapture::MissingRequiredLayer() const noexcept
{
    for (std::size_t index = 0; index < captured_.size(); ++index)
        if (!captured_[index] && requirements_[index] == NativeUiLayerRequirement::required) return true;
    return false;
}

NativeUiFrameStatus NativeUiCapture::Status() const noexcept
{
    if (!CanUse() || frame_ == 0) return NativeUiFrameStatus::unprepared;
    if (failed_ || !PresentationSceneCurrent()) return NativeUiFrameStatus::failed;
    if (!presentationTarget_) return NativeUiFrameStatus::targetUnqualified;
    if (MissingRequiredLayer()) return NativeUiFrameStatus::missingRequired;
    return NativeUiFrameStatus::ready;
}

bool NativeUiCapture::QualifiedDraw(const QualifiedNativeUiDraw& draw) const noexcept
{
    if (!SameDevice(draw.shader, device_.Get())) return false;
    if (!PixelShaderMatches(context_.Get(), draw.shader)) return false;
    ComPtr<ID3D11ShaderResourceView> source;
    context_->PSGetShaderResources(0, 1, &source);
    if (!SameDevice(source.Get(), device_.Get())) return false;
    if (!SourceView(source.Get(), draw.role, configuration_)) return false;
    const auto texture = Texture(source.Get());
    if (SameObject(texture.Get(), Texture(presentationTarget_.Get()).Get())) return false;
    if (OwnedLayerAlias(texture.Get())) return false;
    if (draw.role != NativeUiShaderRole::gammaBackdrop) return true;
    return draw.kind == NativeUiDrawKind::indexedSix;
}

bool NativeUiCapture::PresentationSceneCurrent() const noexcept
{
    if (presentationProxy_ == nullptr) return true;
    if (scenes_ == nullptr || !scenes_->CurrentSceneOperatorProduced()) return false;
    if (scenes_->CurrentSceneDomain() != SceneColorDomain::bt2020UiRelative) return false;
    const auto owned = presentationProxy_->SceneViews();
    const auto selected = scenes_->Views(scenes_->CurrentScene());
    return owned.texture != nullptr && SameObject(owned.texture, selected.texture) &&
        SameObject(owned.renderTarget, presentationTarget_.Get());
}

bool NativeUiCapture::CanCapture(const QualifiedNativeUiDraw& draw) const noexcept
{
    if (!CanUse() || !PresentationSceneCurrent()) return false;
    if (draw.frame != frame_ || !presentationTarget_) return false;
    return scenes_ != nullptr && scenes_->FrameGeneration() == draw.frame;
}

SceneDrawOutcome NativeUiCapture::ExecuteQualified(QualifiedNativeUiDraw draw,
    FiniteDrawInvocation invocation) noexcept
{
    if (!CanCapture(draw)) return SceneDrawOutcome::forwarded;
    if (!KnownUiRole(draw.role)) return SceneDrawOutcome::forwarded;
    const auto index = LayerIndex(draw.role);
    if (failed_) return SceneDrawOutcome::forwarded;
    if (requirements_[index] == NativeUiLayerRequirement::qualifiedAbsent)
    { failed_ = true; return SceneDrawOutcome::forwarded; }
    if (invocation.draw == nullptr || !QualifiedDraw(draw)) { failed_ = true; return SceneDrawOutcome::forwarded; }
    const auto result = SubmitDraw(index, draw.role, invocation);
    // Publish only after the captured native draw and every touched binding restore.
    if (result != SceneDrawOutcome::replaced) { failed_ = true; return result; }
    captured_[index] = true;
    return SceneDrawOutcome::replaced;
}

SceneDrawOutcome NativeUiCapture::SubmitDraw(std::size_t index, NativeUiShaderRole role,
    FiniteDrawInvocation invocation) noexcept
{
    const InvocationScope invoking{invoking_};
    ScopedUiState state{context_.Get(), presentationTarget_.Get(), layers_[index].target.Get()};
    if (!state.Ready()) return SceneDrawOutcome::forwarded;
    if (role == NativeUiShaderRole::gammaBackdrop && !state.DepthDetached()) return SceneDrawOutcome::forwarded;
    if (!state.Bind(blend_.Get(), !captured_[index])) return SceneDrawOutcome::forwarded;
    return Invoke(invocation, context_.Get()) ? SceneDrawOutcome::replaced : SceneDrawOutcome::callbackFailed;
}

SceneDrawOutcome NativeUiCapture::ExecuteObserved(std::uint64_t frame,
    NativeUiDrawKind kind, FiniteDrawInvocation invocation) noexcept
{
    if (!CanCapture({frame})) return SceneDrawOutcome::forwarded;
    ComPtr<ID3D11PixelShader> shader;
    context_->PSGetShader(&shader, nullptr, nullptr);
    return ExecuteQualified({frame, ObservedRole(shader.Get()), shader.Get(), kind}, invocation);
}

bool NativeUiCapture::PacketReady(std::uint64_t frame, std::uint64_t epoch) const noexcept
{
    if (scenes_ == nullptr || scenes_->FrameGeneration() != frame) return false;
    return frame != 0 && frame == frame_ && epoch == epoch_ && Status() == NativeUiFrameStatus::ready;
}

ID3D11ShaderResourceView* NativeUiCapture::CompletedSource(std::size_t index) const noexcept
{
    return captured_[index] ? layers_[index].source.Get() : nullptr;
}

CompletedNativeUiLayers NativeUiCapture::CompletedLayers() const noexcept
{
    CompletedNativeUiLayers layers;
    if (Status() != NativeUiFrameStatus::ready) return layers;
    layers.owner_ = this;
    layers.frame_ = frame_;
    layers.epoch_ = epoch_;
    return layers;
}

bool CompletedNativeUiLayers::ReadyFor(std::uint64_t frame, const ScenePassRenderer* scenes) const noexcept
{
    return owner_ != nullptr && scenes == owner_->scenes_ && frame == frame_ && owner_->PacketReady(frame_, epoch_);
}

ID3D11ShaderResourceView* CompletedNativeUiLayers::Backdrop() const noexcept
{
    return owner_ != nullptr && ReadyFor(frame_, owner_->scenes_) ? owner_->CompletedSource(0) : nullptr;
}

ID3D11ShaderResourceView* CompletedNativeUiLayers::Hud() const noexcept
{
    return owner_ != nullptr && ReadyFor(frame_, owner_->scenes_) ? owner_->CompletedSource(1) : nullptr;
}

}  // namespace fo4hdr
