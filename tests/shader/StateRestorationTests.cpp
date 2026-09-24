// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderTestSuite.hpp"
#include "d3d11/GpuHarness.hpp"
#include <d3d11_1.h>
#include "reference/ShaderReference.hpp"
#include "Fallout4HDR/TestShaders.generated.hpp"
#include "d3d11/BorrowedComputeState.hpp"
#include <array>
#include <cstdint>
#include "passes/NativeReferenceGradePass.hpp"
#include <cmath>
#include "d3d11/ScopedD3D11PixelBindings.hpp"
#include "d3d11/ContextQuerySafety.hpp"
#include <cstddef>

namespace fo4hdr::tests::gpu {

template <class DispatchOutput>
void VerifyPresentationUavAlias(ID3D11DeviceContext& context, UINT lastUav,
    Texture& destination, const DispatchOutput& dispatch)
{
    for (bool outputMerger : {false, true}) {
        auto* originalDestination = destination.uav.Get();
        if (outputMerger)
            context.OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
                lastUav, 1, &originalDestination, nullptr);
        else
            context.CSSetUnorderedAccessViews(lastUav, 1, &originalDestination, nullptr);
        dispatch();
        ComPtr<ID3D11UnorderedAccessView> restoredDestination;
        if (outputMerger)
            context.OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
                lastUav, 1, &restoredDestination);
        else
            context.CSGetUnorderedAccessViews(lastUav, 1, &restoredDestination);
        FO4HDR_WARP_CHECK(restoredDestination.Get() == originalDestination);
        context.ClearState();
    }
}

template <class DispatchOutput>
void VerifyPresentationAllTargets(ID3D11Device& gpu, ID3D11DeviceContext& context,
    ID3D11DepthStencilView* depthView, const DispatchOutput& dispatch)
{
    std::array<Texture, 8> renderTargets;
    std::array<ID3D11RenderTargetView*, 8> allRtvs{};
    for (UINT index = 0; index < renderTargets.size(); ++index) {
        renderTargets[index] = CreateTexture(gpu, DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D11_BIND_RENDER_TARGET);
        allRtvs[index] = renderTargets[index].rtv.Get();
    }
    context.OMSetRenderTargets(8, allRtvs.data(), depthView);
    dispatch();
    std::array<ID3D11RenderTargetView*, 8> restoredAll{};
    context.OMGetRenderTargets(8, restoredAll.data(), nullptr);
    FO4HDR_WARP_CHECK(restoredAll == allRtvs);
    for (auto* view : restoredAll) if (view) view->Release();
    context.ClearState();
}

struct PresentationTestContext
{
    ID3D11Device& gpu;
    ID3D11DeviceContext& context;
    ComPtr<ID3D11DeviceContext1>& context1;
};

std::uint32_t ReadPresentationPixel(ID3D11DeviceContext& context, Texture& backBuffer)
{
    context.CopyResource(backBuffer.staging.Get(), backBuffer.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(context.Map(backBuffer.staging.Get(), 0, D3D11_MAP_READ,
        0, &mapped), "Read presentation predicate output");
    const auto pixel = *static_cast<const std::uint32_t*>(mapped.pData);
    context.Unmap(backBuffer.staging.Get(), 0);
    return pixel;
}

template <class DispatchAndCopy>
void VerifyPresentationPredication(const PresentationTestContext& fixture,
    Texture& output, Texture& backBuffer, const DispatchAndCopy& dispatchAndCopy)
{
    auto& gpu = fixture.gpu;
    auto& context = fixture.context;
    auto& context1 = fixture.context1;
    const auto ranged = CreateConstantBuffer(gpu, std::array<float, 128>{});
    constexpr UINT first = 16, count = 16;
    auto* rangedBinding = ranged.Get();
    constexpr float black[4]{};

    D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    ComPtr<ID3D11Predicate> predicate;
    CheckHresult(gpu.CreatePredicate(&query, &predicate), "Presentation predicate");
    context.Begin(predicate.Get());
    context.End(predicate.Get());

    // An empty occlusion query suppresses work when PredicateValue is FALSE.
    context.ClearRenderTargetView(output.rtv.Get(), black);
    context.ClearRenderTargetView(backBuffer.rtv.Get(), black);
    context.SetPredication(predicate.Get(), FALSE);
    dispatchAndCopy();
    context.SetPredication(nullptr, FALSE);
    FO4HDR_WARP_CHECK(ReadPresentationPixel(context, backBuffer) == 0U);

    for (bool rangedContext : {false, true}) {
        context.ClearRenderTargetView(output.rtv.Get(), black);
        context.ClearRenderTargetView(backBuffer.rtv.Get(), black);
        if (rangedContext)
            context1->CSSetConstantBuffers1(0, 1, &rangedBinding, &first, &count);
        else
            context.CSSetConstantBuffers(0, 1, &rangedBinding);
        context.SetPredication(predicate.Get(), FALSE);
        {
            BorrowedComputeState saved(&context,
                rangedContext ? context1.Get() : nullptr);
            ComPtr<ID3D11Predicate> active;
            BOOL value{};
            context.GetPredication(&active, &value);
            FO4HDR_WARP_CHECK(!active);
            dispatchAndCopy();
        }
        ComPtr<ID3D11Predicate> restoredPredicate;
        BOOL restoredValue = TRUE;
        context.GetPredication(&restoredPredicate, &restoredValue);
        FO4HDR_WARP_CHECK(restoredPredicate.Get() == predicate.Get());
        FO4HDR_WARP_CHECK(restoredValue == FALSE);
        ComPtr<ID3D11Buffer> restored;
        UINT restoredFirst{}, restoredCount{};
        context1->CSGetConstantBuffers1(0, 1, &restored, &restoredFirst, &restoredCount);
        FO4HDR_WARP_CHECK(restored.Get() == ranged.Get());
        if (rangedContext) {
            FO4HDR_WARP_CHECK(restoredFirst == first);
            FO4HDR_WARP_CHECK(restoredCount == count);
        }
        context.SetPredication(nullptr, FALSE);
        FO4HDR_WARP_CHECK(ReadPresentationPixel(context, backBuffer) == 0xFFFFFFFFU);

        // The failure path's clear must execute under the same production guard.
        context.SetPredication(predicate.Get(), FALSE);
        {
            BorrowedComputeState saved(&context, context1.Get());
            context.ClearRenderTargetView(backBuffer.rtv.Get(), black);
        }
        context.SetPredication(nullptr, FALSE);
        FO4HDR_WARP_CHECK(ReadPresentationPixel(context, backBuffer) == 0U);
    }
    context.ClearState();
}

struct PresentationCounters
{
    std::array<ComPtr<ID3D11Buffer>, 2> counterBuffers;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 2> counters;
    ComPtr<ID3D11Buffer> counterCopy, counterRead;

    explicit PresentationCounters(ID3D11Device& gpu)
    {
        D3D11_BUFFER_DESC bufferDescription{};
        bufferDescription.ByteWidth = 64;
        bufferDescription.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bufferDescription.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufferDescription.StructureByteStride = sizeof(UINT);
        D3D11_UNORDERED_ACCESS_VIEW_DESC counterDescription{};
        counterDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        counterDescription.Buffer.NumElements = 16;
        counterDescription.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
        for (UINT index = 0; index < counters.size(); ++index) {
            CheckHresult(gpu.CreateBuffer(&bufferDescription, nullptr, &counterBuffers[index]),
                "Presentation counter buffer");
            CheckHresult(gpu.CreateUnorderedAccessView(counterBuffers[index].Get(),
                &counterDescription, &counters[index]), "Presentation counter UAV");
        }
        bufferDescription = {};
        bufferDescription.ByteWidth = sizeof(UINT);
        CheckHresult(gpu.CreateBuffer(&bufferDescription, nullptr, &counterCopy), "Counter copy");
        bufferDescription.Usage = D3D11_USAGE_STAGING;
        bufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CheckHresult(gpu.CreateBuffer(&bufferDescription, nullptr, &counterRead), "Counter read");
        }
};

void CheckPresentationCounters(ID3D11DeviceContext& context,
    const PresentationCounters& resources, UINT csCount, UINT omCount)
{
    const auto& counters = resources.counters;
    for (UINT index = 0; index < counters.size(); ++index) {
        context.CopyStructureCount(resources.counterCopy.Get(), 0, counters[index].Get());
        context.CopyResource(resources.counterRead.Get(), resources.counterCopy.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(context.Map(resources.counterRead.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "Read preserved UAV counter");
        FO4HDR_WARP_CHECK(*static_cast<const UINT*>(mapped.pData) == (index == 0 ? csCount : omCount));
        context.Unmap(resources.counterRead.Get(), 0);
    }
}

ComPtr<ID3D11DepthStencilView> CreatePresentationDepth(ID3D11Device& gpu)
{
    D3D11_TEXTURE2D_DESC depthDescription{};
    depthDescription.Width = kWidth;
    depthDescription.Height = kHeight;
    depthDescription.MipLevels = depthDescription.ArraySize = 1;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthView;
    CheckHresult(gpu.CreateTexture2D(&depthDescription, nullptr, &depthTexture), "Presentation depth");
    CheckHresult(gpu.CreateDepthStencilView(depthTexture.Get(), nullptr, &depthView), "Presentation DSV");
    return depthView;
}

void CheckPresentationDestination(ID3D11DeviceContext& context, Texture& destination)
{
    context.CopyResource(destination.staging.Get(), destination.texture.Get());
    D3D11_MAPPED_SUBRESOURCE destinationPixels{};
    CheckHresult(context.Map(destination.staging.Get(), 0, D3D11_MAP_READ, 0,
        &destinationPixels), "Read direct presentation output");
    FO4HDR_WARP_CHECK(*static_cast<const UINT*>(destinationPixels.pData) == 0xFFFFFFFFU);
    context.Unmap(destination.staging.Get(), 0);
}

void CheckPresentationAliases(ID3D11DeviceContext& context,
    ID3D11ShaderResourceView* expected)
{
    constexpr std::array getters{
        &ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::HSGetShaderResources,
        &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::GSGetShaderResources,
        &ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::CSGetShaderResources};
    for (auto getter : getters) {
        ComPtr<ID3D11ShaderResourceView> actual;
        (context.*getter)(127, 1, &actual);
        FO4HDR_WARP_CHECK(actual.Get() == expected);
    }
}

struct PresentationOutputState
{
    std::array<ID3D11RenderTargetView*, 3> rtvs;
    ID3D11DepthStencilView* depth;
    ID3D11UnorderedAccessView* omCounter;
    ID3D11UnorderedAccessView* csCounter;
};

void CheckPresentationOutputs(ID3D11DeviceContext& context, UINT lastUav,
    const PresentationOutputState& expected)
{
    std::array<ID3D11RenderTargetView*, 3> restoredRtvs{};
    ComPtr<ID3D11DepthStencilView> restoredDepth;
    ComPtr<ID3D11UnorderedAccessView> restoredOm, restoredCs;
    context.OMGetRenderTargetsAndUnorderedAccessViews(3, restoredRtvs.data(), &restoredDepth,
        lastUav, 1, &restoredOm);
    context.CSGetUnorderedAccessViews(lastUav, 1, &restoredCs);
    FO4HDR_WARP_CHECK(restoredRtvs == expected.rtvs);
    FO4HDR_WARP_CHECK(restoredDepth.Get() == expected.depth);
    FO4HDR_WARP_CHECK(restoredOm.Get() == expected.omCounter && restoredCs.Get() == expected.csCounter);
    for (auto* view : restoredRtvs) if (view) view->Release();
}

void CheckPresentationComputeInputs(ID3D11DeviceContext& context,
    ID3D11ShaderResourceView* originalInput, ID3D11ComputeShader* shader)
{
    ComPtr<ID3D11ShaderResourceView> restoredInput;
    context.CSGetShaderResources(3, 1, &restoredInput);
    FO4HDR_WARP_CHECK(restoredInput.Get() == originalInput);
    ComPtr<ID3D11ComputeShader> restoredShader;
    context.CSGetShader(&restoredShader, nullptr, nullptr);
    FO4HDR_WARP_CHECK(restoredShader.Get() == shader);
}

void CheckPresentationUavsUnbound(ID3D11DeviceContext& context, UINT lastUav)
{
    ComPtr<ID3D11UnorderedAccessView> activeCs, activeOm;
    context.CSGetUnorderedAccessViews(lastUav, 1, &activeCs);
    context.OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
        lastUav, 1, &activeOm);
    FO4HDR_WARP_CHECK(!activeCs && !activeOm);
}

void VerifyPresentationState(Device& device)
{
    auto& gpu = *device.device.Get();
    auto& context = *device.context.Get();
    context.ClearState();
    ComPtr<ID3D11DeviceContext1> context1;
    CheckHresult(device.context.As(&context1), "Presentation context1");
    auto scene = CreateTexture(gpu, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    auto output = CreateTexture(gpu, DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET);
    auto backBuffer = CreateTexture(gpu, DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D11_BIND_RENDER_TARGET);
    const auto shader = CreateShader(gpu, fo4hdr::generated::test::kTransferEvaluationCs);
    const auto constants = CreateConstantBuffer(gpu,
        OutputConstants{kWidth, kHeight, 1.0F, 1U});
    constexpr float white[4]{1, 1, 1, 1};
    context.ClearRenderTargetView(scene.rtv.Get(), white);
    const auto dispatchAndCopy = [&]() {
        auto* input = scene.srv.Get();
        Dispatch(context, { shader.Get(), constants.Get(), &input, 1, output.uav.Get()});
        context.CopyResource(backBuffer.texture.Get(), output.texture.Get());
    };

    VerifyPresentationPredication({gpu, context, context1}, output, backBuffer, dispatchAndCopy);

    auto destination = CreateTexture(gpu, DXGI_FORMAT_R10G10B10A2_UNORM,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    const UINT lastUav = gpu.GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 63U : 7U;
    PresentationCounters counterResources(gpu);
    const auto& counters = counterResources.counters;
    constexpr std::array setters{
        &ID3D11DeviceContext::VSSetShaderResources, &ID3D11DeviceContext::HSSetShaderResources,
        &ID3D11DeviceContext::DSSetShaderResources, &ID3D11DeviceContext::GSSetShaderResources,
        &ID3D11DeviceContext::PSSetShaderResources, &ID3D11DeviceContext::CSSetShaderResources};
    constexpr UINT csCount = 5, omCount = 9;
    auto* csCounter = counters[0].Get();
    auto* omCounter = counters[1].Get();
    const auto depthView = CreatePresentationDepth(gpu);
    std::array<ID3D11RenderTargetView*, 3> originalRtvs{nullptr, nullptr, backBuffer.rtv.Get()};
    context.OMSetRenderTargetsAndUnorderedAccessViews(3, originalRtvs.data(), depthView.Get(),
        lastUav, 1, &omCounter, &omCount);
    context.CSSetUnorderedAccessViews(lastUav, 1, &csCounter, &csCount);
    auto* alias = destination.srv.Get();
    for (auto setter : setters) (context.*setter)(127, 1, &alias);
    auto* originalInput = scene.srv.Get();
    context.CSSetShaderResources(3, 1, &originalInput);
    context.CSSetShader(shader.Get(), nullptr, 0);
    {
        BorrowedComputeState saved(&context, context1.Get(), destination.texture.Get());
        CheckPresentationAliases(context, nullptr);
        CheckPresentationUavsUnbound(context, lastUav);
        Dispatch(context, { shader.Get(), constants.Get(), &originalInput, 1, destination.uav.Get()});
        // Leave an output bound to exercise the guard's hazard cleanup itself.
        auto* target = destination.uav.Get();
        context.CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    }
    CheckPresentationAliases(context, alias);
    CheckPresentationComputeInputs(context, originalInput, shader.Get());
    CheckPresentationOutputs(context, lastUav,
        {originalRtvs, depthView.Get(), omCounter, csCounter});
    CheckPresentationDestination(context, destination);
    CheckPresentationCounters(context, counterResources, csCount, omCount);
    context.ClearState();

    const auto guardedDispatch = [&] {
        BorrowedComputeState saved(&context, context1.Get(), destination.texture.Get());
        Dispatch(context, { shader.Get(), constants.Get(), &originalInput, 1, destination.uav.Get()});
    };
    VerifyPresentationUavAlias(context, lastUav, destination, guardedDispatch);
    VerifyPresentationAllTargets(gpu, context, depthView.Get(), guardedDispatch);
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

void VerifyNativeReferenceReadback(ID3D11Device& gpu,
    ID3D11DeviceContext& context, const NativeReferenceResources& prepared)
{
    D3D11_TEXTURE3D_DESC readbackDescription{};
    prepared.preblendTexture->GetDesc(&readbackDescription);
    readbackDescription.Usage = D3D11_USAGE_STAGING;
    readbackDescription.BindFlags = 0;
    readbackDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture3D> readback;
    CheckHresult(gpu.CreateTexture3D(&readbackDescription, nullptr, &readback),
        "Native Reference preparation readback");
    context.CopyResource(readback.Get(), prepared.preblendTexture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(context.Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read Native Reference preparation");
    const auto* firstTexel = static_cast<const float*>(mapped.pData);
    FO4HDR_WARP_CHECK(std::abs(firstTexel[0] - 32.0F / 255.0F) < 1.0e-6F);
    FO4HDR_WARP_CHECK(std::abs(firstTexel[1] - 64.0F / 255.0F) < 1.0e-6F);
    FO4HDR_WARP_CHECK(std::abs(firstTexel[2] - 128.0F / 255.0F) < 1.0e-6F);
    context.Unmap(readback.Get(), 0);
}

ComPtr<ID3D11ShaderResourceView> CreateNativeReferenceStateLut(ID3D11Device& gpu)
{
    std::array<std::uint32_t, 16 * 16 * 16> pixels;
    pixels.fill(0xFF804020U);
    D3D11_TEXTURE3D_DESC nativeDescription{};
    nativeDescription.Width = nativeDescription.Height = nativeDescription.Depth = 16;
    nativeDescription.MipLevels = 1;
    nativeDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nativeDescription.Usage = D3D11_USAGE_IMMUTABLE;
    nativeDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA nativeData{pixels.data(), 16 * 4, 16 * 16 * 4};
    ComPtr<ID3D11Texture3D> nativeTexture;
    ComPtr<ID3D11ShaderResourceView> nativeView;
    CheckHresult(gpu.CreateTexture3D(&nativeDescription, &nativeData, &nativeTexture),
        "Native Reference native LUT");
    CheckHresult(gpu.CreateShaderResourceView(nativeTexture.Get(), nullptr, &nativeView),
        "Native Reference native LUT view");
    return nativeView;
}

void VerifyNativeReferencePredication(ID3D11Device& gpu,
    ID3D11DeviceContext& context, ID3D11DeviceContext1& context1,
    NativeReferenceInputs& rejected)
{
    D3D11_QUERY_DESC predicateDescription{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    ComPtr<ID3D11Predicate> predicate;
    CheckHresult(gpu.CreatePredicate(&predicateDescription, &predicate), "Native Reference predicate");
    context.Begin(predicate.Get());
    context.End(predicate.Get());
    context.SetPredication(predicate.Get(), FALSE);

    FO4HDR_WARP_CHECK(!CaptureNativeReferenceInputs(&context, &context1, rejected));
    ComPtr<ID3D11Predicate> restoredPredicate;
    BOOL restoredValue = TRUE;
    context.GetPredication(&restoredPredicate, &restoredValue);
    FO4HDR_WARP_CHECK(restoredPredicate == predicate && restoredValue == FALSE);
    context.SetPredication(nullptr, FALSE);
    FO4HDR_WARP_CHECK(CaptureNativeReferenceInputs(&context, &context1, rejected));
}

void CheckNativeReferenceViews(ID3D11DeviceContext& context,
    const ComPtr<ID3D11ShaderResourceView>& nativeView)
{
    for (UINT slot = 0; slot < 4; ++slot)
    {
        ComPtr<ID3D11ShaderResourceView> restored;
        context.CSGetShaderResources(slot, 1, &restored);
        FO4HDR_WARP_CHECK(restored == nativeView);
        restored.Reset();
        context.PSGetShaderResources(slot + 3, 1, &restored);
        FO4HDR_WARP_CHECK(restored == nativeView);
    }
}

template <class GetBuffer>
void CheckNativeReferenceBuffer(const GetBuffer& getBuffer,
    const ComPtr<ID3D11Buffer>& expected, UINT first, UINT count)
{
    ComPtr<ID3D11Buffer> restoredBuffer;
    UINT restoredFirst{}, restoredCount{};
    getBuffer(restoredBuffer.GetAddressOf(), &restoredFirst, &restoredCount);
    FO4HDR_WARP_CHECK(restoredBuffer == expected);
    FO4HDR_WARP_CHECK(restoredFirst == first && restoredCount == count);
}

void VerifyNativeReferenceDeferredRejection(ID3D11Device& gpu,
    NativeReferenceInputs& rejected)
{
    ComPtr<ID3D11DeviceContext> deferred;
    ComPtr<ID3D11DeviceContext1> deferred1;
    CheckHresult(gpu.CreateDeferredContext(0, &deferred), "Native Reference deferred context");
    CheckHresult(deferred.As(&deferred1), "Native Reference deferred context1");
    FO4HDR_WARP_CHECK(!CaptureNativeReferenceInputs(deferred.Get(), deferred1.Get(), rejected));
}

void VerifyNativeReferenceRebuild(ID3D11Device& gpu,
    NativeReferenceResources& prepared)
{
    const auto oldTexture = prepared.preblendTexture;
    FO4HDR_WARP_CHECK(!BuildNativeReferenceResources(nullptr, prepared));
    FO4HDR_WARP_CHECK(prepared.preblendTexture == oldTexture);
    FO4HDR_WARP_CHECK(BuildNativeReferenceResources(&gpu, prepared));
    FO4HDR_WARP_CHECK(prepared.preblendTexture != oldTexture);
}

void VerifyNativeReferenceRetirement(NativeReferenceResources& resources)
{
    FO4HDR_WARP_CHECK(NativeReferenceResourcesReady(resources));
    ResetNativeReferenceResources(resources);
    FO4HDR_WARP_CHECK(!NativeReferenceResourcesReady(resources));
    FO4HDR_WARP_CHECK(!resources.shader && !resources.preblendShader);
    FO4HDR_WARP_CHECK(!resources.preblendTexture && !resources.preblendSrv && !resources.preblendUav);
    ResetNativeReferenceResources(resources);
    FO4HDR_WARP_CHECK(!NativeReferenceResourcesReady(resources));
}

void VerifyNativeReferenceState(Device& device)
{
    auto& gpu = *device.device.Get();
    auto& context = *device.context.Get();
    context.ClearState();
    ComPtr<ID3D11DeviceContext1> context1;
    CheckHresult(device.context.As(&context1), "Native Reference context1");
    NativeReferenceResources prepared, foreign;
    FO4HDR_WARP_CHECK(BuildNativeReferenceResources(&gpu, prepared));
    FO4HDR_WARP_CHECK(BuildNativeReferenceResources(&gpu, foreign));
    if (!prepared.shader || !foreign.shader) return;
    VerifyNativeReferenceRebuild(gpu, prepared);

    const auto nativeView = CreateNativeReferenceStateLut(gpu);
    std::array<ID3D11ShaderResourceView*, 4> nativeViews;
    nativeViews.fill(nativeView.Get());
    context.PSSetShaderResources(3, 4, nativeViews.data());
    std::array<float, 128> nativeConstants{};
    nativeConstants[68] = nativeConstants[69] = nativeConstants[70] = nativeConstants[71] = 0.25F;
    const auto weights = CreateConstantBuffer(gpu, nativeConstants);
    auto* weightsBinding = weights.Get();
    constexpr UINT nativeFirst = 16, nativeCount = 16;
    context1->PSSetConstantBuffers1(2, 1, &weightsBinding, &nativeFirst, &nativeCount);
    NativeReferenceInputs inputs;
    FO4HDR_WARP_CHECK(CaptureNativeReferenceInputs(device.context.Get(), context1.Get(), inputs));
    FO4HDR_WARP_CHECK(inputs.constants == weights);
    FO4HDR_WARP_CHECK(inputs.firstConstant == nativeFirst && inputs.constantCount == nativeCount);

    const auto foreignConstants = CreateConstantBuffer(gpu, std::array<float, 128>{});
    auto* foreignBinding = foreignConstants.Get();
    constexpr UINT foreignFirst = 0, foreignCount = 16;
    context1->CSSetConstantBuffers1(2, 1, &foreignBinding, &foreignFirst, &foreignCount);
    context.CSSetShader(foreign.preblendShader.Get(), nullptr, 0);
    context.CSSetShaderResources(0, 4, nativeViews.data());
    auto* foreignUav = foreign.preblendUav.Get();
    context.CSSetUnorderedAccessViews(0, 1, &foreignUav, nullptr);
    DispatchNativeReferencePrepare(device.context.Get(), context1.Get(), inputs, prepared);
    ComPtr<ID3D11ComputeShader> restoredShader;
    context.CSGetShader(&restoredShader, nullptr, nullptr);
    FO4HDR_WARP_CHECK(restoredShader == foreign.preblendShader);
    ComPtr<ID3D11UnorderedAccessView> restoredUav;
    context.CSGetUnorderedAccessViews(0, 1, &restoredUav);
    FO4HDR_WARP_CHECK(restoredUav == foreign.preblendUav);
    CheckNativeReferenceViews(context, nativeView);
    CheckNativeReferenceBuffer([&](auto* buffer, auto* first, auto* count) {
        context1->CSGetConstantBuffers1(2, 1, buffer, first, count);
    }, foreignConstants, foreignFirst, foreignCount);
    CheckNativeReferenceBuffer([&](auto* buffer, auto* first, auto* count) {
        context1->PSGetConstantBuffers1(2, 1, buffer, first, count);
    }, weights, nativeFirst, nativeCount);

    VerifyNativeReferenceReadback(gpu, context, prepared);

    NativeReferenceInputs rejected;
    VerifyNativeReferencePredication(gpu, context, *context1.Get(), rejected);

    ID3D11ShaderResourceView* missing{};
    context.PSSetShaderResources(3, 1, &missing);
    FO4HDR_WARP_CHECK(!CaptureNativeReferenceInputs(device.context.Get(), context1.Get(), rejected));
    auto* unsupported = prepared.preblendSrv.Get();
    context.PSSetShaderResources(3, 1, &unsupported);
    FO4HDR_WARP_CHECK(!CaptureNativeReferenceInputs(device.context.Get(), context1.Get(), rejected));
    context.PSSetShaderResources(3, 4, nativeViews.data());
    const auto shortConstants = CreateConstantBuffer(gpu, std::array<float, 4>{});
    auto* shortBinding = shortConstants.Get();
    context.PSSetConstantBuffers(2, 1, &shortBinding);
    FO4HDR_WARP_CHECK(!CaptureNativeReferenceInputs(device.context.Get(), context1.Get(), rejected));
    context1->PSSetConstantBuffers1(2, 1, &weightsBinding, &nativeFirst, &nativeCount);
    FO4HDR_WARP_CHECK(CaptureNativeReferenceInputs(device.context.Get(), context1.Get(), rejected));
    FO4HDR_WARP_CHECK(!CaptureNativeReferenceInputs(device.context.Get(), nullptr, rejected));
    VerifyNativeReferenceDeferredRejection(gpu, rejected);
    context.ClearState();
    VerifyNativeReferenceRetirement(prepared);
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

struct PixelBindingRestoration
{
    Device& device;
    static constexpr UINT constantSlot = 13;
    static constexpr UINT resourceSlot = 7;
    static constexpr UINT extraResourceSlot = 8;
    static constexpr UINT samplerSlot = 7;
    static constexpr UINT foreignFirstConstant = 16;
    static constexpr UINT foreignNumConstants = 16;
    ComPtr<ID3D11Buffer> foreignConstants, ownedConstants;
    Texture3D foreignLut, ownedLut;
    ComPtr<ID3D11SamplerState> foreignSampler, ownedSampler;
    ID3D11Buffer* foreignConstantPointer{};
    ID3D11ShaderResourceView* foreignResourcePointer{};
    ID3D11SamplerState* foreignSamplerPointer{};
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D11Buffer> restoredConstants;
    ComPtr<ID3D11ShaderResourceView> restoredResource;
    ComPtr<ID3D11SamplerState> restoredSampler;
    fo4hdr::ScopedD3D11PixelBindings invalid;
    ComPtr<ID3D11ShaderResourceView> restoredExtra;
    explicit PixelBindingRestoration(Device& source) : device(source) {}
    void Initialize()
    {
        foreignConstants = CreateConstantBuffer(
            *device.device.Get(), std::array<float, 128>{});
        ownedConstants = CreateConstantBuffer(
            *device.device.Get(), std::array<float, 4>{5.0F, 6.0F, 7.0F, 8.0F});
        foreignLut = CreateIdentityLut(*device.device.Get());
        ownedLut = CreateIdentityLut(*device.device.Get());
        foreignSampler = CreateClampPointSampler(*device.device.Get());
        ownedSampler = CreateClampLinearSampler(*device.device.Get());

        foreignConstantPointer = foreignConstants.Get();
        foreignResourcePointer = foreignLut.srv.Get();
        foreignSamplerPointer = foreignSampler.Get();
        CheckHresult(device.context.As(&context1),
            "Query ID3D11DeviceContext1 for pixel binding restoration");
        context1->PSSetConstantBuffers1(constantSlot, 1,
            &foreignConstantPointer, &foreignFirstConstant, &foreignNumConstants);
        CheckForeignConstantRange();
        device.context->PSSetShaderResources(
            resourceSlot, 1, &foreignResourcePointer);
        device.context->PSSetSamplers(samplerSlot, 1, &foreignSamplerPointer);

    }
    void CheckForeignConstantRange()
    {

        ComPtr<ID3D11Buffer> actualConstants;
        UINT actualFirstConstant{};
        UINT actualNumConstants{};
        context1->PSGetConstantBuffers1(constantSlot, 1, &actualConstants,
            &actualFirstConstant, &actualNumConstants);
        FO4HDR_WARP_CHECK(actualConstants.Get() == foreignConstants.Get());
        FO4HDR_WARP_CHECK(actualFirstConstant == foreignFirstConstant);
        FO4HDR_WARP_CHECK(actualNumConstants == foreignNumConstants);
        }
    void CheckState(ID3D11Buffer* constants, ID3D11ShaderResourceView* resource, ID3D11SamplerState* sampler)
    {

        ComPtr<ID3D11Buffer> actualConstants;
        ComPtr<ID3D11ShaderResourceView> actualResource;
        ComPtr<ID3D11SamplerState> actualSampler;
        device.context->PSGetConstantBuffers(
            constantSlot, 1, &actualConstants);
        device.context->PSGetShaderResources(
            resourceSlot, 1, &actualResource);
        device.context->PSGetSamplers(samplerSlot, 1, &actualSampler);
        FO4HDR_WARP_CHECK(actualConstants.Get() == constants);
        FO4HDR_WARP_CHECK(actualResource.Get() == resource);
        FO4HDR_WARP_CHECK(actualSampler.Get() == sampler);
        }
    void CheckOwned()
    {
        {
            fo4hdr::ScopedD3D11PixelBindings bindings;
            FO4HDR_WARP_CHECK(bindings.Bind(
                device.context.Get(), context1.Get(), { constantSlot,
                ownedConstants.Get(), resourceSlot, ownedLut.srv.Get(), samplerSlot,
                ownedSampler.Get(), true}));
            FO4HDR_WARP_CHECK(bindings.Active());

            ComPtr<ID3D11Buffer> observedConstants;
            ComPtr<ID3D11ShaderResourceView> observedResource;
            ComPtr<ID3D11SamplerState> observedSampler;
            device.context->PSGetConstantBuffers(
                constantSlot, 1, &observedConstants);
            device.context->PSGetShaderResources(
                resourceSlot, 1, &observedResource);
            device.context->PSGetSamplers(samplerSlot, 1, &observedSampler);
            FO4HDR_WARP_CHECK(observedConstants.Get() == ownedConstants.Get());
            FO4HDR_WARP_CHECK(observedResource.Get() == ownedLut.srv.Get());
            FO4HDR_WARP_CHECK(observedSampler.Get() == ownedSampler.Get());
        }

        device.context->PSGetConstantBuffers(
            constantSlot, 1, &restoredConstants);
        device.context->PSGetShaderResources(
            resourceSlot, 1, &restoredResource);
        device.context->PSGetSamplers(samplerSlot, 1, &restoredSampler);
        FO4HDR_WARP_CHECK(restoredConstants.Get() == foreignConstants.Get());
        FO4HDR_WARP_CHECK(restoredResource.Get() == foreignLut.srv.Get());
        FO4HDR_WARP_CHECK(restoredSampler.Get() == foreignSampler.Get());
        CheckForeignConstantRange();

        FO4HDR_WARP_CHECK(!invalid.Bind(nullptr, nullptr, { constantSlot,
            ownedConstants.Get(), resourceSlot, ownedLut.srv.Get(), samplerSlot,
            ownedSampler.Get(), true}));

    }
    void CheckReset()
    {
        // Production temporarily owns b13/t7/s7 even when another component had
        // valid bindings there. The scope must bind the complete operator tuple,
        // reject a second transaction while active, and restore the exact foreign
        // objects on explicit, idempotent reset.
        {
            fo4hdr::ScopedD3D11PixelBindings bindings;
            FO4HDR_WARP_CHECK(bindings.Bind(
                device.context.Get(), context1.Get(), { constantSlot,
                ownedConstants.Get(), resourceSlot, ownedLut.srv.Get(), samplerSlot,
                ownedSampler.Get(), true}));
            CheckState(ownedConstants.Get(), ownedLut.srv.Get(),
                ownedSampler.Get());
            FO4HDR_WARP_CHECK(!bindings.Bind(
                device.context.Get(), context1.Get(), { constantSlot,
                ownedConstants.Get(), resourceSlot, ownedLut.srv.Get(), samplerSlot,
                ownedSampler.Get(), true}));
            CheckState(ownedConstants.Get(), ownedLut.srv.Get(),
                ownedSampler.Get());
            bindings.Reset();
            bindings.Reset();
            FO4HDR_WARP_CHECK(!bindings.Active());
            CheckState(foreignConstants.Get(), foreignLut.srv.Get(),
                foreignSampler.Get());
            CheckForeignConstantRange();
        }

    }
    void CheckBypass()
    {
        // The constants-only bypass path owns b13 but must neither inspect nor
        // disturb the existing t7/s7 pair. Destruction restores b13 exactly.
        {
            fo4hdr::ScopedD3D11PixelBindings bypass;
            FO4HDR_WARP_CHECK(bypass.Bind(
                device.context.Get(), context1.Get(), { constantSlot,
                ownedConstants.Get(), resourceSlot, nullptr, samplerSlot, nullptr,
                false}));
            FO4HDR_WARP_CHECK(bypass.Active());
            CheckState(ownedConstants.Get(), foreignLut.srv.Get(),
                foreignSampler.Get());
        }
        CheckState(foreignConstants.Get(), foreignLut.srv.Get(),
            foreignSampler.Get());

    }
    void CheckExtra()
    {
        // A positively identified replacement shader may temporarily borrow an
        // extra SRV slot. D3D11 keeps stale resources in unused slots, so
        // occupancy alone is not foreign pass ownership; the exact object must
        // nevertheless be restored at scope exit.
        device.context->PSSetShaderResources(
            extraResourceSlot, 1, &foreignResourcePointer);
        {
            fo4hdr::ScopedD3D11PixelBindings borrowed;
            FO4HDR_WARP_CHECK(borrowed.Bind(
                device.context.Get(), context1.Get(), { constantSlot,
                ownedConstants.Get(), resourceSlot, ownedLut.srv.Get(), samplerSlot,
                ownedSampler.Get(), true, extraResourceSlot, ownedLut.srv.Get(),
                false}));
            ComPtr<ID3D11ShaderResourceView> observedExtra;
            device.context->PSGetShaderResources(
                extraResourceSlot, 1, &observedExtra);
            FO4HDR_WARP_CHECK(observedExtra.Get() == ownedLut.srv.Get());
            CheckState(ownedConstants.Get(), ownedLut.srv.Get(),
                ownedSampler.Get());
        }
        device.context->PSGetShaderResources(
            extraResourceSlot, 1, &restoredExtra);
        FO4HDR_WARP_CHECK(restoredExtra.Get() == foreignLut.srv.Get());
        CheckState(foreignConstants.Get(), foreignLut.srv.Get(),
            foreignSampler.Get());

    }
    void CheckCollision()
    {
        // Feature-local SRV seams can opt into collision rejection. The failed
        // preflight must leave constants, the primary SRV, and the sampler intact.
        {
            fo4hdr::ScopedD3D11PixelBindings collision;
            FO4HDR_WARP_CHECK(!collision.Bind(
                device.context.Get(), context1.Get(), { constantSlot,
                ownedConstants.Get(), resourceSlot, ownedLut.srv.Get(), samplerSlot,
                ownedSampler.Get(), true, 0, nullptr, true, true}));
            FO4HDR_WARP_CHECK(!collision.Active());
        }
        CheckState(foreignConstants.Get(), foreignLut.srv.Get(),
            foreignSampler.Get());
        CheckForeignConstantRange();

    }
    void CheckMalformed()
    {
        // An incomplete production tuple must not partially modify b13/t7/s7.
        {
            fo4hdr::ScopedD3D11PixelBindings malformed;
            FO4HDR_WARP_CHECK(!malformed.Bind(
                device.context.Get(), context1.Get(), { constantSlot,
                ownedConstants.Get(), resourceSlot, nullptr, samplerSlot,
                ownedSampler.Get(), true}));
            FO4HDR_WARP_CHECK(!malformed.Bind(
                device.context.Get(), context1.Get(), { constantSlot,
                ownedConstants.Get(), resourceSlot, ownedLut.srv.Get(), samplerSlot,
                nullptr, true}));
            FO4HDR_WARP_CHECK(!malformed.Active());
        }
        CheckState(foreignConstants.Get(), foreignLut.srv.Get(),
            foreignSampler.Get());
        CheckForeignConstantRange();

    }
    void CheckLegacy()
    {
        device.context->PSSetConstantBuffers(
            constantSlot, 1, &foreignConstantPointer);
        {
            fo4hdr::ScopedD3D11PixelBindings legacy;
            FO4HDR_WARP_CHECK(legacy.Bind(device.context.Get(), nullptr, {
                constantSlot, ownedConstants.Get(), resourceSlot, ownedLut.srv.Get(),
                samplerSlot, ownedSampler.Get(), true}));
        }
        CheckState(foreignConstants.Get(), foreignLut.srv.Get(),
            foreignSampler.Get());
    }
    void Run()
    {
        Initialize();
        CheckOwned();
        CheckReset();
        CheckBypass();
        CheckExtra();
        CheckCollision();
        CheckMalformed();
        CheckLegacy();
    }
};

void VerifyScopedPixelBindingRestoration(Device& device)
{
    PixelBindingRestoration scenario(device);
    scenario.Run();
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

void VerifyContextQuerySafety()
{
    using fo4hdr::ContextQuerySafety;
    std::array<int, ContextQuerySafety::capacity + 1> keys{};
    ContextQuerySafety queries;
    FO4HDR_WARP_CHECK(queries.CanDispatch());
    queries.Begin(&keys[0]);
    queries.Begin(&keys[1]);
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    queries.End(&keys[0]);
    queries.End(&keys[2]);  // Unrelated End does not clear a live scope.
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    queries.End(&keys[1]);
    FO4HDR_WARP_CHECK(queries.CanDispatch());
    queries.Begin(&keys[0]);
    queries.Begin(&keys[0]);
    queries.End(&keys[0]);
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    queries = {};
    queries.Begin(nullptr);
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    queries = {};
    for (const auto& key : keys) queries.Begin(&key);
    for (const auto& key : keys) queries.End(&key);
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    queries = {};
    FO4HDR_WARP_CHECK(queries.EnterCommandList());
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    FO4HDR_WARP_CHECK(queries.EnterCommandList());
    queries.LeaveCommandList();
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    queries.LeaveCommandList();
    FO4HDR_WARP_CHECK(queries.CanDispatch());
    queries.Begin(&keys[0]);
    FO4HDR_WARP_CHECK(queries.EnterCommandList());
    queries.LeaveCommandList();
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    queries.End(&keys[0]);
    FO4HDR_WARP_CHECK(queries.CanDispatch());
    queries.Begin(nullptr);  // Genuine tracking failure remains sticky.
    FO4HDR_WARP_CHECK(queries.EnterCommandList());
    queries.LeaveCommandList();
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    queries = {};
    queries.LeaveCommandList();
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    queries = {};
    for (std::size_t i = 0; i < ContextQuerySafety::capacity; ++i)
        FO4HDR_WARP_CHECK(queries.EnterCommandList());
    FO4HDR_WARP_CHECK(!queries.EnterCommandList());
    for (std::size_t i = 0; i < ContextQuerySafety::capacity; ++i)
        queries.LeaveCommandList();
    FO4HDR_WARP_CHECK(!queries.CanDispatch());
    for (const auto query : {D3D11_QUERY_OCCLUSION,
             D3D11_QUERY_PIPELINE_STATISTICS, D3D11_QUERY_OCCLUSION_PREDICATE,
             D3D11_QUERY_SO_STATISTICS, D3D11_QUERY_SO_OVERFLOW_PREDICATE,
             D3D11_QUERY_SO_STATISTICS_STREAM0, D3D11_QUERY_SO_STATISTICS_STREAM1,
             D3D11_QUERY_SO_STATISTICS_STREAM2, D3D11_QUERY_SO_STATISTICS_STREAM3,
             D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0,
             D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1,
             D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2,
             D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3})
        FO4HDR_WARP_CHECK(fo4hdr::IsComputeSensitiveQuery(query));
    for (const auto query : {D3D11_QUERY_EVENT, D3D11_QUERY_TIMESTAMP,
             D3D11_QUERY_TIMESTAMP_DISJOINT})
        FO4HDR_WARP_CHECK(!fo4hdr::IsComputeSensitiveQuery(query));

}

}  // namespace fo4hdr::tests::gpu
