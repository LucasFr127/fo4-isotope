// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#include "ShaderTestSuite.hpp"
#include "d3d11/PresentationContextFixture.hpp"
#include "DxbcChecksum.hpp"
#include "FO4Isotope/ShippingShaders.generated.hpp"
#include "FO4Isotope/TestShaders.generated.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {

constexpr UINT kGrid = 8;
constexpr float kDiscarded = -1.0F;
using GridPixels = std::array<Float4, kGrid * kGrid>;

// b0 also feeds the synthetic stand-in for the game's shader, which reads its
// header, cb0[2] as its color and cb0[3] as its pixel rectangle.
struct BackdropQuadConstants
{
    std::array<std::uint32_t, 4> header{kWidth, kHeight, 0, 0};
    Float4 unused{};
    Float4 control{};
    std::array<Float4, 16> quads{};
    Float4 color{};
};
static_assert(sizeof(BackdropQuadConstants) == 320);

struct BackdropAlphaTestConstants
{
    std::array<Float4, 13> unused{};
    Float4 gain{};
    Float4 range{};
};
static_assert(sizeof(BackdropAlphaTestConstants) == 240);

struct BackdropCase
{
    std::string_view name;
    BackdropQuadConstants quads;
    float alphaScale{1}, alphaGain{1}, alphaMinimum{}, alphaMaximum{1};
};

// The game's arithmetic followed by the 8-bit conversion: NaN to zero, the rest to [0, 1].
float UnormOutput(float value)
{
    return std::isnan(value) ? 0.0F : std::clamp(value, 0.0F, 1.0F);
}

// The gradient reaches 0.0025 beyond the quad's top and bottom.
bool InGradientBand(float top, float bottom, float v)
{
    return v >= top - 0.0025F && bottom + 0.0025F >= v;
}

float BackdropCoverage(const BackdropCase& input, float u, float v, float& alpha)
{
    const auto& [count, falloff, linear, opacity] = input.quads.control;
    float coverage = 0.0F;
    for (int index = 0; index < static_cast<int>(count); ++index)
    {
        const auto& [x, top, right, bottom] = input.quads.quads[static_cast<std::size_t>(index)];
        const bool gradient = x < 0.0F;
        const float left = gradient ? -x : x;
        const float outside =
            std::fmax(std::fmax(top - v, v - bottom), std::fmax(left - u, u - right));
        coverage = std::fmax(coverage, 1.0F - falloff * outside);
        if (gradient && InGradientBand(top, bottom, v)) alpha *= (v - top) / (bottom - top);
    }
    return coverage;
}

Float4 ModelBackdrop(const BackdropCase& input, float u, float v, float uiAlpha)
{
    const float scaled = uiAlpha * input.alphaScale;
    const bool rejected = scaled * input.alphaGain - input.alphaMinimum < 0.0F ||
        (input.alphaMaximum < 1.0F && input.alphaMaximum - uiAlpha < 0.0F);
    if (rejected) return {kDiscarded, kDiscarded, kDiscarded, kDiscarded};
    float alpha = scaled * input.alphaGain;
    const float coverage = BackdropCoverage(input, u, v, alpha);
    const auto& control = input.quads.control;
    const auto channel = [&](float value) {
        return UnormOutput(control.blue != 0.0F ? value : std::pow(value, 2.2F));
    };
    const auto& color = input.quads.color;
    return {channel(color.red), channel(color.green), channel(color.blue),
        UnormOutput(alpha * UnormOutput(coverage) * control.alpha)};
}

// Column zero has no UI alpha, so 0 * Inf reaches the gradient in every row.
std::uint8_t UiAlphaByte(UINT x, UINT y)
{
    return x == 0 ? 0 : static_cast<std::uint8_t>(std::min(255U, x * 36 + y * 4));
}

struct BackdropShaders
{
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11SamplerState> sampler;
    Texture uiImage;

    explicit BackdropShaders(ID3D11Device& device) :
        vertex(CreateVertexShader(device, generated::test::kFullscreenUiBackdropVs)),
        pixel(CreatePixelShader(device, generated::kUiBackdrop)),
        sampler(CreateClampPointSampler(device))
    {
        std::array<std::uint32_t, kGrid * kGrid> texels{};
        for (UINT y = 0; y < kGrid; ++y)
            for (UINT x = 0; x < kGrid; ++x)
                texels[y * kGrid + x] = 0x00ffffffU | (std::uint32_t{UiAlphaByte(x, y)} << 24);
        const D3D11_SUBRESOURCE_DATA data{texels.data(), kGrid * sizeof(std::uint32_t), 0};
        uiImage = CreateSizedTexture(device,
            {kGrid, kGrid, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &data});
    }
};

struct BackdropBuffers
{
    ComPtr<ID3D11Buffer> quads, scale, test;

    BackdropBuffers(ID3D11Device& device, const BackdropCase& input) :
        quads(CreateConstantBuffer(device, input.quads)),
        scale(CreateConstantBuffer(device, Float4{0, 0, 0, input.alphaScale})),
        test(CreateConstantBuffer(device,
            BackdropAlphaTestConstants{.gain = {0, 0, 0, input.alphaGain},
                .range = {input.alphaMinimum, input.alphaMaximum, 0, 0}}))
    {
    }

    void Bind(ID3D11DeviceContext& context) const
    {
        const std::array buffers{quads.Get(), scale.Get(), test.Get()};
        context.PSSetConstantBuffers(0, static_cast<UINT>(buffers.size()), buffers.data());
    }
};

GridPixels ReadGrid(ID3D11DeviceContext& context, const Texture& target)
{
    context.CopyResource(target.staging.Get(), target.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(context.Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Map UI backdrop output");
    GridPixels pixels{};
    const auto* rows = static_cast<const std::byte*>(mapped.pData);
    for (UINT row = 0; row < kGrid; ++row)
        std::memcpy(&pixels[row * kGrid], rows + row * mapped.RowPitch, kGrid * sizeof(Float4));
    context.Unmap(target.staging.Get(), 0);
    return pixels;
}

GridPixels RenderBackdrop(Device& device, const BackdropShaders& shaders, const BackdropCase& input)
{
    auto& context = *device.context.Get();
    const BackdropBuffers buffers(*device.device.Get(), input);
    const auto target = CreateSizedTexture(*device.device.Get(),
        {kGrid, kGrid, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET});
    constexpr float sentinel[4]{kDiscarded, kDiscarded, kDiscarded, kDiscarded};
    context.ClearRenderTargetView(target.rtv.Get(), sentinel);
    const D3D11_VIEWPORT viewport{0, 0, kGrid, kGrid, 0, 1};
    auto* image = shaders.uiImage.srv.Get();
    auto* sampler = shaders.sampler.Get();
    auto* rtv = target.rtv.Get();
    context.IASetInputLayout(nullptr);
    context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context.RSSetViewports(1, &viewport);
    context.VSSetShader(shaders.vertex.Get(), nullptr, 0);
    context.PSSetShader(shaders.pixel.Get(), nullptr, 0);
    buffers.Bind(context);
    context.PSSetShaderResources(0, 1, &image);
    context.PSSetSamplers(0, 1, &sampler);
    context.OMSetRenderTargets(1, &rtv, nullptr);
    context.Draw(3, 0);
    context.ClearState();
    return ReadGrid(context, target);
}

void CheckBackdropPixel(const BackdropCase& input, const Float4& value, UINT x, UINT y)
{
    const auto expected = ModelBackdrop(input, (static_cast<float>(x) + 0.5F) / kGrid,
        (static_cast<float>(y) + 0.5F) / kGrid, static_cast<float>(UiAlphaByte(x, y)) / 255.0F);
    const std::array got{value.red, value.green, value.blue, value.alpha};
    const std::array want{expected.red, expected.green, expected.blue, expected.alpha};
    for (std::size_t channel = 0; channel < got.size(); ++channel)
    {
        // A NaN output fails this comparison.
        const bool close = std::fabs(got[channel] - want[channel]) <= 1.0e-5F;
        if (!close)
            std::cerr << "UI backdrop " << input.name << ": x=" << x << " y=" << y
                      << " channel=" << channel << " actual=" << got[channel]
                      << " expected=" << want[channel] << '\n';
        FO4HDR_WARP_CHECK(close);
    }
}

void CheckBackdropCase(const GridPixels& actual, const BackdropCase& input)
{
    for (UINT index = 0; index < kGrid * kGrid; ++index)
        CheckBackdropPixel(input, actual[index], index % kGrid, index / kGrid);
}

BackdropCase FiniteCase()
{
    BackdropCase input{.name = "finite"};
    input.quads.control = {2, 4, 0, 0.75F};
    input.quads.quads[0] = {0.125F, 0.125F, 0.625F, 0.5F};
    // A vertical gradient; row 2 lies just above its top, where the ratio is negative.
    input.quads.quads[1] = {-0.25F, 0.3135F, 0.875F, 0.8135F};
    input.quads.color = {0.5F, 0.25F, 1.0F, 0};
    return input;
}

BackdropCase DegenerateCase()
{
    BackdropCase input{.name = "degenerate", .alphaScale = 2};
    input.quads.control = {3, 1, 1, 1};
    input.quads.quads[0] = {0, 0, 1, 1};
    // Zero-height gradients: row 2 lies below the first (+Inf), row 4 above the second (-Inf).
    input.quads.quads[1] = {-0.5F, 0.3115F, 0.5F, 0.3115F};
    input.quads.quads[2] = {-0.5F, 0.5635F, 0.5F, 0.5635F};
    input.quads.color = {1.5F, -0.25F, 0.5F, 0};
    return input;
}

BackdropCase AlphaRangeCase()
{
    BackdropCase input{.name = "alpha-range", .alphaMinimum = 0.5F, .alphaMaximum = 0.75F};
    input.quads.control = {1, 2, 0, 1};
    input.quads.quads[0] = {0.25F, 0.25F, 0.75F, 0.75F};
    input.quads.color = {0.25F, 0.5F, 0.75F, 0};
    return input;
}

void VerifyUiBackdropArithmetic(Device& device)
{
    const BackdropShaders shaders(*device.device.Get());
    for (const auto& input : {FiniteCase(), DegenerateCase(), AlphaRangeCase()})
        CheckBackdropCase(RenderBackdrop(device, shaders, input), input);
}

// A downstream hook that binds the game's shader whatever FO4Isotope asks for.
struct RefusingSetter
{
    ContextRecord& record;
    PsSetShader next;
    ID3D11PixelShader* native;
    static inline RefusingSetter* active{};

    RefusingSetter(ContextRecord& value, ID3D11PixelShader* shader) :
        record(value), next(value.nextPsSetShader), native(shader)
    {
        active = this;
        record.nextPsSetShader = Invoke;
    }
    ~RefusingSetter()
    {
        record.nextPsSetShader = next;
        active = nullptr;
    }
    RefusingSetter(const RefusingSetter&) = delete;
    RefusingSetter& operator=(const RefusingSetter&) = delete;

    static void STDMETHODCALLTYPE Invoke(ID3D11DeviceContext* context, ID3D11PixelShader*,
        ID3D11ClassInstance* const* classes, UINT count)
    {
        active->next(context, active->native, classes, count);
    }
};

// The synthetic stand-in carries the game backdrop's identity and draws cb0[2].
struct BackdropSubstitutionFixture : SharedPresentationContextFixture
{
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> native;
    ComPtr<ID3D11SamplerState> sampler;
    Texture uiImage, offscreen;

    explicit BackdropSubstitutionFixture(Device& value) : SharedPresentationContextFixture(value)
    {
        auto& gpu = *device.device.Get();
        vertex = CreateVertexShader(gpu, generated::test::kFullscreenUiBackdropVs);
        native = CreatePixelShader(gpu, generated::test::kSharedUiSyntheticPs);
        CreatedShaderIdentity identity;
        identity.lateGameUiBackdrop = true;
        auto* raw = native.Get();
        identity.Register(S_OK, &raw);
        g_presentation.uiBackdropShader = CreatePixelShader(gpu, generated::kUiBackdrop);
        sampler = CreateClampPointSampler(gpu);
        std::array<std::uint32_t, kWidth> texels{};
        texels.fill(0xffffffffU);
        const D3D11_SUBRESOURCE_DATA data{texels.data(), kWidth * sizeof(std::uint32_t), 0};
        uiImage = CreateTexture(gpu, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &data);
        offscreen = CreateTexture(gpu, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET);
        PrepareScene();
    }

    // The registry keeps raw pointers, so the stand-in must leave it first.
    ~BackdropSubstitutionFixture() { ResetShaderRegistry(); }
    BackdropSubstitutionFixture(const BackdropSubstitutionFixture&) = delete;
    BackdropSubstitutionFixture& operator=(const BackdropSubstitutionFixture&) = delete;

    // Returns the first pixel the draw left in the target.
    Float4 Draw(const BackdropCase& input, const Texture& target)
    {
        BindHud();
        const BackdropBuffers buffers(*device.device.Get(), input);
        buffers.Bind(*device.context.Get());
        auto* image = uiImage.srv.Get();
        auto* state = sampler.Get();
        auto* rtv = target.rtv.Get();
        device.context->PSSetShaderResources(0, 1, &image);
        device.context->PSSetSamplers(0, 1, &state);
        device.context->OMSetRenderTargets(1, &rtv, nullptr);
        device.context->VSSetShader(vertex.Get(), nullptr, 0);
        device.context->PSSetShader(native.Get(), nullptr, 0);
        device.context->Draw(3, 0);
        ComPtr<ID3D11PixelShader> bound;
        device.context->PSGetShader(&bound, nullptr, nullptr);
        FO4HDR_WARP_CHECK(bound.Get() == native.Get());
        FO4HDR_WARP_CHECK(record->expectedBoundShader == native.Get());
        FO4HDR_WARP_CHECK(!record->currentReplacementActive);
        return ReadFirstPixel(target);
    }

    Float4 ReadFirstPixel(const Texture& target)
    {
        // Diagnostic readback stays outside the observed copy path.
        record->nextCopyResource(device.context.Get(), target.staging.Get(), target.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHresult(device.context->Map(target.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "Read UI backdrop target");
        std::array<std::uint16_t, 4> half{};
        std::memcpy(half.data(), mapped.pData, sizeof(half));
        device.context->Unmap(target.staging.Get(), 0);
        return {Binary16ToFloat(half[0]), Binary16ToFloat(half[1]), Binary16ToFloat(half[2]),
            Binary16ToFloat(half[3])};
    }
};

bool SamePixel(const Float4& actual, const Float4& expected)
{
    return actual.red == expected.red && actual.green == expected.green &&
        actual.blue == expected.blue && actual.alpha == expected.alpha;
}

void VerifyUiBackdropSubstitution(Device& device)
{
    BackdropCase input{.name = "substitution"};
    input.quads.control = {1, 0, 1, 1};
    input.quads.quads[0] = {0, 0, kWidth, kHeight};
    input.quads.color = {0.25F, 0.375F, 0.625F, 0};
    constexpr Float4 replaced{0.25F, 0.375F, 0.625F, 1};
    constexpr Float4 standIn{1, 0, 1, 1};
    BackdropSubstitutionFixture fixture(device);
    FO4HDR_WARP_CHECK(SamePixel(fixture.Draw(input, fixture.proxy), replaced));
    auto degenerate = input;
    degenerate.quads.control.red = 2;
    degenerate.quads.quads[1] = {-0.25F, 0.501F, 0.75F, 0.501F};
    constexpr Float4 contained{0.25F, 0.375F, 0.625F, 0};
    FO4HDR_WARP_CHECK(SamePixel(fixture.Draw(degenerate, fixture.proxy), contained));
    // Only the public image loses the 8-bit conversion; other targets keep the game's shader.
    FO4HDR_WARP_CHECK(SamePixel(fixture.Draw(input, fixture.offscreen), standIn));
    {
        const RefusingSetter setter(*fixture.record, fixture.native.Get());
        FO4HDR_WARP_CHECK(SamePixel(fixture.Draw(input, fixture.proxy), standIn));
    }
    g_presentation.uiBackdropShader.Reset();
    FO4HDR_WARP_CHECK(SamePixel(fixture.Draw(input, fixture.proxy), standIn));
}

DxbcChecksum EmbeddedChecksum(std::span<const std::byte> container)
{
    DxbcChecksum checksum{};
    std::memcpy(checksum.data(), container.data() + 4, sizeof(checksum));
    return checksum;
}

// fxc's own checksums, for containers ending in each final-block layout.
void CheckContainerChecksums()
{
    const std::array<std::span<const std::byte>, 6> containers{generated::kUiBackdrop,
        generated::kVideo, generated::kFxaa, generated::kToneMapA, generated::kToneMapBSdr,
        generated::test::kUiBackdropStrippedPs};
    bool shortTail{}, longTail{};
    for (const auto container : containers)
    {
        FO4HDR_WARP_CHECK(ComputeDxbcChecksum(container) == EmbeddedChecksum(container));
        if ((container.size() - 20) % 64 < 56) shortTail = true;
        else longTail = true;
    }
    FO4HDR_WARP_CHECK(shortTail && longTail);
}

// The game ships its backdrop compiled without reflection; a recompile that keeps
// it, as Community Shaders' does, reduces to the same checksum.
void CheckProgramChecksums()
{
    const std::span<const std::byte> full = generated::kUiBackdrop;
    const std::span<const std::byte> stripped = generated::test::kUiBackdropStrippedPs;
    FO4HDR_WARP_CHECK(ComputeDxbcProgramChecksum(full) == EmbeddedChecksum(stripped));
    FO4HDR_WARP_CHECK(EmbeddedChecksum(full) != EmbeddedChecksum(stripped));
    FO4HDR_WARP_CHECK(!ComputeDxbcProgramChecksum(stripped));
    FO4HDR_WARP_CHECK(!ComputeDxbcProgramChecksum(full.first(31)));
    std::vector<std::byte> damaged(full.begin(), full.end());
    damaged[0] = std::byte{'X'};
    FO4HDR_WARP_CHECK(!ComputeDxbcProgramChecksum(damaged));
    damaged.assign(full.begin(), full.end());
    const auto past = static_cast<std::uint32_t>(full.size());
    std::memcpy(damaged.data() + 32, &past, sizeof(past));
    FO4HDR_WARP_CHECK(!ComputeDxbcProgramChecksum(damaged));
}

}  // namespace

void VerifyUiBackdrop(Device& device)
{
    CheckContainerChecksums();
    CheckProgramChecksums();
    VerifyUiBackdropArithmetic(device);
    VerifyUiBackdropSubstitution(device);
}

}  // namespace fo4hdr::tests::gpu
