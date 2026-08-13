// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "passes/PresentationPass.hpp"
#include "hooks/ContextHooks.hpp"
#include "hooks/SwapChainHooks.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <type_traits>

namespace fo4hdr::tests::gpu {
namespace {

using namespace detail;

void VerifyBorrowedRouting(PixelFixture& fixture, const std::filesystem::path& directory)
{
    ScenePassRenderer borrowed;
    const ScenePassConfiguration configuration{width, height, fixture.whites};
    FO4HDR_CHECK(borrowed.InitializeBorrowed(fixture.gpu.device.Get(),
        fixture.gpu.context.Get(), configuration));
    const auto original = SceneDescription();
    auto description = original;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<ID3D11Texture2D> native;
    CheckHresult(fixture.gpu.device->CreateTexture2D(&description, nullptr, &native),
        "Borrowed native texture result");
    SceneTextureHandle adopted;
    CheckHresult(borrowed.AdoptSceneTexture(original, native.Get(), adopted), "Adopt native texture");
    FO4HDR_CHECK(borrowed.Views(adopted).texture == native.Get());
    const auto code = ReadShader(directory / "ScenePassPlain.dxbc");
    FO4HDR_CHECK(!borrowed.ObserveCreatedPixelShader(std::as_bytes(std::span{code}),
        fixture.plain.Get()));
    fixture.Bind(true);
    FO4HDR_CHECK(borrowed.Retire());
    ComPtr<ID3D11PixelShader> bound;
    fixture.gpu.context->PSGetShader(&bound, nullptr, nullptr);
    FO4HDR_CHECK(bound.Get() == fixture.sentinel.Get());
    FO4HDR_CHECK(borrowed.Views(adopted).texture == nullptr);
}

struct QueryAdapterEvidence
{
    ContextQueryObserver* observer{};
    ID3D11Asynchronous* scoped{};
    UINT begins{};
    UINT ends{};
    UINT lists{};
    bool beginInsideScope{};
    bool endsInsideScope{true};
    bool listInsideScope{};
};

QueryAdapterEvidence queryAdapterEvidence;

void STDMETHODCALLTYPE QueryBeginNext(ID3D11DeviceContext*, ID3D11Asynchronous* asynchronous)
{
    ++queryAdapterEvidence.begins;
    const auto& active = queryAdapterEvidence.observer->Queries().active;
    queryAdapterEvidence.beginInsideScope =
        std::find(active.begin(), active.end(), asynchronous) != active.end();
}

void STDMETHODCALLTYPE QueryEndNext(ID3D11DeviceContext*, ID3D11Asynchronous*)
{
    ++queryAdapterEvidence.ends;
    const auto& active = queryAdapterEvidence.observer->Queries().active;
    queryAdapterEvidence.endsInsideScope &=
        std::find(active.begin(), active.end(), queryAdapterEvidence.scoped) != active.end();
}

void STDMETHODCALLTYPE QueryCommandListNext(ID3D11DeviceContext*, ID3D11CommandList*, BOOL)
{
    ++queryAdapterEvidence.lists;
    queryAdapterEvidence.listInsideScope =
        queryAdapterEvidence.observer->Queries().executionDepth == 1;
}

void VerifyQueryAdapters(PixelFixture& fixture)
{
    int first{}, unmatched{};
    ContextQuerySafety policy;
    FO4HDR_CHECK(policy.CanDispatch());
    policy.Begin(&first);
    policy.End(&unmatched);
    policy.End(&first);
    FO4HDR_CHECK(!policy.CanDispatch());
    ContextQueryObserver observer;
    const ContextQueryPredecessors next{QueryBeginNext, QueryEndNext, QueryCommandListNext};
    FO4HDR_CHECK(observer.Prepare(fixture.gpu.context.Get(), 1, next, &fixture.renderer) ==
        ContextQueryPreparation::coverageUnobserved);
    ComPtr<ID3D11Query> scope, event;
    const D3D11_QUERY_DESC scopedDescription{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    const D3D11_QUERY_DESC eventDescription{D3D11_QUERY_EVENT, 0};
    CheckHresult(fixture.gpu.device->CreateQuery(&scopedDescription, &scope), "Scoped query fixture");
    CheckHresult(fixture.gpu.device->CreateQuery(&eventDescription, &event), "End-only query fixture");
    queryAdapterEvidence = {&observer, scope.Get()};
    observer.Begin(fixture.gpu.context.Get(), scope.Get());
    observer.End(fixture.gpu.context.Get(), event.Get());
    observer.End(fixture.gpu.context.Get(), scope.Get());
    FO4HDR_CHECK(queryAdapterEvidence.begins == 1 && queryAdapterEvidence.ends == 2);
    FO4HDR_CHECK(queryAdapterEvidence.beginInsideScope && queryAdapterEvidence.endsInsideScope);
    FO4HDR_CHECK(std::all_of(observer.Queries().active.begin(), observer.Queries().active.end(),
        [](const void* key) { return key == nullptr; }));
    ComPtr<ID3D11DeviceContext> deferred;
    ComPtr<ID3D11CommandList> commands;
    CheckHresult(fixture.gpu.device->CreateDeferredContext(0, &deferred), "Deferred list fixture");
    CheckHresult(deferred->FinishCommandList(FALSE, &commands), "Empty finished list fixture");
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    observer.ExecuteCommandList(fixture.gpu.context.Get(), commands.Get(), FALSE);
    FO4HDR_CHECK(queryAdapterEvidence.lists == 1 && queryAdapterEvidence.listInsideScope);
    FO4HDR_CHECK(fixture.renderer.FrameGeneration() == 0);
    FO4HDR_CHECK(!observer.CanDispatch());
    FO4HDR_CHECK(observer.Retire());
    queryAdapterEvidence = {};
}

struct CapturedPresentProbe
{
    void** vtable{};
    std::array<void*, 18> methods{};
    PresentationPass* pass;
    UINT wrappers{};
    UINT natives{};
    UINT interval{};
    UINT flags{};

    explicit CapturedPresentProbe(PresentationPass& owner) : pass(&owner)
    {
        methods[8] = reinterpret_cast<void*>(&Intercepted);
        vtable = methods.data();
    }

    static HRESULT STDMETHODCALLTYPE Native(IDXGISwapChain* chain, UINT interval, UINT flags)
    {
        auto& probe = *reinterpret_cast<CapturedPresentProbe*>(chain);
        ++probe.natives;
        probe.interval = interval;
        probe.flags = flags;
        return DXGI_STATUS_OCCLUDED;
    }

    static HRESULT STDMETHODCALLTYPE Intercepted(IDXGISwapChain* chain, UINT interval, UINT flags)
    {
        auto& probe = *reinterpret_cast<CapturedPresentProbe*>(chain);
        if (++probe.wrappers != 1) return E_FAIL;
        probe.pass->PrepareNativePresent(chain, flags, PresentationScope::unavailable);
        const auto result = Native(chain, interval, flags);
        probe.pass->CompleteNativePresent(chain, result);
        return result;
    }
};

void VerifyCapturedPresentBoundary()
{
    PresentationPass pass;
    CapturedPresentProbe probe{pass};
    static_assert(std::is_standard_layout_v<CapturedPresentProbe>);
    // Only the owned fixture's Present slot is reachable; no driver table is cloned.
    auto* chain = reinterpret_cast<IDXGISwapChain*>(&probe);
    const auto result = pass.Present(chain, 2, DXGI_PRESENT_TEST, PresentationScope::unavailable);
    FO4HDR_CHECK(result == DXGI_STATUS_OCCLUDED);
    FO4HDR_CHECK(probe.wrappers == 1 && probe.natives == 1);
    FO4HDR_CHECK(probe.interval == 2 && probe.flags == DXGI_PRESENT_TEST);
}

void VerifyNativeFrameLifecycle(PixelFixture& fixture)
{
    ScenePassRenderer borrowed;
    const ScenePassConfiguration configuration{width, height, fixture.whites};
    FO4HDR_CHECK(borrowed.InitializeBorrowed(fixture.gpu.device.Get(),
        fixture.gpu.context.Get(), configuration));
    SceneTextureHandle oldScene;
    CheckHresult(borrowed.CreateSceneTexture(SceneDescription(), nullptr, oldScene), "Resize scene fixture");
    FO4HDR_CHECK(borrowed.BeginFrame());
    const auto firstFrame = borrowed.FrameGeneration();
    FO4HDR_CHECK(CloseNativePresentFrame(borrowed, DXGI_PRESENT_TEST));
    FO4HDR_CHECK(OpenNativePresentFrame(borrowed, DXGI_PRESENT_TEST, S_OK));
    FO4HDR_CHECK(borrowed.FrameGeneration() == firstFrame);
    PresentationPass inactive;
    CapturedPresentProbe probe{inactive};
    auto* chain = reinterpret_cast<IDXGISwapChain*>(&probe);
    FO4HDR_CHECK(CloseNativePresentFrame(borrowed, DXGI_PRESENT_DO_NOT_WAIT));
    FO4HDR_CHECK(borrowed.FrameGeneration() == 0);
    const auto result = CapturedPresentProbe::Native(chain, 3, DXGI_PRESENT_DO_NOT_WAIT);
    FO4HDR_CHECK(OpenNativePresentFrame(borrowed, DXGI_PRESENT_DO_NOT_WAIT, result));
    FO4HDR_CHECK(probe.natives == 1 && probe.interval == 3 && probe.flags == DXGI_PRESENT_DO_NOT_WAIT);
    FO4HDR_CHECK(borrowed.FrameGeneration() > firstFrame);
    FO4HDR_CHECK(borrowed.RetireForResize());
    FO4HDR_CHECK(borrowed.RetireForResize());
    FO4HDR_CHECK(borrowed.Views(oldScene).texture == nullptr);
    auto invalid = configuration;
    invalid.width = 0;
    FO4HDR_CHECK(!borrowed.ReinitializeBorrowed(fixture.gpu.device.Get(), fixture.gpu.context.Get(), invalid));
    FO4HDR_CHECK(!borrowed.Active() && borrowed.FrameGeneration() == 0);
    FO4HDR_CHECK(borrowed.ReinitializeBorrowed(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
    SceneTextureHandle freshScene;
    CheckHresult(borrowed.CreateSceneTexture(SceneDescription(), nullptr, freshScene), "Fresh resize scene fixture");
    FO4HDR_CHECK(borrowed.Views(freshScene).texture != nullptr);
    FO4HDR_CHECK(borrowed.Views(oldScene).texture == nullptr);
    FO4HDR_CHECK(borrowed.FrameGeneration() == 0);
    FO4HDR_CHECK(OpenNativePresentFrame(borrowed, 0, S_OK));
    FO4HDR_CHECK(borrowed.FrameGeneration() > firstFrame);
    FO4HDR_CHECK(borrowed.Retire());
    FO4HDR_CHECK(!borrowed.ReinitializeBorrowed(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
}

}  // namespace

void VerifyNativeRouting(detail::PixelFixture& fixture, const std::filesystem::path& directory)
{
    VerifyBorrowedRouting(fixture, directory);
    VerifyQueryAdapters(fixture);
    VerifyCapturedPresentBoundary();
    VerifyNativeFrameLifecycle(fixture);
}

}  // namespace fo4hdr::tests::gpu
