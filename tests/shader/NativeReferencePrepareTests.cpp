// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "TestHarness.hpp"
#include "RenoDxShaderIdentity.hpp"

#include <array>
#include <cstdint>
#include <filesystem>

namespace fo4hdr::tests::gpu {
namespace {

using namespace detail;

ComPtr<ID3D11ShaderResourceView> NativeReferenceFixtureLook(ID3D11Device* device)
{
    std::array<std::uint32_t, 16 * 16 * 16> codes{};
    codes.fill(0xFF604020);
    D3D11_TEXTURE3D_DESC description{};
    description.Width = description.Height = description.Depth = 16;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{codes.data(), 16 * sizeof(std::uint32_t), 256 * sizeof(std::uint32_t)};
    ComPtr<ID3D11Texture3D> texture;
    CheckHresult(device->CreateTexture3D(&description, &data, &texture), "Native reference fixture LUT");
    ComPtr<ID3D11ShaderResourceView> view;
    CheckHresult(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "Native reference fixture view");
    return view;
}

void CheckNativeReferenceShader(ID3D11DeviceContext1* context, ID3D11ComputeShader* shader,
    ID3D11ClassInstance* instance)
{
    ComPtr<ID3D11ComputeShader> actualShader;
    ID3D11ClassInstance* rawClass{};
    UINT count = 1;
    context->CSGetShader(&actualShader, &rawClass, &count);
    ComPtr<ID3D11ClassInstance> actualClass;
    actualClass.Attach(rawClass);
    FO4HDR_CHECK(actualShader.Get() == shader && count == 1 && actualClass.Get() == instance);
}

void CheckNativeReferenceResources(ID3D11DeviceContext1* context,
    ID3D11ShaderResourceView* source, ID3D11Buffer* buffer)
{
    for (UINT slot = 0; slot < 4; ++slot)
    {
        ComPtr<ID3D11ShaderResourceView> actual;
        context->CSGetShaderResources(slot, 1, &actual);
        FO4HDR_CHECK(actual.Get() == source);
    }
    ComPtr<ID3D11UnorderedAccessView> unordered;
    context->CSGetUnorderedAccessViews(0, 1, &unordered);
    FO4HDR_CHECK(!unordered);
    ComPtr<ID3D11Buffer> actualBuffer;
    UINT first{};
    UINT constants{};
    context->CSGetConstantBuffers1(2, 1, &actualBuffer, &first, &constants);
    FO4HDR_CHECK(actualBuffer.Get() == buffer && first == 16 && constants == 16);
}

constexpr Float4 referenceScene{2, 4, 1, 0.75f};

struct ReferenceDraw
{
    UINT calls{};
    UINT classes{};
    ComPtr<ID3D11PixelShader> shader;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11Buffer> constants;

    static void Draw(ID3D11DeviceContext* context, void* argument)
    {
        auto& evidence = *static_cast<ReferenceDraw*>(argument);
        ++evidence.calls;
        std::array<ID3D11ClassInstance*, 256> instances{};
        evidence.classes = static_cast<UINT>(instances.size());
        context->PSGetShader(&evidence.shader, instances.data(), &evidence.classes);
        for (auto* instance : instances) if (instance) instance->Release();
        context->PSGetShaderResources(3, 1, &evidence.source);
        context->PSGetConstantBuffers(13, 1, &evidence.constants);
        context->Draw(3, 0);
    }
};

struct ReferenceBindings
{
    ComPtr<ID3D11Buffer> weights;
    ComPtr<ID3D11Buffer> selector;

    ReferenceBindings(PixelFixture& fixture, ID3D11ShaderResourceView* look)
    {
        fixture.Bind(true);
        Pixels input;
        input.fill(referenceScene);
        FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
        UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, input);
        std::array<ID3D11ShaderResourceView*, 7> views{};
        views[0] = fixture.renderer.Views(fixture.input).shaderResource;
        for (std::size_t slot = 3; slot < views.size(); ++slot) views[slot] = look;
        fixture.gpu.context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
        const std::array<ID3D11SamplerState*, 7> samplers{fixture.point.Get(), nullptr, nullptr,
            fixture.linear.Get(), fixture.linear.Get(), fixture.linear.Get(), fixture.linear.Get()};
        fixture.gpu.context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
        std::array<Float4, 32> values{};
        values[17] = {0.25f, 0.5f, 0.75f, 0.125f};
        weights = ConstantBuffer(fixture.gpu.device.Get(), values);
        selector = ConstantBuffer(fixture.gpu.device.Get(), values);
        ComPtr<ID3D11DeviceContext1> context;
        CheckHresult(fixture.gpu.context.As(&context), "Reference draw Context1");
        auto* weightBuffer = weights.Get();
        auto* selectorBuffer = selector.Get();
        constexpr UINT first = 16;
        constexpr UINT count = 16;
        context->PSSetConstantBuffers1(2, 1, &weightBuffer, &first, &count);
        context->PSSetConstantBuffers1(13, 1, &selectorBuffer, &first, &count);
    }
};

void CheckReferenceWindow(ID3D11DeviceContext1* context, UINT slot, ID3D11Buffer* expected)
{
    ComPtr<ID3D11Buffer> actual;
    UINT first{};
    UINT count{};
    context->PSGetConstantBuffers(slot, 1, &actual);
    context->PSGetConstantBuffers1(slot, 1, nullptr, &first, &count);
    FO4HDR_CHECK(actual.Get() == expected);
    FO4HDR_CHECK(first == 16 && count == 16);
}

ReferenceDraw DrawReference(PixelFixture& fixture, ID3D11ShaderResourceView* look, Float4 expected)
{
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    const ReferenceBindings borrowed(fixture, look);
    ReferenceDraw evidence;
    FO4HDR_CHECK(fixture.renderer.Execute(kRenoDxFallout4Shaders[4], fixture.input,
        fixture.destination, {ReferenceDraw::Draw, &evidence}) == SceneDrawOutcome::replaced);
    FO4HDR_CHECK(evidence.calls == 1 && evidence.classes == 0);
    FO4HDR_CHECK(evidence.shader && evidence.shader.Get() != fixture.sentinel.Get());
    FO4HDR_CHECK(evidence.constants && evidence.constants.Get() != borrowed.selector.Get());
    fixture.CheckRestored(true);
    ComPtr<ID3D11DeviceContext1> context;
    CheckHresult(fixture.gpu.context.As(&context), "Reference restored Context1");
    CheckReferenceWindow(context.Get(), 2, borrowed.weights.Get());
    CheckReferenceWindow(context.Get(), 13, borrowed.selector.Get());
    ComPtr<ID3D11ShaderResourceView> restored;
    context->PSGetShaderResources(3, 1, &restored);
    FO4HDR_CHECK(restored.Get() == look);
    const auto pixels = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    for (const auto pixel : pixels) CheckPixel(pixel, expected);
    FO4HDR_CHECK(fixture.renderer.EndFrame());
    return evidence;
}

void CheckPreparedReference(ID3D11ShaderResourceView* source)
{
    FO4HDR_CHECK(source);
    if (!source) return;
    ComPtr<ID3D11Resource> resource;
    source->GetResource(&resource);
    ComPtr<ID3D11Texture3D> texture;
    CheckHresult(resource.As(&texture), "Prepared reference texture");
    D3D11_TEXTURE3D_DESC description{};
    texture->GetDesc(&description);
    FO4HDR_CHECK(description.Width == 20);
    FO4HDR_CHECK(description.Height == 16 && description.Depth == 16);
    FO4HDR_CHECK(description.Format == DXGI_FORMAT_R32G32B32A32_FLOAT);
}

void VerifyNativeReferenceDraws(const std::filesystem::path& directory)
{
    const SceneGradeConfiguration grade{1, {}, ColorGradingMethod::nativeReference};
    PixelFixture fixture(directory, width, height, {}, nullptr, grade);
    auto look = NativeReferenceFixtureLook(fixture.gpu.device.Get());
    // A constant look has zero neutral/chroma secants and returns its decoded black sample.
    const Float4 endpoint{static_cast<float>(DecodeAuthoredSrgb(32.0 / 255 * 1.625)),
        static_cast<float>(DecodeAuthoredSrgb(64.0 / 255 * 1.625)),
        static_cast<float>(DecodeAuthoredSrgb(96.0 / 255 * 1.625)), referenceScene.alpha};
    const auto prepared = DrawReference(fixture, look.Get(), endpoint);
    FO4HDR_CHECK(prepared.source.Get() != look.Get());
    CheckPreparedReference(prepared.source.Get());
    std::array<LutPixels, 4> fallback;
    for (auto& pixels : fallback) pixels.assign(lutSize * lutSize * lutSize, {0.125f, 0.25f, 0.375f, 1});
    auto floating = LutTexture(fixture.gpu.device.Get(), fallback[0]);
    const auto radial = DrawReference(fixture, floating.Get(), LutExpected(referenceScene, fallback));
    FO4HDR_CHECK(radial.source.Get() == floating.Get());
    FO4HDR_CHECK(radial.shader.Get() != prepared.shader.Get());
    const SceneGradeConfiguration zero{0, {}, ColorGradingMethod::nativeReference};
    PixelFixture bypass(directory, width, height, {}, nullptr, zero);
    auto unsupported = LutTexture(bypass.gpu.device.Get(), fallback[0]);
    const auto ungraded = DrawReference(bypass, unsupported.Get(), referenceScene);
    FO4HDR_CHECK(ungraded.source.Get() == unsupported.Get());
}
}  // namespace

void VerifyNativeReferencePreparation(const std::filesystem::path& directory)
{
    PixelFixture fixture{directory};
    ComPtr<ID3D11DeviceContext1> context;
    CheckHresult(fixture.gpu.context.As(&context), "Native reference Context1");
    auto look = NativeReferenceFixtureLook(fixture.gpu.device.Get());
    const std::array<ID3D11ShaderResourceView*, 4> views{look.Get(), look.Get(), look.Get(), look.Get()};
    context->PSSetShaderResources(3, 4, views.data());
    context->CSSetShaderResources(0, 4, views.data());
    std::array<Float4, 32> values{};
    values[17] = {1.0F, 0.0F, 0.0F, 0.0F};
    auto constants = ConstantBuffer(fixture.gpu.device.Get(), values);
    auto* buffer = constants.Get();
    constexpr UINT first = 16;
    constexpr UINT count = 16;
    context->PSSetConstantBuffers1(2, 1, &buffer, &first, &count);
    context->CSSetConstantBuffers1(2, 1, &buffer, &first, &count);
    ComPtr<ID3D11ClassLinkage> linkage;
    CheckHresult(fixture.gpu.device->CreateClassLinkage(&linkage), "Reference compute linkage");
    const auto bytecode = ReadShader(directory / "ScenePassCompute.dxbc");
    ComPtr<ID3D11ComputeShader> shader;
    CheckHresult(fixture.gpu.device->CreateComputeShader(bytecode.data(), bytecode.size(),
        linkage.Get(), &shader), "Reference compute sentinel");
    ComPtr<ID3D11ClassInstance> instance;
    CheckHresult(linkage->CreateClassInstance("SentinelColor", 0, 0, 0, 0, &instance), "Reference compute class");
    auto* rawClass = instance.Get();
    context->CSSetShader(shader.Get(), &rawClass, 1);
    context->SetPredication(nullptr, TRUE);
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    FO4HDR_CHECK(!fixture.renderer.PrepareNativeReferenceLook(NativeReferencePreparationScope::unavailable));
    FO4HDR_CHECK(fixture.renderer.PrepareNativeReferenceLook(NativeReferencePreparationScope::qualifiedExclusive));
    CheckNativeReferenceShader(context.Get(), shader.Get(), instance.Get());
    CheckNativeReferenceResources(context.Get(), look.Get(), buffer);
    ComPtr<ID3D11ShaderResourceView> pixelLook;
    context->PSGetShaderResources(3, 1, &pixelLook);
    FO4HDR_CHECK(pixelLook.Get() == look.Get());
    FO4HDR_CHECK(fixture.renderer.EndFrame());
    FO4HDR_CHECK(!fixture.renderer.PrepareNativeReferenceLook(NativeReferencePreparationScope::qualifiedExclusive));
    FO4HDR_CHECK(fixture.renderer.Retire());
    FO4HDR_CHECK(!fixture.renderer.PrepareNativeReferenceLook(NativeReferencePreparationScope::qualifiedExclusive));
    VerifyNativeReferenceDraws(directory);
}

}  // namespace fo4hdr::tests::gpu
