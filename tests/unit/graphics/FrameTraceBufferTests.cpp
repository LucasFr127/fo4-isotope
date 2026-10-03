// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"

#include "FrameTraceBuffer.hpp"
#include "TestHarness.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace fo4hdr::tests {
namespace {

struct TraceEvent
{
    std::uint64_t sequence{};
    std::uint64_t value{};
};

void VerifyHotkeySurvivesPresentThreadMigration()
{
    FrameTraceBuffer<TraceEvent, 2> trace;
    std::array<bool, 4> gameplay{};
    std::jthread([&] {
        gameplay = {trace.HotkeyPressed(false), trace.HotkeyPressed(true),
            trace.HotkeyPressed(true), trace.Begin()};
    }).join();
    FO4HDR_CHECK((gameplay == std::array{false, true, false, true}));
    trace.Stop(FrameTraceStop::retired);
    FO4HDR_CHECK(trace.Report([](auto, auto, auto) {}));
    std::array<bool, 4> loading{};
    std::jthread([&] {
        loading = {trace.HotkeyPressed(true), trace.HotkeyPressed(false),
            trace.HotkeyPressed(true), trace.Begin()};
    }).join();
    FO4HDR_CHECK((loading == std::array{false, false, true, true}));
    trace.Stop(FrameTraceStop::complete);
    FO4HDR_CHECK(trace.Report([](auto, auto, auto) {}));
    FO4HDR_CHECK(!trace.HotkeyPressed(true));
    FO4HDR_CHECK(!trace.HotkeyPressed(false));
    FO4HDR_CHECK(trace.HotkeyPressed(true));
}

void VerifyStartPublishesMetadataOnlyWhenAccepted()
{
    FrameTraceBuffer<TraceEvent, 2> trace;
    unsigned session{};
    bool recordingDuringStart{};
    bool reentrantStartAccepted{};
    bool reentrantWriteAccepted{};
    const auto start = [&]() noexcept {
        ++session;
        recordingDuringStart = trace.Recording();
        reentrantStartAccepted = trace.Begin();
        reentrantWriteAccepted = trace.Record([](TraceEvent&, std::uint64_t) noexcept {});
    };
    FO4HDR_CHECK(trace.Begin(start));
    FO4HDR_CHECK(session == 1);
    FO4HDR_CHECK(!recordingDuringStart);
    FO4HDR_CHECK(!reentrantStartAccepted);
    FO4HDR_CHECK(!reentrantWriteAccepted);
    FO4HDR_CHECK(trace.Recording());
    FO4HDR_CHECK(!trace.Begin(start));
    trace.Stop(FrameTraceStop::complete);
    FO4HDR_CHECK(!trace.Begin(start));
    FO4HDR_CHECK(session == 1);
    FO4HDR_CHECK(trace.Report([](auto events, auto, auto dropped) {
        FO4HDR_CHECK(events.empty());
        FO4HDR_CHECK(dropped == 0);
    }));
    FO4HDR_CHECK(trace.Begin(start));
    FO4HDR_CHECK(session == 2);
}

void VerifyCapacityPreservesEvents()
{
    FrameTraceBuffer<TraceEvent, 2> trace;
    unsigned calls{};
    const auto write = [&](TraceEvent& event, std::uint64_t sequence) noexcept {
        ++calls;
        event = {sequence, sequence + 10};
    };
    FO4HDR_CHECK(!trace.Record(write));
    FO4HDR_CHECK(!trace.Report([](auto, auto, auto) {}));
    FO4HDR_CHECK(calls == 0);
    FO4HDR_CHECK(trace.Begin());
    FO4HDR_CHECK(trace.Record(write));
    FO4HDR_CHECK(trace.Record(write));
    FO4HDR_CHECK(trace.State() == FrameTraceStop::capacity);
    FO4HDR_CHECK(!trace.Record(write));
    FO4HDR_CHECK(!trace.Begin());
    trace.Stop(FrameTraceStop::complete);
    FO4HDR_CHECK(trace.Report([](auto events, auto reason, auto dropped) {
        FO4HDR_CHECK(reason == FrameTraceStop::capacity);
        FO4HDR_CHECK(dropped == 0);
        FO4HDR_CHECK(events.size() == 2);
        if (events.size() != 2) return;
        FO4HDR_CHECK(events[0].sequence == 1 && events[0].value == 11);
        FO4HDR_CHECK(events[1].sequence == 2 && events[1].value == 12);
    }));
    FO4HDR_CHECK(calls == 2);
}

void VerifyStopAndReportRetry()
{
    FrameTraceBuffer<TraceEvent, 2> trace;
    FO4HDR_CHECK(trace.Begin());
    FO4HDR_CHECK(trace.Record([](TraceEvent& event, std::uint64_t sequence) noexcept {
        event = {sequence, 27};
    }));
    trace.Stop(FrameTraceStop::complete);
    trace.Stop(FrameTraceStop::retired);
    bool threw{};
    try
    {
        static_cast<void>(trace.Report([](auto, auto, auto) {
            throw std::runtime_error("report failed");
        }));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    FO4HDR_CHECK(threw);
    FO4HDR_CHECK(!trace.Begin());
    unsigned reports{};
    const auto read = [&](auto events, auto reason, auto dropped) {
        ++reports;
        FO4HDR_CHECK(reason == FrameTraceStop::complete);
        FO4HDR_CHECK(dropped == 0);
        FO4HDR_CHECK(events.size() == 1);
        if (events.size() != 1) return;
        FO4HDR_CHECK(events[0].sequence == 1 && events[0].value == 27);
    };
    FO4HDR_CHECK(trace.Report(read));
    FO4HDR_CHECK(!trace.Report(read));
    FO4HDR_CHECK(reports == 1);
}

void VerifyFrozenReportExcludesWriterAndResetsCapture()
{
    FrameTraceBuffer<TraceEvent, 3> trace;
    FO4HDR_CHECK(trace.Begin());
    std::atomic<bool> entered{};
    std::atomic<bool> release{};
    std::atomic<bool> accepted{};
    std::atomic<bool> releasedInTime{};
    std::thread writer([&] {
        accepted.store(trace.Record([&](TraceEvent& event, std::uint64_t sequence) noexcept {
            event.sequence = sequence;
            entered.store(true, std::memory_order_release);
            releasedInTime.store(test::WaitUntil([&] {
                return release.load(std::memory_order_acquire);
            }));
            event.value = 91;
        }));
    });
    const bool writerEntered = test::WaitUntil([&] {
        return entered.load(std::memory_order_acquire);
    });
    FO4HDR_CHECK(writerEntered);
    unsigned rejectedCalls{};
    const auto rejected = [&](TraceEvent&, std::uint64_t) noexcept { ++rejectedCalls; };
    if (writerEntered)
    {
        FO4HDR_CHECK(!trace.Record(rejected));
        trace.Stop(FrameTraceStop::retired);
        FO4HDR_CHECK(!trace.Report([](auto, auto, auto) {}));
        FO4HDR_CHECK(!trace.Record(rejected));
    }
    release.store(true, std::memory_order_release);
    writer.join();
    trace.Stop(FrameTraceStop::retired);
    FO4HDR_CHECK(accepted.load());
    FO4HDR_CHECK(releasedInTime.load());
    FO4HDR_CHECK(rejectedCalls == 0);
    FO4HDR_CHECK(trace.Report([&](auto events, auto reason, auto dropped) {
        FO4HDR_CHECK(reason == FrameTraceStop::retired);
        FO4HDR_CHECK(dropped == (writerEntered ? 1U : 0U));
        FO4HDR_CHECK(events.size() == 1);
        if (events.size() != 1) return;
        FO4HDR_CHECK(events[0].sequence == 1 && events[0].value == 91);
    }));

    FO4HDR_CHECK(trace.Begin());
    FO4HDR_CHECK(trace.Record([](TraceEvent& event, std::uint64_t sequence) noexcept {
        event = {sequence, 92};
    }));
    trace.Stop(FrameTraceStop::complete);
    FO4HDR_CHECK(trace.Report([](auto events, auto reason, auto dropped) {
        FO4HDR_CHECK(reason == FrameTraceStop::complete);
        FO4HDR_CHECK(dropped == 0);
        FO4HDR_CHECK(events.size() == 1);
        if (events.size() != 1) return;
        FO4HDR_CHECK(events[0].sequence == 1 && events[0].value == 92);
    }));
}

void VerifyConcurrentProducerAccounting()
{
    constexpr std::size_t producerCount = 4;
    constexpr std::size_t attemptsPerProducer = 1000;
    constexpr std::size_t totalAttempts = producerCount * attemptsPerProducer;
    FrameTraceBuffer<TraceEvent, totalAttempts + 1> trace;
    FO4HDR_CHECK(trace.Begin());
    std::atomic<std::uint64_t> accepted{};
    std::array<std::jthread, producerCount> producers;
    for (auto& producer : producers)
    {
        producer = std::jthread([&] {
            for (std::size_t attempt = 0; attempt < attemptsPerProducer; ++attempt)
            {
                if (trace.Record([](TraceEvent& event, std::uint64_t sequence) noexcept {
                    event = {sequence, sequence * 3};
                }))
                {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& producer : producers) producer.join();
    trace.Stop(FrameTraceStop::complete);
    FO4HDR_CHECK(trace.Report([&](auto events, auto reason, auto dropped) {
        FO4HDR_CHECK(reason == FrameTraceStop::complete);
        FO4HDR_CHECK(events.size() == accepted.load());
        FO4HDR_CHECK(events.size() + dropped == totalAttempts);
        std::uint64_t sequence{};
        for (const auto& event : events)
        {
            ++sequence;
            FO4HDR_CHECK(event.sequence == sequence && event.value == sequence * 3);
        }
    }));
}

void VerifyRollingTailIsChronologicalAndFrozen()
{
    FrameTraceBuffer<TraceEvent, 3> trace;
    FO4HDR_CHECK(trace.Begin([]() noexcept {}, true));
    for (unsigned attempt = 0; attempt < 11; ++attempt)
        FO4HDR_CHECK(trace.Record([](TraceEvent& event, std::uint64_t sequence) noexcept {
            event = {sequence, sequence * 3};
        }));
    FO4HDR_CHECK(trace.Recording() && trace.Overwritten() == 8);
    trace.Stop(FrameTraceStop::complete);
    FO4HDR_CHECK(!trace.Record([](auto&, auto) noexcept {}));
    const auto read = [](auto events, auto reason, auto dropped) {
        FO4HDR_CHECK(reason == FrameTraceStop::complete && dropped == 0);
        FO4HDR_CHECK(events.size() == 3);
        for (std::size_t index = 0; index < events.size(); ++index)
        {
            FO4HDR_CHECK(events[index].sequence == index + 9);
            FO4HDR_CHECK(events[index].value == (index + 9) * 3);
        }
    };
    bool threw{};
    try
    {
        (void)trace.Report([&](auto events, auto reason, auto dropped) {
            read(events, reason, dropped);
            throw std::runtime_error("rolling report retry");
        });
    }
    catch (const std::runtime_error&) { threw = true; }
    FO4HDR_CHECK(threw && trace.Overwritten() == 8);
    FO4HDR_CHECK(!trace.Begin());
    FO4HDR_CHECK(trace.Report(read));
}

void VerifyCaptureScopeProtectsPolicyAndNestedRecord()
{
    FrameTraceBuffer<TraceEvent, 3> trace;
    FO4HDR_CHECK(trace.Begin());
    const auto epoch = trace.Epoch();
    FO4HDR_CHECK(trace.WithCapture(epoch, [&]() noexcept {
        FO4HDR_CHECK(trace.Record(epoch, [](TraceEvent& event, std::uint64_t sequence) noexcept {
            event = {sequence, 41};
        }));
        trace.Stop(FrameTraceStop::retired);
        FO4HDR_CHECK(!trace.Report([](auto, auto, auto) {}));
        FO4HDR_CHECK(!trace.Begin());
    }));
    FO4HDR_CHECK(trace.Report([](auto events, auto reason, auto dropped) {
        FO4HDR_CHECK(reason == FrameTraceStop::retired && dropped == 0);
        FO4HDR_CHECK(events.size() == 1);
    }));
    FO4HDR_CHECK(trace.Begin());
    FO4HDR_CHECK(!trace.WithCapture(epoch, [&]() noexcept { trace.Stop(FrameTraceStop::retired); }));
    FO4HDR_CHECK(trace.Recording());
}

void VerifyPriorSessionWritersCannotAppendAfterRestart()
{
    FrameTraceBuffer<TraceEvent, 3> trace;
    FO4HDR_CHECK(trace.Begin());
    const auto epoch = trace.Epoch();
    FO4HDR_CHECK(trace.Record([](TraceEvent& event, std::uint64_t sequence) noexcept {
        event = {sequence, 51};
    }));
    trace.Stop(FrameTraceStop::retired);
    FO4HDR_CHECK(trace.Report([](auto events, auto reason, auto dropped) {
        FO4HDR_CHECK(reason == FrameTraceStop::retired && dropped == 0);
        FO4HDR_CHECK(events.size() == 1);
        if (!events.empty()) FO4HDR_CHECK(events[0].value == 51);
    }));
    FO4HDR_CHECK(trace.Begin());
    FO4HDR_CHECK(!trace.Record(epoch, [](auto&, auto) noexcept {}));
    FO4HDR_CHECK(!trace.WithCapture(epoch, [&]() noexcept { trace.Stop(FrameTraceStop::complete); }));
    FO4HDR_CHECK(trace.Recording());
    trace.Stop(FrameTraceStop::complete);
    bool staleReportCalled{};
    FO4HDR_CHECK(!trace.Report([&](auto, auto, auto) { staleReportCalled = true; }, epoch));
    FO4HDR_CHECK(!staleReportCalled && trace.State() == FrameTraceStop::complete);
    FO4HDR_CHECK(trace.Report([](auto events, auto, auto dropped) {
        FO4HDR_CHECK(events.empty() && dropped == 1);
    }, trace.Epoch()));
}

void VerifyConcurrentCaptureScopeRejectsReport()
{
    FrameTraceBuffer<TraceEvent, 3> trace;
    FO4HDR_CHECK(trace.Begin());
    const auto epoch = trace.Epoch();
    std::atomic<bool> entered{}, release{}, completed{};
    std::jthread observer([&] {
        completed.store(trace.WithCapture(epoch, [&]() noexcept {
            entered.store(true);
            (void)test::WaitUntil([&] { return release.load(); });
            trace.Stop(FrameTraceStop::retired);
        }));
    });
    const bool active = test::WaitUntil([&] { return entered.load(); });
    FO4HDR_CHECK(active);
    if (active)
    {
        trace.Stop(FrameTraceStop::complete);
        FO4HDR_CHECK(!trace.Report([](auto, auto, auto) {}));
        FO4HDR_CHECK(!trace.Begin());
    }
    release.store(true);
    observer.join();
    FO4HDR_CHECK(completed.load());
    FO4HDR_CHECK(trace.Report([&](auto, auto reason, auto dropped) {
        FO4HDR_CHECK(reason == (active ? FrameTraceStop::complete : FrameTraceStop::retired));
        FO4HDR_CHECK(dropped == 0);
    }));
}

} // namespace

void RunFrameTraceBufferTests()
{
    VerifyHotkeySurvivesPresentThreadMigration();
    VerifyStartPublishesMetadataOnlyWhenAccepted();
    VerifyCapacityPreservesEvents();
    VerifyStopAndReportRetry();
    VerifyFrozenReportExcludesWriterAndResetsCapture();
    VerifyConcurrentProducerAccounting();
    VerifyRollingTailIsChronologicalAndFrozen();
    VerifyCaptureScopeProtectsPolicyAndNestedRecord();
    VerifyPriorSessionWritersCannotAppendAfterRestart();
    VerifyConcurrentCaptureScopeRejectsReport();
}

} // namespace fo4hdr::tests
