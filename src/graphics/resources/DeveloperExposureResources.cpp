// SPDX-License-Identifier: GPL-3.0-or-later

#include "DeveloperExposureResources.hpp"
#include "ReplacementShaders.hpp"
#include "../RendererLifetime.hpp"
#include "../GraphicsDiagnostics.hpp"
#include "../LiveSettings.hpp"
#include <Fallout4HDR/ShippingShaders.generated.hpp>
#include <algorithm>
#include <cstring>
#include <utility>

namespace fo4hdr {
using Microsoft::WRL::ComPtr;

DeveloperExposureResources g_developerExposure;

template <std::size_t Size>
[[nodiscard]] HRESULT CreateComputeShader(
    ID3D11Device* device, const std::array<std::byte, Size>& bytecode,
    ID3D11ComputeShader** shader) noexcept
{
    return device->CreateComputeShader(
        bytecode.data(), bytecode.size(), nullptr, shader);
}

template <std::size_t Size>
void SetD3dDebugName(
    ID3D11DeviceChild* object,
    const char (&name)[Size]) noexcept
{
    if (object != nullptr)
        (void)object->SetPrivateData(WKPDID_D3DDebugObjectName,
            static_cast<UINT>(Size - 1), name);
}

[[nodiscard]] bool CreateDeveloperExposureTexture(
    ID3D11Device* device,
    ComPtr<ID3D11Texture2D>& texture,
    ComPtr<ID3D11ShaderResourceView>& srv,
    ComPtr<ID3D11UnorderedAccessView>& uav) noexcept
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 1;
    description.Height = 1;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R32G32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE |
        D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateTexture2D(
               &description, nullptr, &texture)) &&
        SUCCEEDED(device->CreateShaderResourceView(
            texture.Get(), nullptr, &srv)) &&
        SUCCEEDED(device->CreateUnorderedAccessView(
            texture.Get(), nullptr, &uav));
}

[[nodiscard]] bool DeveloperExposureResourcesReady() noexcept
{
    const auto& diagnostic = g_developerExposure;
    const std::array<ID3D11DeviceChild*, 3> required{
        diagnostic.texture.Get(), diagnostic.constants.Get(), diagnostic.captureShader.Get()
    };
    return std::ranges::find(required, nullptr) == required.end();
}

[[nodiscard]] bool EnsureDeveloperExposureResources(ID3D11Device* device) noexcept
{
    auto& diagnostic = g_developerExposure;
    if (DeveloperExposureResourcesReady()) return true;
    if (!device || !g_rendererDevice.activeContext) return false;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants;
    if (!CreateDeveloperExposureTexture(device, texture, srv, uav) ||
        FAILED(CreateComputeShader(device, generated::kDeveloperExposureCaptureCs, &shader)))
        return false;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(OperatorConstants);
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&description, nullptr, &constants))) return false;
    diagnostic.texture = std::move(texture);
    diagnostic.srv = std::move(srv);
    diagnostic.uav = std::move(uav);
    diagnostic.captureShader = std::move(shader);
    diagnostic.constants = std::move(constants);
    (void)g_rendererDevice.activeContext->QueryInterface(IID_PPV_ARGS(&diagnostic.context1));
    SetD3dDebugName(diagnostic.texture.Get(), "Fallout4HDR developer exposure snapshot");
    return true;
}

[[nodiscard]] bool DeveloperExposureFrameEligible(
    ID3D11DeviceContext* context, bool rendered, bool wasActive) noexcept
{
    return g_configuration.developerMode && rendered && context &&
        g_gameplayReadyApplied.load(std::memory_order_acquire) &&
        g_liveSettingsThreadCompatible.load(std::memory_order_acquire) &&
        (!wasActive || g_developerExposure.observedFrame == g_frame);
}

void UpdateDeveloperExposureConstants(ID3D11DeviceContext* context, bool wasActive) noexcept
{
    auto& diagnostic = g_developerExposure;
    // A separate b13 copy keeps the diagnostic outside persisted settings,
    // production configuration identity, and asynchronous operator LUT keys.
    auto constants = g_replacements.configurationKey;
    constants.liveSettingsReserved = 1U;
    if (wasActive && std::memcmp(&constants, &diagnostic.constantsKey, sizeof(constants)) == 0)
        return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(diagnostic.constants.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        diagnostic.failed = true;
        diagnostic.state.Reset();
        return;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(diagnostic.constants.Get(), 0);
    diagnostic.constantsKey = constants;
}

void CommitDeveloperExposureAtEndPresent(
    ID3D11Device* device, ID3D11DeviceContext* context, bool rendered) noexcept
{
    auto& diagnostic = g_developerExposure;
    const bool wasActive = diagnostic.state.Active();
    if (!diagnostic.state.Requested() && !wasActive) return;
    const bool eligible = DeveloperExposureFrameEligible(context, rendered, wasActive);
    if (eligible && diagnostic.state.Requested()) {
        if (!EnsureDeveloperExposureResources(device)) {
            diagnostic.failed = true;
            diagnostic.state.Reset();
        }
    }
    diagnostic.state.EndFrame(g_frame,
        g_evidence.resourceGeneration.load(std::memory_order_acquire), eligible);
    if (diagnostic.state.Active()) UpdateDeveloperExposureConstants(context, wasActive);
}


void ResetDeveloperExposureResources() noexcept { g_developerExposure = {}; }

} // namespace fo4hdr
