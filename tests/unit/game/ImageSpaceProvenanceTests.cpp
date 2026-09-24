// SPDX-License-Identifier: GPL-3.0-or-later
#include "../UnitTestSuite.hpp"
#include "ImageSpaceObserver.hpp"
#include "TestHarness.hpp"
#include <thread>

namespace fo4hdr::tests {
namespace {
using namespace game::imagespace_detail;

Provenance Candidate(std::uint32_t form) noexcept
{
    Provenance result{};
    result.values.paths[0][0] = 'a';
    result.values.weights[0] = 1;
    result.identities[0].formId = form;
    result.validSlots = 1;
    result.generation = 7;
    result.observationRevision = 91;
    result.status = Status::matched;
    return result;
}

void VerifyThreadMigrationAndSeparateTransfer()
{
    Publication weather, transferred;
    const auto first = Candidate(11);
    std::thread producer([&] {
        auto token = weather.Begin();
        FO4HDR_CHECK(weather.Finish(token, first));
        FO4HDR_CHECK(token.revision == 0);
        FO4HDR_CHECK(!weather.Finish(token, first));
    });
    producer.join();
    std::thread manager([&] {
        Provenance candidate{};
        FO4HDR_CHECK(weather.TryRead(candidate));
        FO4HDR_CHECK(candidate.Matches(first.values, 7));
        FO4HDR_CHECK(!candidate.Matches(first.values, 8));
        candidate.manager = 123;
        auto token = transferred.Begin();
        FO4HDR_CHECK(transferred.Finish(token, candidate));
    });
    manager.join();
    std::thread nextProducer([&] {
        auto token = weather.Begin();
        FO4HDR_CHECK(weather.Finish(token, Candidate(22)));
    });
    nextProducer.join();
    std::thread renderer([&] {
        Provenance latest{}, rendered{};
        FO4HDR_CHECK(weather.TryRead(latest));
        FO4HDR_CHECK(transferred.TryRead(rendered));
        FO4HDR_CHECK(latest.identities[0].formId == 22);
        FO4HDR_CHECK(rendered.identities[0].formId == 11);
        FO4HDR_CHECK(rendered.manager == 123);
        FO4HDR_CHECK(rendered.observationRevision == 91);
        FO4HDR_CHECK(latest.values == rendered.values);
        auto token = transferred.Begin();
        FO4HDR_CHECK(transferred.Finish(token, latest));
        FO4HDR_CHECK(transferred.TryRead(rendered));
        FO4HDR_CHECK(rendered.identities[0].formId == 22);
        auto changed = first.values;
        changed.weights[0] = 0.5F;
        FO4HDR_CHECK(!rendered.Matches(changed, 7));
    });
    renderer.join();
}

void VerifyOverlapAndRecovery(bool finishFirstWriterFirst)
{
    Publication publication;
    Provenance result{};
    FO4HDR_CHECK(!publication.TryRead(result));
    FO4HDR_CHECK(result.status == Status::waiting);
    auto first = publication.Begin();
    auto second = publication.Begin();
    FO4HDR_CHECK(first.exclusive);
    FO4HDR_CHECK(!second.exclusive);
    FO4HDR_CHECK(!publication.TryRead(result));
    FO4HDR_CHECK(result.status == Status::updating);
    auto& earlierFinish = finishFirstWriterFirst ? first : second;
    auto& laterFinish = finishFirstWriterFirst ? second : first;
    FO4HDR_CHECK(!publication.Finish(earlierFinish, Candidate(11)));
    FO4HDR_CHECK(!publication.TryRead(result));
    FO4HDR_CHECK(result.status == Status::updating);
    FO4HDR_CHECK(!publication.Finish(laterFinish, Candidate(22)));
    FO4HDR_CHECK(!publication.TryRead(result));
    FO4HDR_CHECK(result.status == Status::waiting);
    auto recovery = publication.Begin();
    FO4HDR_CHECK(recovery.exclusive);
    FO4HDR_CHECK(publication.Finish(recovery, Candidate(33)));
    FO4HDR_CHECK(publication.TryRead(result));
    FO4HDR_CHECK(result.identities[0].formId == 33);
    FO4HDR_CHECK(publication.IsCurrent(result.revision));
}

void VerifyRoomGapAndInvalidation()
{
    Publication publication;
    auto initial = publication.Begin();
    FO4HDR_CHECK(publication.Finish(initial, Candidate(11)));
    Provenance previous{}, result{};
    FO4HDR_CHECK(publication.TryRead(previous));
    auto room = publication.Begin();
    FO4HDR_CHECK(!publication.IsCurrent(previous.revision));
    FO4HDR_CHECK(!publication.TryRead(result));
    FO4HDR_CHECK(result.status == Status::updating);
    publication.Invalidate();
    FO4HDR_CHECK(!publication.TryRead(result));
    FO4HDR_CHECK(result.status == Status::updating);
    FO4HDR_CHECK(!publication.Finish(room, Candidate(22)));
    FO4HDR_CHECK(room.revision == 0);
    FO4HDR_CHECK(!publication.TryRead(result));
    FO4HDR_CHECK(result.status == Status::waiting);
    auto recovered = publication.Begin();
    FO4HDR_CHECK(publication.Finish(recovered, Candidate(33)));
    FO4HDR_CHECK(publication.TryRead(result));
    const auto revision = result.revision;
    publication.Invalidate();
    FO4HDR_CHECK(!publication.IsCurrent(revision));
    FO4HDR_CHECK(!publication.TryRead(result));
    FO4HDR_CHECK(result.identities[0].formId == 0);
    auto unqualified = publication.Begin();
    Provenance rejected{};
    rejected.status = Status::unqualifiedSource;
    FO4HDR_CHECK(publication.Finish(unqualified, rejected));
    FO4HDR_CHECK(publication.TryRead(result));
    FO4HDR_CHECK(result.status == Status::unqualifiedSource);
    FO4HDR_CHECK(!result.Matches(Candidate(33).values, 7));
}
}

void RunImageSpaceProvenanceTests()
{
    VerifyThreadMigrationAndSeparateTransfer();
    VerifyOverlapAndRecovery(true);
    VerifyOverlapAndRecovery(false);
    VerifyRoomGapAndInvalidation();
}
}
