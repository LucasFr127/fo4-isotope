// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShippingPixelShaderTests.hpp"
#include "TestHarness.hpp"

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
}

}  // namespace fo4hdr::tests::gpu
