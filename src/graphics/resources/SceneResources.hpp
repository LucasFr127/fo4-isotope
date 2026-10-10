// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "FO4Isotope/output/RenderPipelineContract.hpp"
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace fo4hdr {

struct SceneView
{
    Microsoft::WRL::ComPtr<IUnknown> resourceIdentity;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
};

inline constexpr std::size_t kMaximumSceneViews = 8;

struct SceneResources
{
    std::array<SceneView, kMaximumSceneViews> sceneViews{};
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> currentScene;
    LinearColorDomain currentSceneDomain{LinearColorDomain::unknown};
    std::size_t sceneViewCursor{};
    bool temporalPassObserved{};
    bool temporalSceneReady{};
    bool videoSceneReady{};
    bool productionOperatorApplied{};
};
extern SceneResources g_scene;
struct DrawObservation;

bool IsQualifiedSceneTarget(ID3D11RenderTargetView*) noexcept;
bool IsFirstFp16RenderTarget(ID3D11RenderTargetView*) noexcept;
bool SameResourceIdentity(ID3D11ShaderResourceView*, ID3D11ShaderResourceView*) noexcept;
std::uintptr_t CanonicalResourceIdentity(ID3D11Resource*) noexcept;
bool SameResourceObjects(ID3D11Resource*, ID3D11Resource*) noexcept;
bool RenderTargetMatchesResource(ID3D11RenderTargetView*, ID3D11Resource*) noexcept;
bool IsPresentationProxyTarget(ID3D11RenderTargetView*) noexcept;
bool CurrentPassConsumesSelectedScene(ID3D11DeviceContext*, const DrawObservation&) noexcept;
ID3D11ShaderResourceView* AcquireSceneView(ID3D11RenderTargetView*) noexcept;
void ResetSceneResources() noexcept;

} // namespace fo4hdr
