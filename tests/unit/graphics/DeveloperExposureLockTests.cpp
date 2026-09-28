// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "DeveloperExposureLock.hpp"
#include "TestHarness.hpp"

namespace fo4hdr::tests {
namespace {

void TestExposureLockRetirement(DeveloperExposureLock& lock)
{
    // Hard retirement is immediate, unlike a normal off request.
    lock.Request(true);
    lock.Capture(60, 9);
    lock.EndFrame(60, 9, true);
    FO4HDR_CHECK(lock.Active());
    lock.Reset();
    FO4HDR_CHECK(!lock.Requested());
    FO4HDR_CHECK(!lock.Active());
    FO4HDR_CHECK(lock.CapturedFrame() == 0);
    FO4HDR_CHECK(lock.Generation() == 0);

    // Eligibility loss and generation replacement are hard retirements. They
    // require a fresh request and capture before activation can rearm.
    lock.Request(true);
    lock.Capture(70, 10);
    lock.EndFrame(70, 10, true);
    FO4HDR_CHECK(lock.Active());
    lock.EndFrame(71, 10, false);
    FO4HDR_CHECK(!lock.Requested());
    FO4HDR_CHECK(!lock.Active());
    FO4HDR_CHECK(!lock.NeedsCapture());
    lock.Request(true);
    lock.Capture(72, 10);
    lock.EndFrame(72, 10, true);
    FO4HDR_CHECK(lock.Active());
    lock.EndFrame(73, 11, true);
    FO4HDR_CHECK(!lock.Requested());
    FO4HDR_CHECK(!lock.Active());
    lock.Request(true);
    lock.Capture(74, 11);
    lock.EndFrame(74, 11, true);
    FO4HDR_CHECK(lock.Requested());
    FO4HDR_CHECK(lock.Active());
    FO4HDR_CHECK(lock.CapturedFrame() == 74);
    FO4HDR_CHECK(lock.Generation() == 11);
}

}  // namespace

void TestDeveloperExposureLockFramePolicy()
{
    DeveloperExposureLock lock;
    FO4HDR_CHECK(!lock.Requested());
    FO4HDR_CHECK(!lock.Active());
    FO4HDR_CHECK(!lock.NeedsCapture());
    FO4HDR_CHECK(lock.CapturedFrame() == 0);
    FO4HDR_CHECK(lock.Generation() == 0);

    // A request cannot affect the capture frame. It becomes active only at
    // that frame's eligible Present boundary.
    lock.Request(true);
    FO4HDR_CHECK(lock.Requested());
    FO4HDR_CHECK(lock.NeedsCapture());
    lock.EndFrame(39, 7, true);
    FO4HDR_CHECK(!lock.Active());
    FO4HDR_CHECK(lock.NeedsCapture());
    lock.Capture(40, 7);
    FO4HDR_CHECK(!lock.Active());
    FO4HDR_CHECK(!lock.NeedsCapture());
    FO4HDR_CHECK(lock.CapturedFrame() == 40);
    FO4HDR_CHECK(lock.Generation() == 7);
    lock.EndFrame(40, 7, true);
    FO4HDR_CHECK(lock.Active());

    // Held locks survive ordinary frames in the generation that captured
    // them. Turning the request off still keeps the current frame stable and
    // releases only at its Present boundary.
    lock.EndFrame(41, 7, true);
    FO4HDR_CHECK(lock.Active());
    lock.Request(false);
    FO4HDR_CHECK(lock.Active());
    FO4HDR_CHECK(!lock.Requested());
    lock.EndFrame(42, 7, true);
    FO4HDR_CHECK(!lock.Active());
    FO4HDR_CHECK(lock.CapturedFrame() == 0);
    FO4HDR_CHECK(lock.Generation() == 0);

    // Canceling before activation discards the capture; repeated off requests
    // and end-frame processing remain harmless.
    lock.Request(true);
    lock.Capture(50, 8);
    lock.Request(false);
    FO4HDR_CHECK(!lock.Active());
    FO4HDR_CHECK(lock.CapturedFrame() == 0);
    lock.EndFrame(50, 8, true);
    lock.Request(false);
    lock.EndFrame(51, 8, true);
    FO4HDR_CHECK(!lock.Requested());
    FO4HDR_CHECK(!lock.Active());

    TestExposureLockRetirement(lock);
}

}  // namespace fo4hdr::tests
