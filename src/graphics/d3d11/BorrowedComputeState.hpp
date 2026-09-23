// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ScopedD3D11ComputeConstantBuffer.hpp"

#include <wrl/client.h>

#include <array>

namespace fo4hdr {

class BorrowedComputeState
{
public:
    explicit BorrowedComputeState(ID3D11DeviceContext* context,
        ID3D11DeviceContext1* context1,
        ID3D11Resource* destination = nullptr) noexcept;

    ~BorrowedComputeState() noexcept;

    BorrowedComputeState(const BorrowedComputeState&) = delete;
    BorrowedComputeState& operator=(const BorrowedComputeState&) = delete;

private:
    void UnbindOutputs() noexcept;

    [[nodiscard]] static bool DestinationHasSrvs(ID3D11Resource* destination) noexcept;

    [[nodiscard]] UINT RenderTargetCount() const noexcept;

    void ReleaseGraphicsSrvs() noexcept;

    void UnbindDestinationAliases(UINT stage, ID3D11Resource* destination) noexcept;

    ID3D11DeviceContext* context_{};
    UINT uavCount_{};
    UINT csSrvCount_{};
    bool destinationHasSrvs_{};
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs_{};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_;
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> omUavs_{};
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> csUavs_{};
    // GetShaderResources initializes only the ranges selected for capture.
    std::array<std::array<ID3D11ShaderResourceView*,
        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>, 6> srvs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> classes_{};
    UINT classCount_{D3D11_SHADER_MAX_INTERFACES};
    fo4hdr::ScopedD3D11ComputeConstantBuffer constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate_;
    BOOL predicateValue_{};
};

} // namespace fo4hdr
