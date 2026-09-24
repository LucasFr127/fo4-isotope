// SPDX-License-Identifier: GPL-3.0-or-later

#include "../UnitTestSuite.hpp"
#include "PipboyScreenObservation.hpp"
#include "TestHarness.hpp"

namespace fo4hdr::tests {

void TestPipboyScreenObservation()
{
    FO4HDR_CHECK(IdentifyPipboyScreenShader(0xB7FC78BBu, 1840) == PipboyScreenShader::producer);
    FO4HDR_CHECK(IdentifyPipboyScreenShader(0x807CFBC4u, 3768) == PipboyScreenShader::consumer);
    FO4HDR_CHECK(IdentifyPipboyScreenShader(0x807CFBC4u, 1840) == PipboyScreenShader::unknown);
    FO4HDR_CHECK(IdentifyPipboyScreenShader(0u, 3768) == PipboyScreenShader::unknown);

    PipboyScreenObservation observation;
    observation.Producer(1);
    observation.Consumer(1, true, false);
    FO4HDR_CHECK(observation.producerDraws == 0);
    FO4HDR_CHECK(observation.consumerDraws == 0);
    observation.Begin(0, 2);
    FO4HDR_CHECK(!observation.requested);
    observation.Begin(3, 0);
    FO4HDR_CHECK(!observation.requested);
    observation.Begin(3, 2);
    FO4HDR_CHECK(observation.Matches(3, 2));
    FO4HDR_CHECK(!observation.Matches(4, 2));
    FO4HDR_CHECK(!observation.Matches(3, 4));

    // The capture reads retained screen content before refreshing it. The
    // first read is pending; ordinary Present must not erase the next match.
    observation.Consumer(10, false, false);
    FO4HDR_CHECK(observation.pendingConsumers == 1);
    FO4HDR_CHECK(observation.unmatchedConsumers == 0);
    observation.Producer(10);
    observation.Consumer(11, true, true);
    FO4HDR_CHECK(observation.matchedConsumers == 1);
    FO4HDR_CHECK(observation.lastProducerAge == 1);
    FO4HDR_CHECK(observation.lastMatchedFrame == 11);
    observation.Consumer(11, false, true);
    FO4HDR_CHECK(observation.unmatchedConsumers == 1);
    FO4HDR_CHECK(observation.matchedConsumers == 1);
    observation.Producer(11);
    observation.Consumer(11, true, true);
    FO4HDR_CHECK(observation.lastProducerAge == 0);
    FO4HDR_CHECK(observation.matchedConsumers == 2);

    // Retention age is evidence, not a guessed timeout or permission to alter
    // content. Reversed frame order can never establish producer lineage.
    observation.Consumer(20, true, true);
    FO4HDR_CHECK(observation.lastProducerAge == 9);
    FO4HDR_CHECK(observation.matchedConsumers == 3);
    observation.Consumer(9, true, true);
    FO4HDR_CHECK(observation.unmatchedConsumers == 2);
    FO4HDR_CHECK(observation.matchedConsumers == 3);
    observation.Producer(0);
    observation.Consumer(0, true, true);
    FO4HDR_CHECK(observation.producerDraws == 2);
    FO4HDR_CHECK(observation.consumerDraws == 6);

    observation.Stop();
    observation.Stop();
    FO4HDR_CHECK(!observation.Matches(3, 2));
    observation.Producer(21);
    observation.Consumer(21, true, true);
    FO4HDR_CHECK(observation.producerDraws == 2);
    FO4HDR_CHECK(observation.consumerDraws == 6);
    observation.Begin(4, 5);
    FO4HDR_CHECK(observation.producerDraws == 0);
    FO4HDR_CHECK(observation.lastProducerFrame == 0);
    observation.Consumer(1, false, false);
    FO4HDR_CHECK(observation.matchedConsumers == 0);
    FO4HDR_CHECK(observation.pendingConsumers == 1);
    observation.Producer(1);
    observation.Consumer(2, false, false);
    FO4HDR_CHECK(observation.pendingConsumers == 2);
    FO4HDR_CHECK(observation.unmatchedConsumers == 0);
}

}  // namespace fo4hdr::tests
