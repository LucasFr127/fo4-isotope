// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationFacadeFixture.hpp"
#include "d3d11/BorrowedFullscreenState.hpp"
#include "d3d11/ContextQuerySafety.hpp"
#include "resources/SharedUiComposition.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"

#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <iostream>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {

constexpr UINT kCompositionWidth = 16, kCompositionHeight = 8;
using UiPixel = std::array<double, 4>;
using HalfPixel = std::array<std::uint16_t, 4>;

struct SharedUiConstants
{
    UINT width{kCompositionWidth}, height{kCompositionHeight}, operation{};
    float uiWhite{203}, sceneWhite{203};
    std::array<float, 3> reserved{};
    Float4 color{};
    Float4 rectangle{0, 0, kCompositionWidth, kCompositionHeight};
};
static_assert(sizeof(SharedUiConstants) == 64);

double SignedSrgb(double value, bool encode)
{
    const auto magnitude = std::abs(value);
    if (encode)
        return std::copysign(magnitude <= 0.0031308 ? 12.92 * magnitude :
            1.055 * std::pow(magnitude, 1.0 / 2.4) - 0.055, value);
    return std::copysign(magnitude <= 0.04045 ? magnitude / 12.92 :
        std::pow((magnitude + 0.055) / 1.055, 2.4), value);
}

UiPixel ToBt709(const UiPixel& value)
{
    return {1.660491002108435 * value[0] - 0.587641138788550 * value[1] -
            0.072849863319884 * value[2],
        -0.124550474521591 * value[0] + 1.132899897125960 * value[1] -
            0.008349422604369 * value[2],
        -0.018150763354905 * value[0] - 0.100578898008007 * value[1] +
            1.118729661362912 * value[2], 1};
}

UiPixel ToBt2020(const UiPixel& value)
{
    return {0.627403895934699 * value[0] + 0.329283038377884 * value[1] +
            0.043313065687417 * value[2],
        0.069097289358232 * value[0] + 0.919540395075459 * value[1] +
            0.011362315566309 * value[2],
        0.016391438875150 * value[0] + 0.088013307877226 * value[1] +
            0.895595253247624 * value[2], 1};
}

double TruncateTransport(double value)
{
    const auto source = static_cast<float>(value);
    auto bits = FloatToBinary16(source);
    // Finite test pixels follow D3D11's truncating format conversion; uploads remain independent.
    if (std::abs(Binary16ToFloat(bits)) > std::abs(source)) --bits;
    return Binary16ToFloat(bits);
}

void VerifyTransportTruncation()
{
    FO4HDR_WARP_CHECK(TruncateTransport(1.00075F) == 1);
    FO4HDR_WARP_CHECK(TruncateTransport(-1.00075F) == -1);
    const auto adjacent = Binary16ToFloat(0x3c01);
    FO4HDR_WARP_CHECK(TruncateTransport(adjacent) == adjacent);
    FO4HDR_WARP_CHECK(TruncateTransport(1.0 - 0x1p-30) == 1);
    const auto subnormal = 0x1p-24;
    FO4HDR_WARP_CHECK(TruncateTransport(1.5 * subnormal) == subnormal);
    FO4HDR_WARP_CHECK(TruncateTransport(-1.5 * subnormal) == -subnormal);
    FO4HDR_WARP_CHECK(TruncateTransport(70000) == 65504);
    FO4HDR_WARP_CHECK(TruncateTransport(-70000) == -65504);
    FO4HDR_WARP_CHECK(TruncateTransport(1.0e-20) == 0);
    const auto negativeZero = TruncateTransport(-1.0e-20);
    FO4HDR_WARP_CHECK(negativeZero == 0 && std::signbit(negativeZero));
}

struct UiOracle
{
    UINT width, height;
    bool roundStores;
    std::vector<UiPixel> pixels;
    double inheritedError{};

    UiOracle(UINT x, UINT y, bool rounded) : width(x), height(y), roundStores(rounded),
        pixels(static_cast<std::size_t>(x) * y) {}

    void Store(std::size_t index, UiPixel value)
    {
        if (roundStores)
            for (auto& channel : value)
                channel = TruncateTransport(channel);
        pixels[index] = value;
    }

    const UiPixel& At(int x, int y) const
    {
        const auto boundedX = std::clamp(x, 0, static_cast<int>(width) - 1);
        const auto boundedY = std::clamp(y, 0, static_cast<int>(height) - 1);
        return pixels[static_cast<std::size_t>(boundedY) * width + boundedX];
    }

    double ErrorAfterStore() const
    {
        double magnitude{};
        for (const auto& pixel : pixels)
            for (UINT channel = 0; channel < 3; ++channel)
                magnitude = std::max(magnitude, std::abs(pixel[channel]));
        return inheritedError + Binary16LargestAdjacentStep(
            static_cast<float>(magnitude + inheritedError)) + 4.0e-6;
    }
};

UiOracle SeedOracle(const std::vector<HalfPixel>& scene, float white, bool rounded)
{
    UiOracle result(kCompositionWidth, kCompositionHeight, rounded);
    for (std::size_t index = 0; index < scene.size(); ++index)
    {
        UiPixel source{};
        for (UINT channel = 0; channel < 3; ++channel)
            source[channel] = Binary16ToFloat(scene[index][channel]);
        auto encoded = ToBt709(source);
        for (UINT channel = 0; channel < 3; ++channel)
            encoded[channel] = SignedSrgb(encoded[channel] * 203.0 / white, true);
        result.Store(index, encoded);
    }
    return result;
}

UiPixel FilterPixel(const UiOracle& input, int x, int y, UINT operation)
{
    UiPixel value{};
    if (operation == 1)
    {
        for (UINT channel = 0; channel < 4; ++channel)
            value[channel] = (input.At(x * 2, y * 2)[channel] +
                input.At(x * 2 + 1, y * 2)[channel] + input.At(x * 2, y * 2 + 1)[channel] +
                input.At(x * 2 + 1, y * 2 + 1)[channel]) * 0.25;
        return value;
    }
    const int dx = operation == 2 ? 1 : 0, dy = operation == 3 ? 1 : 0;
    for (UINT channel = 0; channel < 4; ++channel)
        value[channel] = input.At(x - dx, y - dy)[channel] * 0.25 +
            input.At(x, y)[channel] * 0.5 + input.At(x + dx, y + dy)[channel] * 0.25;
    return value;
}

UiOracle FilterOracle(const UiOracle& input, UINT operation)
{
    const bool downsample = operation == 1;
    UiOracle result(downsample ? input.width / 2 : input.width,
        downsample ? input.height / 2 : input.height, input.roundStores);
    result.inheritedError = input.ErrorAfterStore();
    for (UINT y = 0; y < result.height; ++y)
        for (UINT x = 0; x < result.width; ++x)
            result.Store(static_cast<std::size_t>(y) * result.width + x,
                FilterPixel(input, static_cast<int>(x), static_cast<int>(y), operation));
    return result;
}

bool InsideRectangle(UINT x, UINT y, const Float4& rectangle)
{
    const auto [left, top, right, bottom] = rectangle;
    const std::array inside{x >= left, x < right, y >= top, y < bottom};
    return std::ranges::all_of(inside, [](bool value) { return value; });
}

UiPixel BlendPixel(UiPixel destination, const UiPixel& color, const UiPixel* background)
{
    for (UINT channel = 0; channel < 3; ++channel)
    {
        const double source = background ? (*background)[channel] * 0.8 + color[channel] * 0.2 :
            color[channel];
        destination[channel] = source * color[3] + destination[channel] * (1 - color[3]);
    }
    destination[3] = color[3] + destination[3] * (1 - color[3]);
    return destination;
}

void BlendOracle(UiOracle& image, const SharedUiConstants& constants,
    const UiOracle* background = nullptr)
{
    const auto [red, green, blue, alpha] = constants.color;
    const UiPixel color{red, green, blue, alpha};
    const double previousError = image.ErrorAfterStore();
    const double backgroundError = background ? background->ErrorAfterStore() : 0;
    for (UINT y = 0; y < image.height; ++y)
        for (UINT x = 0; x < image.width; ++x)
        {
            if (!InsideRectangle(x, y, constants.rectangle)) continue;
            const auto index = static_cast<std::size_t>(y) * image.width + x;
            const auto* sample = background ? &background->At(x / 2, y / 2) : nullptr;
            image.Store(index, BlendPixel(image.pixels[index], color, sample));
        }
    // Untouched pixels retain their old error; covered pixels combine positive blend weights.
    image.inheritedError = std::max(previousError,
        previousError * (1 - alpha) + backgroundError * 0.8 * alpha);
}

Texture FacadeTexture(Device& device, IDXGISwapChain& facade)
{
    Texture result;
    result.texture = FacadeBuffer(facade);
    CheckHresult(device.device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv),
        "Create shared UI facade RTV");
    CheckHresult(device.device->CreateShaderResourceView(
        result.texture.Get(), nullptr, &result.srv),
        "Create shared UI facade SRV");
    D3D11_TEXTURE2D_DESC description{};
    result.texture->GetDesc(&description);
    description.BindFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CheckHresult(device.device->CreateTexture2D(&description, nullptr, &result.staging),
        "Create shared UI facade readback");
    return result;
}

Texture DescriptorTexture(Device& device, const Texture& source, bool halfSize)
{
    D3D11_TEXTURE2D_DESC description{};
    source.texture->GetDesc(&description);
    return CreateSizedTexture(*device.device.Get(),
        {halfSize ? description.Width / 2 : description.Width,
            halfSize ? description.Height / 2 : description.Height,
            description.Format, description.BindFlags});
}

template <class Pixel>
std::vector<Pixel> ReadTexturePixels(Device& device, Texture& image)
{
    D3D11_TEXTURE2D_DESC description{};
    image.texture->GetDesc(&description);
    device.context->CopyResource(image.staging.Get(), image.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(device.context->Map(image.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Read shared UI texture content");
    std::vector<Pixel> result(static_cast<std::size_t>(description.Width) * description.Height);
    for (UINT y = 0; y < description.Height; ++y)
    {
        const auto* row = reinterpret_cast<const Pixel*>(
            static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch);
        std::copy_n(row, description.Width, result.begin() + y * description.Width);
    }
    device.context->Unmap(image.staging.Get(), 0);
    return result;
}

std::vector<HalfPixel> ReadComposition(Device& device, Texture& image)
{
    return ReadTexturePixels<HalfPixel>(device, image);
}

void CheckTransport(Device& device, Texture& image, const UiOracle& oracle,
    std::string_view stage)
{
    const auto actual = ReadComposition(device, image);
    FO4HDR_WARP_CHECK(actual.size() == oracle.pixels.size());
    double maximumError{}, maximumBound{};
    for (std::size_t index = 0; index < actual.size(); ++index)
        for (UINT channel = 0; channel < 4; ++channel)
        {
            const auto expected = oracle.pixels[index][channel];
            const double step = Binary16LargestAdjacentStep(static_cast<float>(std::abs(expected)));
            const double value = Binary16ToFloat(actual[index][channel]);
            const double error = std::abs(value - expected);
            const double bound = oracle.inheritedError + step + 4.0e-6;
            maximumError = std::max(maximumError, error);
            maximumBound = std::max(maximumBound, bound);
            const bool withinBudget = error <= bound;
            if (!withinBudget)
                std::cerr << "Shared UI transport " << stage << " size=" << oracle.width << 'x'
                    << oracle.height << " index=" << index << " channel=" << channel << " actual="
                    << value << " expected=" << expected << " error=" << error << " bound="
                    << bound << '\n';
            FO4HDR_WARP_CHECK(withinBudget);
        }
    std::cout << "Shared UI transport " << stage << ": max error=" << maximumError
        << " max bound=" << maximumBound << '\n';
}

enum class UiPhase { sceneReady, nativeHud, background, panels, cursor, complete, unsupported };

bool IsUiDrawPhase(UiPhase phase)
{
    return phase == UiPhase::nativeHud || phase == UiPhase::panels || phase == UiPhase::cursor;
}

struct UiSeed
{
    std::uint64_t generation, revision;
    float white;
    UiPhase phase{UiPhase::sceneReady};
};

struct SharedUiPass
{
    const PresentationShaders& shaders;
    Texture& source;
    Texture& target;
    SharedUiConstants constants;
};

struct SharedUiFixture
{
    Device& device;
    PresentationWindow window;
    ComPtr<IDXGISwapChain1> native;
    ComPtr<IDXGISwapChain> facade;
    ComPtr<ID3D11DeviceContext1> context1;
    Texture scene, composition, copy, down, horizontal, vertical, output;
    SharedUiComposition seed;
    PresentationShaders syntheticShader, finalShader, blendShader;
    std::uint64_t generation{1};
    UiSeed content{};
    UiPhase next{UiPhase::sceneReady};
    ContextQuerySafety queries;
    UINT finalDraws{};

    explicit SharedUiFixture(Device& value) : device(value)
    {
        native = CreatePresentationChain(
            device, window.handle, kCompositionWidth, kCompositionHeight);
        facade = CreateFacade(*native.Get());
        composition = FacadeTexture(device, *facade.Get());
        scene = DescriptorTexture(device, composition, false);
        copy = DescriptorTexture(device, composition, false);
        down = DescriptorTexture(device, composition, true);
        horizontal = DescriptorTexture(device, down, false);
        vertical = DescriptorTexture(device, down, false);
        output = GetPresentationBuffer(device, *native.Get());
        CheckHresult(device.context.As(&context1), "Query shared UI context1");
        CreateShaders();
    }

    void CreateShaders()
    {
        auto& gpu = *device.device.Get();
        FO4HDR_WARP_CHECK(seed.Create(&gpu, kCompositionWidth, kCompositionHeight));
        syntheticShader = CreatePresentationShaders(gpu);
        syntheticShader.pixel = CreatePixelShader(gpu, generated::test::kSharedUiSyntheticPs);
        finalShader = CreatePresentationShaders(gpu);
        blendShader = syntheticShader;
        D3D11_BLEND_DESC description{};
        auto& target = description.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        CheckHresult(gpu.CreateBlendState(&description, &blendShader.blend),
            "Create synthetic UI blend");
    }

    bool Draw(const SharedUiPass& pass)
    {
        if (!pass.shaders.pixel || !pass.target.rtv) return false;
        if (!queries.CanDispatch()) return false;
        BorrowedFullscreenState borrowed(
            device.context.Get(), context1.Get(), pass.target.texture.Get());
        if (!borrowed.Ready()) return false;
        D3D11_TEXTURE2D_DESC description{};
        pass.target.texture->GetDesc(&description);
        const auto constants = CreateConstantBuffer(*device.device.Get(), pass.constants);
        DrawPresentation(device, {&pass.shaders, constants.Get(), {pass.source.srv.Get(), nullptr,
            nullptr, nullptr}, pass.target.rtv.Get(), description.Width, description.Height});
        return true;
    }

    bool Seed(UiSeed request)
    {
        const std::array valid{
            request.generation == generation, request.revision > content.revision,
            request.revision != 0, std::isfinite(request.white), request.white > 0,
            request.phase == UiPhase::sceneReady,
            next == UiPhase::sceneReady || next == UiPhase::complete};
        if (!std::ranges::all_of(valid, [](bool value) { return value; })) return false;
        if (!queries.CanDispatch()) return false;
        if (!seed.Seed(device.context.Get(), context1.Get(),
                {scene.srv.Get(), composition.rtv.Get(), {203, request.white}}))
            return false;
        content = request;
        next = UiPhase::nativeHud;
        return true;
    }

    bool UiDraw(UiPhase phase, const SharedUiConstants& constants)
    {
        if (content.generation != generation || next != phase) return false;
        if (!IsUiDrawPhase(phase)) return false;
        if (!Draw({blendShader, vertical, composition, constants})) return false;
        if (phase == UiPhase::nativeHud) next = UiPhase::background;
        if (phase == UiPhase::cursor) next = UiPhase::complete;
        return true;
    }

    bool Background()
    {
        if (content.generation != generation || next != UiPhase::background) return false;
        if (!queries.CanDispatch()) return false;
        BorrowedFullscreenState borrowed(device.context.Get(), context1.Get(), copy.texture.Get());
        if (!borrowed.Ready()) return false;
        device.context->CopyResource(copy.texture.Get(), composition.texture.Get());
        SharedUiConstants constants;
        constants.operation = 1;
        if (!Draw({syntheticShader, copy, down, constants})) return false;
        constants.width /= 2;
        constants.height /= 2;
        constants.operation = 2;
        if (!Draw({syntheticShader, down, horizontal, constants})) return false;
        constants.operation = 3;
        if (!Draw({syntheticShader, horizontal, vertical, constants})) return false;
        next = UiPhase::panels;
        return true;
    }

    bool FinishPanels()
    {
        if (content.generation != generation || next != UiPhase::panels) return false;
        next = UiPhase::cursor;
        return true;
    }

    bool Encode(std::uint64_t currentGeneration, UINT encoding = 1)
    {
        const std::array valid{currentGeneration == generation, content.generation == generation,
            next == UiPhase::complete};
        if (!std::ranges::all_of(valid, [](bool value) { return value; })) return false;
        if (!queries.CanDispatch()) return false;
        BorrowedFullscreenState borrowed(device.context.Get(), context1.Get(), output.texture.Get());
        if (!borrowed.Ready()) return false;
        const CompositionConstants constants{
            .width = kCompositionWidth, .height = kCompositionHeight, .mode = 3,
            .uiPaperWhiteNits = content.white, .sceneReferenceWhiteNits = 203,
            .displayPeakNits = 1000, .settingsOverlayPresent = 0, .sharedUiEncoded = encoding};
        const auto buffer = CreateConstantBuffer(*device.device.Get(), constants);
        DrawPresentation(device, {&finalShader, buffer.Get(),
            {composition.srv.Get(), nullptr, nullptr, nullptr}, output.rtv.Get(),
            kCompositionWidth, kCompositionHeight});
        ++finalDraws;
        return true;
    }
};

std::vector<HalfPixel> UploadScene(SharedUiFixture& fixture)
{
    constexpr std::array<UiPixel, 12> samples{{{0, 0, 0, 1}, {0.0031308, 0.0031308, 0.0031308, 1},
        {-0.0031308, -0.0031308, -0.0031308, 1}, {1, 1, 1, 1}, {4, 0, 0, 1},
        {0, 4, 0, 1}, {0, 0, 4, 1}, {-2, 3, 0.5, 1}, {16, 16, 16, 1},
        {0.18, 0.18, 0.18, 1}, {0.0001, -0.0001, 0.0001, 1}, {2, -1, 4, 1}}};
    std::vector<HalfPixel> pixels(kCompositionWidth * kCompositionHeight);
    for (std::size_t index = 0; index < pixels.size(); ++index)
        for (UINT channel = 0; channel < 4; ++channel)
            pixels[index][channel] = FloatToBinary16(
                static_cast<float>(samples[index % samples.size()][channel]));
    fixture.device.context->UpdateSubresource(
        fixture.scene.texture.Get(), 0, nullptr, pixels.data(),
        kCompositionWidth * sizeof(HalfPixel), 0);
    return pixels;
}

UiPixel DecodeNits(const HalfPixel& pixel, float white, UINT encoding = 1)
{
    UiPixel decoded{};
    for (UINT channel = 0; channel < 3; ++channel)
    {
        const auto value = Binary16ToFloat(pixel[channel]);
        decoded[channel] = (encoding == 2 ? std::pow(std::max(value, 0.0F), 2.2) :
            SignedSrgb(value, false)) * white;
    }
    return ToBt2020(decoded);
}

void CheckFinal(SharedUiFixture& fixture, const std::vector<HalfPixel>& input, UINT encoding = 1)
{
    const auto actual = ReadTexturePixels<UINT>(fixture.device, fixture.output);
    for (UINT y = 0; y < kCompositionHeight; ++y)
    {
        const auto* row = actual.data() + y * kCompositionWidth;
        for (UINT x = 0; x < kCompositionWidth; ++x)
        {
            const auto absolute = DecodeNits(
                input[y * kCompositionWidth + x], fixture.content.white, encoding);
            for (UINT channel = 0; channel < 3; ++channel)
            {
                const double bounded = std::isnan(absolute[channel]) ? 0 :
                    std::clamp(absolute[channel], 0.0, 10000.0);
                const auto expected = fo4hdr::QuantizeR10Unorm(fo4hdr::EncodeSt2084Pq(bounded));
                FO4HDR_WARP_CHECK(CodeDistance(
                    static_cast<std::uint16_t>((row[x] >> (10 * channel)) & 1023),
                    expected) <= 1);
            }
            FO4HDR_WARP_CHECK(row[x] >> 30 == 3);
        }
    }
}

void CheckIdealTransport(const std::vector<HalfPixel>& actual, const UiOracle& ideal,
    double maximumMagnitude)
{
    // This graph's positive filter/blend weights bound truncation to five seed-magnitude ULPs.
    const double budget = 5 * Binary16LargestAdjacentStep(
        static_cast<float>(maximumMagnitude)) + 4.0e-5;
    for (std::size_t index = 0; index < actual.size(); ++index)
        for (UINT channel = 0; channel < 3; ++channel)
            FO4HDR_WARP_CHECK(std::abs(Binary16ToFloat(actual[index][channel]) -
                ideal.pixels[index][channel]) <= budget);
}

void ExerciseUiSequence(SharedUiFixture& fixture, UiOracle& oracle, UiOracle& ideal)
{
    auto& device = fixture.device;
    SharedUiConstants constants;
    constants.color = {0.9F, 0.2F, 0.05F, 0.625F};
    constants.rectangle = {0, 0, 8, 4};
    FO4HDR_WARP_CHECK(fixture.UiDraw(UiPhase::nativeHud, constants));
    BlendOracle(oracle, constants);
    BlendOracle(ideal, constants);
    CheckTransport(device, fixture.composition, oracle, "native HUD");
    FO4HDR_WARP_CHECK(!fixture.UiDraw(UiPhase::nativeHud, constants));
    FO4HDR_WARP_CHECK(fixture.Background());
    CheckTransport(device, fixture.copy, oracle, "foreign copy");
    auto down = FilterOracle(oracle, 1), idealDown = FilterOracle(ideal, 1);
    auto horizontal = FilterOracle(down, 2), idealHorizontal = FilterOracle(idealDown, 2);
    auto vertical = FilterOracle(horizontal, 3), idealVertical = FilterOracle(idealHorizontal, 3);
    CheckTransport(device, fixture.down, down, "downsample");
    CheckTransport(device, fixture.horizontal, horizontal, "horizontal filter");
    CheckTransport(device, fixture.vertical, vertical, "vertical filter");
    constants.operation = 4;
    constants.color = {0.1F, 0.3F, 0.6F, 0.8F};
    constants.rectangle = {2, 1, 14, 7};
    FO4HDR_WARP_CHECK(fixture.UiDraw(UiPhase::panels, constants));
    BlendOracle(oracle, constants, &vertical);
    BlendOracle(ideal, constants, &idealVertical);
    constants.operation = 0;
    constants.color = {-0.2F, 1.3F, 0.4F, 0.375F};
    constants.rectangle = {5, 2, 11, 6};
    FO4HDR_WARP_CHECK(fixture.UiDraw(UiPhase::panels, constants));
    BlendOracle(oracle, constants);
    BlendOracle(ideal, constants);
    constants.color = {0.05F, 0.12F, 0.2F, 0.75F};
    constants.rectangle = {7, 0, 15, 5};
    FO4HDR_WARP_CHECK(fixture.UiDraw(UiPhase::panels, constants));
    BlendOracle(oracle, constants);
    BlendOracle(ideal, constants);
    const auto beforeTransparent = ReadComposition(device, fixture.composition);
    constants.color = {-20, 30, 40, 0};
    constants.rectangle = {0, 0, kCompositionWidth, kCompositionHeight};
    FO4HDR_WARP_CHECK(fixture.UiDraw(UiPhase::panels, constants));
    FO4HDR_WARP_CHECK(ReadComposition(device, fixture.composition) == beforeTransparent);
    FO4HDR_WARP_CHECK(fixture.FinishPanels());
    constants.color = {1, 1, 1, 0.875F};
    constants.rectangle = {11, 3, 13, 5};
    FO4HDR_WARP_CHECK(fixture.UiDraw(UiPhase::cursor, constants));
    BlendOracle(oracle, constants);
    BlendOracle(ideal, constants);
    CheckTransport(device, fixture.composition, oracle, "complete UI");
}

void VerifySeedResourceValidation(SharedUiFixture& fixture)
{
    auto& device = fixture.device;
    const auto before = ReadComposition(device, fixture.composition);
    const SharedUiSeedInputs valid{fixture.scene.srv.Get(), fixture.composition.rtv.Get(), {203, 203}};
    auto invalid = valid;
    invalid.white.sceneWhiteNits = 0;
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), fixture.context1.Get(), invalid));
    invalid = valid;
    invalid.white.uiWhiteNits = std::numeric_limits<float>::quiet_NaN();
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), fixture.context1.Get(), invalid));
    invalid = valid;
    invalid.scene = fixture.down.srv.Get();
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), fixture.context1.Get(), invalid));
    invalid = valid;
    invalid.output = fixture.down.rtv.Get();
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), fixture.context1.Get(), invalid));
    auto unorm = CreateSizedTexture(*device.device.Get(), {kCompositionWidth, kCompositionHeight,
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET});
    invalid = valid;
    invalid.scene = unorm.srv.Get();
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), fixture.context1.Get(), invalid));
    invalid = valid;
    invalid.output = unorm.rtv.Get();
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), fixture.context1.Get(), invalid));
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), nullptr, valid));
    fixture.seed.Reset();
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), fixture.context1.Get(), valid));
    FO4HDR_WARP_CHECK(!fixture.seed.Create(device.device.Get(), 0, kCompositionHeight));
    FO4HDR_WARP_CHECK(fixture.seed.Create(device.device.Get(), kCompositionWidth, kCompositionHeight));
    FO4HDR_WARP_CHECK(ReadComposition(device, fixture.composition) == before);
}

void VerifySeedDeviceIdentity(SharedUiFixture& fixture)
{
    auto foreign = CreateDevice(false);
    ComPtr<ID3D11DeviceContext1> foreignContext;
    CheckHresult(foreign.context.As(&foreignContext), "Get foreign seed context1");
    auto texture = CreateSizedTexture(*foreign.device.Get(), {kCompositionWidth, kCompositionHeight,
        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET});
    auto& device = fixture.device;
    const auto before = ReadComposition(device, fixture.composition);
    const SharedUiSeedInputs valid{fixture.scene.srv.Get(), fixture.composition.rtv.Get(), {203, 203}};
    auto invalid = valid;
    invalid.scene = texture.srv.Get();
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), fixture.context1.Get(), invalid));
    invalid = valid;
    invalid.output = texture.rtv.Get();
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), fixture.context1.Get(), invalid));
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(device.context.Get(), foreignContext.Get(), valid));
    FO4HDR_WARP_CHECK(!fixture.seed.Seed(foreign.context.Get(), foreignContext.Get(), valid));
    FO4HDR_WARP_CHECK(ReadComposition(device, fixture.composition) == before);
}

void VerifyAliasedSeed(SharedUiFixture& fixture, const std::vector<HalfPixel>& scene)
{
    auto& context = *fixture.device.context.Get();
    context.CopyResource(fixture.composition.texture.Get(), fixture.scene.texture.Get());
    auto* previous = fixture.scene.rtv.Get();
    auto* alias = fixture.composition.srv.Get();
    context.OMSetRenderTargets(1, &previous, nullptr);
    context.PSSetShaderResources(90, 1, &alias);
    FO4HDR_WARP_CHECK(fixture.seed.Seed(fixture.device.context.Get(), fixture.context1.Get(),
        {fixture.composition.srv.Get(), fixture.composition.rtv.Get(), {203, 203}}));
    ComPtr<ID3D11RenderTargetView> restoredTarget;
    context.OMGetRenderTargets(1, &restoredTarget, nullptr);
    FO4HDR_WARP_CHECK(restoredTarget.Get() == previous);
    ComPtr<ID3D11ShaderResourceView> restoredAlias;
    context.PSGetShaderResources(90, 1, &restoredAlias);
    FO4HDR_WARP_CHECK(restoredAlias.Get() == alias);
    alias = nullptr;
    context.PSSetShaderResources(90, 1, &alias);
    context.OMSetRenderTargets(0, nullptr, nullptr);
    CheckTransport(fixture.device, fixture.composition, SeedOracle(scene, 203, true), "aliased seed");
}

void VerifyContentEpochs(SharedUiFixture& fixture, const std::vector<HalfPixel>& scene)
{
    auto& device = fixture.device;
    constexpr float sentinel[]{-2, 3, 4, 1};
    device.context->ClearRenderTargetView(fixture.composition.rtv.Get(), sentinel);
    const auto untouched = ReadComposition(device, fixture.composition);
    constexpr float outputSentinel[]{0.25F, 0.5F, 0.75F, 1};
    device.context->ClearRenderTargetView(fixture.output.rtv.Get(), outputSentinel);
    const auto untouchedOutput = ReadTexturePixels<UINT>(device, fixture.output);
    FO4HDR_WARP_CHECK(!fixture.Seed({1, 0, 203}));
    FO4HDR_WARP_CHECK(!fixture.Seed({2, 1, 203}));
    FO4HDR_WARP_CHECK(!fixture.Seed({1, 1, 0}));
    FO4HDR_WARP_CHECK(!fixture.Seed({1, 1, std::numeric_limits<float>::quiet_NaN()}));
    FO4HDR_WARP_CHECK(!fixture.Seed({1, 1, 203, UiPhase::unsupported}));
    FO4HDR_WARP_CHECK(!fixture.Encode(1));
    FO4HDR_WARP_CHECK(ReadComposition(device, fixture.composition) == untouched);
    FO4HDR_WARP_CHECK(ReadTexturePixels<UINT>(device, fixture.output) == untouchedOutput);
    FO4HDR_WARP_CHECK(fixture.Seed({1, 1, 203}));
    auto oracle = SeedOracle(scene, 203, true), ideal = SeedOracle(scene, 203, false);
    CheckTransport(device, fixture.composition, oracle, "203-nit seed");
    const auto seeded = ReadComposition(device, fixture.composition);
    FO4HDR_WARP_CHECK(Binary16ToFloat(seeded[4][1]) < 0);
    FO4HDR_WARP_CHECK(Binary16ToFloat(seeded[4][0]) > 1);
    FO4HDR_WARP_CHECK(!fixture.Background());
    FO4HDR_WARP_CHECK(!fixture.Encode(1));
    FO4HDR_WARP_CHECK(!fixture.Seed({1, 2, 100}));
    ExerciseUiSequence(fixture, oracle, ideal);
    const auto completed = ReadComposition(device, fixture.composition);
    double maximumMagnitude = 1;
    for (const auto& pixel : SeedOracle(scene, 203, false).pixels)
        for (UINT channel = 0; channel < 3; ++channel)
            maximumMagnitude = std::max(maximumMagnitude, std::abs(pixel[channel]));
    CheckIdealTransport(completed, ideal, maximumMagnitude);
    FO4HDR_WARP_CHECK(!fixture.Seed({1, 1, 100}));
    FO4HDR_WARP_CHECK(!fixture.UiDraw(UiPhase::panels, {}));
    FO4HDR_WARP_CHECK(!fixture.Encode(2));
    FO4HDR_WARP_CHECK(ReadTexturePixels<UINT>(device, fixture.output) == untouchedOutput);
    for (UINT repeat = 0; repeat < 3; ++repeat)
    {
        const auto before = fixture.finalDraws;
        FO4HDR_WARP_CHECK(fixture.Encode(1));
        FO4HDR_WARP_CHECK(fixture.finalDraws == before + 1);
        CheckFinal(fixture, completed);
        FO4HDR_WARP_CHECK(ReadComposition(device, fixture.composition) == completed);
    }
    FO4HDR_WARP_CHECK(fixture.content.white == 203);
    FO4HDR_WARP_CHECK(fixture.Seed({1, 2, 100}));
    oracle = SeedOracle(scene, 100, true);
    ideal = SeedOracle(scene, 100, false);
    CheckTransport(device, fixture.composition, oracle, "100-nit seed");
    FO4HDR_WARP_CHECK(ReadComposition(device, fixture.composition) != completed);
    ExerciseUiSequence(fixture, oracle, ideal);
    FO4HDR_WARP_CHECK(fixture.Encode(1));
    CheckFinal(fixture, ReadComposition(device, fixture.composition));
    const auto completedOutput = ReadTexturePixels<UINT>(device, fixture.output);
    ++fixture.generation;
    FO4HDR_WARP_CHECK(!fixture.Encode(1));
    FO4HDR_WARP_CHECK(!fixture.Encode(2));
    FO4HDR_WARP_CHECK(!fixture.UiDraw(UiPhase::cursor, {}));
    FO4HDR_WARP_CHECK(ReadTexturePixels<UINT>(device, fixture.output) == completedOutput);
    fixture.content = {};
    fixture.next = UiPhase::sceneReady;
    FO4HDR_WARP_CHECK(!fixture.Seed({1, 1, 203}));
    FO4HDR_WARP_CHECK(fixture.Seed({2, 1, 203}));
    CheckTransport(device, fixture.composition, SeedOracle(scene, 203, true),
        "new generation seed");
}

void VerifyBorrowingFailure(SharedUiFixture& fixture)
{
    auto& device = fixture.device;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = 64;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> buffer;
    CheckHresult(device.device->CreateBuffer(&description, nullptr, &buffer),
        "Create occupied SO fixture");
    auto* bound = buffer.Get();
    constexpr UINT offset = 0;
    device.context->SOSetTargets(1, &bound, &offset);
    FO4HDR_WARP_CHECK(fixture.seed.Seed(device.context.Get(), fixture.context1.Get(),
        {fixture.scene.srv.Get(), fixture.composition.rtv.Get(), {203, 203}}));
    ComPtr<ID3D11Buffer> retained;
    device.context->SOGetTargets(1, &retained);
    FO4HDR_WARP_CHECK(retained.Get() == buffer.Get());
    device.context->SOSetTargets(0, nullptr, nullptr);
    CheckTransport(device, fixture.composition,
        SeedOracle(ReadComposition(device, fixture.scene), 203, true), "seed with stream output");
    const auto before = ReadComposition(device, fixture.composition);
    FO4HDR_WARP_CHECK(fixture.next == UiPhase::nativeHud);
    auto shader = fixture.blendShader.pixel;
    fixture.blendShader.pixel.Reset();
    FO4HDR_WARP_CHECK(!fixture.UiDraw(UiPhase::nativeHud, {}));
    FO4HDR_WARP_CHECK(ReadComposition(device, fixture.composition) == before);
    fixture.blendShader.pixel = shader;

    D3D11_QUERY_DESC queryDescription{D3D11_QUERY_OCCLUSION, 0};
    ComPtr<ID3D11Query> query;
    CheckHresult(device.device->CreateQuery(&queryDescription, &query), "Create tracked UI query");
    const auto beforeOutput = ReadTexturePixels<UINT>(device, fixture.output);
    device.context->Begin(query.Get());
    fixture.queries.Begin(query.Get());
    FO4HDR_WARP_CHECK(!fixture.UiDraw(UiPhase::nativeHud, {}));
    FO4HDR_WARP_CHECK(!fixture.Seed({fixture.generation, 2, 203}));
    device.context->End(query.Get());
    fixture.queries.End(query.Get());
    FO4HDR_WARP_CHECK(fixture.queries.CanDispatch());
    FO4HDR_WARP_CHECK(ReadComposition(device, fixture.composition) == before);
    FO4HDR_WARP_CHECK(ReadTexturePixels<UINT>(device, fixture.output) == beforeOutput);
}

void VerifyFinalExtremes(SharedUiFixture& fixture)
{
    constexpr std::array<std::uint16_t, 9> values{0x0000, 0x8000, 0x0001, 0x3c00, 0xbc00,
        0x7bff, 0x7c00, 0xfc00, 0x7e00};
    std::vector<HalfPixel> pixels(kCompositionWidth * kCompositionHeight);
    for (std::size_t index = 0; index < pixels.size(); ++index)
        pixels[index] = {values[index % values.size()], values[index % values.size()],
            values[index % values.size()], 0x3c00};
    fixture.device.context->UpdateSubresource(fixture.composition.texture.Get(), 0, nullptr,
        pixels.data(), kCompositionWidth * sizeof(HalfPixel), 0);
    fixture.content = {fixture.generation, 2, 203};
    fixture.next = UiPhase::complete;
    FO4HDR_WARP_CHECK(fixture.Encode(fixture.generation));
    CheckFinal(fixture, pixels);
    FO4HDR_WARP_CHECK(ReadComposition(fixture.device, fixture.composition) == pixels);
}

UiPixel SceneTransportBudget(const UiPixel& encoded, float white)
{
    UiPixel error{};
    for (UINT channel = 0; channel < 3; ++channel)
    {
        const auto value = encoded[channel];
        const double step = 2 * Binary16LargestAdjacentStep(static_cast<float>(std::abs(value)));
        const auto reference = SignedSrgb(value, false);
        error[channel] = std::max(std::abs(SignedSrgb(value - step, false) - reference),
            std::abs(SignedSrgb(value + step, false) - reference)) * white;
    }
    return ToBt2020(error);
}

void CheckWhiteUnits(const std::vector<HalfPixel>& actual, const std::vector<HalfPixel>& scene,
    const UiOracle& ideal, float white)
{
    FO4HDR_WARP_CHECK(actual.front() == HalfPixel({0x3c00, 0x3c00, 0x3c00, 0x3c00}));
    for (std::size_t index = 1; index < actual.size(); ++index)
    {
        const auto decoded = DecodeNits(actual[index], white);
        const auto budget = SceneTransportBudget(ideal.pixels[index], white);
        for (UINT channel = 0; channel < 3; ++channel)
        {
            const double expected = Binary16ToFloat(scene[index][channel]) * 203.0;
            FO4HDR_WARP_CHECK(std::abs(decoded[channel] - expected) <= budget[channel] + 0.001);
        }
    }
}

void VerifyWhiteUnits(SharedUiFixture& fixture, const std::vector<HalfPixel>& scene)
{
    for (float white : {100.0F, 203.0F})
    {
        FO4HDR_WARP_CHECK(fixture.Seed({fixture.generation, fixture.content.revision + 1, white}));
        SharedUiConstants constants;
        constants.color = {1, 1, 1, 1};
        constants.rectangle = {0, 0, 1, 1};
        FO4HDR_WARP_CHECK(fixture.UiDraw(UiPhase::nativeHud, constants));
        FO4HDR_WARP_CHECK(fixture.Background());
        FO4HDR_WARP_CHECK(fixture.FinishPanels());
        constants.color = {0, 0, 0, 0};
        constants.rectangle = {0, 0, kCompositionWidth, kCompositionHeight};
        FO4HDR_WARP_CHECK(fixture.UiDraw(UiPhase::cursor, constants));
        const auto actual = ReadComposition(fixture.device, fixture.composition);
        CheckWhiteUnits(actual, scene, SeedOracle(scene, white, false), white);
        FO4HDR_WARP_CHECK(fixture.Encode(fixture.generation));
        CheckFinal(fixture, actual);
    }
}

void VerifyFinalStateRestoration(SharedUiFixture& fixture)
{
    auto& device = fixture.device;
    BorrowedFullscreenState restore(device.context.Get(), fixture.context1.Get(),
        fixture.composition.texture.Get());
    FO4HDR_WARP_CHECK(restore.Ready());
    const auto constants = CreateConstantBuffer(*device.device.Get(), std::array<float, 128>{});
    auto* cb = constants.Get();
    UINT first = 16, count = 16;
    fixture.context1->PSSetConstantBuffers1(0, 1, &cb, &first, &count);
    auto* srv = fixture.copy.srv.Get();
    device.context->PSSetShaderResources(0, 1, &srv);
    auto* rtv = fixture.composition.rtv.Get();
    device.context->OMSetRenderTargets(1, &rtv, nullptr);
    const auto before = ReadComposition(device, fixture.composition);
    FO4HDR_WARP_CHECK(fixture.Encode(fixture.generation));
    ComPtr<ID3D11RenderTargetView> actualRtv;
    device.context->OMGetRenderTargets(1, &actualRtv, nullptr);
    FO4HDR_WARP_CHECK(actualRtv.Get() == rtv);
    ComPtr<ID3D11ShaderResourceView> actualSrv;
    device.context->PSGetShaderResources(0, 1, &actualSrv);
    FO4HDR_WARP_CHECK(actualSrv.Get() == srv);
    ComPtr<ID3D11Buffer> actualConstants;
    UINT actualFirst{}, actualCount{};
    fixture.context1->PSGetConstantBuffers1(0, 1, &actualConstants, &actualFirst, &actualCount);
    FO4HDR_WARP_CHECK(actualConstants.Get() == cb && actualFirst == first && actualCount == count);
    FO4HDR_WARP_CHECK(ReadComposition(device, fixture.composition) == before);
    CheckFinal(fixture, before);
}

void VerifyVanillaVideoWhite(SharedUiFixture& fixture)
{
    std::vector<HalfPixel> pixels(kCompositionWidth * kCompositionHeight);
    constexpr std::array<float, 4> codes{0, 0.25F, 0.5F, 1};
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        const auto value = FloatToBinary16(codes[index % codes.size()]);
        pixels[index] = {value, value, value, FloatToBinary16(0.375F)};
    }
    auto& device = fixture.device;
    device.context->UpdateSubresource(fixture.scene.texture.Get(), 0, nullptr,
        pixels.data(), kCompositionWidth * sizeof(HalfPixel), 0);
    for (const float videoWhite : {100.0F, 203.0F, 300.0F})
    {
        FO4HDR_WARP_CHECK(fixture.seed.Seed(device.context.Get(), fixture.context1.Get(),
            {fixture.scene.srv.Get(), fixture.composition.rtv.Get(), {videoWhite, 203},
                SharedUiSeedMode::scaleVanillaSdr}));
        UiOracle oracle(kCompositionWidth, kCompositionHeight, true);
        const auto scale = std::pow(videoWhite / 203.0, 1.0 / 2.2);
        for (std::size_t index = 0; index < pixels.size(); ++index)
        {
            const auto code = Binary16ToFloat(pixels[index][0]) * scale;
            oracle.Store(index, {code, code, code, 0.375});
        }
        CheckTransport(device, fixture.composition, oracle, "vanilla video white");
        SharedUiConstants constants;
        constants.color = {1, 1, 1, 1};
        constants.rectangle = {0, 0, 1, 1};
        FO4HDR_WARP_CHECK(fixture.Draw(
            {fixture.blendShader, fixture.vertical, fixture.composition, constants}));
        const auto actual = ReadComposition(device, fixture.composition);
        FO4HDR_WARP_CHECK(actual.front() == HalfPixel({0x3c00, 0x3c00, 0x3c00, 0x3c00}));
        fixture.content = {fixture.generation, fixture.content.revision + 1, 203};
        fixture.next = UiPhase::complete;
        const auto before = fixture.finalDraws;
        FO4HDR_WARP_CHECK(fixture.Encode(fixture.generation, 2));
        FO4HDR_WARP_CHECK(fixture.finalDraws == before + 1);
        CheckFinal(fixture, actual, 2);
        FO4HDR_WARP_CHECK(ReadComposition(device, fixture.composition) == actual);
    }
}

}  // namespace

void VerifySharedUiComposition(Device& device)
{
    VerifyTransportTruncation();
    SharedUiFixture fixture(device);
    const auto scene = UploadScene(fixture);
    VerifySeedResourceValidation(fixture);
    VerifySeedDeviceIdentity(fixture);
    VerifyAliasedSeed(fixture, scene);
    VerifyContentEpochs(fixture, scene);
    VerifyBorrowingFailure(fixture);
    VerifyFinalExtremes(fixture);
    VerifyWhiteUnits(fixture, scene);
    VerifyFinalStateRestoration(fixture);
    VerifyVanillaVideoWhite(fixture);
    CheckDebugMessages(device);
}

}  // namespace fo4hdr::tests::gpu
