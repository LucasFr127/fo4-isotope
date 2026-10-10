// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "../UnitTestSuite.hpp"
#include "TestHarness.hpp"
#include "NativeUiResources.hpp"
#include <array>
#include <cstring>

namespace fo4hdr::tests {
namespace {
namespace policy = game::detail;
struct Properties
{
    std::uint32_t width{640}, height{360}, format{24}, multiSample{};
    bool copyable{}, supportUnorderedAccess{true}, allowMipGeneration{}, forceLinear{};
    std::int32_t mipLevel{-1};
    std::uint32_t textureTarget{};
    bool enableFastClear{};
    std::array<std::uint8_t, 3> padding{0xA1, 0xB2, 0xC3};
};
static_assert(sizeof(Properties) == 0x20);

void TestFormatOnlyPromotion()
{
    const Properties source;
    Properties received;
    unsigned calls{};
    FO4HDR_CHECK(policy::NativePauseProperties(source));
    const auto changed = policy::ForwardNativeUiRegistration(&source, true,
        [&](const Properties* forwarded) {
            ++calls;
            FO4HDR_CHECK(forwarded != &source);
            std::memcpy(&received, forwarded, sizeof(received));
        });
    FO4HDR_CHECK(changed && calls == 1 && received.format == 10);
    Properties expected;
    std::memcpy(&expected, &source, sizeof(expected));
    expected.format = 10;
    FO4HDR_CHECK(std::memcmp(&received, &expected, sizeof(expected)) == 0);
    FO4HDR_CHECK(source.format == 24);
}

void TestUnknownDescriptorsForwardOnce()
{
    const Properties source;
    unsigned calls{};
    FO4HDR_CHECK(!policy::ForwardNativeUiRegistration(&source, false,
        [&](const Properties* forwarded) { ++calls; FO4HDR_CHECK(forwarded == &source); }));
    FO4HDR_CHECK(calls == 1);
    FO4HDR_CHECK(!policy::ForwardNativeUiRegistration<Properties>(nullptr, true,
        [&](const Properties* forwarded) { ++calls; FO4HDR_CHECK(forwarded == nullptr); }));
    FO4HDR_CHECK(calls == 2);
    bool caught{};
    try {
        (void)policy::ForwardNativeUiRegistration(&source, true,
            [&](const Properties*) { ++calls; throw 1; });
    } catch (int value) { caught = value == 1; }
    FO4HDR_CHECK(caught && calls == 3);
    auto wrong = source;
    wrong.format = 10;
    FO4HDR_CHECK(!policy::NativePauseProperties(wrong));
    wrong = source; wrong.multiSample = 1;
    FO4HDR_CHECK(!policy::NativePauseProperties(wrong));
    wrong = source; wrong.copyable = true;
    FO4HDR_CHECK(!policy::NativePauseProperties(wrong));
    wrong = source; wrong.supportUnorderedAccess = false;
    FO4HDR_CHECK(!policy::NativePauseProperties(wrong));
    wrong = source; wrong.width = 0;
    FO4HDR_CHECK(!policy::NativePauseProperties(wrong));
}

void TestCompletedPairReadiness()
{
    policy::NativeUiResourcePair pair;
    const policy::NativeUiRegistration first{1, 2, 24, 1}, second{1, 2, 25, 1};
    FO4HDR_CHECK(!pair.Begin(second));
    FO4HDR_CHECK(pair.Begin(first));
    FO4HDR_CHECK(!pair.Complete(24, true));
    FO4HDR_CHECK(pair.Begin(second));
    FO4HDR_CHECK(pair.Complete(25, true));
    FO4HDR_CHECK(!pair.Begin(second));
    FO4HDR_CHECK(pair.Begin(first));
    FO4HDR_CHECK(!pair.Complete(24, false));
    FO4HDR_CHECK(!pair.Begin(second));
    FO4HDR_CHECK(pair.Begin(first));
    FO4HDR_CHECK(!pair.Complete(24, true));
    FO4HDR_CHECK(!pair.Begin({1, 3, 25, 1}));
    FO4HDR_CHECK(pair.Begin(first));
    FO4HDR_CHECK(!pair.Complete(24, true));
    FO4HDR_CHECK(!pair.Begin({3, 2, 25, 1}));
    FO4HDR_CHECK(!pair.Begin({1, 2, 24, 0}));
    FO4HDR_CHECK(!pair.Begin(second));
}
}

void RunNativeUiResourcesTests()
{
    TestFormatOnlyPromotion();
    TestUnknownDescriptorsForwardOnce();
    TestCompletedPairReadiness();
}
}  // namespace fo4hdr::tests
