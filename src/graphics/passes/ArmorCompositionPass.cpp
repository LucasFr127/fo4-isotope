// SPDX-License-Identifier: GPL-3.0-or-later
#include "ArmorCompositionPass.hpp"

#include <Windows.h>

namespace {
bool IsArmorSubmission(const fo4hdr::ArmorUiScopeEntry& entry) noexcept
{
    return entry.owner == fo4hdr::ArmorUiOwner::powerArmor && entry.postAa && entry.hasRoot;
}

bool IsLinearPostAaFrame(const fo4hdr::ArmorCompositionFrame& frame) noexcept
{
    return frame.inputDomain == fo4hdr::ArmorCompositionDomain::linearBt2020UiRelative &&
        frame.postAa && frame.rgbOnly;
}

bool QualifiedScope(const fo4hdr::ArmorUiScopeToken& scope) noexcept
{
    const auto& entry = scope.entry;
    return !scope.nested && scope.serial != 0 && entry.threadId == ::GetCurrentThreadId() &&
        IsArmorSubmission(entry);
}
}

namespace fo4hdr {

bool ArmorCompositionPass::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
    ArmorEncodedConfiguration configuration, ArmorCompositionFrameSource frame) noexcept
{
    if (frame_ || !frame.read) return false;
    if (!encoded_.Initialize(device, context, configuration)) return false;
    ownerThread_ = std::this_thread::get_id();
    resourceGeneration_ = configuration.resourceGeneration;
    context_ = context;
    renderTarget_ = configuration.renderTarget;
    target_ = configuration.target;
    frame_ = frame.read;
    frameState_ = frame.state;
    return true;
}

bool ArmorCompositionPass::OnOwnerThread() const noexcept
{
    return std::this_thread::get_id() == ownerThread_;
}

bool ArmorCompositionPass::MatchingTarget(const ArmorCompositionFrame& frame) const noexcept
{
    return frame.resourceGeneration == resourceGeneration_ && frame.context == context_ &&
        frame.renderTarget == renderTarget_ && frame.target == target_;
}

bool ArmorCompositionPass::QualifiedFrame(const ArmorCompositionFrame& frame) const noexcept
{
    return MatchingTarget(frame) && frame.frameGeneration != 0 && IsLinearPostAaFrame(frame) && frame.contextSafe;
}

void ArmorCompositionPass::Notify(void* state, const ArmorUiScopeToken& token, bool entering) noexcept
{
    auto& pass = *static_cast<ArmorCompositionPass*>(state);
    if (!pass.OnOwnerThread() || !pass.frame_) return;
    if (entering) pass.Enter(token);
    else pass.Exit(token);
}

void ArmorCompositionPass::Enter(const ArmorUiScopeToken& scope) noexcept
{
    if (active_)
    {
        // A nested native submission can escape the RGB-only contract. Never
        // decode through that unqualified interval or keep publishing its image.
        encoded_.Invalidate();
        active_ = false;
        return;
    }
    if (!QualifiedScope(scope)) return;
    const auto frame = frame_(frameState_);
    if (!QualifiedFrame(frame)) return;
    activeToken_ = {ArmorCompositionOwner::powerArmor, ownerThread_, resourceGeneration_,
        frame.frameGeneration, scope.serial, ArmorCompositionScope::qualifiedExclusive, true};
    if (!encoded_.Begin(activeToken_)) return;
    activeScope_ = scope;
    active_ = true;
}

void ArmorCompositionPass::Exit(const ArmorUiScopeToken& scope) noexcept
{
    auto identity = scope;
    identity.aborted = false;
    if (!active_ || !(identity == activeScope_)) return;
    const auto frame = scope.aborted ? ArmorCompositionFrame{} : frame_(frameState_);
    const bool valid = !scope.aborted && QualifiedFrame(frame) &&
        frame.frameGeneration == activeToken_.frameGeneration;
    if (!valid || !encoded_.End(activeToken_)) encoded_.Invalidate();
    active_ = false;
}

bool ArmorCompositionPass::Retire() noexcept
{
    if (!OnOwnerThread() || active_) return false;
    if (!encoded_.Retire()) return false;
    frame_ = nullptr;
    frameState_ = nullptr;
    return true;
}

}  // namespace fo4hdr
