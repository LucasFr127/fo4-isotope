// SPDX-License-Identifier: GPL-3.0-or-later

#include "RendererLifetime.hpp"
#include "RenoDxShaderIdentity.hpp"
#include "Fallout4HDR/output/RenderPipelineContract.hpp"
#include "TestHarness.hpp"
#include "UnitTestSuite.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

void TestShaderCrc32()
{
    constexpr std::array digits{std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
        std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
        std::byte{0x38}, std::byte{0x39}};
    FO4HDR_CHECK(fo4hdr::ComputeRenoDxShaderCrc32({}) == 0);
    FO4HDR_CHECK(fo4hdr::ComputeRenoDxShaderCrc32(digits) == 0xCBF43926u);
}

void TestShaderIdentityLookup()
{
    const auto* known = fo4hdr::FindRenoDxFallout4Shader(0x8024E8B5u);
    FO4HDR_CHECK(known != nullptr);
    if (known != nullptr)
    {
        FO4HDR_CHECK(known->pass == fo4hdr::RenoDxShaderPass::toneMapA);
        FO4HDR_CHECK(known->sceneInputSlot == 1);
    }
    FO4HDR_CHECK(fo4hdr::FindRenoDxFallout4Shader(0) == nullptr);
    FO4HDR_CHECK(fo4hdr::DecideUnknownShaderOutcome(fo4hdr::PipelinePassRequirement::untracked) ==
        fo4hdr::UnknownShaderOutcome::passThrough);
}

void TestShaderCatalog()
{
    struct ExpectedShader
    {
        fo4hdr::RenoDxShaderPass pass;
        std::uint32_t crc32;
        std::uint8_t sceneInputSlot;
    };
    using Pass = fo4hdr::RenoDxShaderPass;
    constexpr std::array<ExpectedShader, 15> expected{{
        {Pass::toneMapA, 0x8024E8B5u, 1},
        {Pass::toneMapB, 0x676B8B5Du, 1},
        {Pass::toneMapNewA, 0x67685D89u, 0},
        {Pass::toneMapNewB, 0xFEBC673Cu, 0},
        {Pass::lut, 0x80802E60u, 0},
        {Pass::taa, 0x8CAC3BD9u, 0},
        {Pass::taa2, 0x61CC29E6u, 0},
        {Pass::fxaa, 0x4FB4DA20u, 0},
        {Pass::video, 0x0F2CC0D1u, 0},
        {Pass::dofAuxiliary1, 0x7543CFBCu, 0},
        {Pass::dofAuxiliary2, 0x5E75012Au, 0},
        {Pass::dofAuxiliary3, 0xC7AD6297u, 0},
        {Pass::dofBlender, 0x8F009507u, 0},
        {Pass::gameDof, 0x83660755u, 0},
        {Pass::vignette, 0xB74B05F4u, 0}
    }};
    FO4HDR_CHECK(fo4hdr::kRenoDxFallout4Shaders.size() == expected.size());
    for (const auto& shader : expected)
    {
        const auto* descriptor = fo4hdr::FindRenoDxFallout4Shader(shader.crc32);
        FO4HDR_CHECK(descriptor != nullptr);
        if (descriptor == nullptr)
        {
            continue;
        }
        FO4HDR_CHECK(descriptor->pass == shader.pass);
        FO4HDR_CHECK(descriptor->crc32 == shader.crc32);
        FO4HDR_CHECK(descriptor->sceneInputSlot == shader.sceneInputSlot);
    }
}

void TestUniqueShaderMappings()
{
    const auto& catalog = fo4hdr::kRenoDxFallout4Shaders;
    for (std::size_t first = 0; first < catalog.size(); ++first)
    {
        for (auto second = first + 1; second < catalog.size(); ++second)
        {
            FO4HDR_CHECK(catalog[first].crc32 != catalog[second].crc32);
            FO4HDR_CHECK(catalog[first].pass != catalog[second].pass);
        }
    }
}

void TestShaderCrcByteRange()
{
    std::array<std::byte, 32> bytes{};
    bytes[0] = std::byte{0x44};
    bytes[1] = std::byte{0x58};
    bytes[2] = std::byte{0x42};
    bytes[3] = std::byte{0x43};
    const auto original = fo4hdr::ComputeRenoDxShaderCrc32(bytes);
    const auto prefix = fo4hdr::ComputeRenoDxShaderCrc32(std::span{bytes}.first(31));
    FO4HDR_CHECK(original != prefix);
    bytes[4] = std::byte{1};
    FO4HDR_CHECK(fo4hdr::ComputeRenoDxShaderCrc32(bytes) != original);
    bytes[4] = std::byte{0};
    bytes[31] = std::byte{1};
    FO4HDR_CHECK(fo4hdr::ComputeRenoDxShaderCrc32(bytes) != original);
    FO4HDR_CHECK(fo4hdr::ComputeRenoDxShaderCrc32(std::span{bytes}.first(31)) == prefix);
}

void TestQualifiedUnknownShaderPolicy()
{
    using Requirement = fo4hdr::PipelinePassRequirement;
    using Outcome = fo4hdr::UnknownShaderOutcome;
    FO4HDR_CHECK(fo4hdr::FindRenoDxFallout4Shader(0xDEADBEEFu) == nullptr);
    FO4HDR_CHECK(fo4hdr::DecideUnknownShaderOutcome(Requirement::untracked) == Outcome::passThrough);
    // These roles represent prior semantic qualification, not an arbitrary lookup miss.
    FO4HDR_CHECK(fo4hdr::DecideUnknownShaderOutcome(Requirement::optional) ==
        Outcome::disableOptionalFeature);
    FO4HDR_CHECK(fo4hdr::DecideUnknownShaderOutcome(Requirement::required) ==
        Outcome::disableHdrGeneration);
}

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
    TestShaderCrc32();
    TestShaderIdentityLookup();
    TestShaderCatalog();
    TestUniqueShaderMappings();
    TestShaderCrcByteRange();
    TestQualifiedUnknownShaderPolicy();
    TestLifetimePreparation();
    TestStaleResizeCompletion();
    TestEpochExhaustion();
    TestTerminalRetirement();
}

}  // namespace fo4hdr::tests
