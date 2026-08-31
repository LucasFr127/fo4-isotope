// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "DeveloperExposureLock.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <span>

namespace fo4hdr::tests::gpu {
namespace {

using namespace detail;
constexpr Float4 exposureScene{2, 4, 1, 0.75f};
constexpr DeveloperExposureFrameQualification qualifiedExposureFrame{true, true, true, true};

struct NativeExposure
{
    float measured;
    Float4 bounds;
};
constexpr NativeExposure capturedExposure{0.5f, {4, 0.25f, 1.25f, 0}};
constexpr NativeExposure changedExposure{2, {8, 0.5f, 3, 0}};

ComPtr<ID3D11ShaderResourceView> ExposureView(ID3D11Device* device,
    D3D11_TEXTURE2D_DESC description, const D3D11_SUBRESOURCE_DATA& data)
{
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    CheckHresult(device->CreateTexture2D(&description, &data, &texture), "Exposure texture");
    CheckHresult(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "Exposure texture SRV");
    return view;
}

ComPtr<ID3D11ShaderResourceView> ExposureTexture(ID3D11Device* device, float measured)
{
    auto description = SceneDescription(1, 1);
    description.Format = DXGI_FORMAT_R32_FLOAT;
    const D3D11_SUBRESOURCE_DATA data{&measured, sizeof(measured), 0};
    return ExposureView(device, description, data);
}

ComPtr<ID3D11ShaderResourceView> ExposureAuxiliary(ID3D11Device* device, const Pixels& pixels)
{
    auto description = SceneDescription();
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), width * sizeof(Float4), 0};
    return ExposureView(device, description, data);
}

void CheckExposureWindow(ID3D11DeviceContext1& context, UINT slot, ID3D11Buffer* expected)
{
    ComPtr<ID3D11Buffer> buffer;
    UINT first{};
    UINT count{};
    context.PSGetConstantBuffers(slot, 1, &buffer);
    context.PSGetConstantBuffers1(slot, 1, nullptr, &first, &count);
    FO4HDR_CHECK(buffer.Get() == expected);
    FO4HDR_CHECK(first == 16);
    FO4HDR_CHECK(count == 16);
}

struct ToneExposureInputs
{
    PixelFixture& fixture;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3D11ShaderResourceView> bloom;
    ComPtr<ID3D11ShaderResourceView> mask;
    ComPtr<ID3D11ShaderResourceView> exposure;
    ComPtr<ID3D11Buffer> nativeConstants;
    ComPtr<ID3D11Buffer> borrowedSelector;

    explicit ToneExposureInputs(PixelFixture& owner) : fixture(owner)
    {
        CheckHresult(owner.gpu.context.As(&context), "Exposure context");
        bloom = ExposureAuxiliary(owner.gpu.device.Get(), Pixels{});
        Pixels tags{};
        tags[0].alpha = 4.0f / 255.0f;
        mask = ExposureAuxiliary(owner.gpu.device.Get(), tags);
        std::array<Float4, 32> borrowed{};
        borrowed[16] = {9, 8, 7, 6};
        borrowedSelector = ConstantBuffer(owner.gpu.device.Get(), borrowed);
    }

    void Bind(NativeExposure values)
    {
        fixture.Bind(true);
        FO4HDR_CHECK(fixture.renderer.InvalidateScene(fixture.input));
        Pixels scene;
        scene.fill(exposureScene);
        UploadScene(fixture.gpu.context.Get(), fixture.renderer.Views(fixture.input).texture, scene);
        exposure = ExposureTexture(fixture.gpu.device.Get(), values.measured);
        std::array<Float4, 32> constants{};
        constants[1] = {16, 8, 4, 0};
        const auto tone = ToneConstants();
        std::copy_n(tone.begin(), 5, constants.begin() + 16);
        constants[17] = values.bounds;
        nativeConstants = ConstantBuffer(fixture.gpu.device.Get(), constants);
        const UINT first = 16;
        const UINT count = 16;
        auto* native = nativeConstants.Get();
        auto* selector = borrowedSelector.Get();
        context->PSSetConstantBuffers1(2, 1, &native, &first, &count);
        context->PSSetConstantBuffers1(13, 1, &selector, &first, &count);
        const std::array<ID3D11ShaderResourceView*, 4> views{bloom.Get(),
            fixture.renderer.Views(fixture.input).shaderResource, exposure.Get(), mask.Get()};
        context->PSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
        auto* borrowed = bloom.Get();
        context->PSSetShaderResources(17, 1, &borrowed);
        const std::array<ID3D11SamplerState*, 4> samplers{fixture.point.Get(), fixture.point.Get(),
            fixture.point.Get(), fixture.point.Get()};
        context->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
        Check();
    }

    void Check()
    {
        CheckExposureWindow(*context.Get(), 2, nativeConstants.Get());
        CheckExposureWindow(*context.Get(), 13, borrowedSelector.Get());
        const std::array<ID3D11ShaderResourceView*, 4> expected{bloom.Get(),
            fixture.renderer.Views(fixture.input).shaderResource, exposure.Get(), mask.Get()};
        for (UINT slot = 0; slot < expected.size(); ++slot)
        {
            ComPtr<ID3D11ShaderResourceView> source;
            ComPtr<ID3D11SamplerState> sampler;
            context->PSGetShaderResources(slot, 1, &source);
            context->PSGetSamplers(slot, 1, &sampler);
            FO4HDR_CHECK(source.Get() == expected[slot]);
            FO4HDR_CHECK(sampler.Get() == fixture.point.Get());
        }
        ComPtr<ID3D11ShaderResourceView> borrowed;
        context->PSGetShaderResources(17, 1, &borrowed);
        FO4HDR_CHECK(borrowed.Get() == bloom.Get());
    }
};

struct ExposureComputeState
{
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Predicate> predicate;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11UnorderedAccessView> computeTarget;
    ComPtr<ID3D11UnorderedAccessView> outputTarget;
    UINT first{};
    UINT count{};
    BOOL predicateValue{};
    UINT classCount{256};

    explicit ExposureComputeState(ID3D11DeviceContext1& context)
    {
        std::array<ID3D11ClassInstance*, 256> classes{};
        context.CSGetShader(&shader, classes.data(), &classCount);
        FO4HDR_CHECK(classCount == 0);
        for (auto* instance : classes)
            if (instance) instance->Release();
        context.GetPredication(&predicate, &predicateValue);
        context.CSGetShaderResources(0, 1, &source);
        context.CSGetConstantBuffers(2, 1, &constants);
        context.CSGetConstantBuffers1(2, 1, nullptr, &first, &count);
        context.CSGetUnorderedAccessViews(0, 1, &computeTarget);
        context.OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &outputTarget);
    }

    void Check(ID3D11DeviceContext1& context) const
    {
        const ExposureComputeState actual(context);
        FO4HDR_CHECK(actual.shader.Get() == shader.Get());
        FO4HDR_CHECK(actual.classCount == classCount);
        FO4HDR_CHECK(actual.predicate.Get() == predicate.Get());
        FO4HDR_CHECK(actual.predicateValue == predicateValue);
        FO4HDR_CHECK(actual.source.Get() == source.Get());
        FO4HDR_CHECK(actual.constants.Get() == constants.Get());
        FO4HDR_CHECK(actual.first == first);
        FO4HDR_CHECK(actual.count == count);
        FO4HDR_CHECK(actual.computeTarget.Get() == computeTarget.Get());
        FO4HDR_CHECK(actual.outputTarget.Get() == outputTarget.Get());
    }
};

double ExposureGain(NativeExposure values)
{
    return std::clamp(static_cast<double>(values.bounds.blue) /
        (0.00100000005 + values.measured),
        static_cast<double>(values.bounds.green), static_cast<double>(values.bounds.red));
}

Float4 ExposureExpected(double gain, double measured)
{
    const std::array color{2 * gain, 4 * gain, gain};
    const auto luminance = color[0] * 0.212500006 + color[1] * 0.715399981 + color[2] * 0.0720999986;
    const std::array shape{0.5, 0.75, 1.25};
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel)
    {
        auto filtered = (color[channel] - luminance) * 0.75 + luminance;
        filtered += 0.25 * (luminance * shape[channel] - filtered);
        filtered = 0.5 * (1.5 * filtered - measured) + measured;
        result[channel] = static_cast<float>(filtered);
    }
    return {result[0], result[1], result[2], 1};
}

void CheckExposurePixels(PixelFixture& fixture, Float4 expected)
{
    const auto pixels = ReadScene(fixture.gpu, fixture.renderer.Views(fixture.destination).texture);
    CheckPixel(pixels[0], {exposureScene.red, exposureScene.green, exposureScene.blue, 1});
    for (std::size_t index = 1; index < pixels.size(); ++index) CheckPixel(pixels[index], expected);
}

std::uint64_t DrawExposureFrame(ToneExposureInputs& inputs, NativeExposure values, Float4 expected)
{
    auto& fixture = inputs.fixture;
    FO4HDR_CHECK(fixture.renderer.BeginFrame());
    const auto token = fixture.renderer.FrameGeneration();
    FO4HDR_CHECK(token != 0);
    inputs.Bind(values);
    const ExposureComputeState compute(*inputs.context.Get());
    FO4HDR_CHECK(DrawPass(fixture, 0, true) == SceneDrawOutcome::replaced);
    inputs.Check();
    compute.Check(*inputs.context.Get());
    CheckExposurePixels(fixture, expected);
    FO4HDR_CHECK(fixture.renderer.EndFrame());
    return token;
}

void VerifyExposureCapture(ToneExposureInputs& inputs)
{
    auto& renderer = inputs.fixture.renderer;
    FO4HDR_CHECK(renderer.SetExposureLockRequested(true));
    const auto expected = ExposureExpected(ExposureGain(capturedExposure), capturedExposure.measured);
    const auto capturedFrame = DrawExposureFrame(inputs, capturedExposure, expected);
    FO4HDR_CHECK(!renderer.ExposureLockActive());
    FO4HDR_CHECK(renderer.CompleteExposureFrame(capturedFrame, qualifiedExposureFrame));
    FO4HDR_CHECK(renderer.ExposureLockActive());
    // This affine filter retains measured reference, so replaying gain alone differs.
    const auto gainOnly = ExposureExpected(ExposureGain(capturedExposure), changedExposure.measured);
    FO4HDR_CHECK(std::abs(expected.red - gainOnly.red) > 0.5f);
    const auto replayFrame = DrawExposureFrame(inputs, changedExposure, expected);
    FO4HDR_CHECK(renderer.CompleteExposureFrame(replayFrame, qualifiedExposureFrame));
    FO4HDR_CHECK(renderer.ExposureLockActive());
}

void VerifyExposureRefusal(ToneExposureInputs& inputs, const std::filesystem::path& directory)
{
    auto& fixture = inputs.fixture;
    FO4HDR_CHECK(fixture.renderer.SetExposureLockRequested(false));
    FO4HDR_CHECK(!fixture.renderer.ExposureLockActive());
    FO4HDR_CHECK(fixture.renderer.SetExposureLockRequested(true));
    const auto bytes = ReadShader(directory / "ScenePassCompute.dxbc");
    ComPtr<ID3D11ComputeShader> foreign;
    CheckHresult(fixture.gpu.device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, &foreign),
        "Foreign exposure compute state");
    inputs.context->CSSetShader(foreign.Get(), nullptr, 0);
    const auto native = ExposureExpected(ExposureGain(changedExposure), changedExposure.measured);
    const auto frame = DrawExposureFrame(inputs, changedExposure, native);
    FO4HDR_CHECK(!fixture.renderer.ExposureLockActive());
    FO4HDR_CHECK(!fixture.renderer.CompleteExposureFrame(frame, {}));
    FO4HDR_CHECK(!fixture.renderer.ExposureLockActive());
    inputs.context->CSSetShader(nullptr, nullptr, 0);
}

void VerifyExposurePolicy()
{
    DeveloperExposureLock policy;
    policy.Request(true);
    policy.Capture(10, 20);
    policy.EndFrame(10, 20, true);
    FO4HDR_CHECK(policy.Active());
    policy.Observe(11, 21);
    FO4HDR_CHECK(!policy.Active());
    FO4HDR_CHECK(!policy.Requested());
    FO4HDR_CHECK(!policy.NeedsCapture());
}

}  // namespace

void VerifyDeveloperExposure(const std::filesystem::path& fixtureDirectory)
{
    VerifyExposurePolicy();
    PixelFixture fixture(fixtureDirectory);
    ToneExposureInputs inputs(fixture);
    VerifyExposureCapture(inputs);
    VerifyExposureRefusal(inputs, fixtureDirectory);
}

}  // namespace fo4hdr::tests::gpu
