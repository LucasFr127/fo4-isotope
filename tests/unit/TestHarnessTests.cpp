// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "UnitTestSuite.hpp"

#include "TestHarness.hpp"

#include <atomic>
#include <chrono>
#include <limits>
#include <thread>

namespace fo4hdr::tests {

void RunApproximateComparisonTests()
{
    using fo4hdr::test::Approx;
    constexpr auto infinity = std::numeric_limits<double>::infinity();
    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    FO4HDR_CHECK(Approx(1.0, 1.0));
    FO4HDR_CHECK(Approx(1.0, 1.0 + 1.0e-11));
    FO4HDR_CHECK(!Approx(1.0, 2.0));
    FO4HDR_CHECK(!Approx(infinity, 1.0));
    FO4HDR_CHECK(!Approx(1.0, -infinity));
    FO4HDR_CHECK(!Approx(nan, 1.0));
    FO4HDR_CHECK(!Approx(1.0, nan));
    FO4HDR_CHECK(!Approx(1.0, 2.0, infinity));
    FO4HDR_CHECK(!Approx(1.0, 1.0, nan));
    FO4HDR_CHECK(!Approx(1.0, 1.0, -1.0));
}

void RunBoundedWaitReleaseTests()
{
    std::atomic<bool> ready{};
    std::atomic<bool> release{};
    std::atomic<bool> stopped{};
    std::thread worker([&]() {
        ready.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        stopped.store(true, std::memory_order_release);
    });

    FO4HDR_CHECK(fo4hdr::test::WaitUntil(
        [&]() { return ready.load(std::memory_order_acquire); }));
    const auto timedOut = !fo4hdr::test::WaitUntil(
        []() { return false; },
        std::chrono::steady_clock::duration::zero());
    release.store(true, std::memory_order_release);
    worker.join();

    FO4HDR_CHECK(timedOut);
    FO4HDR_CHECK(stopped.load(std::memory_order_acquire));
}

}  // namespace fo4hdr::tests
