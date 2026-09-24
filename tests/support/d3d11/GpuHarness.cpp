// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuHarness.hpp"
#include "Fallout4HDR/config/HdrSettings.hpp"
#include "Fallout4HDR/tonemap/OperatorLut.hpp"
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "reference/NativeGradeReference.hpp"

namespace fo4hdr::tests::gpu {

[[nodiscard]] std::string HresultText(HRESULT value)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result(8, '0');
    const auto bits = static_cast<std::uint32_t>(value);
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto shift =
            static_cast<unsigned>((result.size() - index - 1) * 4);
        result[index] = digits[(bits >> shift) & 0xFU];
    }
    return result;
}

void CheckHresult(HRESULT result, std::string_view operation)
{
    if (SUCCEEDED(result)) return;
    throw std::runtime_error(
        std::string(operation) + " failed with HRESULT 0x" +
        HresultText(result));
}

void CheckDebugMessages(Device& device)
{
    ComPtr<ID3D11InfoQueue> queue;
    if (FAILED(device.device.As(&queue))) return;
    const auto count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T size{};
        CheckHresult(queue->GetMessage(index, nullptr, &size), "Size D3D11 diagnostic");
        std::vector<std::byte> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        CheckHresult(queue->GetMessage(index, message, &size), "Read D3D11 diagnostic");
        if (message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
            message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
            ++g_failures;
            std::cerr << "D3D11 diagnostic " << message->ID << ": " << message->pDescription << '\n';
        }
    }
    queue->ClearStoredMessages();
}

Device CreateDevice()
{
    Device result;
    constexpr D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL selected{};
    CheckHresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        &requested, 1, D3D11_SDK_VERSION, result.device.GetAddressOf(), &selected,
        result.context.GetAddressOf()), "D3D11CreateDevice(WARP)");
    if (selected != requested)
    {
        throw std::runtime_error("WARP did not provide feature level 11_0");
    }
    return result;
}

std::vector<char> ReadShader(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Cannot open test shader bytecode");
    }
    const auto size = file.tellg();
    if (size <= 0 || size > 1024 * 1024)
    {
        throw std::runtime_error("Cannot read bounded test shader bytecode");
    }
    std::vector<char> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
    {
        throw std::runtime_error("Incomplete test shader bytecode");
    }
    return bytes;
}

[[nodiscard]] Texture CreateSizedTexture(ID3D11Device& device, const TextureSpec& spec)
{
    const auto& [width, height, format, bindFlags, initialData] = spec;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = bindFlags;
    Texture result;
    CheckHresult(device.CreateTexture2D(
        &description, initialData, &result.texture),
        "ID3D11Device::CreateTexture2D(sized)");
    if ((bindFlags & D3D11_BIND_SHADER_RESOURCE) != 0U)
        CheckHresult(device.CreateShaderResourceView(
            result.texture.Get(), nullptr, &result.srv),
            "ID3D11Device::CreateShaderResourceView(sized)");
    if ((bindFlags & D3D11_BIND_RENDER_TARGET) != 0U)
        CheckHresult(device.CreateRenderTargetView(
            result.texture.Get(), nullptr, &result.rtv),
            "ID3D11Device::CreateRenderTargetView(sized)");
    if ((bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0U)
        CheckHresult(device.CreateUnorderedAccessView(
            result.texture.Get(), nullptr, &result.uav),
            "ID3D11Device::CreateUnorderedAccessView(sized)");
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CheckHresult(device.CreateTexture2D(
        &description, nullptr, &result.staging),
        "ID3D11Device::CreateTexture2D(sized staging)");
    return result;
}

[[nodiscard]] Texture CreateTexture(
    ID3D11Device& device,
    DXGI_FORMAT format,
    UINT bindFlags,
    const D3D11_SUBRESOURCE_DATA* initialData)
{
    return CreateSizedTexture(
        device, {kWidth, kHeight, format, bindFlags, initialData});
}

[[nodiscard]] Texture3D CreateIdentityLut(ID3D11Device& device)
{
    constexpr UINT size = 16;
    std::array<Float4, size * size * size> texels{};
    for (UINT blue = 0; blue < size; ++blue) {
        for (UINT green = 0; green < size; ++green) {
            for (UINT red = 0; red < size; ++red) {
                texels[(blue * size + green) * size + red] = {
                    static_cast<float>(red) / static_cast<float>(size - 1),
                    static_cast<float>(green) / static_cast<float>(size - 1),
                    static_cast<float>(blue) / static_cast<float>(size - 1),
                    1.0F};
            }
        }
    }

    D3D11_TEXTURE3D_DESC description{};
    description.Width = size;
    description.Height = size;
    description.Depth = size;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{
        texels.data(),
        static_cast<UINT>(size * sizeof(Float4)),
        static_cast<UINT>(size * size * sizeof(Float4))};
    Texture3D result;
    CheckHresult(
        device.CreateTexture3D(&description, &data, &result.texture),
        "ID3D11Device::CreateTexture3D(identity LUT)");
    CheckHresult(
        device.CreateShaderResourceView(
            result.texture.Get(), nullptr, &result.srv),
        "ID3D11Device::CreateShaderResourceView(identity LUT)");
    return result;
}

[[nodiscard]] Texture3D CreateOperatorLut(
    ID3D11Device& device,
    const fo4hdr::OperatorLutData& lut)
{
    if (!lut.IsValid())
        throw std::runtime_error("CPU operator LUT generation failed");

    D3D11_TEXTURE3D_DESC description{};
    description.Width = lut.textureEdge;
    description.Height = lut.textureEdge;
    description.Depth = lut.textureEdge;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{
        lut.rgba16.data(),
        static_cast<UINT>(lut.textureEdge * 4U * sizeof(std::uint16_t)),
        static_cast<UINT>(
            lut.textureEdge * lut.textureEdge * 4U * sizeof(std::uint16_t))};

    Texture3D result;
    CheckHresult(
        device.CreateTexture3D(&description, &data, &result.texture),
        "ID3D11Device::CreateTexture3D(operator LUT)");
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = description.Format;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    view.Texture3D.MipLevels = 1;
    CheckHresult(
        device.CreateShaderResourceView(
            result.texture.Get(), &view, &result.srv),
        "ID3D11Device::CreateShaderResourceView(operator LUT)");
    return result;
}

[[nodiscard]] Texture3D CreateOperatorLut(
    ID3D11Device& device,
    fo4hdr::ProductionToneMapperAlgorithm algorithm,
    float displayPeakNits)
{
    return CreateOperatorLut(
        device, fo4hdr::BuildOperatorLut(algorithm, displayPeakNits));
}

[[nodiscard]] ComPtr<ID3D11SamplerState> CreateClampSampler(
    ID3D11Device& device, D3D11_FILTER filter, std::string_view operation)
{
    D3D11_SAMPLER_DESC description{};
    description.Filter = filter;
    description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.MaxAnisotropy = 1;
    description.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    description.MinLOD = 0.0F;
    description.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    CheckHresult(
        device.CreateSamplerState(&description, &sampler),
        operation);
    return sampler;
}

[[nodiscard]] ComPtr<ID3D11SamplerState> CreateClampPointSampler(ID3D11Device& device)
{
    return CreateClampSampler(device, D3D11_FILTER_MIN_MAG_MIP_POINT,
        "ID3D11Device::CreateSamplerState(point clamp)");
}

[[nodiscard]] ComPtr<ID3D11SamplerState> CreateClampLinearSampler(ID3D11Device& device)
{
    return CreateClampSampler(device, D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        "ID3D11Device::CreateSamplerState(linear clamp)");
}

void DrawFullscreen(ID3D11DeviceContext& context, const FullscreenDraw& draw)
{
    const auto& [vertexShader, pixelShader, cb2, toneMapConstants, resources,
        resourceCount, samplers, samplerCount, targets, targetCount] = draw;
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(kWidth);
    viewport.Height = static_cast<float>(kHeight);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;

    context.IASetInputLayout(nullptr);
    context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context.RSSetViewports(1, &viewport);
    context.VSSetShader(vertexShader, nullptr, 0);
    context.PSSetShader(pixelShader, nullptr, 0);
    if (cb2 != nullptr) context.PSSetConstantBuffers(2, 1, &cb2);
    if (toneMapConstants != nullptr)
        context.PSSetConstantBuffers(13, 1, &toneMapConstants);
    context.PSSetShaderResources(0, resourceCount, resources);
    context.PSSetSamplers(0, samplerCount, samplers);
    context.OMSetRenderTargets(targetCount, targets, nullptr);
    context.Draw(3, 0);

    context.OMSetRenderTargets(0, nullptr, nullptr);
    std::array<ID3D11ShaderResourceView*, 8> nullResources{};
    std::array<ID3D11SamplerState*, 8> nullSamplers{};
    ID3D11Buffer* nullBuffer = nullptr;
    context.PSSetShaderResources(
        0, static_cast<UINT>(nullResources.size()), nullResources.data());
    context.PSSetSamplers(
        0, static_cast<UINT>(nullSamplers.size()), nullSamplers.data());
    context.PSSetConstantBuffers(2, 1, &nullBuffer);
    context.PSSetConstantBuffers(13, 1, &nullBuffer);
    context.PSSetShader(nullptr, nullptr, 0);
    context.VSSetShader(nullptr, nullptr, 0);
}

void Dispatch(ID3D11DeviceContext& context, const ComputeDispatch& dispatch)
{
    const auto& [shader, constants, resources, resourceCount, output] = dispatch;
    context.CSSetShader(shader, nullptr, 0);
    context.CSSetConstantBuffers(0, 1, &constants);
    context.CSSetShaderResources(0, resourceCount, resources);
    context.CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context.Dispatch(1, 1, 1);

    ID3D11UnorderedAccessView* nullUav = nullptr;
    std::array<ID3D11ShaderResourceView*, 4> nullSrvs{};
    ID3D11Buffer* nullBuffer = nullptr;
    context.CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    context.CSSetShaderResources(
        0, static_cast<UINT>(nullSrvs.size()), nullSrvs.data());
    context.CSSetConstantBuffers(0, 1, &nullBuffer);
    context.CSSetShader(nullptr, nullptr, 0);
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

[[nodiscard]] ComPtr<ID3D11Buffer> CreateConstantBuffer(
    ID3D11Device& device, ToneMapConstants constants)
{
    if (constants.productionMode >= 1U && constants.productionMode <= 3U)
    {
        const auto shaper = fo4hdr::OperatorLutShaperParameters(
            static_cast<fo4hdr::ProductionToneMapperAlgorithm>(constants.productionMode - 1U),
            constants.displayPeakNits);
        constants.operatorShaperAngleGain = shaper[0];
        constants.operatorShaperBias = shaper[1];
        constants.operatorShaperParameters = {shaper[2], shaper[3]};
    }
    return CreateConstantBuffer<ToneMapConstants>(device, constants);
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

[[nodiscard]] Texture3D CreateNativeGrade(ID3D11Device& device, unsigned grade)
{
    constexpr UINT size = 16;
    std::array<Float4, size * size * size> pixels{};
    for (UINT b = 0; b < size; ++b)
        for (UINT g = 0; g < size; ++g)
            for (UINT r = 0; r < size; ++r) {
                const auto v = SyntheticNativeGrade({r / 15.0, g / 15.0, b / 15.0}, grade);
                pixels[(b * size + g) * size + r] = {
                    static_cast<float>(v[0]), static_cast<float>(v[1]),
                    static_cast<float>(v[2]), 1.0F};
            }
    D3D11_TEXTURE3D_DESC desc{};
    desc.Width = desc.Height = desc.Depth = size;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), size * sizeof(Float4),
        size * size * sizeof(Float4)};
    Texture3D result;
    CheckHresult(device.CreateTexture3D(&desc, &data, &result.texture), "Native grading grade texture");
    CheckHresult(device.CreateShaderResourceView(result.texture.Get(), nullptr,
        &result.srv), "Native grading grade SRV");
    return result;
}

}  // namespace fo4hdr::tests::gpu

namespace fo4hdr::tests::gpu {

int g_failures = 0;
int g_skips = 0;

void Check(bool condition, std::string_view expression,
    std::string_view file, int line)
{
    if (condition) return;
    ++g_failures;
    std::cerr << file << ':' << line << ": FAIL: " << expression << '\n';
}

void DispatchPresentation(Device& device, const PresentationDispatch& request)
{
    auto& context = *device.context.Get();
    context.CSSetShader(request.shader, nullptr, 0);
    context.CSSetConstantBuffers(0, 1, &request.constants);
    context.CSSetShaderResources(0, 4, request.inputs.data());
    context.CSSetUnorderedAccessViews(0, 1, &request.output, nullptr);
    context.Dispatch((request.width + 7) / 8, (request.height + 7) / 8, 1);
    ID3D11UnorderedAccessView* noOutput{};
    const std::array<ID3D11ShaderResourceView*, 4> noInputs{};
    context.CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
    context.CSSetShaderResources(0, 4, noInputs.data());
}

PresentationWindow::PresentationWindow()
    : handle(CreateWindowExW(0, L"STATIC", L"Fallout4HDR presentation test",
        WS_OVERLAPPEDWINDOW, 0, 0, 128, 128, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr))
{}

PresentationWindow::~PresentationWindow() { if (handle) DestroyWindow(handle); }

ComPtr<IDXGISwapChain1> CreatePresentationChain(Device& device, HWND window,
    UINT width, UINT height)
{
    if (!window) throw std::runtime_error("Create presentation HWND failed");
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    CheckHresult(device.device.As(&dxgi), "Get presentation DXGI device");
    CheckHresult(dxgi->GetAdapter(&adapter), "Get presentation adapter");
    CheckHresult(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get presentation factory");
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    CheckHresult(factory->CreateSwapChainForHwnd(device.device.Get(), window,
        &description, nullptr, nullptr, &chain), "Create R10 flip-discard UAV chain");
    return chain;
}

Texture GetPresentationBuffer(Device& device, IDXGISwapChain1& chain)
{
    Texture result;
    CheckHresult(chain.GetBuffer(0, IID_PPV_ARGS(&result.texture)), "Get flip buffer zero");
    CheckHresult(device.device->CreateUnorderedAccessView(result.texture.Get(),
        nullptr, &result.uav), "Create real flip-buffer UAV");
    D3D11_TEXTURE2D_DESC description{};
    result.texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CheckHresult(device.device->CreateTexture2D(&description, nullptr,
        &result.staging), "Create flip-buffer readback");
    return result;
}

}  // namespace fo4hdr::tests::gpu
