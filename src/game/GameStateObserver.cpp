// SPDX-License-Identifier: GPL-3.0-or-later

#include "GameStateObserver.hpp"
#include "GameSnapshotStore.hpp"
#include "Fallout4HDR/ui/HdrSettingsInput.hpp"

#include <F4SE/F4SE.h>
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

namespace {

enum class InitializationState
{
    Uninitialized,
    Initializing,
    Active
};

fo4hdr::GameSnapshotStore snapshotStore;
fo4hdr::GraphicsLifecyclePublisher graphicsLifecycle;
fo4hdr::MenuEventQueue<256> menuEvents;
std::atomic<InitializationState> initializationState{};
std::atomic<bool> collectorFailed{};
std::atomic_flag collectionActive = ATOMIC_FLAG_INIT;
const F4SE::TaskInterface* taskInterface{};

void DisableCollection() noexcept
{
    collectorFailed.store(true, std::memory_order_release);
    try
    {
        snapshotStore.Update(fo4hdr::DisableGameSnapshotCollection);
    }
    catch (...)
    {
    }
}

void RunCollectedRequest(std::uint64_t resourceGeneration) noexcept
{
    if (collectorFailed.load(std::memory_order_acquire))
    {
        return;
    }
    if (collectionActive.test_and_set(std::memory_order_acquire))
    {
        DisableCollection();
        return;
    }
    try
    {
        const auto threadId = GetCurrentThreadId();
        snapshotStore.Update([resourceGeneration, threadId](
            fo4hdr::GameHdrSnapshot& snapshot) noexcept {
            static_cast<void>(fo4hdr::CollectGameMenuSnapshot(
                snapshot, menuEvents, resourceGeneration, threadId));
            if (snapshot.collectorFailed)
            {
                collectorFailed.store(true, std::memory_order_release);
            }
        });
    }
    catch (...)
    {
        DisableCollection();
    }
    collectionActive.clear(std::memory_order_release);
}

class ObservationTask final : public F4SE::ITaskDelegate
{
public:
    explicit ObservationTask(std::uint64_t resourceGeneration) noexcept :
        resourceGeneration_(resourceGeneration)
    {}

    void Run() noexcept override
    {
        RunCollectedRequest(resourceGeneration_);
    }

private:
    std::uint64_t resourceGeneration_;
};

void ScheduleCollectedRequest(std::uint64_t resourceGeneration)
{
    if (taskInterface == nullptr || collectorFailed.load(std::memory_order_acquire))
    {
        return;
    }
    auto task = std::make_unique<ObservationTask>(resourceGeneration);
    taskInterface->AddTask(task.get());
    // Normal return transfers ownership; F4SE runs and deletes this delegate.
    static_cast<void>(task.release());
}

[[nodiscard]] fo4hdr::GameLifecycleEvent LifecycleEvent(std::uint32_t type) noexcept
{
    using Event = fo4hdr::GameLifecycleEvent;
    constexpr std::array events{Event::PostLoad, Event::PostPostLoad, Event::PreLoadGame,
        Event::PostLoadGame, Event::PreSaveGame, Event::PostSaveGame, Event::DeleteGame,
        Event::InputLoaded, Event::NewGame, Event::GameLoaded, Event::GameDataReady};
    static_assert(F4SE::MessagingInterface::kPostLoad == 0);
    static_assert(F4SE::MessagingInterface::kGameDataReady == events.size() - 1);
    return type < events.size() ? events[type] : Event::None;
}

void ActivateSettingsInputAtLifecycle(fo4hdr::GameLifecycleEvent event, bool succeeded) noexcept
{
    if (!succeeded) return;
    using Event = fo4hdr::GameLifecycleEvent;
    constexpr std::array opportunities{Event::InputLoaded, Event::NewGame,
        Event::GameLoaded, Event::GameDataReady};
    for (const auto opportunity : opportunities)
    {
        if (event == opportunity)
        {
            (void)fo4hdr::TryActivateSettingsInputHostOnCurrentThread();
            return;
        }
    }
}

void F4SEAPI ObserveLifecycle(F4SE::MessagingInterface::Message* message) noexcept
{
    try
    {
        if (message == nullptr || message->sender == nullptr ||
            std::string_view{message->sender} != "F4SE")
        {
            return;
        }
        const auto event = LifecycleEvent(message->type);
        const auto outcome = fo4hdr::DecodeGameLifecycleOutcome(
            event, message->dataLen, reinterpret_cast<std::uintptr_t>(message->data));
        if (event == fo4hdr::GameLifecycleEvent::None || !outcome)
        {
            return;
        }
        ActivateSettingsInputAtLifecycle(event, *outcome);
        std::uint64_t resourceGeneration{};
        snapshotStore.Update([event, succeeded = *outcome, &resourceGeneration](
            fo4hdr::GameHdrSnapshot& snapshot) noexcept {
            fo4hdr::ApplyGameLifecycleTransition(
                snapshot, event, succeeded, menuEvents.PublishedSequence());
            graphicsLifecycle.PublishTransition(event, succeeded);
            resourceGeneration = snapshot.resourceGeneration;
        });
        if (fo4hdr::ShouldCollectGameSnapshotAtLifecycle(event, *outcome))
        {
            ScheduleCollectedRequest(resourceGeneration);
        }
    }
    catch (...)
    {
        graphicsLifecycle.Disable();
        DisableCollection();
    }
}

[[nodiscard]] bool PinObserverModule() noexcept
{
    HMODULE module{};
    // F4SE retains listeners and queued delegates without an unregister operation.
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&ObserveLifecycle), &module) != 0 && module != nullptr;
}

[[nodiscard]] bool TaskInterfaceAvailable(const F4SE::TaskInterface* task) noexcept
{
    if (task == nullptr)
    {
        return false;
    }
    const auto& proxy = reinterpret_cast<const F4SE::Impl::F4SETaskInterface&>(*task);
    return proxy.interfaceVersion == 2 && proxy.AddTask != nullptr;
}

[[nodiscard]] bool MessagingInterfaceAvailable(
    const F4SE::Impl::F4SEMessagingInterface* messaging) noexcept
{
    if (messaging == nullptr)
    {
        return false;
    }
    return messaging->interfaceVersion == 1 && messaging->RegisterListener != nullptr;
}

void PublishObserverAvailability() noexcept
{
    try
    {
        snapshotStore.Update([](fo4hdr::GameHdrSnapshot& snapshot) noexcept {
            snapshot.messagingAvailable = true;
            snapshot.taskCollectionAvailable =
                taskInterface != nullptr && !snapshot.collectorFailed;
        });
    }
    catch (...)
    {
        DisableCollection();
    }
}

[[nodiscard]] bool TryInitializeObserver(const F4SE::LoadInterface* f4se)
{
    const auto& load = reinterpret_cast<const F4SE::Impl::F4SEInterface&>(*f4se);
    if (load.QueryInterface == nullptr || load.GetPluginHandle == nullptr)
    {
        return false;
    }
    const auto handle = f4se->GetPluginHandle();
    if (handle == UINT32_MAX)
    {
        return false;
    }
    const auto* messaging = static_cast<const F4SE::Impl::F4SEMessagingInterface*>(
        f4se->QueryInterface(F4SE::LoadInterface::kMessaging));
    if (!MessagingInterfaceAvailable(messaging))
    {
        return false;
    }
    const auto* task = static_cast<const F4SE::TaskInterface*>(
        f4se->QueryInterface(F4SE::LoadInterface::kTask));
    taskInterface = TaskInterfaceAvailable(task) ? task : nullptr;
    if (!PinObserverModule() ||
        !messaging->RegisterListener(handle, "F4SE", reinterpret_cast<void*>(&ObserveLifecycle)))
    {
        return false;
    }
    initializationState.store(InitializationState::Active, std::memory_order_release);
    PublishObserverAvailability();
    return true;
}

}  // namespace

namespace fo4hdr::game {

void InitializeGameStateObserver(const F4SE::LoadInterface* f4se) noexcept
{
    if (f4se == nullptr)
    {
        return;
    }
    auto expected = InitializationState::Uninitialized;
    if (!initializationState.compare_exchange_strong(expected, InitializationState::Initializing,
            std::memory_order_acq_rel))
    {
        return;
    }
    try
    {
        if (!TryInitializeObserver(f4se))
        {
            initializationState.store(InitializationState::Uninitialized, std::memory_order_release);
        }
    }
    catch (...)
    {
        DisableCollection();
        if (initializationState.load(std::memory_order_acquire) != InitializationState::Active)
        {
            initializationState.store(InitializationState::Uninitialized, std::memory_order_release);
        }
    }
}

}  // namespace fo4hdr::game

namespace fo4hdr {

GraphicsLifecyclePublication ReadGraphicsLifecycle() noexcept
{
    return graphicsLifecycle.Read();
}

}  // namespace fo4hdr
