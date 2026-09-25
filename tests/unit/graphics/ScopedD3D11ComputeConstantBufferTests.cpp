// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "d3d11/ScopedD3D11ComputeConstantBuffer.hpp"
#include "TestHarness.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

namespace fo4hdr::tests {
namespace {

using Microsoft::WRL::ComPtr;

struct WarpContext
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
};

[[nodiscard]] bool CreateWarpContext(WarpContext& result)
{
    D3D_FEATURE_LEVEL featureLevel{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &result.device, &featureLevel, &result.context)))
        return false;
    return SUCCEEDED(result.context.As(&result.context1));
}

[[nodiscard]] ComPtr<ID3D11Buffer> CreateConstants(ID3D11Device* device)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = 512;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> result;
    if (FAILED(device->CreateBuffer(&description, nullptr, &result))) result.Reset();
    return result;
}

[[nodiscard]] ULONG ReferenceCount(IUnknown* value)
{
    const ULONG incremented = value->AddRef();
    value->Release();
    return incremented - 1;
}

void VerifyRangedContext1Restoration(WarpContext& warp)
{
    auto original = CreateConstants(warp.device.Get());
    auto replacement = CreateConstants(warp.device.Get());
    FO4HDR_CHECK(original && replacement);
    if (!original || !replacement) return;

    constexpr UINT slot = 3;
    constexpr UINT originalFirst = 16;
    constexpr UINT originalCount = 16;
    ID3D11Buffer* originalRaw = original.Get();
    warp.context1->CSSetConstantBuffers1(slot, 1, &originalRaw, &originalFirst,
        &originalCount);
    const ULONG boundReferenceCount = ReferenceCount(original.Get());

    {
        ScopedD3D11ComputeConstantBuffer saved;
        saved.Capture(warp.context.Get(), warp.context1.Get(), slot);
        FO4HDR_CHECK(ReferenceCount(original.Get()) == boundReferenceCount + 1);
        ID3D11Buffer* replacementRaw = replacement.Get();
        constexpr UINT replacementFirst = 0;
        constexpr UINT replacementCount = 16;
        warp.context1->CSSetConstantBuffers1(slot, 1, &replacementRaw,
            &replacementFirst, &replacementCount);
    }

    ComPtr<ID3D11Buffer> restored;
    UINT restoredFirst{};
    UINT restoredCount{};
    warp.context1->CSGetConstantBuffers1(slot, 1, &restored, &restoredFirst,
        &restoredCount);
    FO4HDR_CHECK(restored.Get() == original.Get());
    FO4HDR_CHECK(restoredFirst == originalFirst);
    FO4HDR_CHECK(restoredCount == originalCount);
    restored.Reset();
    FO4HDR_CHECK(ReferenceCount(original.Get()) == boundReferenceCount);
}

void VerifyOrdinaryFallbackAndRecapture(WarpContext& warp)
{
    auto first = CreateConstants(warp.device.Get());
    auto second = CreateConstants(warp.device.Get());
    auto replacement = CreateConstants(warp.device.Get());
    const bool created = first && second && replacement;
    FO4HDR_CHECK(created);
    if (!created) return;

    constexpr UINT firstSlot = 3;
    constexpr UINT secondSlot = 4;
    ID3D11Buffer* firstRaw = first.Get();
    ID3D11Buffer* secondRaw = second.Get();
    warp.context->CSSetConstantBuffers(firstSlot, 1, &firstRaw);
    warp.context->CSSetConstantBuffers(secondSlot, 1, &secondRaw);
    const ULONG firstBoundReferenceCount = ReferenceCount(first.Get());
    const ULONG secondBoundReferenceCount = ReferenceCount(second.Get());

    ScopedD3D11ComputeConstantBuffer saved;
    saved.Capture(warp.context.Get(), nullptr, firstSlot);
    FO4HDR_CHECK(ReferenceCount(first.Get()) == firstBoundReferenceCount + 1);
    ID3D11Buffer* replacementRaw = replacement.Get();
    warp.context->CSSetConstantBuffers(firstSlot, 1, &replacementRaw);

    // Recapture restores the first slot before taking ownership of the second.
    saved.Capture(warp.context.Get(), nullptr, secondSlot);
    ComPtr<ID3D11Buffer> restoredFirst;
    warp.context->CSGetConstantBuffers(firstSlot, 1, &restoredFirst);
    FO4HDR_CHECK(restoredFirst.Get() == first.Get());
    restoredFirst.Reset();
    FO4HDR_CHECK(ReferenceCount(first.Get()) == firstBoundReferenceCount);

    warp.context->CSSetConstantBuffers(secondSlot, 1, &replacementRaw);
    saved.Restore();
    saved.Restore();
    ComPtr<ID3D11Buffer> restoredSecond;
    warp.context->CSGetConstantBuffers(secondSlot, 1, &restoredSecond);
    FO4HDR_CHECK(restoredSecond.Get() == second.Get());
    restoredSecond.Reset();
    FO4HDR_CHECK(ReferenceCount(second.Get()) == secondBoundReferenceCount);
}

} // namespace

void RunScopedD3D11ComputeConstantBufferTests()
{
    WarpContext warp;
    FO4HDR_CHECK(CreateWarpContext(warp));
    if (!warp.context1) return;

    VerifyRangedContext1Restoration(warp);
    warp.context->ClearState();
    VerifyOrdinaryFallbackAndRecapture(warp);
    warp.context->ClearState();
}

} // namespace fo4hdr::tests
