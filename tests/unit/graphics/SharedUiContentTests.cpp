// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "SharedUiContent.hpp"
#include "TestHarness.hpp"
#include <limits>

namespace fo4hdr::tests {
namespace {

constexpr SharedUiFrameIdentity kIdentity{1, 2, 3};

void TestSharedUiResourceReadiness()
{
    SharedUiResourceReadiness readiness;
    FO4HDR_CHECK(!readiness.Ready());
    readiness.SetReady(true);
    FO4HDR_CHECK(readiness.Ready());
    readiness.SetReady(false);
    FO4HDR_CHECK(!readiness.Ready());
    readiness.SetReady(true);
    FO4HDR_CHECK(readiness.Ready());
    readiness.Disable();
    FO4HDR_CHECK(!readiness.Ready());
    readiness.SetReady(false);
    readiness.SetReady(true);
    FO4HDR_CHECK(!readiness.Ready());
}

void TestSharedUiFreshContent()
{
    SharedUiContent content;
    FO4HDR_CHECK(!content.Ready(kIdentity));
    FO4HDR_CHECK(content.Begin(kIdentity, 4, 203.0F));
    FO4HDR_CHECK(!content.Ready(kIdentity));
    content.ObserveNativeWrite(4);
    content.Close(kIdentity, 4);
    FO4HDR_CHECK(content.Ready(kIdentity));
    FO4HDR_CHECK(content.Ready(kIdentity));
    FO4HDR_CHECK(!content.Begin(kIdentity, 5, 300.0F));
    FO4HDR_CHECK(content.white == 203.0F);
    FO4HDR_CHECK(content.revision == 1);

    content.Advance();
    FO4HDR_CHECK(!content.Ready(kIdentity));
    const SharedUiFrameIdentity next{1, 2, 4};
    FO4HDR_CHECK(!content.Ready(next));
    FO4HDR_CHECK(content.Begin(next, 5, 300.0F));
    content.ObserveNativeWrite(5);
    content.Close(next, 5);
    FO4HDR_CHECK(content.Ready(next));
    FO4HDR_CHECK(content.white == 300.0F);
    FO4HDR_CHECK(content.revision == 2);
}

void TestSharedUiIdentityRejection()
{
    for (const auto changed : {SharedUiFrameIdentity{2, 2, 3},
             SharedUiFrameIdentity{1, 3, 3}, SharedUiFrameIdentity{1, 2, 4},
             SharedUiFrameIdentity{1, 2, 3, 1}})
    {
        SharedUiContent content;
        FO4HDR_CHECK(content.Begin(kIdentity, 4, 100.0F));
        content.ObserveNativeWrite(4);
        content.Close(kIdentity, 4);
        FO4HDR_CHECK(!content.Ready(changed));
        content.Close(changed, 4);
        FO4HDR_CHECK(!content.Ready(kIdentity));
    }
    SharedUiContent wrongScope;
    FO4HDR_CHECK(wrongScope.Begin(kIdentity, 4, 203.0F));
    wrongScope.Close(kIdentity, 5);
    FO4HDR_CHECK(!wrongScope.Ready(kIdentity));
    wrongScope.Advance();
    wrongScope.rejected = true;
    FO4HDR_CHECK(!wrongScope.Begin(kIdentity, 6, 203.0F));
}

void TestSharedUiEmptyScopes()
{
    SharedUiContent content;
    FO4HDR_CHECK(content.Begin(kIdentity, 4, 203.0F));
    FO4HDR_CHECK(!content.Ready(kIdentity));
    content.Close(kIdentity, 4);
    FO4HDR_CHECK(content.Ready(kIdentity) && !content.nativeWritten);
    FO4HDR_CHECK(content.Open(kIdentity, 5));
    FO4HDR_CHECK(!content.Ready(kIdentity));
    content.Close(kIdentity, 5);
    FO4HDR_CHECK(content.Ready(kIdentity) && !content.nativeWritten);
    FO4HDR_CHECK(!content.Ready({1, 2, 4}));
    content.rejected = true;
    FO4HDR_CHECK(!content.Ready(kIdentity));
    content.Advance();
    FO4HDR_CHECK(!content.Ready(kIdentity));
}

void TestSharedUiSequentialScopes()
{
    SharedUiContent content;
    FO4HDR_CHECK(content.Begin(kIdentity, 4, 203.0F));
    FO4HDR_CHECK(content.ScopeActive(kIdentity, 4));
    FO4HDR_CHECK(!content.Open(kIdentity, 5));
    content.ObserveNativeWrite(4);
    content.Close(kIdentity, 4);
    for (const auto serial : {5U, 6U})
    {
        FO4HDR_CHECK(!content.Open(kIdentity, 0));
        FO4HDR_CHECK(!content.Open(kIdentity, content.scope));
        FO4HDR_CHECK(content.Open(kIdentity, serial));
        FO4HDR_CHECK(content.ScopeActive(kIdentity, serial));
        FO4HDR_CHECK(!content.ScopeActive(kIdentity, serial - 1));
        FO4HDR_CHECK(!content.Ready(kIdentity));
        content.ObserveNativeWrite(serial);
        content.Close(kIdentity, serial);
        FO4HDR_CHECK(content.Ready(kIdentity));
        FO4HDR_CHECK(content.white == 203.0F && content.revision == 1);
    }
    FO4HDR_CHECK(!content.Open({1, 2, 4}, 7));
    FO4HDR_CHECK(content.Ready(kIdentity));
    FO4HDR_CHECK(content.Open(kIdentity, 7));
    content.Close(kIdentity, 6);
    FO4HDR_CHECK(!content.Ready(kIdentity));
    FO4HDR_CHECK(!content.Open(kIdentity, 8));
    content.Advance();
    FO4HDR_CHECK(!content.Open(kIdentity, 8));
    FO4HDR_CHECK(!content.ScopeActive(kIdentity, 7));
}

void TestSharedUiInvalidSeed()
{
    SharedUiContent content;
    FO4HDR_CHECK(!content.Begin({}, 4, 203.0F));
    FO4HDR_CHECK(!content.Begin(kIdentity, 0, 203.0F));
    for (const auto white : {0.0F, -1.0F, std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::quiet_NaN()})
        FO4HDR_CHECK(!content.Begin(kIdentity, 4, white));
    FO4HDR_CHECK(content.revision == 0);
}

} // namespace

void RunSharedUiContentTests()
{
    TestSharedUiResourceReadiness();
    TestSharedUiFreshContent();
    TestSharedUiIdentityRejection();
    TestSharedUiEmptyScopes();
    TestSharedUiSequentialScopes();
    TestSharedUiInvalidSeed();
}

} // namespace fo4hdr::tests
