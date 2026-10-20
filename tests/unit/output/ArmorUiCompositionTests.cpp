// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "../UnitTestSuite.hpp"

#include "FO4Isotope/output/ArmorUiComposition.hpp"
#include "ArmorUiObserver.hpp"
#include "DirectCallPatch.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
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
    SetArmorUiObservationEnabled(false);
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

void TestObservationRequestLifecycle()
{
    ResetState();
    const auto disabledGeneration = ArmorUiScopeGeneration();
    SetArmorUiObservationEnabled(false);
    FO4HDR_CHECK(ArmorUiScopeGeneration() == disabledGeneration);
    FO4HDR_CHECK(!ArmorUiObservationEnabled());
    SetArmorUiObservationEnabled(true);
    const auto observationGeneration = ArmorUiScopeGeneration();
    FO4HDR_CHECK(observationGeneration != disabledGeneration);
    FO4HDR_CHECK(ArmorUiScopeEnabled() && !ArmorUiCorrectionEnabled());
    FO4HDR_CHECK(ArmorUiObservationEnabled());
    SetArmorUiObservationEnabled(true);
    FO4HDR_CHECK(ArmorUiScopeGeneration() == observationGeneration);
    SetArmorUiCorrectionEnabled(true);
    SetArmorUiObservationEnabled(false);
    FO4HDR_CHECK(ArmorUiScopeEnabled() && ArmorUiCorrectionEnabled());
    FO4HDR_CHECK(!ArmorUiObservationEnabled());
    SetArmorUiObservationEnabled(true);
    SetArmorUiCorrectionEnabled(false);
    ResetArmorUiScope();
    FO4HDR_CHECK(ArmorUiScopeEnabled() && !ArmorUiCorrectionEnabled());
    FO4HDR_CHECK(ArmorUiObservationEnabled());
    SetArmorUiObservationEnabled(false);
    FO4HDR_CHECK(!ArmorUiScopeEnabled() && !ArmorUiCorrectionEnabled());
}

void TestNativeRendererScopeIdentity(ArmorUiOwner owner)
{
    ResetState();
    SetArmorUiCorrectionEnabled(true);
    SetArmorUiBoundaryCallback(&RecordBoundary);
    {
        ArmorUiScope scope{{.owner = owner, .threadId = 17,
            .targetArgument = 29, .postAa = true, .hasRoot = true}};
        FO4HDR_CHECK(scope.Entered());
        const auto token = CurrentArmorUiScope();
        FO4HDR_CHECK(token.owner == owner);
        FO4HDR_CHECK(token.postAa && token.hasRoot && token.depth == 1);
        FO4HDR_CHECK(token.threadId == 17 && token.targetArgument == 29);
    }
    FO4HDR_CHECK(CurrentArmorUiScope().serial == 0);
    FO4HDR_CHECK(g_boundaryCount == 2);
    FO4HDR_CHECK(g_boundaryEntering[0] && !g_boundaryEntering[1]);
    FO4HDR_CHECK(g_boundaryTokens[0].owner == owner);
    FO4HDR_CHECK(g_boundaryTokens[0].serial == g_boundaryTokens[1].serial);
}

void TestNativeRendererCodeFingerprint()
{
    std::array<std::uint8_t, 131> memory{};
    for (std::size_t i = 0; i < memory.size(); ++i) memory[i] = static_cast<std::uint8_t>(i);
    const auto expected = memory;
    const auto address = reinterpret_cast<std::uintptr_t>(memory.data());
    std::size_t reads{};
    const auto read = [&](const void* source, void* destination, std::size_t size) noexcept {
        ++reads;
        std::memcpy(destination, source, size);
        return true;
    };
    for (const auto size : {39U, 130U}) {
        const std::span<const std::uint8_t> fingerprint{expected.data(), size};
        FO4HDR_CHECK(game::direct_call_patch::MatchesNativeCode(address, fingerprint, read));
        memory[size - 1] ^= 1;
        FO4HDR_CHECK(!game::direct_call_patch::MatchesNativeCode(address, fingerprint, read));
        memory[size - 1] ^= 1;
    }
    FO4HDR_CHECK(!game::direct_call_patch::MatchesNativeCode(address, expected, read));
    FO4HDR_CHECK(reads == 4);
    const auto unavailable = [](const void*, void*, std::size_t) noexcept { return false; };
    const std::array<std::uint8_t, 39> unreadable{};
    FO4HDR_CHECK(!game::direct_call_patch::MatchesNativeCode(address, unreadable, unavailable));
}

void TestNativeRendererPooledKey(const char* text)
{
    struct PooledString { const char* entry{}; };
    std::array<char, 32> duplicateText{};
    std::copy_n(text, std::string_view(text).size() + 1, duplicateText.begin());
    const PooledString canonical{text};
    const PooledString exactAlias{text};
    const PooledString sameText{duplicateText.data()};
    FO4HDR_CHECK(game::detail::SameNativeRendererKey(exactAlias, canonical));
    FO4HDR_CHECK(!game::detail::SameNativeRendererKey(sameText, canonical));
    FO4HDR_CHECK(!game::detail::SameNativeRendererKey(PooledString{}, canonical));
    FO4HDR_CHECK(!game::detail::SameNativeRendererKey(canonical, PooledString{}));
    FO4HDR_CHECK(!game::detail::SameNativeRendererKey(PooledString{}, PooledString{}));
    constexpr std::array otherKeys{"HUDScreenModel", "FlatScreenModel",
        "PowerArmorRenderer", "HUDRainRenderer", "WorkshopMenu3D"};
    for (const auto* other : otherKeys)
        if (std::string_view(other) != text)
            FO4HDR_CHECK(!game::detail::SameNativeRendererKey(PooledString{other}, canonical));
}

void TestObservationOnlyScopesAndNames()
{
    ResetState();
    SetArmorUiObservationEnabled(true);
    SetArmorUiBoundaryCallback(&RecordBoundary);
    ArmorUiScopeEntry outerEntry{.owner = ArmorUiOwner::powerArmor, .threadId = 17,
        .targetArgument = 29, .postAa = true, .hasRoot = true};
    std::copy_n("PowerArmorRenderer", sizeof("PowerArmorRenderer"), outerEntry.rendererName.begin());
    ArmorUiScopeToken outerToken{};
    {
        ArmorUiScope outer{outerEntry};
        FO4HDR_CHECK(outer.Entered() && !ArmorUiCorrectionEnabled());
        outerToken = CurrentArmorUiScope();
        FO4HDR_CHECK(std::string_view(outerToken.rendererName.data()) == "PowerArmorRenderer");
        outerEntry.rendererName.fill('z');
        ResetArmorUiScope();
        FO4HDR_CHECK(outerToken.requestGeneration != ArmorUiScopeGeneration());
        {
            ArmorUiScopeEntry nestedEntry{.owner = ArmorUiOwner::other, .threadId = 17,
                .targetArgument = 31, .postAa = false, .hasRoot = true};
            nestedEntry.rendererName.fill('x');
            ArmorUiScope nested{nestedEntry};
            FO4HDR_CHECK(nested.Entered() && !ArmorUiCorrectionEnabled());
            const auto nestedToken = CurrentArmorUiScope();
            FO4HDR_CHECK(nestedToken.owner == ArmorUiOwner::other && nestedToken.depth == 2);
            FO4HDR_CHECK(nestedToken.requestGeneration == ArmorUiScopeGeneration());
            FO4HDR_CHECK(nestedToken.serial != outerToken.serial);
            FO4HDR_CHECK(nestedToken.rendererName.front() == 'x');
            FO4HDR_CHECK(nestedToken.rendererName.back() == '\0');
            FO4HDR_CHECK(std::string_view(nestedToken.rendererName.data()).size() == 31);
        }
        FO4HDR_CHECK(CurrentArmorUiScope().serial == outerToken.serial);
        FO4HDR_CHECK(CurrentArmorUiScope().rendererName == outerToken.rendererName);
        SetArmorUiObservationEnabled(false);
        SetArmorUiBoundaryCallback(nullptr);
    }
    FO4HDR_CHECK(CurrentArmorUiScope().serial == 0);
    FO4HDR_CHECK(!ArmorUiScopeEnabled() && !ArmorUiCorrectionEnabled());
    FO4HDR_CHECK(g_boundaryCount == 4);
    FO4HDR_CHECK(g_boundaryEntering[0] && g_boundaryEntering[1]);
    FO4HDR_CHECK(!g_boundaryEntering[2] && !g_boundaryEntering[3]);
    FO4HDR_CHECK(g_boundaryTokens[0].serial == g_boundaryTokens[3].serial);
    FO4HDR_CHECK(g_boundaryTokens[0].rendererName == g_boundaryTokens[3].rendererName);
    FO4HDR_CHECK(g_boundaryTokens[1].serial == g_boundaryTokens[2].serial);
    FO4HDR_CHECK(g_boundaryTokens[1].rendererName == g_boundaryTokens[2].rendererName);
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
    TestObservationRequestLifecycle();
    TestNativeRendererScopeIdentity(ArmorUiOwner::hud);
    TestNativeRendererScopeIdentity(ArmorUiOwner::flatMenu);
    TestNativeRendererScopeIdentity(ArmorUiOwner::powerArmor);
    TestNativeRendererScopeIdentity(ArmorUiOwner::powerArmorRain);
    TestNativeRendererScopeIdentity(ArmorUiOwner::workshop);
    TestNativeRendererCodeFingerprint();
    TestNativeRendererPooledKey("HUDScreenModel");
    TestNativeRendererPooledKey("FlatScreenModel");
    TestNativeRendererPooledKey("PowerArmorRenderer");
    TestNativeRendererPooledKey("HUDRainRenderer");
    TestNativeRendererPooledKey("WorkshopMenu3D");
    TestObservationOnlyScopesAndNames();
    TestRejectedNestedScopeMasksOuterScope();
    TestDepthLimitAndThreadIsolation();
    TestCompositionExitPolicy();
    ResetState();
}

}  // namespace fo4hdr::tests
