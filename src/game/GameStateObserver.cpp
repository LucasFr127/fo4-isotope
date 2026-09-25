// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include <RE/Fallout.h>
#include <F4SE/F4SE.h>
#include <REX/W32/KERNEL32.h>
#include "GameStateObserver.hpp"
#include "ImageSpaceObserver.hpp"
#include "GameSnapshotStore.hpp"
#include "Fallout4HDR/output/HdrActivationPolicy.hpp"
#include "Fallout4HDR/graphics/HdrRenderer.hpp"
#include "MenuEventQueue.hpp"
#include "Fallout4HDR/ui/HdrSettingsInput.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include "GameObservationMemory.hpp"

namespace {

static_assert(std::is_base_of_v<
              RE::BSTEventSource<RE::MenuOpenCloseEvent>, RE::UI>);

fo4hdr::GameSnapshotStore g_snapshotStore;

enum class ObserverInitializationState : std::uint8_t
{
    Uninitialized,
    Initializing,
    Active,
    Failed
};

enum class UiRegistrationState : std::uint8_t
{
    Unregistered,
    Registering,
    Registered
};

enum class UiRegistrationAttempt : std::uint8_t
{
    Unavailable,
    InProgress,
    AlreadyRegistered,
    RegisteredNow
};

std::atomic<ObserverInitializationState> g_initializationState{};
std::atomic<UiRegistrationState> g_uiRegistrationState{};
std::atomic<bool> g_menuSynchronizationBoundaryReached{};
std::atomic<std::uint64_t> g_menuSynchronizationFloor{};
std::atomic<std::uint64_t> g_reportedObservation{};
std::atomic<std::uint64_t> g_observationRequestGeneration{};
std::atomic<std::uint32_t> g_collectorThread{};
std::atomic<bool> g_collectorFailed{};
std::atomic<bool> g_uiCallbackFailed{};
std::atomic<fo4hdr::game::LifecycleDiagnosticsCallback>
    g_lifecycleDiagnostics{};
std::atomic_flag g_collectionActive = ATOMIC_FLAG_INIT;
class AtomicFlagClearGuard
{
public:
    explicit AtomicFlagClearGuard(std::atomic_flag& flag) noexcept :
        flag_(&flag)
    {}

    ~AtomicFlagClearGuard()
    {
        flag_->clear(std::memory_order_release);
    }

    AtomicFlagClearGuard(const AtomicFlagClearGuard&) = delete;
    AtomicFlagClearGuard& operator=(const AtomicFlagClearGuard&) = delete;

private:
    std::atomic_flag* flag_;
};

using LegacyAddTaskFunction =
    void(F4SEAPI*)(F4SE::ITaskDelegate*);
using LegacyRegisterListenerFunction =
    bool(F4SEAPI*)(std::uint32_t, const char*, void*);

struct LegacyMessagingInterfaceView
{
    std::uint32_t version{};
    std::uint32_t padding{};
    LegacyRegisterListenerFunction registerListener{};
    void* dispatch{};
    void* getEventDispatcher{};
};

static_assert(sizeof(LegacyMessagingInterfaceView) == 0x20);
static_assert(offsetof(LegacyMessagingInterfaceView, registerListener) == 0x08);

struct LegacyTaskInterfaceView
{
    std::uint32_t version{};
    std::uint32_t padding{};
    LegacyAddTaskFunction addTask{};
    LegacyAddTaskFunction addUiTask{};
};

static_assert(sizeof(LegacyTaskInterfaceView) == 0x18);
static_assert(offsetof(LegacyTaskInterfaceView, addTask) == 0x08);
static_assert(offsetof(LegacyTaskInterfaceView, addUiTask) == 0x10);

const LegacyTaskInterfaceView* g_taskInterface{};

using fo4hdr::game::observation::ValidateCommonLibObject;

void ReportFeatureFailure(fo4hdr::GameSnapshotFeature feature,
    std::string_view name, std::string_view reason) noexcept
{
    fo4hdr::game::observation::ReportFeatureFailure(g_snapshotStore, feature, name, reason);
}
void ReportMessagingAvailable(
    std::uint32_t interfaceVersion,
    bool taskInterfaceValid) noexcept
{
    try
    {
        g_snapshotStore.Update(
            [](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
                fo4hdr::MarkGameSnapshotFeatureAvailable(
                    snapshot,
                    fo4hdr::GameSnapshotFeature::Messaging);
            });
    }
    catch (...)
    {
        ::OutputDebugStringA(
            "Fallout4HDR: messaging snapshot publication failed.\n");
    }
    try
    {
        REX::INFO(
            "m2 messaging available=true interface_version={} thread={} "
            "commonlib_mappings=pinned task_interface_v2={}",
            interfaceVersion,
            REX::W32::GetCurrentThreadId(),
            taskInterfaceValid);
    }
    catch (...)
    {
        ::OutputDebugStringA(
            "Fallout4HDR: messaging availability logging failed.\n");
    }
}

constexpr std::size_t kMenuEventCapacity = 256;
fo4hdr::MenuEventQueue<kMenuEventCapacity> g_menuEvents;
std::atomic_flag g_menuDrainLock = ATOMIC_FLAG_INIT;
std::uint64_t g_menuEpochOpenMask{};

void AccumulateMenuEpoch(const fo4hdr::MenuEventRecord& event,
    bool synchronizationBoundaryReached, std::uint64_t synchronizationFloor)
{
    if (synchronizationBoundaryReached &&
        event.sequence > synchronizationFloor)
    {
        const auto contextBit = fo4hdr::MenuContextBit(
            static_cast<fo4hdr::MenuCode>(event.code));
        if (event.opening)
        {
            g_menuEpochOpenMask |= contextBit;
        }
        else
        {
            g_menuEpochOpenMask &= ~contextBit;
        }
    }
}

void ReportMenuDrain(const fo4hdr::MenuDrainEvidence& evidence,
    std::span<const fo4hdr::MenuEventRecord> detailed)
{
    const auto detailCount = (std::min<std::uint64_t>)(
        evidence.processed,
        detailed.size());
    for (std::size_t index = 0; index < detailCount; ++index)
    {
        const auto& detail = detailed[index];
        const auto menu = static_cast<fo4hdr::MenuCode>(detail.code);
        REX::INFO(
            "m2 menu transition={} name={} sequence={} source_thread={} "
            "drain_thread={}",
            detail.opening ? "open" : "close",
            fo4hdr::MenuName(menu),
            detail.sequence,
            detail.sourceThread,
            REX::W32::GetCurrentThreadId());
    }
    const auto published = g_snapshotStore.Read();
    REX::INFO(
        "m2 menu drain complete=true processed={} details_omitted={} "
        "events_dropped={} cumulative_dropped={} open_mask=0x{:X} "
        "last_sequence={} published_sequence={} caught_up={} "
        "history_complete={} state_synchronized={}",
        evidence.processed,
        evidence.processed - detailCount,
        evidence.dropped,
        published.menuEventsDropped,
        evidence.openMask,
        published.lastMenuSequence,
        evidence.publishedSequence,
        evidence.caughtUp,
        published.menuHistoryComplete,
        published.menuStateSynchronized);
    REX::INFO(
        "m2 menu witness valid={} pause_open_sequence={} "
        "pause_close_sequence={} pause_complete={} "
        "pipboy_open_sequence={} pipboy_close_sequence={} "
        "pipboy_complete={}",
        published.menuTransitionWitnessValid,
        published.pauseMenuPair.openSequence,
        published.pauseMenuPair.closeSequence,
        fo4hdr::IsCompleteMenuTransitionPair(published.pauseMenuPair),
        published.pipBoyMenuPair.openSequence,
        published.pipBoyMenuPair.closeSequence,
        fo4hdr::IsCompleteMenuTransitionPair(published.pipBoyMenuPair));
}

void DrainMenuEvents()
{
    if (g_menuDrainLock.test_and_set(std::memory_order_acquire))
    {
        return;
    }
    const AtomicFlagClearGuard drainGuard(g_menuDrainLock);

    constexpr std::size_t kDetailedLogLimit = 16;
    const auto cutoff = g_menuEvents.Published();
    const auto synchronizationBoundaryReached =
        g_menuSynchronizationBoundaryReached.load(std::memory_order_acquire);
    const auto synchronizationFloor =
        g_menuSynchronizationFloor.load(std::memory_order_acquire);
    std::array<fo4hdr::MenuEventRecord, kDetailedLogLimit> detailed{};
    const auto before = g_snapshotStore.Read();
    auto evidence = fo4hdr::BeginMenuDrainEvidence(before);
    fo4hdr::MenuEventRecord event{};
    while (evidence.processed < kMenuEventCapacity)
    {
        std::uint64_t dropped{};
        if (!g_menuEvents.TryPop(event, dropped, cutoff.sequence))
        {
            break;
        }
        if (evidence.processed < detailed.size())
        {
            detailed[static_cast<std::size_t>(evidence.processed)] = event;
        }
        fo4hdr::AccumulateMenuDrainRecord(
            evidence,
            event,
            dropped);
        AccumulateMenuEpoch(event, synchronizationBoundaryReached, synchronizationFloor);
    }

    const auto finalState = g_menuEvents.Published();
    evidence.publishedSequence = finalState.sequence;
    evidence.openMask = synchronizationBoundaryReached ?
                            g_menuEpochOpenMask :
                            0;
    evidence.caughtUp = evidence.lastSequence == finalState.sequence;
    g_snapshotStore.Update(
        [&evidence](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
            fo4hdr::ApplyMenuDrainEvidence(snapshot, evidence);
        });

    ReportMenuDrain(evidence, detailed);
}

class MenuEventSink final :
    public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    RE::BSEventNotifyControl ProcessEvent(
        const RE::MenuOpenCloseEvent& event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*) noexcept override
    {
        try
        {
            const std::string_view source = event.menuName;
            const auto menu = fo4hdr::ClassifyMenu(source);
            if (menu == fo4hdr::MenuCode::Loading)
            {
                // Publish only plain lock-free state from the engine callback.
                // Present applies the transition at its next frame boundary.
                fo4hdr::PublishGraphicsPresentationBlocked(event.opening);
            }
            [[maybe_unused]] const auto sequence = g_menuEvents.Push(
                static_cast<std::uint8_t>(menu),
                event.opening,
                REX::W32::GetCurrentThreadId(),
                fo4hdr::MenuContextBit(menu));
        }
        catch (...)
        {
            // The engine invokes this while its event-source lock is held.
            // Defer feature failure reporting to the collector; never log or
            // acquire a plugin lock here.
            g_uiCallbackFailed.store(true, std::memory_order_release);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

MenuEventSink g_menuEventSink;

void PublishUiRegistration()
{
    g_snapshotStore.Update(
        [](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
            const auto ui = static_cast<std::uint32_t>(
                fo4hdr::GameSnapshotFeature::Ui);
            snapshot.availableFeatures =
                static_cast<fo4hdr::GameSnapshotFeature>(
                    static_cast<std::uint32_t>(
                        snapshot.availableFeatures) &
                    ~ui);
            snapshot.failedFeatures =
                static_cast<fo4hdr::GameSnapshotFeature>(
                    static_cast<std::uint32_t>(
                        snapshot.failedFeatures) &
                    ~ui);
            snapshot.menuStateSynchronized = false;
        });
    REX::INFO(
        "m2 ui available=false menu_sink_registered=true "
        "reason=awaiting-post-load-sync thread={}",
        REX::W32::GetCurrentThreadId());
}

[[nodiscard]] UiRegistrationAttempt TryRegisterUi()
{
    auto expected = UiRegistrationState::Unregistered;
    if (!g_uiRegistrationState.compare_exchange_strong(
            expected,
            UiRegistrationState::Registering,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return expected == UiRegistrationState::Registered ?
                   UiRegistrationAttempt::AlreadyRegistered :
                   UiRegistrationAttempt::InProgress;
    }
    bool installed = false;
    try
    {
        // Only the stable MenuOpenCloseEvent base is used. CommonLib owns the
        // singleton, lock, and allocator mappings required by RegisterSink.
        auto* ui = ValidateCommonLibObject(
            RE::UI::GetSingleton(),
            0x70,
            "RE::UI::GetSingleton");
        if (ui == nullptr)
        {
            g_uiRegistrationState.store(
                UiRegistrationState::Unregistered,
                std::memory_order_release);
            ReportFeatureFailure(
                fo4hdr::GameSnapshotFeature::Ui,
                "ui",
                "relocation-or-object-invalid");
            return UiRegistrationAttempt::Unavailable;
        }
        ui->GetEventSource<RE::MenuOpenCloseEvent>()->RegisterSink(
            &g_menuEventSink);
        installed = true;
        g_uiRegistrationState.store(
            UiRegistrationState::Registered,
            std::memory_order_release);
        PublishUiRegistration();
        return UiRegistrationAttempt::RegisteredNow;
    }
    catch (...)
    {
        if (!installed)
        {
            g_uiRegistrationState.store(
                UiRegistrationState::Unregistered,
                std::memory_order_release);
        }
        throw;
    }
}

struct LifecycleDefinition
{
    std::uint32_t message;
    fo4hdr::GameLifecycleEvent event;
    std::string_view name;
};
constexpr LifecycleDefinition kLifecycleDefinitions[]{
    {F4SE::MessagingInterface::kPostLoad, fo4hdr::GameLifecycleEvent::PostLoad, "post-load"},
    {F4SE::MessagingInterface::kPostPostLoad, fo4hdr::GameLifecycleEvent::PostPostLoad, "post-post-load"},
    {F4SE::MessagingInterface::kPreLoadGame, fo4hdr::GameLifecycleEvent::PreLoadGame, "pre-load-game"},
    {F4SE::MessagingInterface::kPostLoadGame, fo4hdr::GameLifecycleEvent::PostLoadGame, "post-load-game"},
    {F4SE::MessagingInterface::kPreSaveGame, fo4hdr::GameLifecycleEvent::PreSaveGame, "pre-save-game"},
    {F4SE::MessagingInterface::kPostSaveGame, fo4hdr::GameLifecycleEvent::PostSaveGame, "post-save-game"},
    {F4SE::MessagingInterface::kDeleteGame, fo4hdr::GameLifecycleEvent::DeleteGame, "delete-game"},
    {F4SE::MessagingInterface::kInputLoaded, fo4hdr::GameLifecycleEvent::InputLoaded, "input-loaded"},
    {F4SE::MessagingInterface::kNewGame, fo4hdr::GameLifecycleEvent::NewGame, "new-game"},
    {F4SE::MessagingInterface::kGameLoaded, fo4hdr::GameLifecycleEvent::GameLoaded, "game-loaded"},
    {F4SE::MessagingInterface::kGameDataReady, fo4hdr::GameLifecycleEvent::GameDataReady, "game-data-ready"},
};

[[nodiscard]] constexpr fo4hdr::GameLifecycleEvent LifecycleEvent(std::uint32_t type) noexcept
{
    for (const auto& definition : kLifecycleDefinitions)
        if (definition.message == type) return definition.event;
    return fo4hdr::GameLifecycleEvent::None;
}

[[nodiscard]] constexpr std::string_view LifecycleName(fo4hdr::GameLifecycleEvent event) noexcept
{
    for (const auto& definition : kLifecycleDefinitions)
        if (definition.event == event) return definition.name;
    return event == fo4hdr::GameLifecycleEvent::MenuChanged ? "menu-changed" : "unknown";
}
void ReportWindowDetails(const fo4hdr::GameHdrSnapshot& snapshot)
{
    if (fo4hdr::HasGameSnapshotFeature(
            snapshot.availableFeatures,
            fo4hdr::GameSnapshotFeature::Renderer))
    {
        for (const auto& window : snapshot.rendererWindows)
        {
            if (window.present)
            {
                REX::INFO(
                    "m2 renderer_window index={} hwnd_present={} "
                    "swap_chain_present={} position={}x{} size={}x{}",
                    window.arrayIndex,
                    window.windowIdentity != 0,
                    window.swapChainIdentity != 0,
                    window.x,
                    window.y,
                    window.width,
                    window.height);
            }
        }
    }


}

void ReportTargetDetails(const fo4hdr::GameHdrSnapshot& snapshot)
{
    if (fo4hdr::HasGameSnapshotFeature(
            snapshot.availableFeatures,
            fo4hdr::GameSnapshotFeature::RenderTargets))
    {
        for (const auto& target : snapshot.renderTargets)
        {
            if (target.resourcePresent)
            {
                REX::INFO(
                    "m2 render_target index={} resource_present=true "
                    "metadata_observed={} metadata_correlated={} "
                    "engine_id={} size={}x{} format_numeric={} "
                    "multisample={}",
                    target.arrayIndex,
                    target.propertiesPresent,
                    target.metadataCorrelationValidated,
                    target.engineId,
                    target.width,
                    target.height,
                    target.formatValue,
                    target.multiSample);
            }
        }
    }
}

void ReportPublishedObservation()
{
    const auto snapshot = g_snapshotStore.Read();
    const auto observation = snapshot.observationGeneration;
    auto reported = g_reportedObservation.load(std::memory_order_acquire);
    if (observation == 0) return;
    if (observation <= reported ||
        !g_reportedObservation.compare_exchange_strong(
            reported,
            observation,
            std::memory_order_acq_rel))
    {
        return;
    }

    REX::INFO(
        "m2 observation generation={} report_thread={}",
        observation,
        REX::W32::GetCurrentThreadId());
    fo4hdr::game::observation::ReportImageSpaceDetails(snapshot);
    ReportWindowDetails(snapshot);
    ReportTargetDetails(snapshot);

}

void ObserveAtLifecycle(
    fo4hdr::GameLifecycleEvent event)
{
    const auto name = LifecycleName(event);
    fo4hdr::game::observation::ObserveImageSpace(g_snapshotStore, name);
    fo4hdr::game::observation::ObserveRendererAndTargets(g_snapshotStore, name);
    g_snapshotStore.Update(
        [](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
            ++snapshot.observationGeneration;
        });
}

void ReportCollectorFailure(std::string_view reason) noexcept
{
    g_collectorFailed.store(true, std::memory_order_release);
    ReportFeatureFailure(
        fo4hdr::GameSnapshotFeature::ImageSpace,
        "image-space",
        reason);
    ReportFeatureFailure(
        fo4hdr::GameSnapshotFeature::Renderer,
        "renderer",
        reason);
    ReportFeatureFailure(
        fo4hdr::GameSnapshotFeature::RenderTargets,
        "render-targets",
        reason);
    ReportFeatureFailure(
        fo4hdr::GameSnapshotFeature::Ui,
        "ui",
        reason);
}

struct ObservationRequest
{
    fo4hdr::GameLifecycleEvent event{};
    std::uint64_t generation{};
    std::uint32_t sourceThread{};
    bool allowMenuSynchronization{};
    bool observe{};
};

void SynchronizeCollectedMenu(fo4hdr::GameLifecycleEvent event, bool allowMenuSynchronization)
{
    if (allowMenuSynchronization)
    {
        const auto menu = g_snapshotStore.Read();
        if (fo4hdr::CanSynchronizeMenuState(
                menu,
                true,
                g_menuSynchronizationBoundaryReached.load(
                    std::memory_order_acquire),
                g_uiRegistrationState.load(std::memory_order_acquire) ==
                    UiRegistrationState::Registered) &&
            menu.lastMenuSequence >
                g_menuSynchronizationFloor.load(
                    std::memory_order_acquire))
        {
            g_snapshotStore.Update(
                [](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
                    snapshot.menuStateSynchronized = true;
                    fo4hdr::MarkGameSnapshotFeatureAvailable(
                        snapshot,
                        fo4hdr::GameSnapshotFeature::Ui);
                });
            REX::INFO(
                "m2 ui available=true menu_state_synchronized=true "
                "boundary={}",
                LifecycleName(event));
        }
    }
}

bool VerifyCollectorThread(const ObservationRequest& request)
{
    const auto event = request.event;
    const auto requestGeneration = request.generation;
    const auto sourceThread = request.sourceThread;
    const auto executeThread = REX::W32::GetCurrentThreadId();
    std::uint32_t expected{};
    if (!g_collectorThread.compare_exchange_strong(
            expected,
            executeThread,
            std::memory_order_acq_rel) &&
        expected != executeThread)
    {
        REX::WARN(
            "m2 collector request={} source_thread={} "
            "execute_thread={} expected_thread={} stable=false",
            requestGeneration,
            sourceThread,
            executeThread,
            expected);
        ReportCollectorFailure("collector-thread-drift");
        return false;
    }

    REX::INFO(
        "m2 collector request={} event={} source_thread={} "
        "execute_thread={} stable=true",
        requestGeneration,
        LifecycleName(event),
        sourceThread,
        executeThread);
    return true;
}

void CollectMenuState(fo4hdr::GameLifecycleEvent event)
{
    if (event == fo4hdr::GameLifecycleEvent::GameDataReady)
        fo4hdr::game::BuildImageSpaceCatalog();
    if (g_uiCallbackFailed.exchange(false, std::memory_order_acq_rel))
    {
        ReportFeatureFailure(
            fo4hdr::GameSnapshotFeature::Ui,
            "ui",
            "menu-event-callback-exception");
    }
    if (event == fo4hdr::GameLifecycleEvent::PostLoadGame ||
        event == fo4hdr::GameLifecycleEvent::NewGame)
    {
        g_menuEpochOpenMask = 0;
        g_menuSynchronizationFloor.store(
            g_menuEvents.PublishedSequence(),
            std::memory_order_release);
        g_menuSynchronizationBoundaryReached.store(
            true,
            std::memory_order_release);
    }
    DrainMenuEvents();
}

void RunCollectedRequest(ObservationRequest request) noexcept
{
    const auto event = request.event;
    const auto allowMenuSynchronization = request.allowMenuSynchronization;
    const auto observe = request.observe;
    if (g_collectorFailed.load(std::memory_order_acquire))
    {
        return;
    }
    if (g_collectionActive.test_and_set(std::memory_order_acquire))
    {
        ReportCollectorFailure("collector-reentrant");
        return;
    }
    const AtomicFlagClearGuard collectionGuard(g_collectionActive);

    try
    {
        if (!VerifyCollectorThread(request)) return;
        CollectMenuState(event);
        SynchronizeCollectedMenu(event, allowMenuSynchronization);
        if (observe)
        {
            ObserveAtLifecycle(event);
        }
    }
    catch (...)
    {
        ReportCollectorFailure("collector-exception");
    }
}

class ObservationTask final : public F4SE::ITaskDelegate
{
public:
    explicit ObservationTask(ObservationRequest request) noexcept : request_(request) {}

    void Run() noexcept override
    {
        RunCollectedRequest(request_);
    }

private:
    ObservationRequest request_;
};

bool TaskInterfaceAvailable(const LegacyTaskInterfaceView* task) noexcept
{
    return task != nullptr && task->version == 2 && task->addTask != nullptr;
}

bool MessagingInterfaceAvailable(const LegacyMessagingInterfaceView* messaging,
    std::uint32_t version, std::uint32_t handle) noexcept
{
    return messaging != nullptr && version == 1 &&
        messaging->registerListener != nullptr && handle != UINT32_MAX;
}
void ScheduleCollectedRequest(
    fo4hdr::GameLifecycleEvent event,
    bool allowMenuSynchronization,
    bool observe)
{
    if (g_collectorFailed.load(std::memory_order_acquire))
    {
        return;
    }
    const auto sourceThread = REX::W32::GetCurrentThreadId();
    const auto requestGeneration =
        g_observationRequestGeneration.fetch_add(
            1,
            std::memory_order_acq_rel) +
        1;
    if (!TaskInterfaceAvailable(g_taskInterface))
    {
        ReportCollectorFailure("legacy-task-interface-v2-unavailable");
        return;
    }

    REX::INFO(
        "m2 collector requested={} event={} source_thread={} "
        "enqueue_attempt=true",
        requestGeneration,
        LifecycleName(event),
        sourceThread);
    auto* task = new (std::nothrow) ObservationTask(
        {event, requestGeneration, sourceThread, allowMenuSynchronization, observe});
    if (task == nullptr)
    {
        ReportCollectorFailure("task-allocation-failed");
        return;
    }
    if (!fo4hdr::TransferOwnedTask(
            task,
            [addTask = g_taskInterface->addTask](
                F4SE::ITaskDelegate* owned) {
                addTask(owned);
            }))
    {
        ReportCollectorFailure("task-enqueue-failed");
    }
}

void PublishLifecycleTransition(fo4hdr::GameLifecycleEvent event, bool succeeded)
{
    if (event == fo4hdr::GameLifecycleEvent::PreLoadGame)
    {
        fo4hdr::PublishGraphicsLifecycle(false, true);
    }
    else if (event == fo4hdr::GameLifecycleEvent::NewGame)
    {
        // NewGame may arrive while readiness is already true. The epoch still
        // changes so Present retires every resource/history generation.
        fo4hdr::PublishGraphicsLifecycle(true, true);
    }
    else if (event == fo4hdr::GameLifecycleEvent::PostLoadGame && succeeded)
    {
        // Readiness completes the PreLoadGame epoch; it must not erase or
        // advance the reset that was already published.
        fo4hdr::PublishGraphicsLifecycle(true, false);
    }
    if (event == fo4hdr::GameLifecycleEvent::PreLoadGame ||
        event == fo4hdr::GameLifecycleEvent::NewGame)
    {
        // Close the synchronization gate before publishing invalidation so a
        // concurrent collector cannot make the old UI epoch available again.
        g_menuSynchronizationBoundaryReached.store(
            false,
            std::memory_order_release);
        g_menuSynchronizationFloor.store(
            g_menuEvents.PublishedSequence(),
            std::memory_order_release);
    }
    g_snapshotStore.Update(
        [event, succeeded](
            fo4hdr::GameHdrSnapshot& snapshot) noexcept {
            fo4hdr::ApplyGameLifecycleTransition(
                snapshot,
                event,
                succeeded);
        });
}

void ActivateSettingsInput(std::string_view name)
{
    const auto activation =
        fo4hdr::TryActivateGraphicsSettingsInputOnCurrentThread();
    REX::INFO(
        "m14 settings_input event={} ready={} active={} "
        "current_thread={} window_thread={}",
        name,
        activation.ready,
        activation.active,
        activation.currentThreadId,
        activation.windowThreadId);
}

bool NeedsLifecycleCollection(fo4hdr::GameLifecycleEvent event, bool succeeded,
    bool shouldRegisterUi, bool hasPendingWork) noexcept
{
    return hasPendingWork || (event == fo4hdr::GameLifecycleEvent::GameDataReady && succeeded) ||
        (shouldRegisterUi && event == fo4hdr::GameLifecycleEvent::PostLoadGame);
}
void ScheduleLifecycleCollection(fo4hdr::GameLifecycleEvent event, bool succeeded, bool shouldRegisterUi)
{
    const auto shouldObserve =
        fo4hdr::ShouldCollectGameSnapshotAtLifecycle(event, succeeded);
    const auto shouldDrainMenus =
        g_menuEvents.PublishedSequence() !=
        g_snapshotStore.Read().lastMenuSequence;
    auto registration = UiRegistrationAttempt::Unavailable;
    if (shouldRegisterUi)
    {
        registration = TryRegisterUi();
    }
    const auto registeredNow =
        registration == UiRegistrationAttempt::RegisteredNow;
    const auto uiRegistered =
        g_uiRegistrationState.load(std::memory_order_acquire) ==
        UiRegistrationState::Registered;
    if (NeedsLifecycleCollection(event, succeeded, shouldRegisterUi, shouldObserve || shouldDrainMenus))
    {
        ScheduleCollectedRequest(
            event,
            uiRegistered && !registeredNow,
            shouldObserve);
    }
}

bool ShouldRegisterUi(fo4hdr::GameLifecycleEvent event, bool succeeded) noexcept
{
    return (event == fo4hdr::GameLifecycleEvent::GameDataReady && succeeded) ||
        event == fo4hdr::GameLifecycleEvent::InputLoaded ||
        (event == fo4hdr::GameLifecycleEvent::PostLoadGame && succeeded);
}

bool HasLifecycleOutcome(fo4hdr::GameLifecycleEvent event) noexcept
{
    return event == fo4hdr::GameLifecycleEvent::GameDataReady ||
        event == fo4hdr::GameLifecycleEvent::PostLoadGame;
}
void ReportLifecycleDiagnostics()
{
    if (const auto diagnostics =
            g_lifecycleDiagnostics.load(std::memory_order_acquire))
    {
        diagnostics(g_snapshotStore.Read());
    }
}

void ObserveLifecycleImpl(F4SE::MessagingInterface::Message* message)
{
    if (message == nullptr)
    {
        return;
    }
    ReportPublishedObservation();
    ReportLifecycleDiagnostics();
    const auto event = LifecycleEvent(message->type);
    if (event == fo4hdr::GameLifecycleEvent::None)
    {
        return;
    }
    const auto name = LifecycleName(event);
    const auto hasOutcome = HasLifecycleOutcome(event);
    const auto succeeded =
        !hasOutcome ||
        reinterpret_cast<std::uintptr_t>(message->data) != 0;
    PublishLifecycleTransition(event, succeeded);
    REX::INFO(
        "m2 lifecycle event={} type={} outcome={} thread={}",
        name,
        message->type,
        hasOutcome ? (succeeded ? "success" : "false-or-failed") :
                     "not-applicable",
        REX::W32::GetCurrentThreadId());

    // The render/D3D thread publishes the initialized ImGui HWND, but Win32
    // subclass installation belongs to the HWND owner thread. F4SE lifecycle
    // callbacks provide bounded retry opportunities; the input boundary still
    // verifies the exact owner before mutation. No D3D ownership moves into
    // the game adapter.
    const auto shouldRegisterUi = ShouldRegisterUi(event, succeeded);
    const auto shouldActivateSettingsInput =
        shouldRegisterUi || event == fo4hdr::GameLifecycleEvent::NewGame;
    if (shouldActivateSettingsInput)
    {
        ActivateSettingsInput(name);
    }

    ScheduleLifecycleCollection(event, succeeded, shouldRegisterUi);
}

void ObserveLifecycle(F4SE::MessagingInterface::Message* message) noexcept
{
    try
    {
        ObserveLifecycleImpl(message);
    }
    catch (...)
    {
        ReportCollectorFailure("lifecycle-callback-exception");
    }
}

}  // namespace

namespace fo4hdr::game {

namespace {

[[nodiscard]] bool InitializeGameStateObserverImpl(
    const void* rawMessagingInterface,
    const void* rawTaskInterface,
    std::uint32_t pluginHandle,
    LifecycleDiagnosticsCallback diagnostics)
{
    fo4hdr::game::observation::ReportImageSpaceSemanticProfile();

    const auto* taskInterface = reinterpret_cast<const LegacyTaskInterfaceView*>(
        rawTaskInterface);
    const auto taskInterfaceValid = TaskInterfaceAvailable(taskInterface);
    if (!taskInterfaceValid)
    {
        ReportCollectorFailure("legacy-task-interface-v2-unavailable");
    }

    const auto* messaging = reinterpret_cast<const LegacyMessagingInterfaceView*>(
        rawMessagingInterface);
    const auto messagingVersion =
        messaging == nullptr ? 0 : messaging->version;
    if (!MessagingInterfaceAvailable(messaging, messagingVersion, pluginHandle))
    {
        ReportFeatureFailure(
            GameSnapshotFeature::Messaging,
            "messaging",
            "legacy-messaging-interface-v1-unavailable");
        return false;
    }

    // Publish complete backing state before the engine can invoke the
    // listener. Subsequent status publication is best-effort and contains all
    // failures, so successful listener ownership is never rolled back.
    g_taskInterface = taskInterfaceValid ? taskInterface : nullptr;
    g_lifecycleDiagnostics.store(diagnostics, std::memory_order_release);
    if (!messaging->registerListener(
            pluginHandle,
            "F4SE",
            reinterpret_cast<void*>(&ObserveLifecycle)))
    {
        ReportFeatureFailure(
            GameSnapshotFeature::Messaging,
            "messaging",
            "listener-registration-failed");
        return false;
    }
    try
    {
        ReportMessagingAvailable(
            messagingVersion,
            taskInterfaceValid);
    }
    catch (...)
    {
        ::OutputDebugStringA(
            "Fallout4HDR: post-registration M2 status logging failed.\n");
    }
    return true;
}

}  // namespace

bool InitializeGameStateObserver(
    const void* messagingInterface,
    const void* taskInterface,
    std::uint32_t pluginHandle,
    LifecycleDiagnosticsCallback diagnostics) noexcept
{
    auto state = g_initializationState.load(std::memory_order_acquire);
    if (state == ObserverInitializationState::Active)
    {
        return true;
    }
    if (state != ObserverInitializationState::Uninitialized ||
        !g_initializationState.compare_exchange_strong(
            state,
            ObserverInitializationState::Initializing,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return state == ObserverInitializationState::Active;
    }

    try
    {
        const auto active = InitializeGameStateObserverImpl(
            messagingInterface,
            taskInterface,
            pluginHandle,
            diagnostics);
        g_initializationState.store(
            active ? ObserverInitializationState::Active :
                     ObserverInitializationState::Failed,
            std::memory_order_release);
        return active;
    }
    catch (...)
    {
        g_initializationState.store(
            ObserverInitializationState::Failed,
            std::memory_order_release);
        ReportFeatureFailure(
            GameSnapshotFeature::Messaging,
            "messaging",
            "observer-initialization-exception");
        return false;
    }
}

}  // namespace fo4hdr::game
