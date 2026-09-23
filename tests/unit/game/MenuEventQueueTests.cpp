// SPDX-License-Identifier: GPL-3.0-or-later
#include "MenuEventQueue.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

namespace fo4hdr::tests {
void RunMenuEventQueueTests()
{
    MenuEventQueue<2> queue;
    const auto menu = ClassifyMenu("PauseMenu");
    FO4HDR_CHECK(menu == MenuCode::Pause);
    FO4HDR_CHECK(ClassifyMenu("authored unknown") == MenuCode::Unknown);
    FO4HDR_CHECK(queue.Push(static_cast<std::uint8_t>(menu), true, 7, MenuContextBit(menu)) == 1);
    FO4HDR_CHECK(queue.CurrentOpenMask() == MenuContextBit(menu));
    MenuEventRecord record{};
    std::uint64_t dropped{};
    FO4HDR_CHECK(queue.TryPop(record, dropped));
    FO4HDR_CHECK(record.sequence == 1 && record.opening && record.sourceThread == 7);
    FO4HDR_CHECK(record.code == static_cast<std::uint8_t>(menu) && dropped == 0);
    FO4HDR_CHECK(!queue.TryPop(record, dropped));
    FO4HDR_CHECK(queue.Push(static_cast<std::uint8_t>(menu), false, 7, MenuContextBit(menu)) == 2);
    FO4HDR_CHECK(queue.CurrentOpenMask() == 0);
    FO4HDR_CHECK(queue.TryPop(record, dropped));
    FO4HDR_CHECK(record.sequence == 2 && !record.opening);
}
} // namespace fo4hdr::tests
