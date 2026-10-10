// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
//
// Fallout 4 shader identities and tone/LUT seams use the pinned RenoDX facts.
// The exact-backbuffer FP16 proxy and UI-relative transport adapt
// clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/
// src/games/fallout4/addon.cpp and src/mods/swapchain_v2.hpp.
// RenoDX material retains its MIT permission in THIRD_PARTY_NOTICES.md.
// Native hooks, generation ownership, settings, grading integration and direct
// display output are project implementations of the Fallout 4 and DXGI contracts.

#include "SceneResources.hpp"
#include "PresentationResources.hpp"
#include "HdrResourcePolicy.hpp"
#include "../DrawObservation.hpp"
#include "../ShaderRegistry.hpp"
#include "../GraphicsDiagnostics.hpp"
#include "../RendererLifetime.hpp"
#include "../LiveSettings.hpp"
#include "../FrameTrace.hpp"
#include <algorithm>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

SceneResources g_scene;

bool SceneTextureLayoutMatches(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    return description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        description.Width != 0 && description.Height != 0 &&
        description.ArraySize == 1 && description.SampleDesc.Count == 1 &&
        (description.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 &&
        (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
}
bool QualifiedSceneDescription(const D3D11_TEXTURE2D_DESC& description) noexcept
{
    if (!SceneTextureLayoutMatches(description)) return false;
    return ShouldPromoteSceneResourceToFp16(
        {
            .format = SceneResourceFormat::r11g11b10Float,
            .width = description.Width,
            .height = description.Height
        },
        g_presentation.width,
        g_presentation.height);
}

bool SceneTargetAvailable(ID3D11RenderTargetView* rtv) noexcept
{
    return rtv != nullptr && g_presentation.width != 0 && g_presentation.height != 0;
}

[[nodiscard]] bool IsQualifiedSceneTarget(
    ID3D11RenderTargetView* rtv) noexcept
{
    if (!SceneTargetAvailable(rtv))
        return false;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    rtv->GetResource(&resource);
    if (!resource || FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return QualifiedSceneDescription(description);
}

std::uintptr_t CanonicalResourceIdentity(ID3D11Resource* resource) noexcept
{
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&identity)))) return 0;
    return reinterpret_cast<std::uintptr_t>(identity.Get());
}

bool SameResourceObjects(ID3D11Resource* left, ID3D11Resource* right) noexcept
{
    if (!left || !right) return false;
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity))) &&
        leftIdentity.Get() == rightIdentity.Get();
}
[[nodiscard]] bool SameResourceIdentity(
    ID3D11ShaderResourceView* left,
    ID3D11ShaderResourceView* right) noexcept
{
    if (left == nullptr || right == nullptr) return false;
    ComPtr<ID3D11Resource> leftResource;
    ComPtr<ID3D11Resource> rightResource;
    left->GetResource(&leftResource);
    right->GetResource(&rightResource);
    if (!leftResource || !rightResource) return false;
    return SameResourceObjects(leftResource.Get(), rightResource.Get());
}

[[nodiscard]] bool RenderTargetMatchesResource(
    ID3D11RenderTargetView* view,
    ID3D11Resource* expected) noexcept
{
    if (view == nullptr || expected == nullptr) return false;
    ComPtr<ID3D11Resource> actual;
    view->GetResource(&actual);
    if (!actual) return false;
    return SameResourceObjects(actual.Get(), expected);
}

bool IsFirstFp16RenderTarget(ID3D11RenderTargetView* view) noexcept
{
    if (view == nullptr) return false;
    D3D11_RENDER_TARGET_VIEW_DESC description{};
    view->GetDesc(&description);
    return description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        description.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D &&
        description.Texture2D.MipSlice == 0;
}

[[nodiscard]] bool IsPresentationProxyTarget(ID3D11RenderTargetView* view) noexcept
{
    return IsFirstFp16RenderTarget(view) &&
        RenderTargetMatchesResource(view, g_presentation.presentationProxy.Get());
}

[[nodiscard]] bool CurrentPassConsumesSelectedScene(
    ID3D11DeviceContext* context,
    const DrawObservation& record) noexcept
{
    if (context == nullptr || g_scene.currentScene == nullptr)
        return false;
    const auto* descriptor = FindShaderDescriptor(record.currentPass);
    if (descriptor == nullptr) return false;
    ComPtr<ID3D11ShaderResourceView> input;
    context->PSGetShaderResources(descriptor->sceneInputSlot, 1, &input);
    return SameResourceIdentity(input.Get(), g_scene.currentScene.Get());
}

const SceneView* FindCachedSceneView(IUnknown* identity) noexcept
{
    for (auto& entry : g_scene.sceneViews)
    {
        if (entry.resourceIdentity.Get() == identity)
            return &entry;
    }
    return nullptr;
}

ID3D11ShaderResourceView* StoreSceneView(ComPtr<IUnknown>& resourceIdentity,
    ComPtr<ID3D11ShaderResourceView>& srv) noexcept
{
    for (auto& entry : g_scene.sceneViews)
    {
        if (entry.resourceIdentity == nullptr)
        {
            entry.resourceIdentity = resourceIdentity;
            entry.srv = std::move(srv);
            return entry.srv.Get();
        }
    }
    // The active scene retains its own SRV, so bounded round-robin cache
    // replacement is safe across long resource-churn sessions and raw pointer
    // reuse cannot alias a stale entry.
    auto& replacement = g_scene.sceneViews[
        g_scene.sceneViewCursor++ % g_scene.sceneViews.size()];
    replacement.resourceIdentity = std::move(resourceIdentity);
    replacement.srv = std::move(srv);
    return replacement.srv.Get();
}

bool ResolveSceneResource(const ComPtr<ID3D11Resource>& resource, ComPtr<IUnknown>& identity,
    ComPtr<ID3D11Texture2D>& texture) noexcept
{
    return resource && SUCCEEDED(resource.As(&identity)) && SUCCEEDED(resource.As(&texture));
}

bool CreateSceneView(ID3D11Texture2D* texture, ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11ShaderResourceView>& srv) noexcept
{
    texture->GetDevice(&device);
    return device && SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, &srv));
}

[[nodiscard]] ID3D11ShaderResourceView* AcquireSceneView(
    ID3D11RenderTargetView* rtv) noexcept
{
    if (rtv == nullptr) return nullptr;
    if (RenderTargetMatchesResource(rtv, g_presentation.presentationProxy.Get()))
        return IsFirstFp16RenderTarget(rtv) ? g_presentation.presentationProxySrv.Get() : nullptr;

    ComPtr<ID3D11Resource> resource;
    rtv->GetResource(&resource);
    ComPtr<IUnknown> resourceIdentity;
    ComPtr<ID3D11Texture2D> texture;
    if (!ResolveSceneResource(resource, resourceIdentity, texture))
        return nullptr;
    if (const auto* cached = FindCachedSceneView(resourceIdentity.Get())) return cached->srv.Get();
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (!QualifiedSceneDescription(description)) return nullptr;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (!CreateSceneView(texture.Get(), device, srv))
        return nullptr;
    return StoreSceneView(resourceIdentity, srv);
}
void ResetSceneResources() noexcept
{
    g_scene.currentScene.Reset();
    g_scene.currentSceneDomain = LinearColorDomain::unknown;
    g_presentation.presentationProxyDomain = LinearColorDomain::unknown;
    g_evidence.colorDomain.store(
        static_cast<std::uint32_t>(LinearColorDomain::unknown),
        std::memory_order_release);
    g_scene.temporalPassObserved = false;
    g_scene.temporalSceneReady = false;
    g_scene.videoSceneReady = false;
    g_scene.productionOperatorApplied = false;
    for (auto& entry : g_scene.sceneViews)
    {
        entry.srv.Reset();
        entry.resourceIdentity.Reset();
    }
    g_scene.sceneViewCursor = 0;
}

} // namespace fo4hdr
