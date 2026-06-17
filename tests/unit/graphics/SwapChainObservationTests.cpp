// SPDX-License-Identifier: GPL-3.0-or-later

#include "Fallout4HDR/hooks/HookOwnership.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <barrier>
#include <cstddef>
#include <new>
#include <thread>

#include <Windows.h>

#include "SwapChainObservation.hpp"

namespace {

using ReleaseFunction = ULONG (STDMETHODCALLTYPE*)(IDXGISwapChain*);
using PresentFunction = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFunction = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
    DXGI_FORMAT, UINT);

struct OwnedChain
{
    void** table;
};

static_assert(offsetof(OwnedChain, table) == 0);

struct LifecycleEvidence
{
    OwnedChain* object{};
    unsigned presentCalls{};
    unsigned foreignCalls{};
    std::barrier<>* presentGate{};
    unsigned resizeCalls{};
    unsigned releaseCalls{};
    unsigned descriptorCalls{};
    unsigned queryCalls{};
    ULONG references{2};
    HRESULT presentResult{31};
    HRESULT resizeResult{37};
    HRESULT descriptorResult{S_OK};
    UINT interval{};
    UINT presentFlags{};
    UINT count{};
    UINT width{};
    UINT height{};
    DXGI_FORMAT format{};
    UINT resizeFlags{};
    bool inactiveInsideResize{};
    bool nestPresent{};
    bool nested{};
    DXGI_SWAP_CHAIN_DESC description{};
};

LifecycleEvidence evidence;

HRESULT STDMETHODCALLTYPE Query(IDXGISwapChain* chain, REFIID, void** result) noexcept
{
    ++evidence.queryCalls;
    ++evidence.references;
    *result = chain;
    return S_OK;
}

ULONG STDMETHODCALLTYPE AddReference(IDXGISwapChain*) noexcept
{
    return ++evidence.references;
}

ULONG STDMETHODCALLTYPE Release(IDXGISwapChain*) noexcept
{
    ++evidence.releaseCalls;
    const auto count = --evidence.references;
    if (count == 0)
    {
        DWORD previous{};
        FO4HDR_CHECK(VirtualProtect(evidence.object, sizeof(OwnedChain), PAGE_NOACCESS,
            &previous) != FALSE);
    }
    return count;
}

HRESULT STDMETHODCALLTYPE SetData(IDXGISwapChain*, REFGUID, UINT, const void*) noexcept
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE SetInterface(IDXGISwapChain*, REFGUID, const IUnknown*) noexcept
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE GetData(IDXGISwapChain*, REFGUID, UINT*, void*) noexcept
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE GetInterface(IDXGISwapChain*, REFIID, void**) noexcept
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* chain, UINT interval, UINT flags) noexcept
{
    const auto result = evidence.presentResult;
    ++evidence.presentCalls;
    evidence.interval = interval;
    evidence.presentFlags = flags;
    if (evidence.nestPresent && !evidence.nested)
    {
        evidence.nested = true;
        const auto function = reinterpret_cast<PresentFunction>(evidence.object->table[8]);
        FO4HDR_CHECK(function(chain, interval, flags) == evidence.presentResult);
    }
    if (auto* gate = evidence.presentGate)
    {
        gate->arrive_and_wait();
        gate->arrive_and_wait();
    }
    return result;
}

HRESULT STDMETHODCALLTYPE ForeignPresent(IDXGISwapChain*, UINT, UINT) noexcept
{
    ++evidence.foreignCalls;
    return 41;
}

HRESULT STDMETHODCALLTYPE GetBuffer(IDXGISwapChain*, UINT, REFIID, void**) noexcept
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE SetFullscreen(IDXGISwapChain*, BOOL, IDXGIOutput*) noexcept
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE GetFullscreen(IDXGISwapChain*, BOOL*, IDXGIOutput**) noexcept
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE Description(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC* description) noexcept
{
    ++evidence.descriptorCalls;
    *description = evidence.description;
    return evidence.descriptorResult;
}

HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain*, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags) noexcept
{
    ++evidence.resizeCalls;
    evidence.count = count;
    evidence.width = width;
    evidence.height = height;
    evidence.format = format;
    evidence.resizeFlags = flags;
    evidence.inactiveInsideResize = !fo4hdr::ReadSwapChainObservation().active;
    return evidence.resizeResult;
}

HRESULT STDMETHODCALLTYPE ResizeTarget(IDXGISwapChain*, const DXGI_MODE_DESC*) noexcept
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE GetOutput(IDXGISwapChain*, IDXGIOutput**) noexcept
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE GetStatistics(IDXGISwapChain*, DXGI_FRAME_STATISTICS*) noexcept
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE LastPresentCount(IDXGISwapChain*, UINT*) noexcept
{
    return E_NOTIMPL;
}

std::array<void*, 18> MakeTable()
{
    return {reinterpret_cast<void*>(&Query), reinterpret_cast<void*>(&AddReference),
        reinterpret_cast<void*>(&Release), reinterpret_cast<void*>(&SetData),
        reinterpret_cast<void*>(&SetInterface), reinterpret_cast<void*>(&GetData),
        reinterpret_cast<void*>(&GetInterface), reinterpret_cast<void*>(&GetInterface),
        reinterpret_cast<void*>(&Present), reinterpret_cast<void*>(&GetBuffer),
        reinterpret_cast<void*>(&SetFullscreen), reinterpret_cast<void*>(&GetFullscreen),
        reinterpret_cast<void*>(&Description), reinterpret_cast<void*>(&Resize),
        reinterpret_cast<void*>(&ResizeTarget), reinterpret_cast<void*>(&GetOutput),
        reinterpret_cast<void*>(&GetStatistics), reinterpret_cast<void*>(&LastPresentCount)};
}

IDXGISwapChain* Chain() noexcept
{
    return reinterpret_cast<IDXGISwapChain*>(evidence.object);
}

class InFlightPresent
{
public:
    ~InFlightPresent()
    {
        if (callback_.joinable())
        {
            Finish();
        }
    }

    bool Start(PresentFunction function)
    {
        evidence.presentGate = &gate_;
        const auto chain = Chain();
        try
        {
            callback_ = std::jthread([this, function, chain] {
                result = function(chain, 5, 0x94);
            });
        }
        catch (...)
        {
            evidence.presentGate = nullptr;
            return false;
        }
        gate_.arrive_and_wait();
        return true;
    }

    void Finish()
    {
        gate_.arrive_and_wait();
        callback_.join();
        evidence.presentGate = nullptr;
    }

    HRESULT result{};

private:
    std::barrier<> gate_{2};
    std::jthread callback_;
};

void CheckTail(const std::array<void*, 18>& table, const std::array<void*, 18>& original)
{
    for (std::size_t index = 0; index < table.size(); ++index)
    {
        switch (index)
        {
        case 2:
        case 8:
        case 13:
            break;
        default:
            FO4HDR_CHECK(table[index] == original[index]);
            break;
        }
    }
}

void CheckPresent(std::array<void*, 18>& table)
{
    const auto function = reinterpret_cast<PresentFunction>(table[8]);
    const auto calls = evidence.presentCalls;
    FO4HDR_CHECK(function(Chain(), 3, 0x12) == 31);
    FO4HDR_CHECK(evidence.presentCalls == calls + 1);
    FO4HDR_CHECK(evidence.interval == 3);
    FO4HDR_CHECK(evidence.presentFlags == 0x12);
    const auto token = fo4hdr::ReadSwapChainObservation().generation;
    evidence.nestPresent = true;
    FO4HDR_CHECK(function(Chain(), 3, 0x12) == 31);
    FO4HDR_CHECK(evidence.presentCalls == calls + 3);
    FO4HDR_CHECK(fo4hdr::ReadSwapChainObservation().generation == token);
    evidence.nestPresent = false;
}

void CheckResize(std::array<void*, 18>& table, HRESULT result)
{
    const auto function = reinterpret_cast<ResizeFunction>(table[13]);
    const auto calls = evidence.resizeCalls;
    evidence.resizeResult = result;
    FO4HDR_CHECK(function(Chain(), 4, 801, 603, DXGI_FORMAT_R8G8B8A8_UNORM, 0x42) == result);
    FO4HDR_CHECK(evidence.resizeCalls == calls + 1);
    FO4HDR_CHECK(evidence.inactiveInsideResize);
    FO4HDR_CHECK(evidence.count == 4);
    FO4HDR_CHECK(evidence.width == 801);
    FO4HDR_CHECK(evidence.height == 603);
    FO4HDR_CHECK(evidence.format == DXGI_FORMAT_R8G8B8A8_UNORM);
    FO4HDR_CHECK(evidence.resizeFlags == 0x42);
}

void CheckResizeTransitions(std::array<void*, 18>& table)
{
    const auto initial = fo4hdr::ReadSwapChainObservation().generation;
    CheckResize(table, E_FAIL);
    FO4HDR_CHECK(!fo4hdr::ReadSwapChainObservation().active);
    CheckResize(table, 37);
    FO4HDR_CHECK(fo4hdr::ReadSwapChainObservation().active);
    FO4HDR_CHECK(fo4hdr::ReadSwapChainObservation().generation != initial);
    evidence.descriptorResult = E_FAIL;
    CheckResize(table, 37);
    FO4HDR_CHECK(!fo4hdr::ReadSwapChainObservation().active);
    evidence.descriptorResult = S_OK;
    CheckResize(table, 37);
    FO4HDR_CHECK(fo4hdr::ReadSwapChainObservation().active);
}

void CheckNonfinalRelease(std::array<void*, 18>& table)
{
    const auto function = reinterpret_cast<ReleaseFunction>(table[2]);
    const auto calls = evidence.releaseCalls;
    FO4HDR_CHECK(fo4hdr::ReadSwapChainObservation().active);
    FO4HDR_CHECK(function(Chain()) == 1);
    FO4HDR_CHECK(fo4hdr::ReadSwapChainObservation().active);
    FO4HDR_CHECK(evidence.releaseCalls == calls + 1);
}

void CheckFinalRelease(std::array<void*, 18>& table)
{
    const auto function = reinterpret_cast<ReleaseFunction>(table[2]);
    const auto calls = evidence.releaseCalls;
    FO4HDR_CHECK(function(Chain()) == 0);
    FO4HDR_CHECK(evidence.releaseCalls == calls + 1);
    FO4HDR_CHECK(!fo4hdr::ReadSwapChainObservation().active);
    // Release made receiver storage unreadable; callback retirement has already completed.
    DWORD previous{};
    FO4HDR_CHECK(VirtualProtect(evidence.object, sizeof(OwnedChain), PAGE_READWRITE,
        &previous) != FALSE);
}

void CheckFatalPresentAcrossResize(std::array<void*, 18>& table)
{
    FO4HDR_CHECK(evidence.references == 2);
    const auto initial = fo4hdr::ReadSwapChainObservation().generation;
    const auto calls = evidence.presentCalls;
    const auto function = reinterpret_cast<PresentFunction>(fo4hdr::AtomicPointerLoad(&table[8]));
    evidence.presentResult = DXGI_ERROR_DEVICE_RESET;
    InFlightPresent callback;
    if (!callback.Start(function))
    {
        FO4HDR_CHECK(false);
        evidence.presentResult = 31;
        return;
    }
    CheckResize(table, 37);
    FO4HDR_CHECK(fo4hdr::ReadSwapChainObservation().active);
    FO4HDR_CHECK(fo4hdr::ReadSwapChainObservation().generation != initial);
    callback.Finish();
    evidence.presentResult = 31;
    FO4HDR_CHECK(callback.result == DXGI_ERROR_DEVICE_RESET);
    FO4HDR_CHECK(evidence.presentCalls == calls + 1);
    FO4HDR_CHECK(evidence.interval == 5);
    FO4HDR_CHECK(evidence.presentFlags == 0x94);
    FO4HDR_CHECK(!fo4hdr::ReadSwapChainObservation().active);
    CheckResize(table, 37);
    FO4HDR_CHECK(!fo4hdr::ReadSwapChainObservation().active);
}

void CheckInFlightForeignConflict(std::array<void*, 18>& table)
{
    FO4HDR_CHECK(fo4hdr::ReadSwapChainObservation().active);
    FO4HDR_CHECK(evidence.references == 1);
    const auto calls = evidence.presentCalls;
    const auto owned = fo4hdr::AtomicPointerLoad(&table[8]);
    const auto foreign = reinterpret_cast<void*>(&ForeignPresent);
    InFlightPresent callback;
    if (!callback.Start(reinterpret_cast<PresentFunction>(owned)))
    {
        FO4HDR_CHECK(false);
        return;
    }
    FO4HDR_CHECK(fo4hdr::WriteProtectedPointer(&table[8], owned, foreign));
    CheckResize(table, 37);
    FO4HDR_CHECK(!fo4hdr::ReadSwapChainObservation().active);
    FO4HDR_CHECK(fo4hdr::AtomicPointerLoad(&table[8]) == foreign);
    callback.Finish();
    FO4HDR_CHECK(callback.result == 31);
    FO4HDR_CHECK(evidence.presentCalls == calls + 1);
    FO4HDR_CHECK(evidence.interval == 5);
    FO4HDR_CHECK(evidence.presentFlags == 0x94);
    FO4HDR_CHECK(fo4hdr::AtomicPointerLoad(&table[8]) == foreign);
    const auto displaced = reinterpret_cast<PresentFunction>(fo4hdr::AtomicPointerLoad(&table[8]));
    FO4HDR_CHECK(displaced(Chain(), 7, 0x55) == 41);
    FO4HDR_CHECK(evidence.foreignCalls == 1);
    FO4HDR_CHECK(evidence.presentCalls == calls + 1);
}

void CheckDeviceLoss(std::array<void*, 18>& table)
{
    evidence.presentResult = DXGI_ERROR_DEVICE_RESET;
    const auto calls = evidence.presentCalls;
    const auto function = reinterpret_cast<PresentFunction>(table[8]);
    FO4HDR_CHECK(function(Chain(), 0, 0x80) == DXGI_ERROR_DEVICE_RESET);
    FO4HDR_CHECK(evidence.presentCalls == calls + 1);
    FO4HDR_CHECK(!fo4hdr::ReadSwapChainObservation().active);
    evidence.presentResult = 31;
}

void CheckSelection(std::array<void*, 18>& table)
{
    auto unrelated = MakeTable();
    const auto original = unrelated;
    OwnedChain other{unrelated.data()};
    const auto otherChain = reinterpret_cast<IDXGISwapChain*>(&other);
    FO4HDR_CHECK(!fo4hdr::SelectOwnedSwapChain(nullptr, table, evidence.description.OutputWindow));
    FO4HDR_CHECK(!fo4hdr::SelectOwnedSwapChain(Chain(), unrelated,
        evidence.description.OutputWindow));
    FO4HDR_CHECK(fo4hdr::SelectOwnedSwapChain(Chain(), table, evidence.description.OutputWindow));
    fo4hdr::OfferCreatedSwapChain(otherChain);
    FO4HDR_CHECK(unrelated == original);
    FO4HDR_CHECK(!fo4hdr::ReadSwapChainObservation().active);
}

}  // namespace

namespace fo4hdr::tests {

void RunSwapChainObservationTests(void (*create)(IDXGISwapChain*))
{
    evidence.object = static_cast<OwnedChain*>(VirtualAlloc(nullptr, sizeof(OwnedChain),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    FO4HDR_CHECK(evidence.object != nullptr);
    if (evidence.object == nullptr)
    {
        return;
    }
    std::array<std::byte, 8> windowToken{};
    evidence.description.OutputWindow = reinterpret_cast<HWND>(windowToken.data());
    auto first = MakeTable();
    const auto original = first;
    new (evidence.object) OwnedChain{first.data()};
    CheckSelection(first);
    create(Chain());
    FO4HDR_CHECK(ReadSwapChainObservation().active);
    const auto firstGeneration = ReadSwapChainObservation().generation;
    const auto firstPresent = first[8];
    CheckTail(first, original);
    CheckPresent(first);
    CheckResizeTransitions(first);
    CheckFatalPresentAcrossResize(first);
    CheckDeviceLoss(first);
    evidence.references = 1;
    const auto firstRelease = reinterpret_cast<ReleaseFunction>(first[2]);
    FO4HDR_CHECK(firstRelease(Chain()) == 0);
    DWORD previous{};
    FO4HDR_CHECK(VirtualProtect(evidence.object, sizeof(OwnedChain), PAGE_READWRITE,
        &previous) != FALSE);
    auto second = MakeTable();
    new (evidence.object) OwnedChain{second.data()};
    evidence.references = 2;
    FO4HDR_CHECK(SelectOwnedSwapChain(Chain(), second, evidence.description.OutputWindow));
    create(Chain());
    FO4HDR_CHECK(ReadSwapChainObservation().active);
    FO4HDR_CHECK(ReadSwapChainObservation().generation != firstGeneration);
    FO4HDR_CHECK(second[8] != firstPresent);
    CheckTail(second, original);
    CheckNonfinalRelease(second);
    CheckInFlightForeignConflict(second);
    CheckFinalRelease(second);
    auto exhausted = MakeTable();
    const auto untouched = exhausted;
    new (evidence.object) OwnedChain{exhausted.data()};
    FO4HDR_CHECK(!SelectOwnedSwapChain(Chain(), exhausted, evidence.description.OutputWindow));
    create(Chain());
    FO4HDR_CHECK(exhausted == untouched);
    FO4HDR_CHECK(!ReadSwapChainObservation().active);
    FO4HDR_CHECK(evidence.queryCalls == 0);
    FO4HDR_CHECK(evidence.references == 0);
    FO4HDR_CHECK(VirtualFree(evidence.object, 0, MEM_RELEASE) != FALSE);
}

}  // namespace fo4hdr::tests
