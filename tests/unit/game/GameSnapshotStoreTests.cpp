// SPDX-License-Identifier: GPL-3.0-or-later
#include "GameSnapshotStore.hpp"
#include "ImageSpaceObserver.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {
void TestCopiedStoreAndLifecycle()
{
    fo4hdr::GameSnapshotStore store;
    store.Update([](fo4hdr::GameHdrSnapshot& value) noexcept {
        value.renderWidth = 1920;
        value.rendererDeviceIdentity = 17;
    });
    auto copy = store.Read();
    FO4HDR_CHECK(copy.generation == 1);
    FO4HDR_CHECK(copy.renderWidth == 1920 && copy.rendererDeviceIdentity == 17);
    copy.renderWidth = 1;
    FO4HDR_CHECK(store.Read().renderWidth == 1920);
    store.Update([](fo4hdr::GameHdrSnapshot& value) noexcept {
        fo4hdr::ApplyGameLifecycleTransition(value, fo4hdr::GameLifecycleEvent::PreLoadGame, true);
    });
    copy = store.Read();
    FO4HDR_CHECK(copy.generation == 2 && copy.resourceGeneration == 1);
    FO4HDR_CHECK(copy.loading && copy.renderWidth == 0 && copy.rendererDeviceIdentity == 0);
    store.Update([](fo4hdr::GameHdrSnapshot& value) noexcept {
        fo4hdr::ApplyGameLifecycleTransition(value, fo4hdr::GameLifecycleEvent::NewGame, true);
    });
    copy = store.Read();
    FO4HDR_CHECK(!copy.loading && copy.resourceGeneration == 2);
}

void TestCopiedImageSpaceAndDrawScope()
{
    std::array<std::byte, 48> bytes{};
    std::array<std::size_t, 12> offsets{};
    for (std::size_t index = 0; index < offsets.size(); ++index)
    {
        offsets[index] = index * sizeof(float);
        const float value = static_cast<float>(index + 1);
        std::memcpy(bytes.data() + offsets[index], &value, sizeof(value));
    }
    fo4hdr::ImageSpaceActiveData active{};
    FO4HDR_CHECK(fo4hdr::DecodeImageSpaceActiveData(bytes, offsets, active));
    FO4HDR_CHECK(active.eyeAdaptSpeed == 1.0F && active.cinematicContrast == 12.0F);
    offsets.back() = bytes.size();
    FO4HDR_CHECK(!fo4hdr::DecodeImageSpaceActiveData(bytes, offsets, active));
    FO4HDR_CHECK(active.cinematicContrast == 12.0F);
    offsets.back() = bytes.size() - sizeof(float);
    const float invalid = std::numeric_limits<float>::infinity();
    std::memcpy(bytes.data(), &invalid, sizeof(invalid));
    FO4HDR_CHECK(!fo4hdr::DecodeImageSpaceActiveData(bytes, offsets, active));

    fo4hdr::NativeLutObservation observed{};
    observed.threadId = ::GetCurrentThreadId();
    observed.validSlots = 1;
    // The controlled plain scope does not install or qualify a native observer.
    const auto previousStatus = fo4hdr::GetNativeLutObserverStatus();
    fo4hdr::SetNativeLutObserverStatus(fo4hdr::NativeLutObserverStatus::ready);
    fo4hdr::ResetNativeLutObservation();
    fo4hdr::NativeLutObservation copy{};
    {
        const fo4hdr::NativeLutScope scope(observed);
        copy = fo4hdr::CurrentNativeLutObservation();
        FO4HDR_CHECK(fo4hdr::IsCurrentNativeLutObservation(copy, observed.threadId));
        FO4HDR_CHECK(copy.serial != 0 && copy.validSlots == 1);
        {
            const fo4hdr::NativeLutScope nested(observed);
            copy = fo4hdr::CurrentNativeLutObservation();
            FO4HDR_CHECK(fo4hdr::IsCurrentNativeLutObservation(copy, observed.threadId));
        }
        copy = fo4hdr::CurrentNativeLutObservation();
        FO4HDR_CHECK(fo4hdr::IsCurrentNativeLutObservation(copy, observed.threadId));
        fo4hdr::ResetNativeLutObservation();
        FO4HDR_CHECK(!fo4hdr::IsCurrentNativeLutObservation(copy, observed.threadId));
    }
    copy = fo4hdr::CurrentNativeLutObservation();
    FO4HDR_CHECK(!fo4hdr::IsCurrentNativeLutObservation(copy, observed.threadId));
    fo4hdr::SetNativeLutObserverStatus(previousStatus);
}


} // namespace
namespace fo4hdr::tests {
void RunGameSnapshotStoreTests()
{
    TestCopiedStoreAndLifecycle();
    TestCopiedImageSpaceAndDrawScope();
}
} // namespace fo4hdr::tests
