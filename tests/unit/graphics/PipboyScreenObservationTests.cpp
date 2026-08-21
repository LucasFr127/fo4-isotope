// SPDX-License-Identifier: GPL-3.0-or-later

#include "UnitTestSuite.hpp"
#include "PipboyScreenObservation.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cstddef>

namespace fo4hdr::tests {

void RunPipboyScreenObservationTests()
{
    FO4HDR_CHECK(IdentifyPipboyScreenShader(0xB7FC78BBu, 1840) == PipboyScreenShader::producer);
    FO4HDR_CHECK(IdentifyPipboyScreenShader(0x807CFBC4u, 3768) == PipboyScreenShader::consumer);
    FO4HDR_CHECK(IdentifyPipboyScreenShader(0x807CFBC4u, 1840) == PipboyScreenShader::unknown);
    const std::array<std::byte, 20> unrelated{};
    FO4HDR_CHECK(MatchPipboyScreenBytecode(unrelated, 0) == PipboyScreenShader::unknown);
    PipboyScreenObservation observation;
    observation.Producer(1);
    FO4HDR_CHECK(observation.producerDraws == 0);
    observation.Begin(0, 2);
    FO4HDR_CHECK(!observation.requested);
    observation.Begin(3, 2);
    FO4HDR_CHECK(observation.Matches(3, 2));
    FO4HDR_CHECK(!observation.Matches(4, 2));
    FO4HDR_CHECK(!observation.Matches(3, 4));
    observation.Consumer(10, false, false);
    FO4HDR_CHECK(observation.pendingConsumers == 1);
    observation.Producer(10);
    // Retained content can be read before the next frame refreshes it.
    observation.Consumer(11, true, true);
    FO4HDR_CHECK(observation.matchedConsumers == 1 && observation.lastProducerAge == 1);
    observation.Consumer(11, false, true);
    observation.Consumer(9, true, true);
    FO4HDR_CHECK(observation.unmatchedConsumers == 2);
    observation.Producer(0);
    observation.Stop();
    observation.Consumer(12, true, true);
    FO4HDR_CHECK(observation.consumerDraws == 4);
    FO4HDR_CHECK(!observation.Matches(3, 2));
    observation.Begin(4, 5);
    FO4HDR_CHECK(observation.producerDraws == 0 && observation.matchedConsumers == 0);
    observation.Consumer(1, false, false);
    FO4HDR_CHECK(observation.pendingConsumers == 1);
}

}  // namespace fo4hdr::tests