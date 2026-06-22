// SPDX-License-Identifier: GPL-3.0-or-later

#include "RendererLifetime.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

namespace {

void TestLifetimePreparation()
{
    fo4hdr::RendererLifetime lifetime;
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 0);
    FO4HDR_CHECK(lifetime.BeginResize(0) == fo4hdr::RendererLifetime::terminalEpoch);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 0);
    FO4HDR_CHECK(!lifetime.Prepare(0, 64));
    FO4HDR_CHECK(!lifetime.Prepare(33, 64));
    FO4HDR_CHECK(!lifetime.Prepare(32, 63));
    FO4HDR_CHECK(!lifetime.Prepare(64, 32));
    FO4HDR_CHECK(!lifetime.Prepare(32, 32));
    FO4HDR_CHECK(!lifetime.Prepare(32, fo4hdr::RendererLifetime::terminalEpoch + 1));
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 0);

    const auto prepared = lifetime.Prepare(32, 64);
    FO4HDR_CHECK(prepared);
    if (!prepared)
    {
        return;
    }
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 32);
    FO4HDR_CHECK(!lifetime.Prepare(64, 96));
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 32);
}

void TestStaleResizeCompletion()
{
    fo4hdr::RendererLifetime lifetime;
    const auto prepared = lifetime.Prepare(32, 64);
    FO4HDR_CHECK(prepared);
    if (!prepared)
    {
        return;
    }
    lifetime.PublishInitial();
    const auto initial = lifetime.CurrentEpoch();
    FO4HDR_CHECK(initial == 33);
    const auto first = lifetime.BeginResize(initial);
    FO4HDR_CHECK(first == 34);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 34);

    // A later resize may begin while the first native call is still pending.
    const auto second = lifetime.BeginResize(lifetime.CurrentEpoch());
    FO4HDR_CHECK(second == 36);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 36);
    FO4HDR_CHECK(lifetime.BeginResize(initial) == fo4hdr::RendererLifetime::terminalEpoch);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 36);
    lifetime.CompleteResize(first);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 36);
    lifetime.CompleteResize(second);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 37);
    lifetime.CompleteResize(first);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == 37);
}

void TestEpochExhaustion()
{
    fo4hdr::RendererLifetime lifetime;
    constexpr auto terminal = fo4hdr::RendererLifetime::terminalEpoch;
    const auto prepared = lifetime.Prepare(terminal - 4, terminal);
    FO4HDR_CHECK(prepared);
    if (!prepared)
    {
        return;
    }
    lifetime.PublishInitial();
    FO4HDR_CHECK(lifetime.CurrentEpoch() == terminal - 3);
    const auto transition = lifetime.BeginResize(lifetime.CurrentEpoch());
    FO4HDR_CHECK(transition == terminal - 2);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == terminal - 2);
    lifetime.CompleteResize(transition);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == terminal - 1);
    FO4HDR_CHECK(lifetime.BeginResize(lifetime.CurrentEpoch()) == terminal);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == terminal);
    lifetime.CompleteResize(transition);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == terminal);
}

void TestTerminalRetirement()
{
    fo4hdr::RendererLifetime lifetime;
    const auto prepared = lifetime.Prepare(32, 64);
    FO4HDR_CHECK(prepared);
    if (!prepared)
    {
        return;
    }
    lifetime.PublishInitial();
    const auto transition = lifetime.BeginResize(lifetime.CurrentEpoch());
    FO4HDR_CHECK(transition == 34);
    lifetime.Retire();
    constexpr auto terminal = fo4hdr::RendererLifetime::terminalEpoch;
    FO4HDR_CHECK(lifetime.CurrentEpoch() == terminal);
    lifetime.CompleteResize(transition);
    FO4HDR_CHECK(lifetime.CurrentEpoch() == terminal);
    FO4HDR_CHECK(lifetime.BeginResize(lifetime.CurrentEpoch()) == terminal);
    FO4HDR_CHECK(!lifetime.Prepare(64, 96));
    lifetime.Retire();
    FO4HDR_CHECK(lifetime.CurrentEpoch() == terminal);
}

}  // namespace

namespace fo4hdr::tests {

void RunPipelineContractTests()
{
    TestLifetimePreparation();
    TestStaleResizeCompletion();
    TestEpochExhaustion();
    TestTerminalRetirement();
}

}  // namespace fo4hdr::tests
