// SPDX-License-Identifier: LicenseRef-Fallout4HDR-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ShaderTestSuite.hpp"
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
using Clock = std::chrono::steady_clock;
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

double Milliseconds(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
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

template <class T> T ReadQuery(ID3D11DeviceContext &context, ID3D11Query &query)
{
    T value{};
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto result = context.GetData(&query, &value, static_cast<UINT>(sizeof(value)), 0);
        if (result == S_OK)
            return value;
        Require(result, "Read operator GPU timer");
        if (Clock::now() >= deadline) {
            throw std::runtime_error("Operator GPU timer exceeded five seconds");
        }
        std::this_thread::yield();
    }
}

// Measures the isolated compute wrapper and input/output traffic; excludes
// the creative-grade pixel shader and gameplay rendering.
double MeasureDispatchMilliseconds(ID3D11Device &device, ID3D11DeviceContext &context, UINT height)
{
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> start;
    ComPtr<ID3D11Query> end;
    D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    Require(device.CreateQuery(&query, &disjoint), "Create disjoint query");
    query.Query = D3D11_QUERY_TIMESTAMP;
    Require(device.CreateQuery(&query, &start), "Create start timestamp");
    Require(device.CreateQuery(&query, &end), "Create end timestamp");
    for (UINT warmup = 0; warmup < 4; ++warmup) {
        context.Dispatch(kWidth / 64, height, 1);
    }
    constexpr UINT repetitions = 128;
    context.Begin(disjoint.Get());
    context.End(start.Get());
    for (UINT iteration = 0; iteration < repetitions; ++iteration) {
        context.Dispatch(kWidth / 64, height, 1);
    }
    context.End(end.Get());
    context.End(disjoint.Get());
    context.Flush();
    const auto frequency = ReadQuery<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(context, *disjoint.Get());
    if (frequency.Disjoint || frequency.Frequency == 0) {
        throw std::runtime_error("Operator GPU timing was disjoint; rerun required");
    }
    const auto first = ReadQuery<UINT64>(context, *start.Get());
    const auto last = ReadQuery<UINT64>(context, *end.Get());
    return static_cast<double>(last - first) * 1000.0 / static_cast<double>(frequency.Frequency) / repetitions;
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

struct OperatorTimings
{
    Clock::time_point bakeStart, bakeEnd, uploadStart, uploadEnd;
    double gpuMilliseconds;
    bool benchmark;
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

    int Report(const OperatorTimings &timing) const
    {
        const auto &[corpus, broadEnd, holdoutBegin, holdoutEnd, darkBegin, darkEnd, neutralBegin, neutralEnd, height,
                     pixels] = dataset;
        const auto &[bakeStart, bakeEnd, uploadStart, uploadEnd, gpuMilliseconds, benchmark] = timing;
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
                  << ')' << " bake_cpu_ms=" << Milliseconds(bakeStart, bakeEnd)
                  << " upload_api_cpu_ms=" << Milliseconds(uploadStart, uploadEnd);
        if (benchmark) {
            std::cout << " isolated_gpu_batch_ms=" << gpuMilliseconds << " (not gameplay cost)";
        }
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
    bool benchmark;
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
    const auto &[algorithm, peak, benchmark, parameters] = test;
    const OperatorCorpus dataset(peak);
    const auto &[corpus, broadEnd, holdoutBegin, holdoutEnd, darkBegin, darkEnd, neutralBegin, neutralEnd, height,
                 pixels] = dataset;
    const OperatorBatch batch(device, dataset);
    const auto &[input, output, staging, inputView, outputView] = batch;

    const auto bakeStart = Clock::now();
    const auto lut = fo4hdr::BuildOperatorLut(algorithm, peak, parameters);
    const auto bakeEnd = Clock::now();
    if (!lut.IsValid())
        throw std::runtime_error("Operator LUT generation failed");
    const auto uploadStart = Clock::now();
    const UploadedOperatorLut uploaded(device, lut);
    const auto &[lutTexture, lutView] = uploaded;
    const auto uploadEnd = Clock::now();

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
    const auto gpuMilliseconds = benchmark ? MeasureDispatchMilliseconds(device, context, height) : 0.0;
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
    return measurements.Report({bakeStart, bakeEnd, uploadStart, uploadEnd, gpuMilliseconds, benchmark});
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

int VerifyOperatorLutSampling(ID3D11Device &device, ID3D11DeviceContext &context, bool benchmark)
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
            failures += VerifyCase(execution, {algorithm, peak, benchmark});
        }
    }
    // Keep the established approximation and sampling budgets for configured
    // responses. Opposite peak endpoints exercise every joint parameter corner.
    for (const auto peak : {400.0, 2000.0}) {
        for (const double strength : {0.0, 0.25, 0.5, 0.75}) {
            fo4hdr::ToneMapperParameters parameters{};
            parameters.acesLookStrength = strength;
            failures += VerifyCase(execution, {Algorithm::aces2Lmt, peak, false, parameters});
        }
        for (unsigned corner = 0; corner < 16; ++corner) {
            const auto parameters = AgxCornerParameters(corner);
            failures += VerifyCase(execution, {Algorithm::agX, peak, false, parameters});
        }
        for (unsigned corner = 0; corner < 8; ++corner) {
            const auto parameters = Gt7CornerParameters(corner);
            failures += VerifyCase(execution, {Algorithm::gt7, peak, false, parameters});
        }
    }
    return failures;
}
