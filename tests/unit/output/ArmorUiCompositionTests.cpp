// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "../UnitTestSuite.hpp"

#include "Fallout4HDR/output/ArmorUiComposition.hpp"
#include "TestHarness.hpp"

#include <array>
#include <optional>
#include <thread>

namespace fo4hdr::tests {
namespace {

std::array<ArmorUiScopeToken, 8> g_boundaryTokens{};
std::array<bool, 8> g_boundaryEntering{};
std::size_t g_boundaryCount{};

void RecordBoundary(const ArmorUiScopeToken& token, bool entering) noexcept
{
    if (g_boundaryCount < g_boundaryTokens.size())
    {
        g_boundaryTokens[g_boundaryCount] = token;
        g_boundaryEntering[g_boundaryCount] = entering;
    }
    ++g_boundaryCount;
}

void ResetState()
{
    SetArmorUiBoundaryCallback(nullptr);
    SetArmorUiCorrectionEnabled(false);
    ResetArmorUiScope();
    g_boundaryTokens = {};
    g_boundaryEntering = {};
    g_boundaryCount = 0;
}

void TestRequestLifecycle()
{
    ResetState();
    const auto disabledGeneration = ArmorUiScopeGeneration();
    FO4HDR_CHECK(!ArmorUiCorrectionEnabled());
    FO4HDR_CHECK(!ArmorUiScopeEnabled());
    SetArmorUiCorrectionEnabled(false);
    FO4HDR_CHECK(ArmorUiScopeGeneration() == disabledGeneration);
    SetArmorUiCorrectionEnabled(true);
    const auto enabledGeneration = ArmorUiScopeGeneration();
    FO4HDR_CHECK(enabledGeneration != disabledGeneration);
    FO4HDR_CHECK(ArmorUiCorrectionEnabled());
    FO4HDR_CHECK(ArmorUiScopeEnabled());
    SetArmorUiCorrectionEnabled(true);
    FO4HDR_CHECK(ArmorUiScopeGeneration() == enabledGeneration);
    ResetArmorUiScope();
    FO4HDR_CHECK(ArmorUiScopeGeneration() != enabledGeneration);
    FO4HDR_CHECK(ArmorUiCorrectionEnabled());
}

void TestScopesAndBoundaries()
{
    ResetState();
    SetArmorUiCorrectionEnabled(true);
    SetArmorUiBoundaryCallback(&RecordBoundary);
    ArmorUiScopeToken outerToken{};
    {
        ArmorUiScope outer{{.owner = ArmorUiOwner::powerArmor, .threadId = 17, .targetArgument = 29,
            .postAa = true, .hasRoot = true}};
        FO4HDR_CHECK(outer.Entered());
        outerToken = CurrentArmorUiScope();
        FO4HDR_CHECK(outerToken.owner == ArmorUiOwner::powerArmor);
        FO4HDR_CHECK(outerToken.threadId == 17);
        FO4HDR_CHECK(outerToken.targetArgument == 29);
        FO4HDR_CHECK(outerToken.depth == 1);
        FO4HDR_CHECK(outerToken.postAa && outerToken.hasRoot);
        FO4HDR_CHECK(outerToken.requestGeneration == ArmorUiScopeGeneration());
        {
            ArmorUiScope nested{{.owner = ArmorUiOwner::other, .threadId = 17, .targetArgument = 31,
            .postAa = false, .hasRoot = false}};
            FO4HDR_CHECK(nested.Entered());
            const auto nestedToken = CurrentArmorUiScope();
            FO4HDR_CHECK(nestedToken.owner == ArmorUiOwner::other);
            FO4HDR_CHECK(nestedToken.depth == 2);
            FO4HDR_CHECK(nestedToken.serial != outerToken.serial);
        }
        FO4HDR_CHECK(CurrentArmorUiScope().serial == outerToken.serial);
        ResetArmorUiScope();
        FO4HDR_CHECK(CurrentArmorUiScope().serial == outerToken.serial);
        FO4HDR_CHECK(CurrentArmorUiScope().requestGeneration !=
            ArmorUiScopeGeneration());
    }
    FO4HDR_CHECK(CurrentArmorUiScope().serial == 0);
    FO4HDR_CHECK(g_boundaryCount == 4);
    FO4HDR_CHECK(g_boundaryEntering[0] && g_boundaryEntering[1]);
    FO4HDR_CHECK(!g_boundaryEntering[2] && !g_boundaryEntering[3]);
    FO4HDR_CHECK(g_boundaryTokens[0].serial == g_boundaryTokens[3].serial);
    FO4HDR_CHECK(g_boundaryTokens[1].serial == g_boundaryTokens[2].serial);

    SetArmorUiCorrectionEnabled(false);
    ArmorUiScope disabled{{.owner = ArmorUiOwner::powerArmor, .threadId = 17, .targetArgument = 0,
            .postAa = true, .hasRoot = true}};
    FO4HDR_CHECK(!disabled.Entered());
    FO4HDR_CHECK(CurrentArmorUiScope().serial == 0);
}

void TestRejectedNestedScopeMasksOuterScope()
{
    ResetState();
    SetArmorUiCorrectionEnabled(true);
    SetArmorUiBoundaryCallback(&RecordBoundary);
    {
        ArmorUiScope outer{{.owner = ArmorUiOwner::powerArmor, .threadId = 7, .targetArgument = 0,
            .postAa = true, .hasRoot = true}};
        const auto outerToken = CurrentArmorUiScope();
        SetArmorUiCorrectionEnabled(false);
        {
            ArmorUiScope rejected{{.owner = ArmorUiOwner::other, .threadId = 7, .targetArgument = 0,
            .postAa = true, .hasRoot = true}};
            FO4HDR_CHECK(!rejected.Entered());
            FO4HDR_CHECK(CurrentArmorUiScope().serial == 0);
        }
        FO4HDR_CHECK(CurrentArmorUiScope().serial == outerToken.serial);
    }
    FO4HDR_CHECK(g_boundaryCount == 4);
    FO4HDR_CHECK(g_boundaryTokens[1].owner == ArmorUiOwner::none);
    FO4HDR_CHECK(g_boundaryTokens[1].depth == 2);
    FO4HDR_CHECK(g_boundaryTokens[1].serial == g_boundaryTokens[2].serial);
}

void TestDepthLimitAndThreadIsolation()
{
    ResetState();
    SetArmorUiCorrectionEnabled(true);
    std::array<std::optional<ArmorUiScope>, ArmorUiScope::maximumDepth + 1>
        scopes{};
    for (std::size_t index = 0; index < ArmorUiScope::maximumDepth; ++index)
    {
        scopes[index].emplace(ArmorUiScopeEntry{.owner = ArmorUiOwner::powerArmor, .threadId = 11, .targetArgument = 0,
            .postAa = true, .hasRoot = true});
        FO4HDR_CHECK(scopes[index]->Entered());
        FO4HDR_CHECK(CurrentArmorUiScope().depth == index + 1);
    }
    scopes.back().emplace(ArmorUiScopeEntry{.owner = ArmorUiOwner::powerArmor, .threadId = 11, .targetArgument = 0,
            .postAa = true, .hasRoot = true});
    FO4HDR_CHECK(!scopes.back()->Entered());
    FO4HDR_CHECK(scopes.back()->NestingRejected());
    FO4HDR_CHECK(CurrentArmorUiScope().serial == 0);
    scopes.back().reset();
    FO4HDR_CHECK(CurrentArmorUiScope().depth == ArmorUiScope::maximumDepth);
    for (std::size_t index = ArmorUiScope::maximumDepth; index > 0; --index)
        scopes[index - 1].reset();

    ArmorUiScope mainScope{{.owner = ArmorUiOwner::powerArmor, .threadId = 23, .targetArgument = 0,
            .postAa = true, .hasRoot = true}};
    const auto mainToken = CurrentArmorUiScope();
    bool workerInitiallyEmpty{};
    bool workerRestoredEmpty{};
    ArmorUiScopeToken workerToken{};
    std::thread worker([&] {
        workerInitiallyEmpty = CurrentArmorUiScope().serial == 0;
        {
            ArmorUiScope workerScope{{.owner = ArmorUiOwner::powerArmor, .threadId = 47, .targetArgument = 0,
            .postAa = true, .hasRoot = true}};
            workerToken = CurrentArmorUiScope();
        }
        workerRestoredEmpty = CurrentArmorUiScope().serial == 0;
    });
    worker.join();
    FO4HDR_CHECK(workerInitiallyEmpty && workerRestoredEmpty);
    FO4HDR_CHECK(workerToken.threadId == 47 && workerToken.serial != 0);
    FO4HDR_CHECK(CurrentArmorUiScope().serial == mainToken.serial);
}

void TestCompositionExitPolicy()
{
    using Exit = ArmorUiCompositionExit;
    FO4HDR_CHECK(DecideArmorUiCompositionExit({false, true, true, true, true, true}) == Exit::ignore);
    FO4HDR_CHECK(DecideArmorUiCompositionExit({true, false, true, true, true, true}) == Exit::ignore);
    FO4HDR_CHECK(DecideArmorUiCompositionExit({true, true, true, true, true, true}) == Exit::decode);
    FO4HDR_CHECK(DecideArmorUiCompositionExit({true, true, false, true, true, true}) == Exit::invalidate);
    FO4HDR_CHECK(DecideArmorUiCompositionExit({true, true, true, false, true, true}) == Exit::invalidate);
    FO4HDR_CHECK(DecideArmorUiCompositionExit({true, true, true, true, false, true}) == Exit::invalidate);
    FO4HDR_CHECK(DecideArmorUiCompositionExit({true, true, true, true, true, false}) == Exit::invalidate);
}

}  // namespace

void RunArmorUiCompositionTests()
{
    TestRequestLifecycle();
    TestScopesAndBoundaries();
    TestRejectedNestedScopeMasksOuterScope();
    TestDepthLimitAndThreadIsolation();
    TestCompositionExitPolicy();
    ResetState();
}

}  // namespace fo4hdr::tests
