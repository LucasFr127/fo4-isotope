// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>

namespace fo4hdr {

inline constexpr GUID kNativeSceneAttributionGuid{
    0x09ba3918, 0xdb6a, 0x4a02, {0xa4, 0x95, 0x6f, 0x87, 0x8d, 0xb7, 0x31, 0x53}};

// Texture-lifetime attribution only: none of these fields qualifies a frame.
struct NativeSceneAttribution
{
    std::uint32_t version{};
    std::uint32_t byteSize{};
    std::uint64_t creationGeneration{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t originalFormat{};
    std::uint32_t publishedFormat{};
    [[nodiscard]] bool operator==(const NativeSceneAttribution&) const noexcept = default;
};

static_assert(sizeof(NativeSceneAttribution) == 32);
static_assert(offsetof(NativeSceneAttribution, creationGeneration) == 8);
static_assert(offsetof(NativeSceneAttribution, width) == 16);
static_assert(offsetof(NativeSceneAttribution, originalFormat) == 24);

struct NativeSceneCreation
{
    ID3D11Device* device{};
    D3D11_TEXTURE2D_DESC original{};
    std::uint64_t generation{};
};

struct NativeSceneViews
{
    ID3D11Texture2D* texture{};
    ID3D11ShaderResourceView* source{};
    ID3D11RenderTargetView* target{};
};

[[nodiscard]] bool ReadNativeSceneAttribution(ID3D11Device* device,
    ID3D11Texture2D* texture, std::uint64_t generation, NativeSceneAttribution& record) noexcept;
[[nodiscard]] bool EquivalentSceneSrv(ID3D11Device* device,
    ID3D11Texture2D* texture, ID3D11ShaderResourceView* view) noexcept;
[[nodiscard]] bool EquivalentSceneRtv(ID3D11Device* device,
    ID3D11Texture2D* texture, ID3D11RenderTargetView* view) noexcept;

// Views are prepared by the caller's captured native creation route while private.
// Recheck native admission after Prepare and immediately before slot publication.
class NativeSceneTexture
{
public:
    [[nodiscard]] bool Prepare(NativeSceneCreation creation, NativeSceneViews views) noexcept;
    [[nodiscard]] bool PreparedFor(ID3D11Device* device) const noexcept
    {
        return device != nullptr && creation_.device == device && creation_.generation != 0;
    }
    [[nodiscard]] const NativeSceneCreation& Creation() const noexcept { return creation_; }
    [[nodiscard]] NativeSceneViews Views() const noexcept
    {
        return {texture_.Get(), source_.Get(), target_.Get()};
    }

private:
    friend class ScenePassRenderer;
    NativeSceneCreation creation_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
};

}  // namespace fo4hdr
