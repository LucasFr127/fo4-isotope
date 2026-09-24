// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ShaderTestSuite.hpp"
#include "TestHarness.hpp"
#include <cstring>
#include <filesystem>
#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/output/Hdr10Color.hpp"
#include <d3d11.h>
#include <wrl/client.h>

#include "Fallout4HDR/tonemap/Aces2.hpp"
#include "Fallout4HDR/tonemap/Agx.hpp"
#include "Fallout4HDR/tonemap/Gt7.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"
#include "Fallout4HDR/TestShaders.generated.hpp"
#include "reference/OperatorLutTestSupport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Microsoft::WRL::ComPtr;
using Algorithm = fo4hdr::ProductionToneMapperAlgorithm;
using Pixel = std::array<float, 4>;
constexpr UINT kWidth = 256;

void Require(HRESULT result, const char *operation,
             const std::source_location location = std::source_location::current())
{
    if (FAILED(result)) {
        throw std::runtime_error(std::string(location.file_name()) + ':' + std::to_string(location.line()) + ": " +
                                 operation + " failed: " + std::to_string(result));
    }
}

const char *Name(Algorithm algorithm)
{
    switch (algorithm) {
    case Algorithm::aces2Lmt:
        return "ACES2LMT";
    case Algorithm::agX:
        return "AgX";
    case Algorithm::gt7:
        return "GT7";
    }
    return "unknown";
}

bool IsAces2Family(Algorithm algorithm)
{
    return algorithm == Algorithm::aces2Lmt;
}

UINT ProductionMode(Algorithm algorithm)
{
    switch (algorithm) {
    case Algorithm::aces2Lmt:
        return 1U;
    case Algorithm::agX:
        return 2U;
    case Algorithm::gt7:
        return 3U;
    }
    return 0U;
}

struct BatchConstants
{
    UINT width;
    UINT count;
    UINT mode;
    float shaperAngleGain;
    float shaperBias;
    float displayPeakNits;
    std::array<float, 2> shaperParameters{};
};
static_assert(sizeof(BatchConstants) == 32);

struct BatchTextureSpec
{
    UINT height, bindFlags;
    const Pixel *initialData = nullptr;
    bool staging = false;
};

ComPtr<ID3D11Texture2D> CreateBatchTexture(ID3D11Device &device, const BatchTextureSpec &spec)
{
    const auto &[height, bindFlags, initialData, staging] = spec;
    D3D11_TEXTURE2D_DESC descriptor{};
    descriptor.Width = kWidth;
    descriptor.Height = height;
    descriptor.MipLevels = 1;
    descriptor.ArraySize = 1;
    descriptor.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    descriptor.SampleDesc.Count = 1;
    descriptor.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    descriptor.BindFlags = bindFlags;
    descriptor.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = initialData;
    data.SysMemPitch = kWidth * sizeof(Pixel);
    ComPtr<ID3D11Texture2D> texture;
    Require(device.CreateTexture2D(&descriptor, initialData != nullptr ? &data : nullptr, &texture),
            "Create float32 operator test batch");
    return texture;
}

struct OperatorCorpus
{
    std::vector<fo4hdr::LinearRgb> corpus;
    std::size_t broadEnd{}, holdoutBegin{}, holdoutEnd{}, darkBegin{}, darkEnd{}, neutralBegin{}, neutralEnd{};
    UINT height{};
    std::vector<Pixel> pixels;

    void AppendNeutral()
    {
        constexpr std::size_t neutralSampleCount{4096};
        neutralBegin = corpus.size();
        const auto maximumInputEv = std::log2(fo4hdr::kOperatorLutSceneMaximum);
        for (std::size_t sampleIndex = 0; sampleIndex < neutralSampleCount; ++sampleIndex) {
            const auto fraction = static_cast<double>(sampleIndex) / static_cast<double>(neutralSampleCount - 1U);
            const auto value =
                static_cast<double>(static_cast<float>(std::exp2(-24.0 + (maximumInputEv + 24.0) * fraction)));
            corpus.push_back({value, value, value});
        }
        neutralEnd = corpus.size();
    }

    void AppendHostile()
    {
        constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
        constexpr auto infinity = std::numeric_limits<float>::infinity();
        for (const fo4hdr::LinearRgb input :
             {fo4hdr::LinearRgb{nan, -infinity, infinity}, fo4hdr::LinearRgb{-1.0, 0.18, 1.0},
              fo4hdr::LinearRgb{infinity, 0.0, 0.0}, fo4hdr::LinearRgb{0.0, infinity, 0.0},
              fo4hdr::LinearRgb{0.0, 0.0, infinity}})
            corpus.push_back(input);
    }

    void PreparePixels()
    {
        if (corpus.empty() || corpus.size() > kWidth * D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            throw std::runtime_error("Operator corpus cannot fit the test texture");
        }
        height = static_cast<UINT>((corpus.size() + kWidth - 1) / kWidth);
        pixels.resize(static_cast<std::size_t>(kWidth) * height);
        for (std::size_t index = 0; index < corpus.size(); ++index) {
            pixels[index] = {static_cast<float>(corpus[index].red), static_cast<float>(corpus[index].green),
                             static_cast<float>(corpus[index].blue), 1.0F};
        }
    }

    explicit OperatorCorpus(double peak)
    {
        corpus = fo4hdr::test::BuildOperatorLutCorpus(peak);
        broadEnd = corpus.size();
        const auto holdout = fo4hdr::test::BuildOperatorLutHoldoutCorpus();
        holdoutBegin = corpus.size();
        corpus.insert(corpus.end(), holdout.begin(), holdout.end());
        holdoutEnd = corpus.size();
        if (broadEnd == 0U || holdout.size() != 32768U)
            throw std::runtime_error("Operator LUT validation corpus is incomplete");
        const auto darkCorpus = fo4hdr::test::BuildNearBlackOperatorCorpus();
        darkBegin = corpus.size();
        corpus.insert(corpus.end(), darkCorpus.begin(), darkCorpus.end());
        darkEnd = corpus.size();
        AppendNeutral();
        AppendHostile();
        PreparePixels();
    }
};

struct OperatorSample
{
    std::size_t index;
    const Pixel &actual;
    fo4hdr::LinearRgb scene, expected;
    std::array<double, 3> expectedChannels, sampledChannels;
};

struct OperatorVerdict
{
    double maximumBound, broadMeanError, holdoutMeanError, meanBound, darkBound;
    bool passed;
};

struct OperatorMeasurements
{
    const OperatorCorpus &dataset;
    Algorithm algorithm;
    double peak;
    const fo4hdr::ToneMapperParameters &parameters;
    const fo4hdr::Aces2TransformHandle &aces;
    const fo4hdr::OperatorLutData &lut;
    double maximumError{};
    double maximumApproximationError{};
    double broadErrorSum{};
    double holdoutErrorSum{};
    double maximumSamplerError{};
    double darkApproximationCodes{};
    double darkSamplerCodes{};
    double darkOutputCodes{};
    std::array<double, 3> previousNeutralPqCodes{};
    std::array<double, 3> runningNeutralMaximumPqCodes{};
    std::array<double, 3> runningNeutralMaximumInputs{};
    std::array<fo4hdr::LinearRgb, 3> runningNeutralMaximumOutputs{};
    std::array<fo4hdr::LinearRgb, 3> runningNeutralMaximumReferences{};
    double maximumNeutralTintPqCodes{};
    double maximumNeutralAdjacentDecreasePqCodes{};
    double maximumNeutralDrawdownPqCodes{};
    std::size_t worstNeutralDrawdownChannel{};
    double worstNeutralDrawdownMaximumInput{};
    double worstNeutralDrawdownCurrentInput{};
    fo4hdr::LinearRgb worstNeutralDrawdownMaximumOutput{};
    fo4hdr::LinearRgb worstNeutralDrawdownCurrentOutput{};
    fo4hdr::LinearRgb worstNeutralDrawdownMaximumReference{};
    fo4hdr::LinearRgb worstNeutralDrawdownCurrentReference{};
    std::size_t worstIndex{};
    std::size_t worstChannel{};
    double worstActual{};
    double worstExpected{};
    bool contained = true;

    void MeasureDark(const OperatorSample &sample, std::size_t channel)
    {
        const auto &[index, actual, scene, expected, expectedChannels, sampledChannels] = sample;
        const auto darkBegin = dataset.darkBegin, darkEnd = dataset.darkEnd;
        if (index >= darkBegin && index < darkEnd) {
            const auto actualPq = fo4hdr::EncodeSt2084Pq(actual[channel]);
            const auto sampledPq = fo4hdr::EncodeSt2084Pq(sampledChannels[channel]);
            const auto expectedPq = fo4hdr::EncodeSt2084Pq(expectedChannels[channel]);
            darkApproximationCodes = std::max(darkApproximationCodes, 1023.0 * std::abs(sampledPq - expectedPq));
            darkSamplerCodes = std::max(darkSamplerCodes, 1023.0 * std::abs(actualPq - sampledPq));
            darkOutputCodes = std::max(darkOutputCodes, 1023.0 * std::abs(actualPq - expectedPq));
        }
    }

    void AccumulateRegionError(std::size_t index, double error)
    {
        const auto broadEnd = dataset.broadEnd, holdoutBegin = dataset.holdoutBegin, holdoutEnd = dataset.holdoutEnd;
        if (index < broadEnd)
            broadErrorSum += error;
        else if (index >= holdoutBegin && index < holdoutEnd)
            holdoutErrorSum += error;
    }

    void MeasureChannel(const OperatorSample &sample, std::size_t channel)
    {
        const auto &[index, actual, scene, expected, expectedChannels, sampledChannels] = sample;

        contained = contained && std::isfinite(actual[channel]) && actual[channel] >= 0.0F && actual[channel] <= peak;
        const auto error = std::abs(actual[channel] - expectedChannels[channel]);
        const auto approximationError = std::abs(sampledChannels[channel] - expectedChannels[channel]);
        MeasureDark(sample, channel);
        AccumulateRegionError(index, error);
        maximumApproximationError = std::max(maximumApproximationError, approximationError);
        maximumSamplerError = std::max(maximumSamplerError, std::abs(actual[channel] - sampledChannels[channel]));
        if (error > maximumError) {
            maximumError = error;
            worstIndex = index;
            worstChannel = channel;
            worstActual = actual[channel];
            worstExpected = expectedChannels[channel];
        }
    }

    void MeasureTint(const std::array<double, 3> &expectedPqCodes, const std::array<double, 3> &actualPqCodes)
    {
        for (std::size_t first = 0; first < 3U; ++first) {
            for (std::size_t second = first + 1U; second < 3U; ++second) {
                const auto sourceDifference = expectedPqCodes[first] - expectedPqCodes[second];
                const auto sampledDifference = actualPqCodes[first] - actualPqCodes[second];
                maximumNeutralTintPqCodes =
                    std::max(maximumNeutralTintPqCodes, std::abs(sampledDifference - sourceDifference));
            }
        }
    }

    void MeasureNeutral(const OperatorSample &sample)
    {
        const auto &[index, actual, scene, expected, expectedChannels, sampledChannels] = sample;
        const auto neutralBegin = dataset.neutralBegin;

        std::array<double, 3> expectedPqCodes{};
        std::array<double, 3> actualPqCodes{};
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            expectedPqCodes[channel] = 1023.0 * fo4hdr::EncodeSt2084Pq(expectedChannels[channel]);
            actualPqCodes[channel] = 1023.0 * fo4hdr::EncodeSt2084Pq(actual[channel]);
            if (index != neutralBegin)
                maximumNeutralAdjacentDecreasePqCodes = std::max(
                    maximumNeutralAdjacentDecreasePqCodes, previousNeutralPqCodes[channel] - actualPqCodes[channel]);
            previousNeutralPqCodes[channel] = actualPqCodes[channel];
            if (index == neutralBegin || actualPqCodes[channel] > runningNeutralMaximumPqCodes[channel]) {
                runningNeutralMaximumPqCodes[channel] = actualPqCodes[channel];
                runningNeutralMaximumInputs[channel] = scene.red;
                runningNeutralMaximumOutputs[channel] = {actual[0], actual[1], actual[2]};
                runningNeutralMaximumReferences[channel] = expected;
            }
            const auto drawdown = runningNeutralMaximumPqCodes[channel] - actualPqCodes[channel];
            if (drawdown > maximumNeutralDrawdownPqCodes) {
                maximumNeutralDrawdownPqCodes = drawdown;
                worstNeutralDrawdownChannel = channel;
                worstNeutralDrawdownMaximumInput = runningNeutralMaximumInputs[channel];
                worstNeutralDrawdownCurrentInput = scene.red;
                worstNeutralDrawdownMaximumOutput = runningNeutralMaximumOutputs[channel];
                worstNeutralDrawdownCurrentOutput = {actual[0], actual[1], actual[2]};
                worstNeutralDrawdownMaximumReference = runningNeutralMaximumReferences[channel];
                worstNeutralDrawdownCurrentReference = expected;
            }
        }
        MeasureTint(expectedPqCodes, actualPqCodes);
    }

    fo4hdr::LinearRgb Reference(const fo4hdr::LinearRgb &scene) const
    {
        return algorithm == Algorithm::aces2Lmt
                   ? fo4hdr::MapAces2LmtToBt2020Nits(*aces, scene, parameters.acesLookStrength)
                   : (algorithm == Algorithm::agX ? fo4hdr::MapAgxToBt2020Nits(scene, peak, parameters)
                                                  : fo4hdr::MapGt7ToBt2020Nits(scene, peak, parameters));
    }

    void Measure(const D3D11_MAPPED_SUBRESOURCE &mapped)
    {
        const auto &[corpus, broadEnd, holdoutBegin, holdoutEnd, darkBegin, darkEnd, neutralBegin, neutralEnd, height,
                     pixels] = dataset;
        for (std::size_t index = 0; index < corpus.size(); ++index) {
            const auto *row = static_cast<const std::byte *>(mapped.pData) + (index / kWidth) * mapped.RowPitch;
            const auto &actual = reinterpret_cast<const Pixel *>(row)[index % kWidth];
            // Match the float32 input actually seen by HLSL, not a different
            // double-precision color or a rounded FP16 intermediate.
            const fo4hdr::LinearRgb scene{pixels[index][0], pixels[index][1], pixels[index][2]};
            const auto expected = Reference(scene);
            const std::array expectedChannels{expected.red, expected.green, expected.blue};
            // Compare exact uploaded texels separately from approximation of the
            // analytical operator; neither creative grading nor UI can mask it.
            const auto sampled = fo4hdr::test::SampleOperatorLut(lut, scene);
            const std::array sampledChannels{sampled.red, sampled.green, sampled.blue};
            contained = contained && actual[3] == 1.0F;
            const OperatorSample sample{index, actual, scene, expected, expectedChannels, sampledChannels};
            for (std::size_t channel = 0; channel < 3; ++channel)
                MeasureChannel(sample, channel);
            if (index >= neutralBegin && index < neutralEnd)
                MeasureNeutral(sample);
        }
    }

    // The CPU lattice limit is 1.25; allow a quarter code for the sampler's
    // finite subtexel interpolation precision, independently of LUT error.
    static constexpr double neutralTintBoundPqCodes{1.5};
    // Hardware filtering adds subtexel rounding to reconstruction jitter. The
    // separate cumulative budget covers error at both interval endpoints.
    static constexpr double adjacentJitterBoundPqCodes{0.75};
    static constexpr double neutralRippleBoundPqCodes{2.0};

    bool SamplingPassed(double darkBound) const
    {
        return contained && darkOutputCodes <= darkBound && darkSamplerCodes <= 3.5 &&
               maximumSamplerError <= peak * 0.005;
    }

    bool NeutralPassed() const
    {
        return maximumNeutralTintPqCodes <= neutralTintBoundPqCodes &&
               maximumNeutralAdjacentDecreasePqCodes <= adjacentJitterBoundPqCodes &&
               maximumNeutralDrawdownPqCodes <= neutralRippleBoundPqCodes;
    }

    OperatorVerdict Evaluate() const
    {
        const auto broadEnd = dataset.broadEnd, holdoutBegin = dataset.holdoutBegin, holdoutEnd = dataset.holdoutEnd;
        // ACES2LMT is an experimental draft look with higher local
        // curvature. Its representation retains the 0.7%-of-peak
        // maximum gate; every other operator retains 0.5% and the mean is shared.
        const auto maximumBound = std::max(1.0, peak * (algorithm == Algorithm::aces2Lmt ? 0.007 : 0.005));
        const auto broadMeanError = broadErrorSum / (static_cast<double>(broadEnd) * 3.0);
        const auto holdoutMeanError = holdoutErrorSum / (static_cast<double>(holdoutEnd - holdoutBegin) * 3.0);
        const auto meanBound = std::max(0.1, peak * 0.0005);
        const bool analyticalPassed =
            maximumApproximationError <= maximumBound && broadMeanError <= meanBound && holdoutMeanError <= meanBound;
        const auto darkBound = algorithm == Algorithm::aces2Lmt ? 3.0 : 32.0;
        const bool passed = SamplingPassed(darkBound) && NeutralPassed() && analyticalPassed;
        return {maximumBound, broadMeanError, holdoutMeanError, meanBound, darkBound, passed};
    }

    void ReportFailure(const OperatorVerdict &verdict) const
    {
        const auto &[maximumBound, broadMeanError, holdoutMeanError, meanBound, darkBound, passed] = verdict;
        const auto &pixels = dataset.pixels;
        const char *label = "Direct operator LUT ";

        const auto &worst = pixels[worstIndex];
        std::cerr << __FILE__ << ':' << __LINE__ << ": FAIL: " << label << Name(algorithm) << " peak=" << peak
                  << " worst_input=(" << worst[0] << ',' << worst[1] << ',' << worst[2] << ')'
                  << " channel=" << worstChannel << " actual=" << worstActual << " expected=" << worstExpected
                  << " cpu_lut_max_budget=" << maximumBound << " dark_max_pq_codes=" << darkOutputCodes << '/'
                  << darkBound << " dark_sampler_pq_codes=" << darkSamplerCodes << "/3.5"
                  << " neutral_tint_pq_codes=" << maximumNeutralTintPqCodes << '/' << neutralTintBoundPqCodes
                  << " neutral_adjacent_jitter_pq_codes=" << maximumNeutralAdjacentDecreasePqCodes << '/'
                  << adjacentJitterBoundPqCodes
                  << " neutral_running_max_drawdown_pq_codes=" << maximumNeutralDrawdownPqCodes << '/'
                  << neutralRippleBoundPqCodes << " broad_mean_nits=" << broadMeanError << '/' << meanBound
                  << " holdout_mean_nits=" << holdoutMeanError << '/' << meanBound << '\n';
    }

    int Report() const
    {
        const auto &[corpus, broadEnd, holdoutBegin, holdoutEnd, darkBegin, darkEnd, neutralBegin, neutralEnd, height,
                     pixels] = dataset;
        const auto verdict = Evaluate();
        const auto &[maximumBound, broadMeanError, holdoutMeanError, meanBound, darkBound, passed] = verdict;
        const char *label = "Direct operator LUT ";
        std::cout << label << Name(algorithm) << " peak=" << peak << " parameters=" << parameters.acesLookStrength
                  << ',' << parameters.agxMiddleGrayEv << ',' << parameters.agxContrast << ','
                  << parameters.agxHdrPurity << ',' << parameters.agxHdrShoulderPower << ','
                  << parameters.gt7ToeStrength << ',' << parameters.gt7ShoulderStart << ',' << parameters.gt7ColorBlend
                  << " colors=" << corpus.size() << " gpu_total_max_nits=" << maximumError
                  << " cpu_lut_max_nits=" << maximumApproximationError << " broad_mean_nits=" << broadMeanError << '/'
                  << meanBound << " holdout_mean_nits=" << holdoutMeanError << '/' << meanBound
                  << " contained=" << contained << " analytical_gate=required"
                  << " gpu_vs_cpu_sampler_max_nits=" << maximumSamplerError
                  << " dark_cpu_approximation_max_pq_codes=" << darkApproximationCodes
                  << " dark_gpu_sampler_max_pq_codes=" << darkSamplerCodes
                  << " dark_gpu_total_max_pq_codes=" << darkOutputCodes
                  << " neutral_tint_max_pq_codes=" << maximumNeutralTintPqCodes << '/' << neutralTintBoundPqCodes
                  << " neutral_adjacent_jitter_max_pq_codes=" << maximumNeutralAdjacentDecreasePqCodes << '/'
                  << adjacentJitterBoundPqCodes
                  << " neutral_running_max_drawdown_pq_codes=" << maximumNeutralDrawdownPqCodes << '/'
                  << neutralRippleBoundPqCodes << " neutral_drawdown_channel=" << worstNeutralDrawdownChannel
                  << " neutral_drawdown_input=" << worstNeutralDrawdownMaximumInput << "->"
                  << worstNeutralDrawdownCurrentInput << " neutral_drawdown_rgb=("
                  << worstNeutralDrawdownMaximumOutput.red << ',' << worstNeutralDrawdownMaximumOutput.green << ','
                  << worstNeutralDrawdownMaximumOutput.blue << ")->(" << worstNeutralDrawdownCurrentOutput.red << ','
                  << worstNeutralDrawdownCurrentOutput.green << ',' << worstNeutralDrawdownCurrentOutput.blue << ')'
                  << " neutral_drawdown_reference=(" << worstNeutralDrawdownMaximumReference.red << ','
                  << worstNeutralDrawdownMaximumReference.green << ',' << worstNeutralDrawdownMaximumReference.blue
                  << ")->(" << worstNeutralDrawdownCurrentReference.red << ','
                  << worstNeutralDrawdownCurrentReference.green << ',' << worstNeutralDrawdownCurrentReference.blue
                  << ')';
        std::cout << '\n';
        if (!passed)
            ReportFailure(verdict);
        return passed ? 0 : 1;
    }
};

struct OperatorExecution
{
    ID3D11Device &device;
    ID3D11DeviceContext &context;
    ID3D11ComputeShader &shader;
    ID3D11SamplerState &sampler;
};

struct OperatorCase
{
    Algorithm algorithm;
    double peak;
    fo4hdr::ToneMapperParameters parameters{};
};

struct UploadedOperatorLut
{
    ComPtr<ID3D11Texture3D> lutTexture;
    ComPtr<ID3D11ShaderResourceView> lutView;

    UploadedOperatorLut(ID3D11Device &device, const fo4hdr::OperatorLutData &lut)
    {
        D3D11_TEXTURE3D_DESC lutDescriptor{};
        lutDescriptor.Width = lut.textureEdge;
        lutDescriptor.Height = lut.textureEdge;
        lutDescriptor.Depth = lut.textureEdge;
        lutDescriptor.MipLevels = 1;
        lutDescriptor.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        lutDescriptor.Usage = D3D11_USAGE_IMMUTABLE;
        lutDescriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA lutData{};
        lutData.pSysMem = lut.rgba16.data();
        lutData.SysMemPitch = lut.textureEdge * 4 * sizeof(std::uint16_t);
        lutData.SysMemSlicePitch = lutData.SysMemPitch * lut.textureEdge;
        Require(device.CreateTexture3D(&lutDescriptor, &lutData, &lutTexture), "Upload production operator LUT");
        Require(device.CreateShaderResourceView(lutTexture.Get(), nullptr, &lutView),
                "Create production operator view");
    }
};

struct OperatorBatch
{
    ComPtr<ID3D11Texture2D> input, output, staging;
    ComPtr<ID3D11ShaderResourceView> inputView;
    ComPtr<ID3D11UnorderedAccessView> outputView;

    OperatorBatch(ID3D11Device &device, const OperatorCorpus &dataset)
    {
        const auto height = dataset.height;
        const auto &pixels = dataset.pixels;
        input = CreateBatchTexture(device, {height, D3D11_BIND_SHADER_RESOURCE, pixels.data()});
        output = CreateBatchTexture(device, {height, D3D11_BIND_UNORDERED_ACCESS});
        staging = CreateBatchTexture(device, {height, 0, nullptr, true});
        Require(device.CreateShaderResourceView(input.Get(), nullptr, &inputView), "Create operator test input view");
        Require(device.CreateUnorderedAccessView(output.Get(), nullptr, &outputView),
                "Create operator test output view");
    }
};

int VerifyCase(const OperatorExecution &execution, const OperatorCase &test)
{
    const auto &[device, context, shader, sampler] = execution;
    const auto &[algorithm, peak, parameters] = test;
    const OperatorCorpus dataset(peak);
    const auto &[corpus, broadEnd, holdoutBegin, holdoutEnd, darkBegin, darkEnd, neutralBegin, neutralEnd, height,
                 pixels] = dataset;
    const OperatorBatch batch(device, dataset);
    const auto &[input, output, staging, inputView, outputView] = batch;

    const auto lut = fo4hdr::BuildOperatorLut(algorithm, peak, parameters);
    if (!lut.IsValid())
        throw std::runtime_error("Operator LUT generation failed");
    const UploadedOperatorLut uploaded(device, lut);
    const auto &[lutTexture, lutView] = uploaded;

    const auto mode = ProductionMode(algorithm);
    const auto shaper = fo4hdr::OperatorLutShaperParameters(algorithm, peak);
    const BatchConstants constants{kWidth,
                                   static_cast<UINT>(corpus.size()),
                                   mode,
                                   shaper[0],
                                   shaper[1],
                                   static_cast<float>(peak),
                                   {shaper[2], shaper[3]}};
    D3D11_BUFFER_DESC bufferDescriptor{};
    bufferDescriptor.ByteWidth = sizeof(constants);
    bufferDescriptor.Usage = D3D11_USAGE_IMMUTABLE;
    bufferDescriptor.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA bufferData{&constants, 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    Require(device.CreateBuffer(&bufferDescriptor, &bufferData, &buffer), "Create operator test constants");

    ID3D11ShaderResourceView *inputPointer = inputView.Get();
    ID3D11ShaderResourceView *lutPointer = lutView.Get();
    ID3D11UnorderedAccessView *outputPointer = outputView.Get();
    ID3D11Buffer *bufferPointer = buffer.Get();
    ID3D11SamplerState *samplerPointer = &sampler;
    context.CSSetShader(&shader, nullptr, 0);
    context.CSSetShaderResources(0, 1, &inputPointer);
    context.CSSetShaderResources(7, 1, &lutPointer);
    context.CSSetSamplers(7, 1, &samplerPointer);
    context.CSSetConstantBuffers(0, 1, &bufferPointer);
    context.CSSetUnorderedAccessViews(0, 1, &outputPointer, nullptr);
    context.Dispatch(kWidth / 64, height, 1);
    // This runner exclusively owns the device/context, not a borrowed game
    // context. Unbind before copying and before the other tests run.
    context.ClearState();
    context.CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Require(context.Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read direct production LUT sampling");

    const auto aces = IsAces2Family(algorithm) ? fo4hdr::CreateAces2Transform(peak) : fo4hdr::Aces2TransformHandle{};
    if (IsAces2Family(algorithm) && !aces) {
        context.Unmap(staging.Get(), 0);
        throw std::runtime_error("Cannot create retained ACES oracle");
    }
    OperatorMeasurements measurements{dataset, algorithm, peak, parameters, aces, lut};
    measurements.Measure(mapped);
    context.Unmap(staging.Get(), 0);
    return measurements.Report();
}

fo4hdr::ToneMapperParameters AgxCornerParameters(unsigned corner)
{
    fo4hdr::ToneMapperParameters parameters{};
    parameters.agxMiddleGrayEv = corner & 1U ? fo4hdr::kMaximumAgxMiddleGrayEv : fo4hdr::kMinimumAgxMiddleGrayEv;
    parameters.agxContrast = corner & 2U ? fo4hdr::kMaximumAgxContrast : fo4hdr::kMinimumAgxContrast;
    parameters.agxHdrPurity = corner & 4U ? fo4hdr::kMaximumAgxHdrPurity : fo4hdr::kMinimumAgxHdrPurity;
    parameters.agxHdrShoulderPower =
        corner & 8U ? fo4hdr::kMaximumAgxHdrShoulderPower : fo4hdr::kMinimumAgxHdrShoulderPower;
    return parameters;
}

fo4hdr::ToneMapperParameters Gt7CornerParameters(unsigned corner)
{
    fo4hdr::ToneMapperParameters parameters{};
    parameters.gt7ToeStrength = corner & 1U ? fo4hdr::kMaximumGt7ToeStrength : fo4hdr::kMinimumGt7ToeStrength;
    parameters.gt7ShoulderStart = corner & 2U ? fo4hdr::kMaximumGt7ShoulderStart : fo4hdr::kMinimumGt7ShoulderStart;
    parameters.gt7ColorBlend = corner & 4U ? fo4hdr::kMaximumGt7ColorBlend : fo4hdr::kMinimumGt7ColorBlend;
    return parameters;
}

} // namespace

int VerifyOperatorLutSampling(ID3D11Device &device, ID3D11DeviceContext &context)
{
    ComPtr<ID3D11ComputeShader> shader;
    const auto &bytecode = fo4hdr::generated::test::kOperatorLutCs;
    Require(device.CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &shader),
            "Create build-time operator LUT test shader");
    D3D11_SAMPLER_DESC descriptor{};
    descriptor.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    descriptor.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    descriptor.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    descriptor.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    descriptor.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    Require(device.CreateSamplerState(&descriptor, &sampler), "Create production-style LUT sampler");
    const OperatorExecution execution{device, context, *shader.Get(), *sampler.Get()};
    int failures{};
    for (const auto algorithm : {Algorithm::aces2Lmt, Algorithm::agX, Algorithm::gt7}) {
        for (const auto peak : {400.0, 430.0, 1000.0, 2000.0}) {
            failures += VerifyCase(execution, {algorithm, peak});
        }
    }
    // Keep the established approximation and sampling budgets for configured
    // responses. Opposite peak endpoints exercise every joint parameter corner.
    for (const auto peak : {400.0, 2000.0}) {
        for (const double strength : {0.0, 0.25, 0.5, 0.75}) {
            fo4hdr::ToneMapperParameters parameters{};
            parameters.acesLookStrength = strength;
            failures += VerifyCase(execution, {Algorithm::aces2Lmt, peak, parameters});
        }
        for (unsigned corner = 0; corner < 16; ++corner) {
            const auto parameters = AgxCornerParameters(corner);
            failures += VerifyCase(execution, {Algorithm::agX, peak, parameters});
        }
        for (unsigned corner = 0; corner < 8; ++corner) {
            const auto parameters = Gt7CornerParameters(corner);
            failures += VerifyCase(execution, {Algorithm::gt7, peak, parameters});
        }
    }
    return failures;
}

namespace fo4hdr::tests::gpu {
namespace {
constexpr float peakNits = 1000;
constexpr UINT batchWidth = 4;
constexpr UINT batchHeight = 4;
using BatchPixels = std::array<Float4, batchWidth * batchHeight>;

ComPtr<ID3D11Buffer> ImmutableConstants(ID3D11Device& device, const void* data, UINT size)
{
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = size;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial{data, 0, 0};
    ComPtr<ID3D11Buffer> result;
    CheckHresult(device.CreateBuffer(&description, &initial, &result), "Operator batch constants");
    return result;
}

BatchPixels SamplingInputs()
{
    const auto samples = test::OperatorLutReferenceSamples();
    static_assert(std::tuple_size_v<decltype(samples)> == batchWidth * batchHeight);
    BatchPixels result{};
    for (std::size_t index = 0; index < samples.size(); ++index)
        result[index] = {static_cast<float>(samples[index].red), static_cast<float>(samples[index].green),
            static_cast<float>(samples[index].blue), 1};
    return result;
}

struct SamplingResources
{
    ComPtr<ID3D11Texture2D> input;
    ComPtr<ID3D11ShaderResourceView> inputView;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11UnorderedAccessView> outputView;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11SamplerState> sampler;

    SamplingResources(ID3D11Device& device, const std::filesystem::path& shaderPath, const BatchPixels& pixels)
    {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = batchWidth;
        description.Height = batchHeight;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{pixels.data(), batchWidth * sizeof(Float4), 0};
        CheckHresult(device.CreateTexture2D(&description, &data, &input), "Operator batch input");
        CheckHresult(device.CreateShaderResourceView(input.Get(), nullptr, &inputView), "Operator batch input SRV");
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        CheckHresult(device.CreateTexture2D(&description, nullptr, &output), "Operator batch output");
        CheckHresult(device.CreateUnorderedAccessView(output.Get(), nullptr, &outputView), "Operator batch output UAV");
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CheckHresult(device.CreateTexture2D(&description, nullptr, &staging), "Operator batch staging");
        const auto bytecode = ReadShader(shaderPath);
        CheckHresult(device.CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &shader),
            "Operator batch shader");
        D3D11_SAMPLER_DESC filter{};
        filter.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        filter.AddressU = filter.AddressV = filter.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        filter.MaxAnisotropy = 1;
        filter.ComparisonFunc = D3D11_COMPARISON_NEVER;
        CheckHresult(device.CreateSamplerState(&filter, &sampler), "Operator batch sampler");
    }
};

struct SamplingTable
{
    ComPtr<ID3D11Texture3D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11Buffer> constants;

    SamplingTable(ID3D11Device& device, const OperatorLutData& table)
    {
        D3D11_TEXTURE3D_DESC description{};
        description.Width = description.Height = description.Depth = table.textureEdge;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const auto rowPitch = table.textureEdge * 8u;
        const D3D11_SUBRESOURCE_DATA data{table.rgba16.data(), rowPitch, rowPitch * table.textureEdge};
        CheckHresult(device.CreateTexture3D(&description, &data, &texture), "Operator coefficient texture");
        CheckHresult(device.CreateShaderResourceView(texture.Get(), nullptr, &view), "Operator coefficient SRV");
        const auto shaper = OperatorLutShaperParameters(table.algorithm, table.displayPeakNits);
        const BatchConstants values{batchWidth, batchWidth * batchHeight,
            ::ProductionMode(table.algorithm), shaper[0], shaper[1],
            static_cast<float>(table.displayPeakNits), {shaper[2], shaper[3]}};
        constants = ImmutableConstants(device, &values, sizeof(values));
    }
};

void BindSampling(ID3D11DeviceContext& context, const SamplingResources& resources, const SamplingTable& table)
{
    auto* constants = table.constants.Get();
    auto* input = resources.inputView.Get();
    auto* coefficients = table.view.Get();
    auto* sampler = resources.sampler.Get();
    auto* output = resources.outputView.Get();
    context.CSSetShader(resources.shader.Get(), nullptr, 0);
    context.CSSetConstantBuffers(0, 1, &constants);
    context.CSSetShaderResources(0, 1, &input);
    context.CSSetShaderResources(7, 1, &coefficients);
    context.CSSetSamplers(7, 1, &sampler);
    context.CSSetUnorderedAccessViews(0, 1, &output, nullptr);
}

void DispatchSampling(ID3D11DeviceContext& context, const SamplingResources& resources, const SamplingTable& table)
{
    BindSampling(context, resources, table);
    context.Dispatch(1, batchHeight, 1);
    // This context is owned by the batch; clear aliases before staging readback.
    context.ClearState();
}

BatchPixels ReadSampling(ID3D11DeviceContext& context, const SamplingResources& resources)
{
    context.CopyResource(resources.staging.Get(), resources.output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(context.Map(resources.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Operator batch map");
    constexpr auto rowBytes = batchWidth * sizeof(Float4);
    if (!mapped.pData || mapped.RowPitch < rowBytes)
    {
        context.Unmap(resources.staging.Get(), 0);
        throw std::runtime_error("Invalid operator batch pitch");
    }
    BatchPixels result{};
    const auto* data = static_cast<const std::byte*>(mapped.pData);
    for (UINT row = 0; row < batchHeight; ++row)
        std::memcpy(result.data() + row * batchWidth, data + row * mapped.RowPitch, rowBytes);
    context.Unmap(resources.staging.Get(), 0);
    return result;
}

void CheckSamplingChannel(float actual, double expected)
{
    // CPU and GPU read the same packed coefficients. This provisional allowance
    // covers differing sampling arithmetic; it is not a proved hardware bound.
    const auto tolerance = 0.5 + std::abs(expected) / 1024.0;
    FO4HDR_CHECK(std::isfinite(actual));
    FO4HDR_CHECK(actual >= 0);
    FO4HDR_CHECK(actual <= peakNits);
    FO4HDR_CHECK(std::abs(static_cast<double>(actual) - expected) <= tolerance);
}

void CheckSampling(const OperatorLutData& table, const BatchPixels& input, const BatchPixels& output)
{
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto& source = input[index];
        const auto expected = test::SampleOperatorLut(table, {source.red, source.green, source.blue});
        const auto actual = output[index];
        CheckSamplingChannel(actual.red, expected.red);
        CheckSamplingChannel(actual.green, expected.green);
        CheckSamplingChannel(actual.blue, expected.blue);
        FO4HDR_CHECK(actual.alpha == 1);
        if (std::max({source.red, source.green, source.blue}) == 0)
        {
            FO4HDR_CHECK(actual.red == 0);
            FO4HDR_CHECK(actual.green == 0);
            FO4HDR_CHECK(actual.blue == 0);
        }
    }
}

using SamplingClock = std::chrono::steady_clock;
constexpr UINT samplingRepetitions = 128;

ComPtr<ID3D11Query> SamplingQuery(ID3D11Device& device, D3D11_QUERY kind)
{
    const D3D11_QUERY_DESC description{kind, 0};
    ComPtr<ID3D11Query> query;
    CheckHresult(device.CreateQuery(&description, &query), "Operator timing query");
    if (!query) throw std::runtime_error("Missing operator timing query");
    return query;
}

template <class T>
T ReadSamplingQuery(ID3D11DeviceContext& context, ID3D11Query& query, SamplingClock::time_point deadline)
{
    T value{};
    for (;;)
    {
        const auto result = context.GetData(&query, &value, static_cast<UINT>(sizeof(value)),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_OK) return value;
        if (result != S_FALSE) throw std::runtime_error("Operator timing query failed");
        if (SamplingClock::now() >= deadline)
            throw std::runtime_error("Operator timing query exceeded five seconds");
        std::this_thread::yield();
    }
}

double ReadSamplingMilliseconds(ID3D11DeviceContext& context, ID3D11Query& disjoint,
    ID3D11Query& start, ID3D11Query& end)
{
    const auto deadline = SamplingClock::now() + std::chrono::seconds(5);
    const auto frequency = ReadSamplingQuery<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(context, disjoint, deadline);
    if (frequency.Disjoint || frequency.Frequency == 0)
        throw std::runtime_error("Invalid operator timing frequency");
    const auto first = ReadSamplingQuery<UINT64>(context, start, deadline);
    const auto last = ReadSamplingQuery<UINT64>(context, end, deadline);
    if (last < first) throw std::runtime_error("Reversed operator timestamps");
    const double milliseconds = static_cast<double>(last - first) * 1000.0 /
        static_cast<double>(frequency.Frequency) / samplingRepetitions;
    if (!std::isfinite(milliseconds)) throw std::runtime_error("Nonfinite operator timing");
    return milliseconds;
}

double MeasureSamplingDispatches(ID3D11Device& device, ID3D11DeviceContext& context)
{
    const auto disjoint = SamplingQuery(device, D3D11_QUERY_TIMESTAMP_DISJOINT);
    const auto start = SamplingQuery(device, D3D11_QUERY_TIMESTAMP);
    const auto end = SamplingQuery(device, D3D11_QUERY_TIMESTAMP);
    for (UINT warmup = 0; warmup < 4; ++warmup) context.Dispatch(1, batchHeight, 1);
    context.Begin(disjoint.Get());
    context.End(start.Get());
    for (UINT iteration = 0; iteration < samplingRepetitions; ++iteration) context.Dispatch(1, batchHeight, 1);
    context.End(end.Get());
    context.End(disjoint.Get());
    context.Flush();
    return ReadSamplingMilliseconds(context, *disjoint.Get(), *start.Get(), *end.Get());
}

double SamplingMilliseconds(SamplingClock::time_point first, SamplingClock::time_point last)
{
    return std::chrono::duration<double, std::milli>(last - first).count();
}

void BenchmarkSamplingCase(const Device& device, const SamplingResources& resources,
    const BatchPixels& inputs, ProductionToneMapperAlgorithm algorithm)
{
    const auto bakeStart = SamplingClock::now();
    const auto table = BuildOperatorLut(algorithm, peakNits);
    const auto bakeEnd = SamplingClock::now();
    FO4HDR_CHECK(table.IsValid());
    if (!table.IsValid()) return;
    const auto uploadStart = SamplingClock::now();
    const SamplingTable uploaded(*device.device.Get(), table);
    const auto uploadEnd = SamplingClock::now();
    BindSampling(*device.context.Get(), resources, uploaded);
    const auto gpuMilliseconds = MeasureSamplingDispatches(*device.device.Get(), *device.context.Get());
    device.context->ClearState();
    CheckSampling(table, inputs, ReadSampling(*device.context.Get(), resources));
    // This measures the isolated wrapper and input/output traffic, not gameplay.
    std::cout << "Isolated WARP operator=" << ToString(algorithm) << " peak=" << peakNits
              << " colors=16 batch=4x4 warmup=4 repetitions=128 bake_cpu_ms="
              << SamplingMilliseconds(bakeStart, bakeEnd) << " upload_api_cpu_ms="
              << SamplingMilliseconds(uploadStart, uploadEnd) << " isolated_warp_batch_ms_per_dispatch="
              << gpuMilliseconds << '\n';
}

}  // namespace

void BenchmarkOperatorLut(const std::filesystem::path& bytecodePath)
{
    const auto device = CreateDevice();
    const auto inputs = SamplingInputs();
    const SamplingResources resources(*device.device.Get(), bytecodePath, inputs);
    for (const auto algorithm : {ProductionToneMapperAlgorithm::aces2Lmt, ProductionToneMapperAlgorithm::agX, ProductionToneMapperAlgorithm::gt7})
        BenchmarkSamplingCase(device, resources, inputs, algorithm);
}

}  // namespace fo4hdr::tests::gpu
