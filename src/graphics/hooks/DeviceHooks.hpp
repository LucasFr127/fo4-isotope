// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../DeviceCreationHook.hpp"
#include "../GameDeviceGeneration.hpp"
#include "../OperatorLutWorker.hpp"

#include <cstdint>

namespace fo4hdr {

enum class NativeRoutingStatus
{
    unqualified,
    residentRecordUnavailable,
    preparationFailed,
    installationFailed,
    installed
};

// Synchronous cold borrows. The exact result initializes both GPU owners before
// moving into the generation's resident settings host after input activation.
struct NativeSettingsStartup
{
    const GraphicsStartupConfiguration& configuration;
    std::unique_ptr<OperatorLutBuildResult>& prepared;
};

// Admission is called only by the successful fresh-creation callback, before
// outputs escape. A resident record is never recycled for another generation.
class NativeRoutingInstaller
{
public:
    [[nodiscard]] static NativeRoutingStatus Install(const CreatedDeviceBinding& created,
        std::uint64_t generation, void* creationNext) noexcept;
    [[nodiscard]] static bool InstallHdr(const CreatedDeviceBinding& created,
        std::uint64_t generation, void* creationNext, const NativeSettingsStartup& startup) noexcept;
    [[nodiscard]] static bool ActivateSettingsHost(const CreatedDeviceBinding& created,
        const NativeSettingsStartup& startup) noexcept;
    // Fresh outputs have not escaped. False retains resident references and forbids retry.
    [[nodiscard]] static bool DiscardFresh(const CreatedDeviceBinding& created) noexcept;
    [[nodiscard]] static bool Retire() noexcept;

private:
    [[nodiscard]] static bool CompleteCoverage() noexcept;
    [[nodiscard]] static NativeRoutingStatus InstallImpl(const CreatedDeviceBinding& created,
        std::uint64_t generation, void* creationNext, const NativeSettingsStartup* startup) noexcept;
};

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(ID3D11Device* device, const void* bytecode,
    SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) noexcept;
HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC* description, const D3D11_SUBRESOURCE_DATA* initialData,
    ID3D11Texture2D** texture) noexcept;
HRESULT STDMETHODCALLTYPE HookCreateSrv(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* description, ID3D11ShaderResourceView** view) noexcept;
HRESULT STDMETHODCALLTYPE HookCreateUav(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC* description, ID3D11UnorderedAccessView** view) noexcept;
HRESULT STDMETHODCALLTYPE HookCreateRtv(ID3D11Device* device, ID3D11Resource* resource,
    const D3D11_RENDER_TARGET_VIEW_DESC* description, ID3D11RenderTargetView** view) noexcept;

}  // namespace fo4hdr
