// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationContextFixture.hpp"
#include "hooks/ContextHooks.hpp"
#include "hooks/DeviceHooks.hpp"
#include "hooks/HookRecords.hpp"
#include "passes/PresentationPass.hpp"
#include "passes/ObservedDraw.hpp"
#include "passes/ArmorCompositionPass.hpp"
#include "resources/PresentationResources.hpp"
#include "resources/SceneResources.hpp"
#include "resources/ArmorCompositionResources.hpp"
#include "RendererLifetime.hpp"
#include "PresentFrame.hpp"
#include "SharedUiFrame.hpp"
#include "SharedSdrFrame.hpp"
#include "ShaderRegistry.hpp"
#include "DisplayDiscovery.hpp"
#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "resources/ReplacementShaders.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"
#include "d3d11/BorrowedFullscreenState.hpp"
#include "resources/SharedUiComposition.hpp"
#include "FO4Isotope/ui/HdrSettingsInput.hpp"
#include "FO4Isotope/ui/SettingsWindow.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace fo4hdr::tests::gpu {
namespace {

template <class Result> Result ReadQuery(Device& device, ID3D11Asynchronous* query)
{
    Result result{};
    device.context->Flush();
    HRESULT status{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((status = device.context->GetData(query, &result, sizeof(result), 0)) == S_FALSE)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Native query timed out");
        Sleep(1);
    }
    CheckHresult(status, "Read native query");
    return result;
}

class ProfilingCounter final : public ID3D11Counter
{
public:
    ULONG references{1};
    UINT acquisitions{}, releases{}, counterQueries{}, beginCalls{}, endCalls{};
    bool recognized{true};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
    {
        *result = nullptr;
        if (iid == __uuidof(ID3D11Counter)) ++counterQueries;
        if (!recognized) return E_NOINTERFACE;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D11DeviceChild) &&
            iid != __uuidof(ID3D11Asynchronous) && iid != __uuidof(ID3D11Counter))
            return E_NOINTERFACE;
        *result = static_cast<ID3D11Counter*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++acquisitions; return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { ++releases; return --references; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override { *device = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    UINT STDMETHODCALLTYPE GetDataSize() override { return sizeof(UINT64); }
    void STDMETHODCALLTYPE GetDesc(D3D11_COUNTER_DESC* description) override { *description = {}; }

    static void STDMETHODCALLTYPE Begin(ID3D11DeviceContext*, ID3D11Asynchronous* asynchronous)
    {
        ++static_cast<ProfilingCounter*>(asynchronous)->beginCalls;
    }
    static void STDMETHODCALLTYPE End(ID3D11DeviceContext*, ID3D11Asynchronous* asynchronous)
    {
        ++static_cast<ProfilingCounter*>(asynchronous)->endCalls;
    }
};

struct NativeStreamOutput
{
    Device& device;
    static constexpr std::array<Float4, 6> vertices{{
        {-1, -1, 0, 1}, {-1, 1, 0, 1}, {1, -1, 0, 1},
        {1, 1, 0, 1}, {0, -1, 0, 1}, {-1, 0, 0, 1}}};
    static constexpr UINT stride = sizeof(Float4), startOffset = 3 * stride;
    ComPtr<ID3D11Buffer> input, stream, staging;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11GeometryShader> geometry;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Query> statistics;
    ComPtr<ID3D11Predicate> overflow;

    explicit NativeStreamOutput(Device& source) : device(source)
    {
        CreateBuffers();
        const auto& vs = generated::test::kFullscreenStateVs;
        vertex = CreateVertexShader(*device.device.Get(), vs);
        const D3D11_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
            0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        CheckHresult(device.device->CreateInputLayout(&element, 1, vs.data(), vs.size(), &layout),
            "Create native SO input layout");
        const auto& gs = generated::test::kFullscreenStateGs;
        const D3D11_SO_DECLARATION_ENTRY declaration{0, "SV_Position", 0, 0, 4, 0};
        CheckHresult(device.device->CreateGeometryShaderWithStreamOutput(gs.data(), gs.size(),
            &declaration, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &geometry),
            "Create native GS+SO");
        const D3D11_QUERY_DESC statisticsDescription{D3D11_QUERY_SO_STATISTICS, 0};
        CheckHresult(device.device->CreateQuery(&statisticsDescription, &statistics),
            "Create native SO statistics");
        const D3D11_QUERY_DESC overflowDescription{D3D11_QUERY_SO_OVERFLOW_PREDICATE, 0};
        CheckHresult(device.device->CreatePredicate(&overflowDescription, &overflow),
            "Create native SO overflow predicate");
    }

    void CreateBuffers()
    {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = sizeof(vertices);
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA inputData{vertices.data(), 0, 0};
        CheckHresult(device.device->CreateBuffer(&description, &inputData, &input),
            "Create native SO vertices");
        const std::array<Float4, 9> initial{};
        description.ByteWidth = sizeof(initial);
        description.BindFlags |= D3D11_BIND_STREAM_OUTPUT;
        const D3D11_SUBRESOURCE_DATA streamData{initial.data(), 0, 0};
        CheckHresult(device.device->CreateBuffer(&description, &streamData, &stream),
            "Create native SO buffer");
        description.BindFlags = 0;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CheckHresult(device.device->CreateBuffer(&description, nullptr, &staging),
            "Create native SO readback");
    }

    void Begin(ContextRecord& record)
    {
        auto& context = *device.context.Get();
        context.VSSetShader(vertex.Get(), nullptr, 0);
        context.GSSetShader(geometry.Get(), nullptr, 0);
        // This fixture supplies scene state without applying a game lifecycle publication.
        record.nextPsSetShader(&context, nullptr, nullptr, 0);
        record.currentPassKnown = false;
        record.currentReplacementActive = false;
        record.expectedBoundShader = nullptr;
        record.currentRequirement = PipelinePassRequirement::untracked;
        context.IASetInputLayout(layout.Get());
        context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto* inputBuffer = input.Get();
        constexpr UINT inputOffset = 0;
        context.IASetVertexBuffers(0, 1, &inputBuffer, &stride, &inputOffset);
        auto* target = stream.Get();
        context.SOSetTargets(1, &target, &startOffset);
        context.Begin(statistics.Get());
        context.Begin(overflow.Get());
        context.Draw(3, 0);
    }

    void CheckBindings() const
    {
        ComPtr<ID3D11GeometryShader> actualGeometry;
        device.context->GSGetShader(&actualGeometry, nullptr, nullptr);
        FO4HDR_WARP_CHECK(actualGeometry.Get() == geometry.Get());
        ComPtr<ID3D11Buffer> actualStream, actualInput;
        device.context->SOGetTargets(1, &actualStream);
        FO4HDR_WARP_CHECK(actualStream.Get() == stream.Get());
        UINT actualStride{}, actualOffset{};
        device.context->IAGetVertexBuffers(0, 1, &actualInput, &actualStride, &actualOffset);
        FO4HDR_WARP_CHECK(actualInput.Get() == input.Get() && actualStride == stride && actualOffset == 0);
        ComPtr<ID3D11InputLayout> actualLayout;
        device.context->IAGetInputLayout(&actualLayout);
        FO4HDR_WARP_CHECK(actualLayout.Get() == layout.Get());
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        device.context->IAGetPrimitiveTopology(&topology);
        FO4HDR_WARP_CHECK(topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void CheckResult(bool forceOverflow)
    {
        device.context->Draw(3, 3);
        if (forceOverflow) device.context->Draw(3, 0);
        device.context->End(statistics.Get());
        device.context->End(overflow.Get());
        const auto counts = ReadQuery<D3D11_QUERY_DATA_SO_STATISTICS>(device, statistics.Get());
        FO4HDR_WARP_CHECK(counts.NumPrimitivesWritten == 2);
        FO4HDR_WARP_CHECK(counts.PrimitivesStorageNeeded == (forceOverflow ? 3U : 2U));
        FO4HDR_WARP_CHECK(ReadQuery<BOOL>(device, overflow.Get()) == (forceOverflow ? TRUE : FALSE));
        CheckBytes();
        CheckDrawAuto();
    }

    void CheckBytes()
    {
        device.context->SOSetTargets(0, nullptr, nullptr);
        device.context->CopyResource(staging.Get(), stream.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "Read native SO bytes");
        std::array<Float4, 9> expected{};
        std::copy(vertices.begin(), vertices.end(), expected.begin() + 3);
        const bool identical = std::memcmp(mapped.pData, expected.data(), sizeof(expected)) == 0;
        device.context->Unmap(staging.Get(), 0);
        FO4HDR_WARP_CHECK(identical);
    }

    void CheckDrawAuto()
    {
        device.context->GSSetShader(nullptr, nullptr, 0);
        auto* source = stream.Get();
        constexpr UINT offset = 0;
        device.context->IASetVertexBuffers(0, 1, &source, &stride, &offset);
        const D3D11_QUERY_DESC description{D3D11_QUERY_PIPELINE_STATISTICS, 0};
        ComPtr<ID3D11Query> query;
        CheckHresult(device.device->CreateQuery(&description, &query), "Create DrawAuto count query");
        device.context->Begin(query.Get());
        device.context->DrawAuto();
        device.context->End(query.Get());
        const auto counts = ReadQuery<D3D11_QUERY_DATA_PIPELINE_STATISTICS>(device, query.Get());
        FO4HDR_WARP_CHECK(counts.IAVertices == 9 && counts.IAPrimitives == 3);
    }
};

struct PresentationContextScenario : PresentationContextFixture
{
    using PresentationContextFixture::PresentationContextFixture;

    void CheckObservation(const DrawObservation& expected)
    {
        FO4HDR_WARP_CHECK(record->currentPrimaryRtv == expected.currentPrimaryRtv);
        FO4HDR_WARP_CHECK(record->currentSecondaryRtv == expected.currentSecondaryRtv);
        FO4HDR_WARP_CHECK(record->currentSinglePresentationTarget == expected.currentSinglePresentationTarget);
        FO4HDR_WARP_CHECK(record->currentPass == expected.currentPass);
        FO4HDR_WARP_CHECK(record->currentPassKnown == expected.currentPassKnown);
        FO4HDR_WARP_CHECK(record->currentReplacementActive == expected.currentReplacementActive);
        FO4HDR_WARP_CHECK(record->expectedBoundShader == expected.expectedBoundShader);
        FO4HDR_WARP_CHECK(record->currentRequirement == expected.currentRequirement);
        ComPtr<ID3D11PixelShader> shader;
        device.context->PSGetShader(&shader, nullptr, nullptr);
        FO4HDR_WARP_CHECK(shader.Get() == expected.expectedBoundShader);
        FO4HDR_WARP_CHECK(ValidateExpectedShaderBinding(device.context.Get(), *record));
    }

    void CheckProxyBinding(ID3D11RenderTargetView* target, bool combined)
    {
        g_presentation.presentationRedirectedThisFrame = false;
        if (combined)
            device.context->OMSetRenderTargetsAndUnorderedAccessViews(
                1, &target, nullptr, 1, 0, nullptr, nullptr);
        else
            device.context->OMSetRenderTargets(1, &target, nullptr);
        ComPtr<ID3D11RenderTargetView> actual;
        device.context->OMGetRenderTargets(1, &actual, nullptr);
        FO4HDR_WARP_CHECK(actual.Get() == target);
        FO4HDR_WARP_CHECK(record->currentPrimaryRtv == target);
        FO4HDR_WARP_CHECK(record->currentSinglePresentationTarget == IsPresentationProxyTarget(target));
        FO4HDR_WARP_CHECK(g_presentation.presentationRedirectedThisFrame == IsPresentationProxyTarget(target));
    }

    void CheckProxyDomainRejection(ID3D11RenderTargetView* target)
    {
        g_presentation.presentationProxyDomain = LinearColorDomain::linearBt709Relative;
        g_presentation.presentationRedirectedThisFrame = false;
        device.context->OMSetRenderTargets(1, &target, nullptr);
        ComPtr<ID3D11RenderTargetView> actual;
        device.context->OMGetRenderTargets(1, &actual, nullptr);
        FO4HDR_WARP_CHECK(actual.Get() == target);
        FO4HDR_WARP_CHECK(g_referenceHealthy.load());
        FO4HDR_WARP_CHECK(!g_presentation.presentationRedirectedThisFrame);
        FO4HDR_WARP_CHECK(record->currentSinglePresentationTarget);
        g_presentation.presentationProxyDomain = g_scene.currentSceneDomain;
        g_referenceHealthy = true;
    }

    void CheckProxyAliases()
    {
        ComPtr<ID3D11RenderTargetView> alias;
        CheckHresult(device.device->CreateRenderTargetView(proxy.texture.Get(), nullptr, &alias),
            "Create application alias of presentation proxy");
        FO4HDR_WARP_CHECK(alias.Get() != proxy.rtv.Get());
        FO4HDR_WARP_CHECK(IsPresentationProxyTarget(alias.Get()));
        FO4HDR_WARP_CHECK(AcquireSceneView(alias.Get()) == proxy.srv.Get());
        for (const auto& entry : g_scene.sceneViews)
            FO4HDR_WARP_CHECK(entry.resourceIdentity == nullptr && entry.srv == nullptr);
        CheckProxyBinding(alias.Get(), false);
        CheckProxyBinding(alias.Get(), true);
        const DrawObservation expected = *record;
        device.context->OMSetRenderTargetsAndUnorderedAccessViews(
            D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 1, 0, nullptr, nullptr);
        CheckObservation(expected);
        CheckProxyDomainRejection(alias.Get());
        auto unrelated = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        FO4HDR_WARP_CHECK(!IsPresentationProxyTarget(unrelated.rtv.Get()));
        FO4HDR_WARP_CHECK(AcquireSceneView(unrelated.rtv.Get()) != proxy.srv.Get());
        CheckProxyBinding(unrelated.rtv.Get(), false);
        CheckProxyBinding(unrelated.rtv.Get(), true);
        auto* original = output.rtv.Get();
        device.context->OMSetRenderTargets(1, &original, nullptr);
        FO4HDR_WARP_CHECK(record->currentPrimaryRtv == proxy.rtv.Get());
        FO4HDR_WARP_CHECK(record->currentSinglePresentationTarget);
        CheckPresentationViewIdentity();
    }

    ID3D11RenderTargetView* BindAndRead(ID3D11RenderTargetView* view)
    {
        device.context->OMSetRenderTargets(1, &view, nullptr);
        ComPtr<ID3D11RenderTargetView> actual;
        device.context->OMGetRenderTargets(1, &actual, nullptr);
        return actual.Get();
    }

    void CheckPresentationViewIdentity()
    {
        // Backbuffer identity is proven at bind time, without a view cache.
        ComPtr<ID3D11RenderTargetView> uncached;
        CheckHresult(device.device->CreateRenderTargetView(output.texture.Get(), nullptr, &uncached),
            "Create uncached actual presentation view");
        FO4HDR_WARP_CHECK(BindAndRead(uncached.Get()) == proxy.rtv.Get());
        FO4HDR_WARP_CHECK(record->currentPrimaryRtv == proxy.rtv.Get());
        FO4HDR_WARP_CHECK(record->currentSinglePresentationTarget);
        FO4HDR_WARP_CHECK(g_referenceHealthy.load() && g_productionHealthy.load());
        auto unrelated = CreateTexture(*device.device.Get(), DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        FO4HDR_WARP_CHECK(!IsPresentationBackBufferTarget(unrelated.rtv.Get()));
        FO4HDR_WARP_CHECK(BindAndRead(unrelated.rtv.Get()) == unrelated.rtv.Get());
        {
            // Overlay work inside the lower Present keeps its own backbuffer.
            const DrawObservation expected = *record;
            const ScopedLowerPresent lower;
            FO4HDR_WARP_CHECK(InsideLowerPresent());
            FO4HDR_WARP_CHECK(BindAndRead(uncached.Get()) == uncached.Get());
            device.context->Draw(0, 0);
            CheckObservation(expected);
        }
        FO4HDR_WARP_CHECK(!InsideLowerPresent());
        // The facade game never sees the native backbuffer; foreign binds stay native.
        g_presentation.applicationFacadeChain = chain.Get();
        FO4HDR_WARP_CHECK(BindAndRead(uncached.Get()) == uncached.Get());
        FO4HDR_WARP_CHECK(BindAndRead(proxy.rtv.Get()) == proxy.rtv.Get());
        FO4HDR_WARP_CHECK(record->currentSinglePresentationTarget);
        g_presentation.applicationFacadeChain = nullptr;
    }

    void CheckProxyViewRejection(DXGI_FORMAT textureFormat, DXGI_FORMAT viewFormat, UINT mip)
    {
        D3D11_TEXTURE2D_DESC description{};
        proxy.texture->GetDesc(&description);
        description.MipLevels = 2;
        description.Format = textureFormat;
        ComPtr<ID3D11Texture2D> mipmapped;
        CheckHresult(device.device->CreateTexture2D(&description, nullptr, &mipmapped),
            "Create mipmapped proxy identity fixture");
        D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
        viewDescription.Format = viewFormat;
        viewDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        viewDescription.Texture2D.MipSlice = mip;
        ComPtr<ID3D11RenderTargetView> otherMip;
        CheckHresult(device.device->CreateRenderTargetView(mipmapped.Get(), &viewDescription, &otherMip),
            "Create incompatible proxy view");
        g_presentation.presentationProxy = mipmapped;
        FO4HDR_WARP_CHECK(RenderTargetMatchesResource(otherMip.Get(), mipmapped.Get()));
        FO4HDR_WARP_CHECK(!IsPresentationProxyTarget(otherMip.Get()));
        FO4HDR_WARP_CHECK(AcquireSceneView(otherMip.Get()) == nullptr);
        g_presentation.presentationProxy = proxy.texture;
    }

    void CheckOutput()
    {
        device.context->CopyResource(output.staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "Read hooked HDR10 output");
        const auto pixel = *static_cast<const std::uint32_t*>(mapped.pData);
        const auto expected = QuantizeR10Unorm(EncodeSt2084Pq(101.5));
        for (UINT channel = 0; channel < 3; ++channel)
            FO4HDR_WARP_CHECK(CodeDistance(static_cast<std::uint16_t>((pixel >> (channel * 10)) & 1023),
                expected) <= 1);
        FO4HDR_WARP_CHECK(pixel >> 30 == 3);
        device.context->Unmap(output.staging.Get(), 0);
        device.context->CopyResource(proxy.staging.Get(), proxy.texture.Get());
        CheckHresult(device.context->Map(proxy.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "Read unchanged hooked FP16 proxy");
        const auto* values = static_cast<const std::uint16_t*>(mapped.pData);
        for (UINT channel = 0; channel < 3; ++channel)
            FO4HDR_WARP_CHECK(Binary16ToFloat(values[channel]) == 0.5F);
        device.context->Unmap(proxy.staging.Get(), 0);
    }

    // Unsafe scopes block optional mid-frame work only. Final output still
    // renders; an application scope left open across Present may count it.
    void CheckQueryGatedPresentation()
    {
        {
            ScopedPresentationContext midFrame(device.context.Get());
            FO4HDR_WARP_CHECK(!midFrame.Ready());
        }
        const DrawObservation expected = *record;
        FO4HDR_WARP_CHECK(RenderReferenceFrame(chain.Get(), device.device.Get(), device.context.Get()) ==
            PresentationResult::rendered);
        FO4HDR_WARP_CHECK(RenderSdrPresentationProxy(chain.Get(), device.device.Get(), device.context.Get()) !=
            PresentationResult::blocked);
        CheckObservation(expected);
    }

    static void STDMETHODCALLTYPE IgnoreBegin(ID3D11DeviceContext*, ID3D11Asynchronous*) {}

    void CheckEndOnlyQueries()
    {
        // Begin is invalid for End-only kinds; terminate the probe before native D3D11.
        const auto nextBegin = record->nextBegin;
        record->nextBegin = &IgnoreBegin;
        for (const auto type : {D3D11_QUERY_EVENT, D3D11_QUERY_TIMESTAMP})
        {
            const D3D11_QUERY_DESC description{type, 0};
            ComPtr<ID3D11Query> query;
            CheckHresult(device.device->CreateQuery(&description, &query), "Create End-only query");
            HookBegin(device.context.Get(), query.Get());
            FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
        }
        HookBegin(device.context.Get(), nullptr);
        record->nextBegin = nextBegin;
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
    }

    void CheckUnsafeQueries()
    {
        CheckEndOnlyQueries();
        const D3D11_QUERY_DESC description{D3D11_QUERY_OCCLUSION, 0};
        ComPtr<ID3D11Query> query;
        CheckHresult(device.device->CreateQuery(&description, &query), "Create hooked presentation query");
        device.context->Begin(query.Get());
        device.context->Begin(query.Get());
        CheckQueryGatedPresentation();
        // A scope left open across Present still blocks the next frame's optional work.
        record->computeQueries.RetireFrame();
        CheckQueryGatedPresentation();
        device.context->End(query.Get());
        FO4HDR_WARP_CHECK(ReadQuery<UINT64>(device, query.Get()) > 0);
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
        const D3D11_QUERY_DESC predicateDescription{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
        ComPtr<ID3D11Predicate> predicate;
        CheckHresult(device.device->CreatePredicate(&predicateDescription, &predicate),
            "Create blocking occlusion predicate");
        device.context->Begin(predicate.Get());
        CheckQueryGatedPresentation();
        device.context->End(predicate.Get());
        FO4HDR_WARP_CHECK(ReadQuery<BOOL>(device, predicate.Get()) == TRUE);
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
        // An unknown scope left by a broken caller expires with its frame.
        record->computeQueries.unknown = true;
        CheckQueryGatedPresentation();
        record->computeQueries.RetireFrame();
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
        FO4HDR_WARP_CHECK(RenderReferenceFrame(chain.Get(), device.device.Get(), device.context.Get()) ==
            PresentationResult::rendered);
    }

    void CheckProfilingQueries()
    {
        const D3D11_QUERY_DESC statisticsDescription{D3D11_QUERY_PIPELINE_STATISTICS, 0};
        const D3D11_QUERY_DESC disjointDescription{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ComPtr<ID3D11Query> statistics, disjoint;
        CheckHresult(device.device->CreateQuery(&statisticsDescription, &statistics),
            "Create presentation pipeline statistics");
        CheckHresult(device.device->CreateQuery(&disjointDescription, &disjoint),
            "Create presentation timestamp disjoint");
        device.context->Begin(statistics.Get());
        device.context->Begin(disjoint.Get());
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
        const DrawObservation expected = *record;
        FO4HDR_WARP_CHECK(RenderReferenceFrame(chain.Get(), device.device.Get(), device.context.Get()) ==
            PresentationResult::rendered);
        CheckObservation(expected);
        device.context->End(disjoint.Get());
        device.context->End(statistics.Get());
        const auto counts = ReadQuery<D3D11_QUERY_DATA_PIPELINE_STATISTICS>(device, statistics.Get());
        FO4HDR_WARP_CHECK(counts.IAVertices > 0 && counts.IAPrimitives > 0);
        FO4HDR_WARP_CHECK(counts.CPrimitives > 0 && counts.PSInvocations > 0);
        FO4HDR_WARP_CHECK(counts.GSInvocations == 0);
        FO4HDR_WARP_CHECK(ReadQuery<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(device, disjoint.Get()).Frequency > 0);
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
    }

    void CheckCounterClassification()
    {
        ProfilingCounter counter;
        const auto nextBegin = record->nextBegin;
        const auto nextEnd = record->nextEnd;
        // Stack probes terminate at counting callbacks and never reach native D3D11.
        record->nextBegin = &ProfilingCounter::Begin;
        record->nextEnd = &ProfilingCounter::End;
        HookBegin(device.context.Get(), &counter);
        const bool allowed = record->computeQueries.CanDispatch();
        HookEnd(device.context.Get(), &counter);
        counter.recognized = false;
        HookBegin(device.context.Get(), &counter);
        HookEnd(device.context.Get(), &counter);
        record->nextBegin = nextBegin;
        record->nextEnd = nextEnd;
        FO4HDR_WARP_CHECK(allowed);
        FO4HDR_WARP_CHECK(!record->computeQueries.CanDispatch());
        FO4HDR_WARP_CHECK(counter.beginCalls == 2 && counter.endCalls == 2);
        FO4HDR_WARP_CHECK(counter.counterQueries == 2);
        FO4HDR_WARP_CHECK(counter.references == 1 && counter.acquisitions == 1 && counter.releases == 1);
    }

    void CheckStreamOutputQuery(ID3D11Asynchronous* query)
    {
        device.context->Begin(query);
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
        {
            ScopedPresentationContext owned(device.context.Get());
            FO4HDR_WARP_CHECK(owned.Ready());
        }
        device.context->End(query);
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
    }

    void CheckStreamOutputQueries()
    {
        // Only the aggregate overflow kind supports the CreatePredicate contract.
        for (const auto kind : {D3D11_QUERY_SO_STATISTICS, D3D11_QUERY_SO_STATISTICS_STREAM0,
                 D3D11_QUERY_SO_STATISTICS_STREAM1, D3D11_QUERY_SO_STATISTICS_STREAM2,
                 D3D11_QUERY_SO_STATISTICS_STREAM3,
                 D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0,
                 D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1,
                 D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2,
                 D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3})
        {
            const D3D11_QUERY_DESC description{kind, 0};
            ComPtr<ID3D11Query> query;
            CheckHresult(device.device->CreateQuery(&description, &query), "Create allowed SO query");
            CheckStreamOutputQuery(query.Get());
        }
        const D3D11_QUERY_DESC description{D3D11_QUERY_SO_OVERFLOW_PREDICATE, 0};
        ComPtr<ID3D11Predicate> predicate;
        CheckHresult(device.device->CreatePredicate(&description, &predicate),
            "Create allowed aggregate SO predicate");
        CheckStreamOutputQuery(predicate.Get());
    }

    void CheckSeedWithStreamOutput(NativeStreamOutput& native)
    {
        SharedUiComposition composition;
        FO4HDR_WARP_CHECK(composition.Create(device.device.Get(), kWidth, kHeight));
        auto seeded = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        {
            ScopedPresentationContext owned(device.context.Get());
            FO4HDR_WARP_CHECK(owned.Ready());
            FO4HDR_WARP_CHECK(composition.Seed(device.context.Get(),
                g_rendererDevice.activeContext1.Get(), {proxy.srv.Get(), seeded.rtv.Get(), {203, 203}}));
        }
        native.CheckBindings();
        device.context->CopyResource(seeded.staging.Get(), seeded.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(seeded.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "Read SO-compatible shared UI seed");
        const auto pixel = *static_cast<const std::array<std::uint16_t, 4>*>(mapped.pData);
        device.context->Unmap(seeded.staging.Get(), 0);
        const auto expected = 1.055 * std::pow(0.5, 1.0 / 2.4) - 0.055;
        for (UINT channel = 0; channel < 3; ++channel)
            FO4HDR_WARP_CHECK(std::abs(Binary16ToFloat(pixel[channel]) - expected) < 0.001);
        FO4HDR_WARP_CHECK(Binary16ToFloat(pixel[3]) == 1);
    }

    void CheckNativeStreamOutput(bool forceOverflow)
    {
        NativeStreamOutput native(device);
        native.Begin(*record);
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
        {
            ScopedPresentationContext owned(device.context.Get());
            FO4HDR_WARP_CHECK(owned.Ready());
            BorrowedFullscreenState borrowed(device.context.Get(),
                g_rendererDevice.activeContext1.Get(), output.texture.Get());
            FO4HDR_WARP_CHECK(borrowed.Ready());
            // The optional UI backend runs before the final pass binds its shaders.
            ComPtr<ID3D11GeometryShader> geometry;
            device.context->GSGetShader(&geometry, nullptr, nullptr);
            FO4HDR_WARP_CHECK(!geometry);
        }
        native.CheckBindings();
        CheckSeedWithStreamOutput(native);
        CheckProfilingQueries();
        native.CheckBindings();
        CheckOutput();
        native.CheckResult(forceOverflow);
        FO4HDR_WARP_CHECK(record->computeQueries.CanDispatch());
    }

    void CheckEarlyReturns(const DrawObservation& expected)
    {
        const auto constants = g_presentation.constants;
        g_presentation.constants = CreateConstantBuffer(*device.device.Get(), fo4hdr::CompositionConstants{});
        FO4HDR_WARP_CHECK(RenderReferenceFrame(chain.Get(), device.device.Get(), device.context.Get()) ==
            PresentationResult::retry);
        g_presentation.constants = constants;
        CheckObservation(expected);
        CheckOutput();
        g_presentation.colorSpaceSupported = false;
        FO4HDR_WARP_CHECK(RenderReferenceFrame(chain.Get(), device.device.Get(), device.context.Get()) ==
            PresentationResult::failed);
        g_presentation.colorSpaceSupported = true;
        CheckObservation(expected);
        CheckOutput();
    }
};


struct ObservedSceneDraw
{
    RenoDxShaderPass pass;
    Texture& target;
    ID3D11ShaderResourceView* input{};
    ID3D11Buffer* expectedConstants{};
    ID3D11RenderTargetView* temporalOutput{};
};

struct ObservedSceneFixture : PresentationContextFixture
{
    ReplacementShaderSet savedReplacements{g_replacements};
    std::uint64_t savedFrame{g_frame};
    ContextAtomicRestore<std::uint64_t, 1> generations{std::array{&g_evidence.resourceGeneration}};
    ContextAtomicRestore<DWORD, 3> threads{std::array{&g_observedPresentThread,
        &g_currentFrameQualifiedDrawThread, &g_observedQualifiedDrawThread}};
    ContextAtomicRestore<bool, 2> states{std::array{&g_liveSettingsThreadCompatible,
        &g_currentFrameQualifiedDrawThreadConflict}};
    ContextAtomicRestore<std::uint32_t, 1> domains{std::array{&g_evidence.colorDomain}};
    Texture sceneA, sceneB, history;
    ComPtr<ID3D11Buffer> foreignConstants;

    explicit ObservedSceneFixture(Device& value) : PresentationContextFixture(value)
    {
        PrepareResources();
        sceneA = CreateSceneTexture();
        sceneB = CreateSceneTexture();
        history = CreateSceneTexture();
        g_activeSettings.colorGradingMethod = ColorGradingMethod::radial;
        g_evidence.resourceGeneration = 1;
        g_frame = 1;
        g_observedPresentThread = 0;
        g_currentFrameQualifiedDrawThread = 0;
        g_currentFrameQualifiedDrawThreadConflict = false;
        g_liveSettingsThreadCompatible = true;
        PrepareObservedProductionBindings(device);
        foreignConstants = CreateConstantBuffer(*device.device.Get(), OperatorConstants{});
    }

    ~ObservedSceneFixture()
    {
        g_replacements = savedReplacements;
        g_frame = savedFrame;
    }

    Texture CreateSceneTexture()
    {
        return CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    }

    void Observe(const ObservedSceneDraw& draw)
    {
        auto* context = device.context.Get();
        ID3D11ShaderResourceView* clear{};
        context->PSSetShaderResources(0, 1, &clear);
        const bool temporal = draw.pass == RenoDxShaderPass::taa || draw.pass == RenoDxShaderPass::taa2;
        std::array<ID3D11RenderTargetView*, 2> targets{draw.target.rtv.Get(),
            temporal ? (draw.temporalOutput ? draw.temporalOutput : proxy.rtv.Get()) : nullptr};
        context->OMSetRenderTargets(temporal ? 2 : 1, targets.data(), nullptr);
        context->PSSetShaderResources(0, 1, &draw.input);
        context->PSSetShader(g_presentation.presentShader.Get(), nullptr, 0);
        auto* previous = foreignConstants.Get();
        context->PSSetConstantBuffers(kProductionOperatorConstantSlot, 1, &previous);
        DrawObservation observation{.immediate = true, .generation = 1,
            .currentPrimaryRtv = targets[0], .currentSecondaryRtv = targets[1],
            .currentPass = draw.pass, .currentPassKnown = true, .currentReplacementActive = true,
            .expectedBoundShader = g_presentation.presentShader.Get()};
        ObservedConstantProbe probe{context, draw.expectedConstants};
        ExecuteObservedDraw(observation, context, {ObservedConstantProbe::Invoke, &probe});
        FO4HDR_WARP_CHECK(probe.calls == 1);
        ComPtr<ID3D11Buffer> restored;
        context->PSGetConstantBuffers(kProductionOperatorConstantSlot, 1, &restored);
        FO4HDR_WARP_CHECK(restored.Get() == previous);
        FO4HDR_WARP_CHECK(g_referenceHealthy.load());
    }

    void BeginScene()
    {
        g_scene = {};
        g_heartbeat.Reset(1);
        g_heartbeat.BeginFrame(1, g_frame);
        g_replacements.configurationReady = true;
        auto* production = g_replacements.operatorConstants.Get();
        Observe({RenoDxShaderPass::toneMapA, sceneA, nullptr, production});
        Observe({RenoDxShaderPass::lut, sceneA, nullptr, production});
        FO4HDR_WARP_CHECK(SameResourceIdentity(g_scene.currentScene.Get(), sceneA.srv.Get()));
        FO4HDR_WARP_CHECK(g_scene.productionOperatorApplied && !g_scene.temporalSceneReady);
    }

    void CheckTemporalInputs(RenoDxShaderPass pass)
    {
        BeginScene();
        Observe({pass, history, sceneB.srv.Get(), g_replacements.bypassConstants.Get()});
        FO4HDR_WARP_CHECK(!g_scene.temporalPassObserved && !g_scene.temporalSceneReady);
        FO4HDR_WARP_CHECK(SameResourceIdentity(g_scene.currentScene.Get(), sceneA.srv.Get()));
        ComPtr<ID3D11ShaderResourceView> alias;
        CheckHresult(device.device->CreateShaderResourceView(sceneA.texture.Get(), nullptr, &alias),
            "Create selected temporal input alias");
        Observe({pass, history, alias.Get(), g_replacements.operatorConstants.Get()});
        FO4HDR_WARP_CHECK(g_scene.temporalPassObserved && g_scene.temporalSceneReady);
        FO4HDR_WARP_CHECK(g_scene.currentScene.Get() == proxy.srv.Get());
    }

    void CheckNewLutRevision(Texture& target, bool production)
    {
        BeginScene();
        Observe({RenoDxShaderPass::taa, history, sceneA.srv.Get(), g_replacements.operatorConstants.Get()});
        FO4HDR_WARP_CHECK(g_scene.temporalSceneReady);
        g_replacements.configurationReady = production;
        Observe({RenoDxShaderPass::lut, target, nullptr, production ?
            g_replacements.operatorConstants.Get() : g_replacements.bypassConstants.Get()});
        FO4HDR_WARP_CHECK(SameResourceIdentity(g_scene.currentScene.Get(), target.srv.Get()));
        FO4HDR_WARP_CHECK(!g_scene.temporalPassObserved && !g_scene.temporalSceneReady);
        FO4HDR_WARP_CHECK(!g_scene.videoSceneReady && g_scene.productionOperatorApplied == production);
        FO4HDR_WARP_CHECK(g_scene.currentSceneDomain == (production ?
            LinearColorDomain::linearBt2020UiRelative : LinearColorDomain::linearBt709Relative));
        if (target.texture.Get() == proxy.texture.Get())
            FO4HDR_WARP_CHECK(g_presentation.presentationProxyDomain == g_scene.currentSceneDomain);
    }

    void CheckVideoAndOptionalContinuation()
    {
        BeginScene();
        Observe({RenoDxShaderPass::taa, history, sceneA.srv.Get(), g_replacements.operatorConstants.Get()});
        Observe({RenoDxShaderPass::fxaa, sceneB, proxy.srv.Get(), foreignConstants.Get()});
        FO4HDR_WARP_CHECK(SameResourceIdentity(g_scene.currentScene.Get(), sceneB.srv.Get()));
        FO4HDR_WARP_CHECK(g_scene.temporalPassObserved && g_scene.temporalSceneReady);
        FO4HDR_WARP_CHECK(g_scene.productionOperatorApplied && !g_scene.videoSceneReady);
        Observe({RenoDxShaderPass::video, sceneA, nullptr, g_replacements.operatorConstants.Get()});
        FO4HDR_WARP_CHECK(SameResourceIdentity(g_scene.currentScene.Get(), sceneA.srv.Get()));
        FO4HDR_WARP_CHECK(g_scene.videoSceneReady && !g_scene.productionOperatorApplied);
        FO4HDR_WARP_CHECK(!g_scene.temporalPassObserved && !g_scene.temporalSceneReady);
        Observe({RenoDxShaderPass::lut, sceneB, nullptr, g_replacements.operatorConstants.Get()});
        FO4HDR_WARP_CHECK(!g_scene.videoSceneReady && g_scene.productionOperatorApplied);
    }

    void CheckMissingProxyRetiresTemporal()
    {
        BeginScene();
        Observe({RenoDxShaderPass::taa, history, sceneA.srv.Get(), g_replacements.operatorConstants.Get()});
        Observe({RenoDxShaderPass::fxaa, sceneB, proxy.srv.Get(), foreignConstants.Get()});
        FO4HDR_WARP_CHECK(g_scene.temporalSceneReady);
        Observe({RenoDxShaderPass::taa2, history, sceneB.srv.Get(),
            g_replacements.bypassConstants.Get(), output.rtv.Get()});
        FO4HDR_WARP_CHECK(g_scene.temporalPassObserved && !g_scene.temporalSceneReady);
        FO4HDR_WARP_CHECK(SameResourceIdentity(g_scene.currentScene.Get(), sceneB.srv.Get()));
    }

    void CheckNonTemporalContinuation()
    {
        BeginScene();
        g_presentation.presentationProxyDomain = LinearColorDomain::unknown;
        Observe({RenoDxShaderPass::fxaa, proxy, sceneA.srv.Get(), foreignConstants.Get()});
        FO4HDR_WARP_CHECK(g_scene.currentScene.Get() == proxy.srv.Get());
        FO4HDR_WARP_CHECK(!g_scene.temporalPassObserved && !g_scene.temporalSceneReady);
        FO4HDR_WARP_CHECK(g_presentation.presentationProxyDomain ==
            LinearColorDomain::linearBt2020UiRelative);
    }
};


struct NativeReplacementDrawProbe
{
    ContextRecord& record;
    Draw next;
    ID3D11PixelShader* expectedShader;
    ID3D11Buffer* expectedConstants;
    UINT calls{};
    static inline thread_local NativeReplacementDrawProbe* active{};

    NativeReplacementDrawProbe(ContextRecord& observation, ID3D11PixelShader* shader,
        ID3D11Buffer* constants) : record(observation), next(record.nextDraw),
        expectedShader(shader), expectedConstants(constants)
    {
        FO4HDR_WARP_CHECK(active == nullptr);
        active = this;
        record.nextDraw = Invoke;
    }
    ~NativeReplacementDrawProbe()
    {
        record.nextDraw = next;
        active = nullptr;
    }
    static void STDMETHODCALLTYPE Invoke(ID3D11DeviceContext* context, UINT count, UINT start)
    {
        auto& probe = *active;
        ComPtr<ID3D11PixelShader> shader;
        ComPtr<ID3D11Buffer> constants;
        context->PSGetShader(&shader, nullptr, nullptr);
        context->PSGetConstantBuffers(kProductionOperatorConstantSlot, 1, &constants);
        FO4HDR_WARP_CHECK(shader.Get() == probe.expectedShader);
        FO4HDR_WARP_CHECK(constants.Get() == probe.expectedConstants);
        ++probe.calls;
        probe.next(context, count, start);
    }
};

struct DisabledReplacementFixture : SharedPresentationContextFixture
{
    ComPtr<ID3D11PixelShader> temporalShader;
    ComPtr<ID3D11Buffer> nativeConstants;
    Texture lut, history;

    explicit DisabledReplacementFixture(Device& source) : SharedPresentationContextFixture(source)
    {
        BindHud();
        PrepareObservedProductionBindings(device);
        g_presentation.colorDomainGeneration = g_evidence.resourceGeneration.load();
        PublishActiveDisplay(nullptr, 1);
        g_activeSettings.videoWhiteNits = 203;
        nativeConstants = CreateConstantBuffer(*device.device.Get(), OperatorConstants{});
        temporalShader = CreatePixelShader(*device.device.Get(), generated::test::kSharedUiTemporalPs);
        lut = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        history = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    }

    ~DisabledReplacementFixture() { ResetShaderRegistry(); }

    void BindReplacement(RenoDxShaderPass pass)
    {
        CreatedShaderIdentity identity{.descriptor = FindShaderDescriptor(pass)};
        auto* shader = hudShader.Get();
        identity.Register(S_OK, &shader);
        auto* target = pass == RenoDxShaderPass::lut ? lut.rtv.Get() : proxy.rtv.Get();
        device.context->OMSetRenderTargets(1, &target, nullptr);
        device.context->PSSetShader(shader, nullptr, 0);
        auto* constants = nativeConstants.Get();
        device.context->PSSetConstantBuffers(kProductionOperatorConstantSlot, 1, &constants);
        FO4HDR_WARP_CHECK(record->currentReplacementActive);
        FO4HDR_WARP_CHECK(record->nativePixelShader.Get() == shader);
        FO4HDR_WARP_CHECK(record->expectedBoundShader == g_presentation.presentShader.Get());
    }

    void CompleteOriginalLut()
    {
        CreatedShaderIdentity identity{.descriptor = FindShaderDescriptor(RenoDxShaderPass::taa)};
        auto* shader = temporalShader.Get();
        identity.Register(S_OK, &shader);
        const std::array targets{history.rtv.Get(), proxy.rtv.Get()};
        device.context->OMSetRenderTargets(2, targets.data(), nullptr);
        auto* input = lut.srv.Get();
        device.context->PSSetShaderResources(0, 1, &input);
        device.context->PSSetShader(shader, nullptr, 0);
        device.context->Draw(3, 0);
    }

    void CheckDisabledReplacement(RenoDxShaderPass pass, bool foreign)
    {
        BindReplacement(pass);
        g_productionHealthy = false;
        auto* expected = hudShader.Get();
        if (foreign)
        {
            expected = temporalShader.Get();
            record->nextPsSetShader(device.context.Get(), expected, nullptr, 0);
        }
        const auto productionDraws = g_evidence.productionOperatorDrawCount.load();
        {
            NativeReplacementDrawProbe probe(*record, expected, nativeConstants.Get());
            device.context->Draw(3, 0);
            FO4HDR_WARP_CHECK(probe.calls == 1);
        }
        FO4HDR_WARP_CHECK(g_evidence.productionOperatorDrawCount.load() == productionDraws);
        FO4HDR_WARP_CHECK(!record->currentReplacementActive && !record->nativePixelShader);
        FO4HDR_WARP_CHECK(record->expectedBoundShader == expected);
        FO4HDR_WARP_CHECK(record->currentPassKnown == !foreign);
        FO4HDR_WARP_CHECK(g_referenceHealthy.load());
        if (foreign)
        {
            FO4HDR_WARP_CHECK(!SharedSdrFrameReady());
            return;
        }
        if (pass == RenoDxShaderPass::lut) CompleteOriginalLut();
        FO4HDR_WARP_CHECK(SharedSdrFrameReady());
        FO4HDR_WARP_CHECK(g_presentation.presentationProxyDomain == LinearColorDomain::vanillaSdrCodes);
        // The synthetic HDR bind must not simulate a real display-generation change.
        PublishActiveDisplay(nullptr, -1);
        FO4HDR_WARP_CHECK(PreparePresentFrame(chain.Get(), true));
        CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(std::pow(0.5, 2.2) * 203)));
        FO4HDR_WARP_CHECK(!g_productionHealthy.load());
        CompletePresentFrame(true, false);
    }

    void CheckDynamicClasses()
    {
        ComPtr<ID3D11ClassLinkage> linkage;
        ComPtr<ID3D11PixelShader> linked;
        ComPtr<ID3D11ClassInstance> instance;
        CheckHresult(device.device->CreateClassLinkage(&linkage), "Create native shader linkage");
        const auto& code = generated::test::kFullscreenStatePs;
        CheckHresult(device.device->CreatePixelShader(code.data(), code.size(), linkage.Get(), &linked),
            "Create native linked shader");
        CheckHresult(linkage->CreateClassInstance("ForeignPixelValue", 0, 0, 0, 0, &instance),
            "Create native shader class");
        CreatedShaderIdentity identity{.descriptor = FindShaderDescriptor(RenoDxShaderPass::lut)};
        auto* shader = linked.Get();
        identity.Register(S_OK, &shader);
        FO4HDR_WARP_CHECK(FindReplacement(RenoDxShaderPass::lut) != nullptr);
        auto* value = instance.Get();
        device.context->PSSetShader(shader, &value, 1);
        ComPtr<ID3D11PixelShader> actual;
        ID3D11ClassInstance* actualClass{};
        UINT count = 1;
        device.context->PSGetShader(&actual, &actualClass, &count);
        ComPtr<ID3D11ClassInstance> retainedClass;
        retainedClass.Attach(actualClass);
        FO4HDR_WARP_CHECK(actual.Get() == shader && count == 1);
        FO4HDR_WARP_CHECK(retainedClass.Get() == instance.Get());
        FO4HDR_WARP_CHECK(!record->currentReplacementActive && !record->nativePixelShader);
        FO4HDR_WARP_CHECK(!record->currentPassKnown);
    }
};

void CheckDisabledReplacements(Device& device)
{
    for (const auto pass : {RenoDxShaderPass::lut, RenoDxShaderPass::video})
    {
        for (const bool foreign : {false, true})
        {
            DisabledReplacementFixture fixture(device);
            fixture.CheckDisabledReplacement(pass, foreign);
        }
    }
    DisabledReplacementFixture fixture(device);
    fixture.CheckDynamicClasses();
    fixture.BindReplacement(RenoDxShaderPass::lut);
    ResetSceneTracking();
    FO4HDR_WARP_CHECK(fixture.record->nativePixelShader.Get() == fixture.hudShader.Get());
    RetireLostDeviceGeneration();
    FO4HDR_WARP_CHECK(!fixture.record->nativePixelShader);
    FO4HDR_WARP_CHECK(FindContext(device.context.Get()) == fixture.record);
}

void CheckRetainedPartialCopy(Device& device, bool productionDisabled, bool empty)
{
    SharedPresentationContextFixture fixture(device);
    fixture.PrepareScene();
    CompletePresentFrame(false, false);
    if (productionDisabled) g_productionHealthy = false;
    else
    {
        PublishGraphicsLifecycle(false, true);
        ApplyGraphicsLifecycleBeforeDraw(device.context.Get());
    }
    const auto source = CreateTexture(*device.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D11_BIND_RENDER_TARGET);
    constexpr float gray[]{0.25F, 0.25F, 0.25F, 1};
    device.context->ClearRenderTargetView(source.rtv.Get(), gray);
    const D3D11_BOX region{0, 0, 0, empty ? 0U : 1U, 1, 1};
    device.context->CopySubresourceRegion(fixture.proxy.texture.Get(), 0, 0, 0, 0,
        source.texture.Get(), 0, &region);
    const auto pixels = fixture.ReadPublicPixels();
    FO4HDR_WARP_CHECK(Binary16ToFloat(pixels[0][0]) == (empty ? 0.5 : 0.25));
    for (UINT x = 1; x < kWidth; ++x)
        FO4HDR_WARP_CHECK(Binary16ToFloat(pixels[x][0]) == 0.5);
    FO4HDR_WARP_CHECK(!ArmSharedSdrFrame(device.context.Get()));
    const auto retained = SharedUiEmergencyPresentation();
    FO4HDR_WARP_CHECK(retained && retained->white == 203 && retained->assumed);
    fixture.PresentReadable(empty ? 0.5 : 0.25);
    fixture.CheckOutputCode(QuantizeR10Unorm(EncodeSt2084Pq(0.5 * 203)), 1);
}

void CheckOutputRecovery(Device& device)
{
    SharedPresentationContextFixture fixture(device);
    FO4HDR_WARP_CHECK(InitializeSharedUiSdr(device.context.Get()));
    constexpr float gray[]{0.5F, 0.5F, 0.5F, 1};
    device.context->ClearRenderTargetView(fixture.proxy.rtv.Get(), gray);
    fixture.PresentReadable(std::pow(0.5, 2.2));
    const auto* source = g_presentation.presentationProxy.Get();
    const auto failures = g_evidence.referenceRenderFailureCount.load();
    g_presentation.constants = CreateConstantBuffer(*device.device.Get(), fo4hdr::CompositionConstants{});
    FO4HDR_WARP_CHECK(!PreparePresentFrame(fixture.chain.Get(), true));
    fixture.CheckOutputCode(0);
    FO4HDR_WARP_CHECK(g_referenceHealthy.load());
    FO4HDR_WARP_CHECK(g_presentation.presentationProxy.Get() == source);
    CompletePresentFrame(false, false);
    fixture.PresentReadable(std::pow(0.5, 2.2));
    FO4HDR_WARP_CHECK(g_evidence.referenceRenderFailureCount.load() == failures + 1);
    FO4HDR_WARP_CHECK(g_presentation.presentationProxy.Get() == source);
    FO4HDR_WARP_CHECK(g_evidence.failure.load() == static_cast<UINT>(GraphicsObservationFailure::none));
    RecordProductionResourceFailure(fixture.chain.Get());
    FO4HDR_WARP_CHECK(g_referenceHealthy.load() && !g_productionHealthy.load());
    g_presentation.bufferCount = 1;
    FO4HDR_WARP_CHECK(!PreparePresentFrame(fixture.chain.Get(), true));
    fixture.CheckOutputCode(QuantizeR10Unorm(0.5));
    FO4HDR_WARP_CHECK(g_referenceHealthy.load() && !g_evidence.hdrColorSpaceActive.load());
    CompletePresentFrame(false, false);
}

bool AcceptSettingsInteraction(bool, std::uint32_t) noexcept { return true; }

// Opens the production settings window on a popup whose client is half as wide
// as the swap-chain-sized settings layer.
struct SettingsOverlayWindow
{
    HWND window{CreateWindowExW(0, L"STATIC", L"FO4Isotope settings overlay test", WS_POPUP,
        0, 0, kWidth / 2, kHeight, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};

    explicit SettingsOverlayWindow(Device& device)
    {
        InitializeSettingsWindow(HdrSettings{}, HdrInterfaceSettings{});
        RegisterHdrSettingsInteractionRequestHandler(&AcceptSettingsInteraction);
        EnsureSettingsWindowBackend(true, window, device.device.Get(), device.context.Get());
        FO4HDR_WARP_CHECK(TryActivateSettingsWindowInput().active);
        FO4HDR_WARP_CHECK(SetSettingsWindowOpen(true) && SettingsWindowOpen());
    }

    ~SettingsOverlayWindow()
    {
        FO4HDR_WARP_CHECK(SetSettingsWindowOpen(false));
        ShutdownSettingsWindowBackend();
        RegisterHdrSettingsInteractionRequestHandler(nullptr);
        if (window) DestroyWindow(window);
    }

    SettingsOverlayWindow(const SettingsOverlayWindow&) = delete;
    SettingsOverlayWindow& operator=(const SettingsOverlayWindow&) = delete;

    [[nodiscard]] SIZE ClientSize() const
    {
        RECT client{};
        GetClientRect(window, &client);
        return {client.right - client.left, client.bottom - client.top};
    }
};

float SettingsLayerAlpha(SharedPresentationContextFixture& fixture, UINT x)
{
    auto* context = fixture.device.context.Get();
    fixture.record->nextCopyResource(context, fixture.proxy.staging.Get(), g_uiTransport.ui.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(context->Map(fixture.proxy.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read settings layer");
    const auto alpha = Binary16ToFloat(static_cast<const std::uint16_t*>(mapped.pData)[x * 4 + 3]);
    context->Unmap(fixture.proxy.staging.Get(), 0);
    return alpha;
}

// After a semantic reset, fallback output rebuilds the settings layer once and
// draws the open window onto it.
void CheckSettingsOverlayFallback(Device& device)
{
    SharedPresentationContextFixture fixture(device);
    SettingsOverlayWindow settings(device);
    const auto render = [&] {
        return RenderSdrPresentationProxy(fixture.chain.Get(), device.device.Get(),
            device.context.Get());
    };
    const auto client = settings.ClientSize();
    FO4HDR_WARP_CHECK(client.cx == kWidth / 2 && client.cy == kHeight);
    ResetResources(true, true);
    FO4HDR_WARP_CHECK(!DeveloperUiResourcesReady());
    FO4HDR_WARP_CHECK(render() == PresentationResult::rendered);
    FO4HDR_WARP_CHECK(DeveloperUiResourcesReady());
    if (!DeveloperUiResourcesReady()) return;
    // The clamped window covers its whole client, so it reaches the last layer
    // column only when its client coordinates are scaled onto the layer.
    FO4HDR_WARP_CHECK(SettingsLayerAlpha(fixture, 0) > 0.0F);
    FO4HDR_WARP_CHECK(SettingsLayerAlpha(fixture, kWidth - 1) > 0.0F);
    const auto* layer = g_uiTransport.ui.Get();
    SetWindowPos(settings.window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    FO4HDR_WARP_CHECK(settings.ClientSize().cx == 0);
    FO4HDR_WARP_CHECK(render() == PresentationResult::rendered);
    FO4HDR_WARP_CHECK(g_uiTransport.ui.Get() == layer);
}

} // namespace

void VerifyPresentationContext(Device& device)
{
    auto native = CreateNativeContextDevice(device);
    VerifyNativeSceneInitialization(native);
    {
        ObservedSceneFixture fixture(native);
        fixture.CheckTemporalInputs(RenoDxShaderPass::taa);
        fixture.CheckTemporalInputs(RenoDxShaderPass::taa2);
        fixture.CheckNewLutRevision(fixture.sceneB, true);
        fixture.CheckNewLutRevision(fixture.sceneA, true);
        fixture.CheckNewLutRevision(fixture.proxy, true);
        fixture.CheckNewLutRevision(fixture.sceneB, false);
        fixture.CheckVideoAndOptionalContinuation();
        fixture.CheckMissingProxyRetiresTemporal();
        fixture.CheckNonTemporalContinuation();
    }
    {
        PresentationContextScenario fixture(native);
        fixture.PrepareResources();
        FO4HDR_WARP_CHECK(DeveloperUiResourcesReady());
        FO4HDR_WARP_CHECK(PresentationResourcesReady());
        FO4HDR_WARP_CHECK(CurrentWideGamutReadiness().uiSet);
        const auto uiSrv = g_uiTransport.uiSrv;
        g_uiTransport.uiSrv.Reset();
        FO4HDR_WARP_CHECK(!DeveloperUiResourcesReady());
        FO4HDR_WARP_CHECK(!PresentationResourcesReady());
        FO4HDR_WARP_CHECK(!CurrentWideGamutReadiness().uiSet);
        g_uiTransport.uiSrv = uiSrv;
        FO4HDR_WARP_CHECK(PresentationResourcesReady());
        FO4HDR_WARP_CHECK(CurrentWideGamutReadiness().uiSet);
        fixture.InstallHooks();
        fixture.CheckProxyAliases();
        fixture.CheckProxyViewRejection(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, 1);
        fixture.CheckProxyViewRejection(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, 0);
        const DrawObservation expected = *fixture.record;
        FO4HDR_WARP_CHECK(RenderReferenceFrame(fixture.chain.Get(), native.device.Get(), native.context.Get()) ==
            PresentationResult::rendered);
        fixture.CheckObservation(expected);
        fixture.CheckOutput();
        fixture.CheckUnsafeQueries();
        fixture.CheckOutput();
        fixture.CheckEarlyReturns(expected);
        native.context->Draw(0, 0);
        fixture.CheckObservation(expected);
        FO4HDR_WARP_CHECK(g_referenceHealthy.load());
    }
    {
        PresentationContextScenario fixture(native);
        fixture.PrepareResources();
        fixture.InstallHooks();
        fixture.CheckStreamOutputQueries();
        fixture.CheckNativeStreamOutput(false);
        fixture.CheckNativeStreamOutput(true);
        fixture.CheckCounterClassification();
    }
    VerifySharedUiContext(native);
    CheckDisabledReplacements(native);
    for (const bool productionDisabled : {false, true})
        for (const bool empty : {false, true})
            CheckRetainedPartialCopy(native, productionDisabled, empty);
    CheckOutputRecovery(native);
    CheckSettingsOverlayFallback(native);
}

} // namespace fo4hdr::tests::gpu
