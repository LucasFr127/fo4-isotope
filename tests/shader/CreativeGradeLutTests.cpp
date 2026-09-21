// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"
#include "Fallout4HDR/output/NativeLutObservation.hpp"
#include "passes/PresentationPass.hpp"
#include "reference/OperatorLutTestSupport.hpp"
#include "TestHarness.hpp"
#include "RenoDxShaderIdentity.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {

using namespace detail;
constexpr Float4 sourceColor{2, 4, 1, 0.75f};
constexpr float displayPeak = 1000;
constexpr float uiWhite = 203;

struct WeightedGradeInputs
{
    PixelFixture& fixture;
    ComPtr<ID3D11DeviceContext1> context;
    std::array<LutPixels, 4> pixels;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> luts;
    ComPtr<ID3D11Buffer> nativeConstants;
    ComPtr<ID3D11Buffer> borrowedConstants;

    explicit WeightedGradeInputs(PixelFixture& owner) : fixture(owner)
    {
        CheckHresult(owner.gpu.context.As(&context), "Grade Context1");
        for (unsigned index = 0; index < luts.size(); ++index)
        {
            pixels[index] = LutInputs(index);
            luts[index] = LutTexture(owner.gpu.device.Get(), pixels[index]);
        }
        const std::array<Float4, 2> weights{{{}, {0.25f, 0.5f, 0.75f, 0.125f}}};
        nativeConstants = ConstantBuffer(owner.gpu.device.Get(), weights);
        std::array<Float4, 32> borrowed{};
        borrowed[16] = {9, 8, 7, 6};
        borrowedConstants = ConstantBuffer(owner.gpu.device.Get(), borrowed);
    }

    void Bind()
    {
        fixture.Bind(true);
        const std::array<ID3D11ShaderResourceView*, 8> views{
            fixture.renderer.Views(fixture.input).shaderResource, nullptr, nullptr,
            luts[0].Get(), luts[1].Get(), luts[2].Get(), luts[3].Get(), luts[0].Get()};
        context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
        const std::array<ID3D11SamplerState*, 8> samplers{
            fixture.point.Get(), nullptr, nullptr, fixture.linear.Get(), fixture.linear.Get(),
            fixture.linear.Get(), fixture.linear.Get(), fixture.point.Get()};
        context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
        auto* native = nativeConstants.Get();
        context->PSSetConstantBuffers(2, 1, &native);
        auto* borrowed = borrowedConstants.Get();
        const UINT first = 16;
        const UINT count = 16;
        context->PSSetConstantBuffers1(13, 1, &borrowed, &first, &count);
    }

    void Check()
    {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> source;
        ComPtr<ID3D11SamplerState> sampler;
        UINT first{};
        UINT count{};
        context->PSGetConstantBuffers(13, 1, &buffer);
        context->PSGetConstantBuffers1(13, 1, nullptr, &first, &count);
        context->PSGetShaderResources(7, 1, &source);
        context->PSGetSamplers(7, 1, &sampler);
        FO4HDR_CHECK(buffer.Get() == borrowedConstants.Get());
        FO4HDR_CHECK(first == 16);
        FO4HDR_CHECK(count == 16);
        FO4HDR_CHECK(source.Get() == luts[0].Get());
        FO4HDR_CHECK(sampler.Get() == fixture.point.Get());
        CheckBoundInputs(fixture, 0, nativeConstants.Get());
    }
};

LinearRgb ExpectedGrade(const WeightedGradeInputs& inputs, float strength)
{
    const auto endpoint = LutExpected(sourceColor, inputs.pixels);
    return {std::lerp(static_cast<double>(sourceColor.red), static_cast<double>(endpoint.red), strength),
        std::lerp(static_cast<double>(sourceColor.green), static_cast<double>(endpoint.green), strength),
        std::lerp(static_cast<double>(sourceColor.blue), static_cast<double>(endpoint.blue), strength)};
}

LinearRgb ExpectedGradeRemoval(LinearRgb graded, const HdrSettingsMath& controls)
{
    if (controls.vanillaGradeLuminanceRemoval == 0 && controls.vanillaGradeColorRemoval == 0)
        return graded;
    // This positive fixture oracle independently separates scalar luma and normalized color.
    constexpr std::array weights{0.2125, 0.7154, 0.0721};
    const double originalLuma = sourceColor.red * weights[0] +
        sourceColor.green * weights[1] + sourceColor.blue * weights[2];
    const double gradedLuma = graded.red * weights[0] + graded.green * weights[1] + graded.blue * weights[2];
    const double targetLuma = std::lerp(gradedLuma, originalLuma, controls.vanillaGradeLuminanceRemoval);
    const std::array original{sourceColor.red, sourceColor.green, sourceColor.blue};
    const std::array channels{graded.red, graded.green, graded.blue};
    std::array<double, 3> result{};
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = targetLuma * std::lerp(channels[index] / gradedLuma,
            original[index] / originalLuma, controls.vanillaGradeColorRemoval);
    return {result[0], result[1], result[2]};
}

void CheckGradeChannel(float actual, double expectedNits)
{
    const auto expected = expectedNits / uiWhite;
    // Same packed coefficients; sampling allowance plus the existing coarse FP16 allowance.
    const auto tolerance = 0.5 / uiWhite + std::abs(expected) * 2 / 1024 + 0.002;
    FO4HDR_CHECK(std::isfinite(actual));
    FO4HDR_CHECK(actual >= 0);
    FO4HDR_CHECK(actual <= displayPeak / uiWhite);
    FO4HDR_CHECK(std::abs(static_cast<double>(actual) - expected) <= tolerance);
}

void VerifyGradeStrength(WeightedGradeInputs& inputs, PresentationPass& presentation,
    const OperatorLutData& table, const SceneGradeConfiguration& configuration)
{
    const auto strength = configuration.vanillaColorGradingStrength;
    const auto& selective = configuration.selectiveColor;
    auto& fixture = inputs.fixture;
    FO4HDR_CHECK(presentation.ApplySettings(table, fixture.whites,
        PresentationScope::qualifiedExclusive, configuration));
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
    Pixels source;
    source.fill(sourceColor);
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, source);
    inputs.Bind();
    FO4HDR_CHECK(DrawPass(fixture, 4, true) == SceneDrawOutcome::replaced);
    inputs.Check();
    const auto grade = ExpectedGradeRemoval(ExpectedGrade(inputs, strength), configuration.imageControls);
    SelectiveColorPrepared prepared;
    FO4HDR_CHECK(PrepareSelectiveColor(selective, prepared));
    const auto adjusted = ApplySelectiveColor({grade.red, grade.green, grade.blue}, prepared);
    const auto expected = fo4hdr::test::SampleOperatorLut(table,
        {adjusted.red, adjusted.green, adjusted.blue});
    const auto output = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (const auto pixel : output)
    {
        CheckGradeChannel(pixel.red, expected.red);
        CheckGradeChannel(pixel.green, expected.green);
        CheckGradeChannel(pixel.blue, expected.blue);
        FO4HDR_CHECK(pixel.alpha == sourceColor.alpha);
    }
    if (strength == 0) FO4HDR_CHECK(std::abs(output.front().green - sourceColor.green) > 0.1f);
}

enum class ProfileDrawCase { matched, unmatched, revoked };

ComPtr<ID3D11ShaderResourceView> CodedProfileLook(ID3D11Device* device,
    std::span<const std::uint32_t> codes)
{
    if (codes.size() != 16 * 16 * 16) throw std::runtime_error("Invalid profile look extent");
    D3D11_TEXTURE3D_DESC description{};
    description.Width = description.Height = description.Depth = 16;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{codes.data(), 16 * sizeof(std::uint32_t), 256 * sizeof(std::uint32_t)};
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> source;
    CheckHresult(device->CreateTexture3D(&description, &initial, &texture), "Profile coded look");
    CheckHresult(device->CreateShaderResourceView(texture.Get(), nullptr, &source), "Profile coded look view");
    return source;
}

ComPtr<ID3D11ShaderResourceView> QuantizedProfileLook(ID3D11Device* device, LutPixels& pixels)
{
    std::vector<std::uint32_t> codes(pixels.size());
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        auto& pixel = pixels[index];
        const std::array channels{&pixel.red, &pixel.green, &pixel.blue, &pixel.alpha};
        for (std::size_t channel = 0; channel < channels.size(); ++channel)
        {
            const auto code = static_cast<std::uint32_t>(std::lround(std::clamp(*channels[channel], 0.0F, 1.0F) * 255));
            codes[index] |= code << (8 * channel);
            // The independent interpolation oracle uses the actually uploaded UNORM nodes.
            *channels[channel] = static_cast<float>(code) / 255;
        }
    }
    return CodedProfileLook(device, codes);
}

class ControlledLutObserverStatus final
{
public:
    ControlledLutObserverStatus() noexcept : previous_(GetNativeLutObserverStatus())
    {
        // This fixture admits the plain scope, not a native hook or shader match.
        SetNativeLutObserverStatus(NativeLutObserverStatus::ready);
    }
    ~ControlledLutObserverStatus() { SetNativeLutObserverStatus(previous_); }

private:
    NativeLutObserverStatus previous_;
};

void VerifyLutProfiles(WeightedGradeInputs& inputs, PresentationPass& presentation,
    const OperatorLutData& table)
{
    const ControlledLutObserverStatus status;
    auto& fixture = inputs.fixture;
    for (std::size_t slot = 0; slot < inputs.luts.size(); ++slot)
        inputs.luts[slot] = QuantizedProfileLook(fixture.gpu.device.Get(), inputs.pixels[slot]);
    SceneGradeConfiguration configuration{0.5F};
    configuration.imageControls.vanillaGradeLuminanceRemoval = 0.35;
    configuration.imageControls.vanillaGradeColorRemoval = 0.8;
    auto& profiles = configuration.lutGradeProfiles;
    profiles.count = 1;
    auto& profile = profiles.entries[0];
    constexpr std::string_view path{"textures/luts/authored-profile.dds"};
    FO4HDR_CHECK(NormalizeLutGradePath(path, profile.path));
    profile.luminancePercent = profile.colorPercent = 0;
    profile.overrideLuminance = profile.overrideColor = true;
    FO4HDR_CHECK(presentation.ApplySettings(table, fixture.whites,
        PresentationScope::qualifiedExclusive, configuration));
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
    Pixels source;
    source.fill(sourceColor);
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, source);
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    for (const auto test : {ProfileDrawCase::matched, ProfileDrawCase::unmatched, ProfileDrawCase::revoked})
    {
        inputs.Bind();
        NativeLutObservation observation;
        observation.generation = NativeLutObservationGeneration();
        observation.threadId = GetCurrentThreadId();
        observation.validSlots = 1;
        observation.luts[0].utf8Path = profile.path;
        observation.luts[0].pathLength = static_cast<std::uint16_t>(path.size());
        observation.luts[0].weight = 0.25F;
        observation.expectedShaderViews[0] = reinterpret_cast<std::uintptr_t>(
            inputs.luts[test == ProfileDrawCase::unmatched ? 1 : 0].Get());
        NativeLutScope scope(observation);
        if (test == ProfileDrawCase::revoked) ResetNativeLutObservation();
        FO4HDR_CHECK(DrawPass(fixture, 4, true) == SceneDrawOutcome::replaced);
        inputs.Check();
        const auto copied = fixture.renderer.CopyLutInspector();
        FO4HDR_CHECK(copied.validatedSlots == (test == ProfileDrawCase::matched ? 1u : 0u));
        auto controls = configuration.imageControls;
        if (test == ProfileDrawCase::matched)
        {
            // Actual native weights sum to 1.625; only slot zero overrides globals.
            controls.vanillaGradeLuminanceRemoval = (0.25 + 1.375 * 0.35) / 1.625;
            controls.vanillaGradeColorRemoval = (0.25 + 1.375 * 0.8) / 1.625;
        }
        const auto graded = ExpectedGradeRemoval(ExpectedGrade(inputs, 0.5F), controls);
        const auto expected = fo4hdr::test::SampleOperatorLut(table, graded);
        const auto output = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
        for (const auto pixel : output)
        {
            CheckGradeChannel(pixel.red, expected.red);
            CheckGradeChannel(pixel.green, expected.green);
            CheckGradeChannel(pixel.blue, expected.blue);
            FO4HDR_CHECK(pixel.alpha == sourceColor.alpha);
        }
    }
    FO4HDR_CHECK(fixture.renderer.EndFrame());
}

ComPtr<ID3D11ShaderResourceView> ProfileFidelityLook(ID3D11Device* device)
{
    std::array<std::uint32_t, 16 * 16 * 16> codes{};
    codes.fill(0xFF604020);
    return CodedProfileLook(device, codes);
}

struct ProfileFidelityCase
{
    Float4 weights;
    double luminanceRemoval;
    double colorRemoval;
    float strength{0.5F};
};

struct ProfileFidelityDraw
{
    UINT calls{};
    UINT lookWidth{};

    static void Draw(ID3D11DeviceContext* context, void* argument)
    {
        auto& evidence = *static_cast<ProfileFidelityDraw*>(argument);
        ++evidence.calls;
        ComPtr<ID3D11ShaderResourceView> source;
        context->PSGetShaderResources(3, 1, &source);
        if (!source) throw std::runtime_error("Missing fidelity look at draw");
        ComPtr<ID3D11Resource> resource;
        source->GetResource(&resource);
        ComPtr<ID3D11Texture3D> texture;
        CheckHresult(resource.As(&texture), "Profile draw look texture");
        D3D11_TEXTURE3D_DESC description{};
        texture->GetDesc(&description);
        evidence.lookWidth = description.Width;
        context->Draw(3, 0);
    }
};

SceneGradeConfiguration FidelityConfiguration(ColorGradingMethod method, const ProfileFidelityCase& test,
    bool profilesActive)
{
    SceneGradeConfiguration configuration{test.strength};
    configuration.method = method;
    auto& controls = configuration.imageControls;
    controls.vanillaGradeLuminanceRemoval = profilesActive ? 0.35 : test.luminanceRemoval;
    controls.vanillaGradeColorRemoval = profilesActive ? 0.8 : test.colorRemoval;
    if (!profilesActive) return configuration;
    auto& profiles = configuration.lutGradeProfiles;
    profiles.count = 1;
    auto& profile = profiles.entries[0];
    FO4HDR_CHECK(NormalizeLutGradePath("textures/luts/profile-fidelity.dds", profile.path));
    profile.luminancePercent = profile.colorPercent = 0;
    profile.overrideLuminance = profile.overrideColor = true;
    return configuration;
}

NativeLutObservation FidelityObservation(const WeightedGradeInputs& inputs)
{
    NativeLutObservation observation;
    observation.generation = NativeLutObservationGeneration();
    observation.threadId = GetCurrentThreadId();
    observation.validSlots = 1;
    constexpr std::string_view path{"textures/luts/profile-fidelity.dds"};
    FO4HDR_CHECK(NormalizeLutGradePath(path, observation.luts[0].utf8Path));
    observation.luts[0].pathLength = static_cast<std::uint16_t>(path.size());
    observation.expectedShaderViews[0] = reinterpret_cast<std::uintptr_t>(inputs.luts[0].Get());
    return observation;
}

void BindFidelityWeights(WeightedGradeInputs& inputs, Float4 weights)
{
    std::array<Float4, 32> values{};
    values[17] = weights;
    inputs.nativeConstants = ConstantBuffer(inputs.fixture.gpu.device.Get(), values);
    inputs.Bind();
    auto* buffer = inputs.nativeConstants.Get();
    constexpr UINT first = 16;
    constexpr UINT count = 16;
    inputs.context->PSSetConstantBuffers1(2, 1, &buffer, &first, &count);
}

void CheckFidelityWeights(const WeightedGradeInputs& inputs)
{
    UINT first{};
    UINT count{};
    inputs.context->PSGetConstantBuffers1(2, 1, nullptr, &first, &count);
    FO4HDR_CHECK(first == 16 && count == 16);
}

std::vector<Float4> RenderProfileFidelity(const std::filesystem::path& directory,
    ColorGradingMethod method, const ProfileFidelityCase& test, bool profilesActive)
{
    const ControlledLutObserverStatus status;
    const auto configuration = FidelityConfiguration(method, test, profilesActive);
    PixelFixture fixture(directory, width, height, {}, nullptr, configuration);
    WeightedGradeInputs inputs(fixture);
    for (auto& look : inputs.luts) look = ProfileFidelityLook(fixture.gpu.device.Get());
    Pixels scene;
    scene.fill(sourceColor);
    FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
    UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, scene);
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    BindFidelityWeights(inputs, test.weights);
    const NativeLutScope scope(FidelityObservation(inputs));
    ProfileFidelityDraw draw;
    FO4HDR_CHECK(fixture.renderer.Execute(kRenoDxFallout4Shaders[4], fixture.input, fixture.destination,
        {ProfileFidelityDraw::Draw, &draw}) == SceneDrawOutcome::replaced);
    FO4HDR_CHECK(draw.calls == 1);
    const bool prepared = method == ColorGradingMethod::nativeReference && test.strength > 0;
    FO4HDR_CHECK(draw.lookWidth == (prepared ? 20u : 16u));
    fixture.CheckRestored(true);
    inputs.Check();
    CheckFidelityWeights(inputs);
    FO4HDR_CHECK(fixture.renderer.CopyLutInspector().validatedSlots == 1);
    const auto output = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    FO4HDR_CHECK(fixture.renderer.EndFrame());
    return output;
}

void CheckProfileFidelity(const std::vector<Float4>& actual, const std::vector<Float4>& reference,
    float strength)
{
    FO4HDR_CHECK(actual.size() == width * height && reference.size() == actual.size());
    if (actual.size() != width * height || reference.size() != actual.size()) return;
    for (std::size_t index = 0; index < actual.size(); ++index)
    {
        // Same-shader comparison retains the existing FP16 integration boundary.
        CheckPixel(actual[index], reference[index]);
        FO4HDR_CHECK(actual[index].alpha == sourceColor.alpha);
        if (strength == 0)
        {
            FO4HDR_CHECK(actual[index].red == sourceColor.red);
            FO4HDR_CHECK(actual[index].green == sourceColor.green);
            FO4HDR_CHECK(actual[index].blue == sourceColor.blue);
        }
    }
}

void VerifyProfileFidelity(const std::filesystem::path& directory)
{
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    // Only slot zero overrides globals; native weights sum to 1.625.
    constexpr double luminance = (0.25 + 1.375 * 0.35) / 1.625;
    constexpr double color = (0.25 + 1.375 * 0.8) / 1.625;
    const std::array cases{
        ProfileFidelityCase{{0.25F, 0.5F, 0.75F, 0.125F}, luminance, color},
        ProfileFidelityCase{{}, 0.35, 0.8},
        ProfileFidelityCase{{nan, 0.5F, 0.75F, 0.125F}, 0.35, 0.8},
        ProfileFidelityCase{{0.25F, 0.5F, 0.75F, 0.125F}, luminance, color, 0}};
    for (const auto method : {ColorGradingMethod::radial, ColorGradingMethod::nativeReference})
        for (const auto& test : cases)
        {
            const auto actual = RenderProfileFidelity(directory, method, test, true);
            const auto reference = RenderProfileFidelity(directory, method, test, false);
            CheckProfileFidelity(actual, reference, test.strength);
        }
}

}  // namespace

void VerifyCreativeGradeLut(const std::filesystem::path& fixtureDirectory)
{
    const auto table = BuildOperatorLut(OperatorLutAlgorithm::gt7, displayPeak);
    FO4HDR_CHECK(table.IsValid());
    if (!table.IsValid()) return;
    PixelFixture fixture(fixtureDirectory, width, height, {uiWhite, 203}, &table);
    PresentationPass presentation;
    const PresentationConfiguration configuration{width, height, displayPeak, &fixture.renderer, fixture.whites};
    FO4HDR_CHECK(presentation.Initialize(fixture.gpu.device.Get(), fixture.gpu.context.Get(), configuration));
    WeightedGradeInputs inputs(fixture);
    for (const float strength : {0.0f, 0.5f, 1.0f})
        VerifyGradeStrength(inputs, presentation, table, {strength});
    SelectiveColorSettings selective;
    selective.regions.fill({3, 125, 30});
    VerifyGradeStrength(inputs, presentation, table, {0, {}, ColorGradingMethod::radial, selective});
    selective.enabled = true;
    VerifyGradeStrength(inputs, presentation, table, {0, {}, ColorGradingMethod::radial, selective});
    SceneGradeConfiguration removal{0.5F};
    removal.imageControls.vanillaGradeLuminanceRemoval = 0.35;
    removal.imageControls.vanillaGradeColorRemoval = 0.8;
    VerifyGradeStrength(inputs, presentation, table, removal);
    VerifyLutProfiles(inputs, presentation, table);
    FO4HDR_CHECK(presentation.Retire());
    VerifyProfileFidelity(fixtureDirectory);
}

}  // namespace fo4hdr::tests::gpu
