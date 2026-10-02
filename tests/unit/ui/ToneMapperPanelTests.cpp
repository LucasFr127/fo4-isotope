// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "../UnitTestSuite.hpp"
#include "FO4Isotope/config/HdrSettings.hpp"
#include "ToneMapperPanel.hpp"
#include "ToneMapperRecommendations.hpp"
#include "TestHarness.hpp"
#include <imgui.h>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace fo4hdr::tests {

namespace {

void TestToneMapperRecommendationStatus()
{
    const ToneMapperRecommendedRange range{true, 0.2, 0.8, 1000.0};
    FO4HDR_CHECK(ToneMapperRecommendationStatus(0.2, range) == nullptr);
    FO4HDR_CHECK(ToneMapperRecommendationStatus(0.5, range) == nullptr);
    FO4HDR_CHECK(ToneMapperRecommendationStatus(0.8, range) == nullptr);
    FO4HDR_CHECK(std::string_view{ToneMapperRecommendationStatus(0.19, range)}
        .starts_with("Outside recommended range"));
    FO4HDR_CHECK(std::string_view{ToneMapperRecommendationStatus(0.81, range)}
        .starts_with("Outside recommended range"));
    FO4HDR_CHECK(std::string_view{ToneMapperRecommendationStatus(0.5, {})}
        .starts_with("Recommended range unavailable"));
    FO4HDR_CHECK(std::string_view{ToneMapperRecommendationStatus(
        std::numeric_limits<double>::quiet_NaN(), range)}.starts_with("Control value is invalid"));
}

void TestToneMapperRecommendationFloatEndpoints()
{
    for (const auto range : {ToneMapperRecommendedRange{true, 0.25, 0.8, 1000.0},
             ToneMapperRecommendedRange{true, 0.7, 0.8, 1000.0}})
    {
        FO4HDR_CHECK(ToneMapperRecommendationStatus(range.minimum, range) == nullptr);
        FO4HDR_CHECK(ToneMapperRecommendationStatus(range.maximum, range) == nullptr);
        FO4HDR_CHECK(ToneMapperRecommendationStatus(
            static_cast<double>(static_cast<float>(range.minimum)), range) == nullptr);
        FO4HDR_CHECK(ToneMapperRecommendationStatus(
            static_cast<double>(static_cast<float>(range.maximum)), range) == nullptr);
        FO4HDR_CHECK(std::string_view{ToneMapperRecommendationStatus(range.minimum - 0.001, range)}
            .starts_with("Outside recommended range"));
        FO4HDR_CHECK(std::string_view{ToneMapperRecommendationStatus(range.maximum + 0.001, range)}
            .starts_with("Outside recommended range"));
    }
}

void TestToneMapperRecommendedPeaks()
{
    constexpr std::array peaks{400.0, 430.0, 600.0, 800.0, 1000.0, 1200.0, 1500.0, 2000.0};
    const auto rangeAt = [](double peak) {
        return GetToneMapperRecommendedRange(ProductionToneMapperAlgorithm::gt7,
            peak, &ToneMapperParameters::gt7ToeStrength);
    };
    for (const auto peak : peaks)
    {
        const auto range = rangeAt(peak);
        FO4HDR_CHECK(range.available);
        FO4HDR_CHECK(range.testedPeakNits == peak);
    }
    for (std::size_t index = 1; index < peaks.size(); ++index)
    {
        const auto midpoint = (peaks[index - 1] + peaks[index]) / 2.0;
        FO4HDR_CHECK(rangeAt(midpoint).testedPeakNits == peaks[index - 1]);
        FO4HDR_CHECK(rangeAt(std::nextafter(midpoint, 0.0)).testedPeakNits == peaks[index - 1]);
        FO4HDR_CHECK(rangeAt(std::nextafter(midpoint,
            std::numeric_limits<double>::infinity())).testedPeakNits == peaks[index]);
    }
    for (const auto invalid : {399.0, 2001.0,
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()})
        FO4HDR_CHECK(!rangeAt(invalid).available);
    FO4HDR_CHECK(!GetToneMapperRecommendedRange(ProductionToneMapperAlgorithm::gt7,
        1000.0, nullptr).available);
    FO4HDR_CHECK(!GetToneMapperRecommendedRange(ProductionToneMapperAlgorithm::agX,
        1000.0, &ToneMapperParameters::gt7ToeStrength).available);
    FO4HDR_CHECK(!GetToneMapperRecommendedRange(
        static_cast<ProductionToneMapperAlgorithm>(255),
        1000.0, &ToneMapperParameters::gt7ToeStrength).available);
}

void TestToneMapperRecommendedIntervals()
{
    const auto acesAt = [](double peak) {
        return GetToneMapperRecommendedRange(ProductionToneMapperAlgorithm::aces2,
            peak, &ToneMapperParameters::acesLookStrength);
    };
    const auto lowPeak = acesAt(1200.0);
    FO4HDR_CHECK(lowPeak.minimum == 0.0 && lowPeak.maximum == 1.0);
    const auto peak1500 = acesAt(1500.0);
    FO4HDR_CHECK(peak1500.minimum == 0.98 && peak1500.maximum == 1.0);
    FO4HDR_CHECK(ToneMapperRecommendationStatus(0.98, peak1500) == nullptr);
    FO4HDR_CHECK(ToneMapperRecommendationStatus(0.97, peak1500) != nullptr);
    const auto peak2000 = acesAt(2000.0);
    FO4HDR_CHECK(peak2000.minimum == 0.99 && peak2000.maximum == 1.0);
    FO4HDR_CHECK(ToneMapperRecommendationStatus(0.99, peak2000) == nullptr);
    FO4HDR_CHECK(ToneMapperRecommendationStatus(0.98, peak2000) != nullptr);
    FO4HDR_CHECK(acesAt(1350.0).minimum == 0.0);
    FO4HDR_CHECK(acesAt(std::nextafter(1350.0, 2000.0)).minimum == 0.98);
    const auto agx = GetToneMapperRecommendedRange(ProductionToneMapperAlgorithm::agX,
        2000.0, &ToneMapperParameters::agxContrast);
    FO4HDR_CHECK(agx.minimum == 2.2 && agx.maximum == 3.0);
    const auto gt7 = GetToneMapperRecommendedRange(ProductionToneMapperAlgorithm::gt7,
        2000.0, &ToneMapperParameters::gt7ShoulderStart);
    FO4HDR_CHECK(gt7.minimum == 0.35 && gt7.maximum == 0.6);
}

}  // namespace

void TestToneMapperPanelSmoke()
{
    TestToneMapperRecommendationStatus();
    TestToneMapperRecommendationFloatEndpoints();
    TestToneMapperRecommendedPeaks();
    TestToneMapperRecommendedIntervals();
    const auto render = [](ProductionToneMapperAlgorithm algorithm,
                            ImVec2 windowSize,
                            float scale)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1920.0F, 1400.0F);
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        FO4HDR_CHECK(pixels != nullptr);
        FO4HDR_CHECK(width > 0 && height > 0);
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleMain = scale;

        HdrSettings settings{};
        settings.productionAlgorithm = algorithm;
        settings.toneMapperParameters = {
            .acesLookStrength = 0.4,
            .agxMiddleGrayEv = 0.25,
            .agxContrast = 3.0,
            .agxHdrPurity = 0.55,
            .agxHdrShoulderPower = 2.1,
            .gt7ToeStrength = 1.5,
            .gt7ShoulderStart = 0.5,
            .gt7ColorBlend = 0.2};
        const auto before = settings.toneMapperParameters;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
        if (ImGui::Begin("Tone mapper parameters smoke", nullptr,
                ImGuiWindowFlags_NoSavedSettings))
        {
            const auto verticesBefore =
                ImGui::GetWindowDrawList()->VtxBuffer.Size;
            FO4HDR_CHECK(!DrawToneMapperPanel(settings));
            FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size >
                verticesBefore);
        }
        ImGui::End();
        ImGui::Render();

        const auto* drawData = ImGui::GetDrawData();
        FO4HDR_CHECK(drawData != nullptr);
        FO4HDR_CHECK(drawData->CmdListsCount > 0);
        FO4HDR_CHECK(drawData->TotalVtxCount > 0);
        FO4HDR_CHECK(drawData->TotalIdxCount > 0);
        FO4HDR_CHECK(settings.toneMapperParameters == before);
        ImGui::DestroyContext();
    };

    for (const auto algorithm : {
             ProductionToneMapperAlgorithm::aces2,
             ProductionToneMapperAlgorithm::agX,
             ProductionToneMapperAlgorithm::gt7})
    {
        render(algorithm, ImVec2(360.0F, 1000.0F), 1.0F);
        render(algorithm, ImVec2(900.0F, 1300.0F), 2.0F);
    }
}

void TestVanillaGradePanelSmoke()
{
    const auto render = [](ImVec2 windowSize, float scale)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1920.0F, 1400.0F);
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        FO4HDR_CHECK(pixels != nullptr);
        FO4HDR_CHECK(width > 0 && height > 0);
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleMain = scale;

        HdrSettings settings{};
        settings.vanillaGradeLuminancePercent = 65.0;
        settings.vanillaGradeColorPercent = 35.0;
        const auto luminanceBefore = settings.vanillaGradeLuminancePercent;
        const auto colorBefore = settings.vanillaGradeColorPercent;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
        if (ImGui::Begin("Vanilla grade controls smoke", nullptr,
                ImGuiWindowFlags_NoSavedSettings))
        {
            const auto verticesBefore =
                ImGui::GetWindowDrawList()->VtxBuffer.Size;
            FO4HDR_CHECK(!DrawVanillaGradePanel(settings));
            FO4HDR_CHECK(ImGui::GetWindowDrawList()->VtxBuffer.Size >
                verticesBefore);
        }
        ImGui::End();
        ImGui::Render();

        const auto* drawData = ImGui::GetDrawData();
        FO4HDR_CHECK(drawData != nullptr);
        FO4HDR_CHECK(drawData->CmdListsCount > 0);
        FO4HDR_CHECK(drawData->TotalVtxCount > 0);
        FO4HDR_CHECK(drawData->TotalIdxCount > 0);
        FO4HDR_CHECK(
            settings.vanillaGradeLuminancePercent == luminanceBefore);
        FO4HDR_CHECK(settings.vanillaGradeColorPercent == colorBefore);
        ImGui::DestroyContext();
    };

    render(ImVec2(360.0F, 1000.0F), 1.0F);
    render(ImVec2(900.0F, 1300.0F), 2.0F);
}

}  // namespace fo4hdr::tests
