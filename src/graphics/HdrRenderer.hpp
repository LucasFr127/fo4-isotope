// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RendererLifetime.hpp"
#include "Fallout4HDR/output/RenderPipelineContract.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

namespace fo4hdr {

class ScenePassRenderer;

class SceneTextureHandle
{
public:
    SceneTextureHandle() noexcept = default;

private:
    friend class ScenePassRenderer;
    const ScenePassRenderer* owner_{};
    std::uint64_t epoch_{};
    std::size_t slot_{};
};

struct SceneTextureViews
{
    ID3D11Texture2D* texture{};
    ID3D11ShaderResourceView* shaderResource{};
    ID3D11RenderTargetView* renderTarget{};
};

struct FiniteDrawInvocation
{
    // The caller issues exactly one finite draw and does not mutate other state.
    void (*draw)(ID3D11DeviceContext*, void*){};
    void* argument{};
};

enum class SceneDrawOutcome : std::uint8_t
{
    invalidInvocation,
    forwarded,
    replaced,
    callbackFailed
};

enum class SceneColorDomain : std::uint8_t
{
    bt709Scene = 0,
    bt2020UiRelative = 1
};

// One exclusively owned immediate context, one initialization thread and one
// nonrecycled generation. Handles/views expire at Retire or owner destruction;
// do not retain them across destruction and reconstruction at the same address.
class ScenePassRenderer
{
public:
    static constexpr std::size_t sceneTextureCapacity = 8;
    static constexpr std::size_t observedShaderCapacity = 64;
    static constexpr std::size_t replacementProgramCount = 9;

    ScenePassRenderer() noexcept = default;
    ~ScenePassRenderer() noexcept;
    ScenePassRenderer(const ScenePassRenderer&) = delete;
    ScenePassRenderer& operator=(const ScenePassRenderer&) = delete;

    [[nodiscard]] bool Initialize(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext, UINT width, UINT height) noexcept;
    [[nodiscard]] bool Retire() noexcept;
    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] bool MatchesContext(ID3D11Device* device,
        ID3D11DeviceContext* immediateContext) const noexcept;

    HRESULT CreateObservedPixelShader(std::span<const std::byte> bytecode,
        ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) noexcept;
    HRESULT CreateSceneTexture(const D3D11_TEXTURE2D_DESC& original,
        const D3D11_SUBRESOURCE_DATA* initialData, SceneTextureHandle& handle) noexcept;
    [[nodiscard]] SceneTextureViews Views(SceneTextureHandle handle) const noexcept;

    [[nodiscard]] bool BeginFrame() noexcept;
    [[nodiscard]] bool EndFrame() noexcept;
    // Query at consumption: retained handles remain storage-valid after lineage expires.
    [[nodiscard]] SceneTextureHandle CurrentScene() const noexcept;
    [[nodiscard]] SceneColorDomain CurrentSceneDomain() const noexcept;
    // Zero denotes a closed or unavailable frame; completed layers use this token.
    [[nodiscard]] std::uint64_t FrameGeneration() const noexcept;
    // Report every external write through a borrowed alias before it occurs.
    [[nodiscard]] bool InvalidateScene(SceneTextureHandle handle) noexcept;

    // Direct execution consumes the caller's independently qualified descriptor;
    // it does not establish an original-bytecode match.
    // FXAA requires the positive-HDR source contract; video requires completed
    // authored BGR codes, and publishes a separate UI-relative BT.2020 domain.
    // Temporal metadata/history require R16G16B16A16_FLOAT or R32G32B32A32_FLOAT
    // views; the caller separately qualifies their native float4 semantics.
    [[nodiscard]] SceneDrawOutcome Execute(const RenoDxShaderDescriptor& descriptor,
        SceneTextureHandle input, SceneTextureHandle destination,
        FiniteDrawInvocation invocation) noexcept;
    [[nodiscard]] SceneDrawOutcome ExecuteObserved(SceneTextureHandle input,
        SceneTextureHandle destination, FiniteDrawInvocation invocation) noexcept;

private:
    struct SceneTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResource;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget;
        bool toneProduced{};
        SceneColorDomain domain{SceneColorDomain::bt709Scene};
    };

    struct ObservedShader
    {
        Microsoft::WRL::ComPtr<ID3D11PixelShader> original;
        const RenoDxShaderDescriptor* descriptor{};
    };

    struct DrawLineage
    {
        bool consumesSelected{};
        bool toneInput{};
        bool selectionExisted{};
        std::uint8_t writes{};
        SceneColorDomain domain{SceneColorDomain::bt709Scene};
    };

    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] bool CanInvoke(FiniteDrawInvocation invocation) const noexcept;
    [[nodiscard]] const SceneTexture* Resolve(SceneTextureHandle handle) const noexcept;
    [[nodiscard]] const RenoDxShaderDescriptor* BoundDescriptor() const noexcept;
    [[nodiscard]] bool PromotableScene(const D3D11_TEXTURE2D_DESC& original) const noexcept;
    HRESULT PrepareSceneTexture(const D3D11_TEXTURE2D_DESC& original,
        SceneTexture& prepared) noexcept;
    [[nodiscard]] bool BoundSceneViewsMatch(const RenoDxShaderDescriptor& descriptor,
        const SceneTexture& source, const SceneTexture& target) const noexcept;
    [[nodiscard]] bool BoundTemporalViewsMatch(const RenoDxShaderDescriptor& descriptor,
        const SceneTexture& source, const SceneTexture& target) const noexcept;
    [[nodiscard]] std::uint8_t BoundWriteMask() const noexcept;
    void ClearLineage() noexcept;
    void InvalidateSlot(std::size_t slot) noexcept;
    void InvalidateWrites(std::uint8_t writes) noexcept;
    [[nodiscard]] std::uint8_t WriteMaskForResource(ID3D11Resource* resource) const noexcept;
    [[nodiscard]] DrawLineage CaptureLineage(SceneTextureHandle input) const noexcept;
    void PublishLineage(const RenoDxShaderDescriptor& descriptor,
        SceneTextureHandle destination, const DrawLineage& lineage) noexcept;
    void CompleteLineage(const RenoDxShaderDescriptor* descriptor,
        SceneTextureHandle destination, SceneDrawOutcome outcome,
        const DrawLineage& lineage) noexcept;
    [[nodiscard]] SceneDrawOutcome InvokeDraw(ID3D11PixelShader* replacement,
        FiniteDrawInvocation invocation) noexcept;
    [[nodiscard]] ID3D11PixelShader* QualifiedReplacement(
        const RenoDxShaderDescriptor* descriptor, SceneTextureHandle input,
        SceneTextureHandle destination) const noexcept;
    [[nodiscard]] SceneDrawOutcome Invoke(const RenoDxShaderDescriptor* descriptor,
        SceneTextureHandle input, SceneTextureHandle destination,
        FiniteDrawInvocation invocation) noexcept;

    RendererLifetime lifetime_;
    std::thread::id ownerThread_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, replacementProgramCount> replacements_;
    std::array<SceneTexture, sceneTextureCapacity> scenes_;
    std::array<ObservedShader, observedShaderCapacity> observed_;
    std::size_t sceneCount_{};
    std::size_t observedCount_{};
    UINT width_{};
    UINT height_{};
    bool invoking_{};
    SceneTextureHandle selectedScene_;
    bool frameOpen_{};
    std::uint64_t frameGeneration_{};
};

}  // namespace fo4hdr
