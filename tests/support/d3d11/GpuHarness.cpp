// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuHarness.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fo4hdr::tests::gpu {
namespace {

using Microsoft::WRL::ComPtr;

struct TransferResources
{
    ComPtr<ID3D11Texture2D> input;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11ComputeShader> shader;
};

struct TransferConstants
{
    std::uint32_t width;
    std::uint32_t height;
    float displayPeakNits;
    float padding{};
};
static_assert(sizeof(TransferConstants) == 16);
static_assert(sizeof(Float4) == 16);

}  // namespace

void CheckHresult(HRESULT result, std::string_view operation)
{
    if (FAILED(result))
    {
        throw std::runtime_error(std::string{operation} + " failed: " +
            std::to_string(static_cast<std::uint32_t>(result)));
    }
}

Device CreateWarpDevice()
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

std::vector<std::uint32_t> ReadPackedTexture(const Device& gpu, ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    CheckHresult(gpu.device->CreateTexture2D(&description, nullptr, &staging), "Terminal staging");
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(description.Width) * description.Height);
    gpu.context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Terminal map");
    if (!mapped.pData || mapped.RowPitch < description.Width * sizeof(std::uint32_t))
    {
        gpu.context->Unmap(staging.Get(), 0);
        throw std::runtime_error("Invalid terminal readback pitch");
    }
    for (UINT row = 0; row < description.Height; ++row)
        std::memcpy(pixels.data() + row * description.Width,
            static_cast<const char*>(mapped.pData) + row * mapped.RowPitch,
            description.Width * sizeof(std::uint32_t));
    gpu.context->Unmap(staging.Get(), 0);
    return pixels;
}

namespace {

D3D11_TEXTURE2D_DESC TextureDescription()
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(kTransferWidth);
    description.Height = static_cast<UINT>(kTransferHeight);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.SampleDesc.Count = 1;
    return description;
}

TransferResources CreateTransferResources(ID3D11Device& device,
    const std::vector<char>& bytecode, const TransferPixels& input, float peak)
{
    TransferResources result;
    auto description = TextureDescription();
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA pixels{
        input.data(), static_cast<UINT>(kTransferWidth * sizeof(Float4)), 0};
    CheckHresult(device.CreateTexture2D(&description, &pixels, result.input.GetAddressOf()),
        "CreateTexture2D(input)");
    CheckHresult(device.CreateShaderResourceView(result.input.Get(), nullptr,
        result.srv.GetAddressOf()), "CreateShaderResourceView");
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    CheckHresult(device.CreateTexture2D(&description, nullptr, result.output.GetAddressOf()),
        "CreateTexture2D(output)");
    CheckHresult(device.CreateUnorderedAccessView(result.output.Get(), nullptr,
        result.uav.GetAddressOf()), "CreateUnorderedAccessView");
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CheckHresult(device.CreateTexture2D(&description, nullptr, result.staging.GetAddressOf()),
        "CreateTexture2D(staging)");
    const TransferConstants constants{static_cast<std::uint32_t>(kTransferWidth),
        static_cast<std::uint32_t>(kTransferHeight), peak};
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = static_cast<UINT>(sizeof(constants));
    buffer.Usage = D3D11_USAGE_IMMUTABLE;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA constantData{&constants, 0, 0};
    CheckHresult(device.CreateBuffer(&buffer, &constantData, result.constants.GetAddressOf()),
        "CreateBuffer(constants)");
    CheckHresult(device.CreateComputeShader(bytecode.data(), bytecode.size(), nullptr,
        result.shader.GetAddressOf()), "CreateComputeShader");
    return result;
}

void DispatchTransfer(ID3D11DeviceContext& context, const TransferResources& resources)
{
    auto* constants = resources.constants.Get();
    auto* srv = resources.srv.Get();
    auto* uav = resources.uav.Get();
    context.CSSetShader(resources.shader.Get(), nullptr, 0);
    context.CSSetConstantBuffers(0, 1, &constants);
    context.CSSetShaderResources(0, 1, &srv);
    context.CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context.Dispatch(static_cast<UINT>((kTransferWidth + 7) / 8),
        static_cast<UINT>(kTransferHeight), 1);
    constants = nullptr;
    srv = nullptr;
    uav = nullptr;
    context.CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context.CSSetShaderResources(0, 1, &srv);
    context.CSSetConstantBuffers(0, 1, &constants);
    context.CSSetShader(nullptr, nullptr, 0);
}

TransferPixels ReadTransfer(ID3D11DeviceContext& context, const TransferResources& resources)
{
    context.CopyResource(resources.staging.Get(), resources.output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CheckHresult(context.Map(resources.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
        "Map(staging)");
    constexpr auto rowBytes = kTransferWidth * sizeof(Float4);
    if (mapped.pData == nullptr || mapped.RowPitch < rowBytes)
    {
        context.Unmap(resources.staging.Get(), 0);
        throw std::runtime_error("Invalid staging row pitch");
    }
    TransferPixels result{};
    const auto* data = static_cast<const std::byte*>(mapped.pData);
    for (std::size_t row = 0; row < kTransferHeight; ++row)
    {
        std::memcpy(result.data() + row * kTransferWidth, data + row * mapped.RowPitch, rowBytes);
    }
    context.Unmap(resources.staging.Get(), 0);
    return result;
}

}  // namespace

TransferPixels EvaluateTransfer(const std::filesystem::path& shaderPath,
    const TransferPixels& input, float displayPeakNits)
{
    const auto device = CreateWarpDevice();
    const auto bytecode = ReadShader(shaderPath);
    const auto resources = CreateTransferResources(*device.device.Get(), bytecode, input,
        displayPeakNits);
    DispatchTransfer(*device.context.Get(), resources);
    return ReadTransfer(*device.context.Get(), resources);
}

}  // namespace fo4hdr::tests::gpu
