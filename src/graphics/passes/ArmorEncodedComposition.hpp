// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace fo4hdr {

enum class ArmorCompositionDomain
{
    unknown,
    linearBt2020UiRelative,
    encodedBt709
};

enum class ArmorCompositionScope
{
    unavailable,
    qualifiedExclusive
};

enum class ArmorCompositionOwner
{
    unknown,
    powerArmor
};

struct ArmorEncodedToken
{
    ArmorCompositionOwner owner{};
    std::thread::id ownerThread;
    std::uint64_t resourceGeneration{};
    std::uint64_t frameGeneration{};
    std::uint64_t serial{};
    ArmorCompositionScope scope{};
    bool postAa{};

    bool operator==(const ArmorEncodedToken&) const noexcept = default;
};

struct ArmorEncodedConfiguration
{
    ID3D11RenderTargetView* renderTarget{};
    ID3D11UnorderedAccessView* target{};
    std::uint64_t resourceGeneration{};
    float peakUiRelative{};
    ArmorCompositionDomain domain{};
};

// Serialized owner-thread calls on an exclusively qualified immediate context.
// The caller proves completed post-AA linear input and no active queries,
// command lists or concurrent/reentrant mutation. Only RGB-native draws may occur between
// Begin and End; a failed End or escaped scope makes the image unusable.
class ArmorEncodedComposition
{
public:
    ArmorEncodedComposition() noexcept = default;
    ~ArmorEncodedComposition() noexcept = default;
    ArmorEncodedComposition(const ArmorEncodedComposition&) = delete;
    ArmorEncodedComposition& operator=(const ArmorEncodedComposition&) = delete;

    [[nodiscard]] bool Initialize(ID3D11Device* device,
        ID3D11DeviceContext* context, ArmorEncodedConfiguration configuration) noexcept;
    [[nodiscard]] bool Begin(const ArmorEncodedToken& token) noexcept;
    [[nodiscard]] bool End(const ArmorEncodedToken& token) noexcept;
    void Invalidate() noexcept;
    // Retire before releasing context qualification or resizing the resource.
    [[nodiscard]] bool Retire() noexcept;
    [[nodiscard]] ArmorCompositionDomain Domain() const noexcept;

private:
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] bool Qualified(const ArmorEncodedToken& token) const noexcept;
    [[nodiscard]] bool CanDecode(const ArmorEncodedToken& token) const noexcept;
    [[nodiscard]] bool Dispatch(std::size_t direction) noexcept;

    std::thread::id ownerThread_;
    ArmorEncodedConfiguration configuration_;
    ArmorEncodedToken activeToken_;
    ArmorCompositionDomain domain_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> target_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 2> constants_;
    UINT width_{};
    UINT height_{};
};

}  // namespace fo4hdr
