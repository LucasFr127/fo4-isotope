// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "d3d11/ContextQuerySafety.hpp"
#include "RenoDxShaderIdentity.hpp"
#include "Fallout4HDR/output/RenderPipelineContract.hpp"
#include <d3d11.h>
#include <cstdint>

namespace fo4hdr {

struct DrawObservation
{
    bool immediate{};
    std::uint64_t generation{};
    ContextQuerySafety computeQueries{};
    ID3D11RenderTargetView* currentPrimaryRtv{};
    ID3D11RenderTargetView* currentSecondaryRtv{};
    bool currentSinglePresentationTarget{};
    RenoDxShaderPass currentPass{};
    bool currentPassKnown{};
    bool currentReplacementActive{};
    ID3D11PixelShader* expectedBoundShader{};
    PipelinePassRequirement currentRequirement{PipelinePassRequirement::untracked};
};
using RenderTargetSetter = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
DrawObservation* FindDrawObservation(ID3D11DeviceContext*) noexcept;
RenderTargetSetter OriginalRenderTargetSetter(ID3D11DeviceContext*) noexcept;
bool HasOriginalPixelShaderSetter(ID3D11DeviceContext*) noexcept;
void BindOriginalPixelShader(ID3D11DeviceContext*, ID3D11PixelShader*) noexcept;

} // namespace fo4hdr
