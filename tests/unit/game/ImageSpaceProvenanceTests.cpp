// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#include "../UnitTestSuite.hpp"
#include "ImageSpaceObserver.hpp"
#include "TestHarness.hpp"
#include <cmath>
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
    result.weatherGrades[0] = {
        .features = {0.1, 0.2, 0.3, -0.4},
        .referenceCount = form,
        .validReferenceCount = form,
        .availability = WeatherGradeAvailability::complete};
    result.weatherGradeValidSlots = 1;
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
        FO4HDR_CHECK(rendered.weatherGradeValidSlots == 1);
        FO4HDR_CHECK(rendered.weatherGrades[0] == first.weatherGrades[0]);
        FO4HDR_CHECK(rendered.manager == 123);
        FO4HDR_CHECK(rendered.observationRevision == 91);
        FO4HDR_CHECK(latest.values == rendered.values);
        auto token = transferred.Begin();
        FO4HDR_CHECK(transferred.Finish(token, latest));
        FO4HDR_CHECK(transferred.TryRead(rendered));
        FO4HDR_CHECK(rendered.identities[0].formId == 22);
        FO4HDR_CHECK(rendered.weatherGrades[0].referenceCount == 22);
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
    FO4HDR_CHECK(result.weatherGradeValidSlots == 0);
    FO4HDR_CHECK(result.weatherGrades[0].referenceCount == 0);
    auto unqualified = publication.Begin();
    Provenance rejected{};
    rejected.status = Status::unqualifiedSource;
    FO4HDR_CHECK(publication.Finish(unqualified, rejected));
    FO4HDR_CHECK(publication.TryRead(result));
    FO4HDR_CHECK(result.status == Status::unqualifiedSource);
    FO4HDR_CHECK(!result.Matches(Candidate(33).values, 7));
}

void VerifyWeatherColorFeatures()
{
    constexpr std::array<std::uint8_t, 8> ring{4, 0, 5, 1, 6, 2, 7, 3};
    std::array<WeatherGradeSlotColors, 8> colors{};
    WeatherGradeFeatures features{};
    FO4HDR_CHECK(DecodeWeatherGradeFeatures(colors, ring, 0, features));
    FO4HDR_CHECK(features == WeatherGradeFeatures{});
    auto whiteColors = colors;
    for (auto& slot : whiteColors)
    {
        slot.sunlight = {255, 255, 255};
        slot.glare = {255, 255, 255};
        slot.diffuse.fill({255, 255, 255});
    }
    FO4HDR_CHECK(DecodeWeatherGradeFeatures(whiteColors, ring, 0, features));
    FO4HDR_CHECK(features == (WeatherGradeFeatures{1.0, 1.0, 1.0, 0.0}));
    colors[0].sunlight = {255, 0, 0};
    colors[0].glare = {0, 255, 0};
    colors[0].diffuse[0] = {0, 0, 255};
    FO4HDR_CHECK(DecodeWeatherGradeFeatures(colors, ring, 0, features));
    FO4HDR_CHECK(std::abs(features.sunlight - 0.2125) < 1e-12);
    FO4HDR_CHECK(std::abs(features.glare - 0.7154) < 1e-12);
    FO4HDR_CHECK(std::abs(features.ambient - 0.0721 / 6.0) < 1e-12);
    FO4HDR_CHECK(std::abs(features.colorChange - 255.0 / 256.0) < 1e-12);
    colors[4].sunlight = {0, 0, 255};
    colors[5].sunlight = {255, 255, 255};
    FO4HDR_CHECK(DecodeWeatherGradeFeatures(colors, ring, 0, features));
    FO4HDR_CHECK(std::abs(features.colorChange - 1.5 * 255.0 / 256.0) < 1e-12);

    double sum{};
    for (std::size_t slot = 0; slot < colors.size(); ++slot)
    {
        FO4HDR_CHECK(DecodeWeatherGradeFeatures(colors, ring, slot, features));
        sum += features.colorChange;
    }
    FO4HDR_CHECK(std::abs(sum) < 1e-12);
    auto invalidRing = ring;
    invalidRing[7] = invalidRing[0];
    FO4HDR_CHECK(!DecodeWeatherGradeFeatures(colors, invalidRing, 0, features));
    invalidRing[7] = 8;
    FO4HDR_CHECK(!DecodeWeatherGradeFeatures(colors, invalidRing, 0, features));
    FO4HDR_CHECK(!DecodeWeatherGradeFeatures(colors, ring, 8, features));
}

void VerifyFixedWeatherAggregation()
{
    WeatherGradeAggregate aggregate{};
    FinalizeWeatherGradeAggregate(aggregate);
    FO4HDR_CHECK(aggregate.availability == WeatherGradeAvailability::unavailable);
    const WeatherGradeFeatures first{0.1, 0.2, 0.3, -1.0};
    const WeatherGradeFeatures second{0.7, 0.6, 0.5, 1.0};
    AccumulateWeatherGradeReference(aggregate, &first);
    AccumulateWeatherGradeReference(aggregate, &second);
    FinalizeWeatherGradeAggregate(aggregate);
    FO4HDR_CHECK(aggregate.referenceCount == 2);
    FO4HDR_CHECK(aggregate.validReferenceCount == 2);
    FO4HDR_CHECK(aggregate.availability == WeatherGradeAvailability::complete);
    FO4HDR_CHECK(std::abs(aggregate.features.sunlight - 0.4) < 1e-12);
    FO4HDR_CHECK(std::abs(aggregate.features.glare - 0.4) < 1e-12);
    FO4HDR_CHECK(std::abs(aggregate.features.ambient - 0.4) < 1e-12);
    FO4HDR_CHECK(aggregate.features.colorChange == 0);

    aggregate = {};
    AccumulateWeatherGradeReference(aggregate, &first);
    AccumulateWeatherGradeReference(aggregate, nullptr);
    FinalizeWeatherGradeAggregate(aggregate);
    FO4HDR_CHECK(aggregate.referenceCount == 2);
    FO4HDR_CHECK(aggregate.validReferenceCount == 1);
    FO4HDR_CHECK(aggregate.availability == WeatherGradeAvailability::incomplete);

    aggregate = {};
    const WeatherGradeFeatures white{1.0, 1.0, 1.0, 0.0};
    for (std::size_t reference = 0; reference < 8192U * 8U; ++reference)
        AccumulateWeatherGradeReference(aggregate, &white);
    FinalizeWeatherGradeAggregate(aggregate);
    FO4HDR_CHECK(aggregate.availability == WeatherGradeAvailability::complete);
    FO4HDR_CHECK(aggregate.features == white);
}
}

void RunImageSpaceProvenanceTests()
{
    VerifyThreadMigrationAndSeparateTransfer();
    VerifyOverlapAndRecovery(true);
    VerifyOverlapAndRecovery(false);
    VerifyRoomGapAndInvalidation();
    VerifyWeatherColorFeatures();
    VerifyFixedWeatherAggregation();
}
}
