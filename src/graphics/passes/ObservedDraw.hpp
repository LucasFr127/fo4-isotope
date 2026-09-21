// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../HdrRenderer.hpp"

#include <array>
#include <thread>

namespace fo4hdr {

[[nodiscard]] bool PrepareLutProfileBuffer(ID3D11Device* device,
    const LutGradeProfiles& profiles, Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer) noexcept;


class NativeUiCapture;
class PresentationPass;
class NativePresentationProxy;

enum class NativeUiCaptureScope
{
    unavailable,
    qualifiedExclusive
};

enum class NativeUiDrawKind
{
    finite,
    indexedSix
};

enum class NativeUiLayerRequirement
{
    required,
    qualifiedAbsent
};

enum class NativeUiFrameStatus
{
    unprepared,
    targetUnqualified,
    missingRequired,
    ready,
    failed
};

struct NativeUiFrame
{
    std::uint64_t frame{};
    // Caller proves this exact logical presentation target and an operator-produced
    // selected scene; matching dimensions or R8 storage alone establish neither.
    ID3D11RenderTargetView* presentationTarget{};
    ScenePassRenderer* scenes{};
    std::array<NativeUiLayerRequirement, 2> requirements{
        NativeUiLayerRequirement::required, NativeUiLayerRequirement::required};
};

struct QualifiedNativeUiDraw
{
    std::uint64_t frame{};
    NativeUiShaderRole role{};
    ID3D11PixelShader* shader{};
    NativeUiDrawKind kind{};
};

// Borrowed code-domain views are usable only while this owner/frame remains ready.
// The owner outlives every packet and its presentation consumer.
class CompletedNativeUiLayers
{
public:
    [[nodiscard]] bool ReadyFor(std::uint64_t frame, const ScenePassRenderer* scenes) const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* Backdrop() const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* Hud() const noexcept;

private:
    friend class NativeUiCapture;
    const NativeUiCapture* owner_{};
    std::uint64_t frame_{};
    std::uint64_t epoch_{};
};

// Cold preparation and all calls belong to one genuine immediate-context thread.
// qualifiedExclusive excludes queries, command lists, reentry and concurrent use.
// Retirement releases only owned references and never clears borrowed context state.
class NativeUiCapture
{
public:
    [[nodiscard]] bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
        ScenePassConfiguration configuration, bool developerReference = false) noexcept;
    [[nodiscard]] bool Retire() noexcept;
    [[nodiscard]] bool ObserveCreatedPixelShader(std::span<const std::byte> bytecode,
        ID3D11PixelShader* shader) noexcept;
    // A native frame initially has no qualified target association.
    void AdvanceFrame(std::uint64_t frame) noexcept;
    void CloseFrame() noexcept;
    [[nodiscard]] bool BeginFrame(NativeUiFrame frame, NativeUiCaptureScope scope) noexcept;
    [[nodiscard]] bool BeginPresentationFrame(NativePresentationProxy& proxy,
        ScenePassRenderer& scenes, NativeUiCaptureScope scope) noexcept;
    [[nodiscard]] NativeUiFrameStatus Status() const noexcept;
    void WithdrawLostPresentationScene() noexcept;
    [[nodiscard]] CompletedNativeUiLayers CompletedLayers() const noexcept;
    // Controlled caller qualifies the supplied shader's authored-code/coverage role.
    // The native path obtains that role only from successful matched shader creation.
    [[nodiscard]] SceneDrawOutcome ExecuteQualified(QualifiedNativeUiDraw draw,
        FiniteDrawInvocation invocation) noexcept;
    [[nodiscard]] SceneDrawOutcome ExecuteObserved(std::uint64_t frame,
        NativeUiDrawKind kind, FiniteDrawInvocation invocation) noexcept;

private:
    friend class CompletedNativeUiLayers;
    struct Layer
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source;
    };
    struct ObservedShader
    {
        Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
        NativeUiShaderRole role{};
    };

    [[nodiscard]] bool CanUse() const noexcept;
    [[nodiscard]] bool FrameAdmissionCurrent(const NativeUiFrame& frame) const noexcept;
    [[nodiscard]] bool CanBeginFrame(const NativeUiFrame& frame, NativeUiCaptureScope scope) const noexcept;
    [[nodiscard]] bool CommitFrame(const NativeUiFrame& frame) noexcept;
    [[nodiscard]] bool PresentationSceneCurrent() const noexcept;
    [[nodiscard]] bool QualifiedPresentationScene(const ScenePassRenderer& scenes) const noexcept;
    [[nodiscard]] bool MissingRequiredLayer() const noexcept;
    [[nodiscard]] bool CanCapture(const QualifiedNativeUiDraw& draw) const noexcept;
    [[nodiscard]] bool OwnedLayerAlias(ID3D11Texture2D* texture) const noexcept;
    [[nodiscard]] bool QualifiedTarget(ID3D11RenderTargetView* target) const noexcept;
    [[nodiscard]] bool QualifiedDraw(const QualifiedNativeUiDraw& draw) const noexcept;
    [[nodiscard]] NativeUiShaderRole ObservedRole(ID3D11PixelShader* shader) const noexcept;
    [[nodiscard]] SceneDrawOutcome SubmitDraw(std::size_t index, NativeUiShaderRole role,
        FiniteDrawInvocation invocation) noexcept;
    [[nodiscard]] bool PacketReady(std::uint64_t frame, std::uint64_t epoch) const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* CompletedSource(std::size_t index) const noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> presentationTarget_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    std::array<Layer, 2> layers_;
    std::array<ObservedShader, 32> observed_;
    std::size_t observedCount_{};
    ScenePassConfiguration configuration_;
    ScenePassRenderer* scenes_{};
    const NativePresentationProxy* presentationProxy_{};
    std::thread::id thread_;
    std::uint64_t frame_{};
    std::uint64_t lastFrame_{};
    std::uint64_t epoch_{};
    std::array<NativeUiLayerRequirement, 2> requirements_{};
    std::array<bool, 2> captured_{};
    bool failed_{};
    bool invoking_{};
    bool developerReference_{};
};

}  // namespace fo4hdr
