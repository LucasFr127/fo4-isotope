// SPDX-License-Identifier: GPL-3.0-or-later

#include "NativeSceneAssociation.hpp"

#include <array>

namespace fo4hdr {
namespace {

using Microsoft::WRL::ComPtr;

ComPtr<IUnknown> Identity(IUnknown* object) noexcept
{
    ComPtr<IUnknown> result;
    if (object != nullptr && FAILED(object->QueryInterface(IID_PPV_ARGS(&result)))) result.Reset();
    return result;
}

bool SameObject(IUnknown* left, IUnknown* right) noexcept
{
    const auto first = Identity(left);
    const auto second = Identity(right);
    return first && first.Get() == second.Get();
}

bool SameDevice(ID3D11DeviceChild* object, ID3D11Device* device) noexcept
{
    if (object == nullptr || device == nullptr) return false;
    ComPtr<ID3D11Device> owner;
    object->GetDevice(&owner);
    return SameObject(owner.Get(), device);
}

bool StrictLayout(const D3D11_TEXTURE2D_DESC& value, DXGI_FORMAT format) noexcept
{
    const std::array layout{value.MipLevels, value.ArraySize,
        value.SampleDesc.Count, value.SampleDesc.Quality, value.CPUAccessFlags, value.MiscFlags};
    constexpr std::array expected{1U, 1U, 1U, 0U, 0U, 0U};
    return layout == expected && value.Width != 0 && value.Height != 0 &&
        value.Format == format && value.Usage == D3D11_USAGE_DEFAULT &&
        value.BindFlags == (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
}

bool MatchesStorage(ID3D11Device* device, ID3D11Texture2D* texture,
    const NativeSceneAttribution& record) noexcept
{
    if (!SameDevice(texture, device)) return false;
    D3D11_TEXTURE2D_DESC actual{};
    texture->GetDesc(&actual);
    return StrictLayout(actual, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
        actual.Width == record.width && actual.Height == record.height;
}

bool ValidRecord(const NativeSceneAttribution& record, std::uint64_t generation) noexcept
{
    return generation != 0 && record.creationGeneration == generation &&
        record.version == 1 && record.byteSize == sizeof(record) &&
        record.originalFormat == DXGI_FORMAT_R11G11B10_FLOAT &&
        record.publishedFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool ViewResourceMatches(ID3D11View* view, ID3D11Texture2D* texture) noexcept
{
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    return SameObject(resource.Get(), texture);
}

bool SceneViewMatches(ID3D11Device* device, ID3D11Texture2D* texture, ID3D11View* view) noexcept
{
    return SameDevice(view, device) && ViewResourceMatches(view, texture);
}

template<class Description, class View>
Description ViewDescription(View* view) noexcept
{
    Description description{};
    view->GetDesc(&description);
    return description;
}

bool SceneSrvDescription(const D3D11_SHADER_RESOURCE_VIEW_DESC& description) noexcept
{
    return description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
        description.Texture2D.MostDetailedMip == 0 && description.Texture2D.MipLevels == 1;
}

bool SceneRtvDescription(const D3D11_RENDER_TARGET_VIEW_DESC& description) noexcept
{
    return description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        description.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && description.Texture2D.MipSlice == 0;
}

NativeSceneAttribution Attribution(NativeSceneCreation creation) noexcept
{
    return {1, sizeof(NativeSceneAttribution), creation.generation,
        creation.original.Width, creation.original.Height,
        DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
}

bool WriteAttribution(NativeSceneCreation creation, ID3D11Texture2D* texture) noexcept
{
    const auto expected = Attribution(creation);
    if (texture->SetPrivateData(kNativeSceneAttributionGuid, sizeof(expected), &expected) != S_OK)
        return false;
    NativeSceneAttribution observed{};
    return ReadNativeSceneAttribution(creation.device, texture, creation.generation, observed) &&
        observed == expected;
}

bool QualifiedCandidate(NativeSceneCreation creation, NativeSceneViews views) noexcept
{
    if (creation.generation == 0 || !StrictLayout(creation.original, DXGI_FORMAT_R11G11B10_FLOAT))
        return false;
    if (!MatchesStorage(creation.device, views.texture, Attribution(creation))) return false;
    return EquivalentSceneSrv(creation.device, views.texture, views.source) &&
        EquivalentSceneRtv(creation.device, views.texture, views.target);
}

}  // namespace

bool ReadNativeSceneAttribution(ID3D11Device* device, ID3D11Texture2D* texture,
    std::uint64_t generation, NativeSceneAttribution& record) noexcept
{
    record = {};
    if (texture == nullptr) return false;
    NativeSceneAttribution candidate{};
    UINT size = sizeof(candidate);
    if (texture->GetPrivateData(kNativeSceneAttributionGuid, &size, &candidate) != S_OK) return false;
    if (size != sizeof(candidate) || !ValidRecord(candidate, generation)) return false;
    if (!MatchesStorage(device, texture, candidate)) return false;
    record = candidate;
    return true;
}

bool EquivalentSceneSrv(ID3D11Device* device, ID3D11Texture2D* texture,
    ID3D11ShaderResourceView* view) noexcept
{
    return SceneViewMatches(device, texture, view) &&
        SceneSrvDescription(ViewDescription<D3D11_SHADER_RESOURCE_VIEW_DESC>(view));
}

bool EquivalentSceneRtv(ID3D11Device* device, ID3D11Texture2D* texture,
    ID3D11RenderTargetView* view) noexcept
{
    return SceneViewMatches(device, texture, view) &&
        SceneRtvDescription(ViewDescription<D3D11_RENDER_TARGET_VIEW_DESC>(view));
}

bool NativeSceneTexture::Prepare(NativeSceneCreation creation, NativeSceneViews views) noexcept
{
    creation_ = {};
    texture_.Reset();
    source_.Reset();
    target_.Reset();
    if (!QualifiedCandidate(creation, views)) return false;
    if (!WriteAttribution(creation, views.texture)) return false;
    texture_ = views.texture;
    source_ = views.source;
    target_ = views.target;
    creation_ = creation;
    return true;
}

}  // namespace fo4hdr
