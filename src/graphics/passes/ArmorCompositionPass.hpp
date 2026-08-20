// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ArmorEncodedComposition.hpp"
#include "Fallout4HDR/output/ArmorUiComposition.hpp"

namespace fo4hdr {

struct ArmorCompositionFrame
{
    std::uint64_t resourceGeneration{};
    std::uint64_t frameGeneration{};
    ArmorCompositionDomain inputDomain{};
    ID3D11DeviceContext* context{};
    ID3D11RenderTargetView* renderTarget{};
    ID3D11UnorderedAccessView* target{};
    bool postAa{};
    bool rgbOnly{};
    bool contextSafe{};
};

// An owner-thread provider returns the currently associated image evidence.
// Missing native association is an empty frame, not inferred from UI names.
// inputDomain records the qualified source; the pass owns its current transport domain.
using ArmorCompositionFrameProvider = ArmorCompositionFrame (*)(void*) noexcept;

struct ArmorCompositionFrameSource
{
    ArmorCompositionFrameProvider read{};
    void* state{};
};

class ArmorCompositionPass
{
public:
    [[nodiscard]] bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
        ArmorEncodedConfiguration configuration, ArmorCompositionFrameSource frame) noexcept;
    [[nodiscard]] const ArmorUiBoundary& Boundary() const noexcept { return boundary_; }
    [[nodiscard]] ArmorCompositionDomain Domain() const noexcept { return encoded_.Domain(); }
    [[nodiscard]] bool Retire() noexcept;

private:
    static void Notify(void* state, const ArmorUiScopeToken& token, bool entering) noexcept;
    void Enter(const ArmorUiScopeToken& scope) noexcept;
    void Exit(const ArmorUiScopeToken& scope) noexcept;
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] bool MatchingTarget(const ArmorCompositionFrame& frame) const noexcept;
    [[nodiscard]] bool QualifiedFrame(const ArmorCompositionFrame& frame) const noexcept;

    ArmorEncodedComposition encoded_;
    ArmorUiBoundary boundary_{&Notify, this};
    ArmorCompositionFrameProvider frame_{};
    void* frameState_{};
    std::thread::id ownerThread_;
    std::uint64_t resourceGeneration_{};
    ID3D11DeviceContext* context_{};
    ID3D11RenderTargetView* renderTarget_{};
    ID3D11UnorderedAccessView* target_{};
    ArmorUiScopeToken activeScope_;
    ArmorEncodedToken activeToken_;
    bool active_{};
};

}  // namespace fo4hdr
