// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../d3d11/ContextQuerySafety.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace fo4hdr {

class ScenePassRenderer;

using ContextQueryCall = void (STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Asynchronous*);
using ContextCommandListCall = void (STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11CommandList*, BOOL);

struct ContextQueryPredecessors
{
    ContextQueryCall begin{};
    ContextQueryCall end{};
    ContextCommandListCall executeCommandList{};
};

enum class ContextQueryPreparation
{
    invalidContext,
    coverageUnobserved
};

// The generation owner retains the context; this record borrows it until cold,
// same-thread retirement with no adapter in flight. Preparation installs nothing
// and cannot establish complete query observation from the context's creation.
class ContextQueryObserver
{
public:
    [[nodiscard]] ContextQueryPreparation Prepare(ID3D11DeviceContext* context,
        std::uint64_t generation, ContextQueryPredecessors next = {},
        ScenePassRenderer* renderer = nullptr) noexcept;
    [[nodiscard]] bool Retire() noexcept;
    [[nodiscard]] bool CanDispatch() const noexcept;
    [[nodiscard]] const ContextQuerySafety& Queries() const noexcept { return queries_; }

    // Predecessors are immutable for the prepared generation. These adapters do
    // no vtable mutation; callers supply captured SDK calls, never virtual reentry.
    void Begin(ID3D11DeviceContext* context, ID3D11Asynchronous* asynchronous) noexcept;
    void End(ID3D11DeviceContext* context, ID3D11Asynchronous* asynchronous) noexcept;
    void ExecuteCommandList(ID3D11DeviceContext* context,
        ID3D11CommandList* commands, BOOL restoreState) noexcept;

private:
    [[nodiscard]] bool Enter(ID3D11DeviceContext* context) noexcept;
    void ObserveBegin(ID3D11Asynchronous* asynchronous) noexcept;
    void ObserveEnd(ID3D11Asynchronous* asynchronous) noexcept;
    void FinishCommandList(bool entered, bool completed, BOOL restoreState) noexcept;

    ID3D11DeviceContext* context_{};
    ScenePassRenderer* renderer_{};
    std::thread::id ownerThread_;
    std::uint64_t generation_{};
    ContextQueryPredecessors next_;
    ContextQuerySafety queries_;
    std::atomic<bool> lostCoverage_{};
    std::atomic_flag observing_ = ATOMIC_FLAG_INIT;
};

}  // namespace fo4hdr
