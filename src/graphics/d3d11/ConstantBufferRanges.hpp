// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_1.h>

#include <cstddef>

namespace fo4hdr {

// The context shadow tags its object with the vtable the object had before the shadow.
// A table another module installs over the shadow later may copy only the 115 D3D11.0
// slots (High FPS Physics Fix's draw-call counter does), so these D3D11.1 calls
// dispatch through the tagged table instead of the object's current one.
inline constexpr GUID kOriginalContextTable{0x5517d329, 0x06ad, 0x491e,
    {0x9d, 0x27, 0xad, 0xfd, 0xfa, 0x73, 0x19, 0x95}};

[[nodiscard]] inline void* const* OriginalContextTable(ID3D11DeviceContext1* context) noexcept
{
    void* const* table{};
    UINT size = sizeof(table);
    if (SUCCEEDED(context->GetPrivateData(kOriginalContextTable, &size, &table)) && table)
        return table;
    return *reinterpret_cast<void* const* const*>(context);
}

// Offset within each group of six D3D11.1 methods, ordered VS, HS, DS, GS, PS, CS.
enum class ConstantBufferStage : std::size_t
{
    vertex = 0,
    pixel = 4,
    compute = 5
};

// One constant-buffer slot of a context stage. Get and Set take the same buffer,
// first-constant and constant-count pointers as the ID3D11DeviceContext1 methods.
class ConstantBufferRangeSlot
{
public:
    ConstantBufferRangeSlot(ID3D11DeviceContext1* context, ConstantBufferStage stage,
        UINT slot) noexcept : context_(context), stage_(stage), slot_(slot)
    {
    }

    void Get(ID3D11Buffer** buffer, UINT* first, UINT* count) const noexcept
    {
        using Getter = void(STDMETHODCALLTYPE*)(
            ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer**, UINT*, UINT*);
        constexpr std::size_t kVsGetConstantBuffers1 = 125;
        const auto getter = reinterpret_cast<Getter>(TableEntry(kVsGetConstantBuffers1));
        getter(context_, slot_, 1, buffer, first, count);
    }

    void Set(ID3D11Buffer* const* buffer, const UINT* first, const UINT* count) const noexcept
    {
        using Setter = void(STDMETHODCALLTYPE*)(
            ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
        constexpr std::size_t kVsSetConstantBuffers1 = 119;
        const auto setter = reinterpret_cast<Setter>(TableEntry(kVsSetConstantBuffers1));
        setter(context_, slot_, 1, buffer, first, count);
    }

private:
    [[nodiscard]] void* TableEntry(std::size_t vertexSlot) const noexcept
    {
        return OriginalContextTable(context_)[vertexSlot + static_cast<std::size_t>(stage_)];
    }

    ID3D11DeviceContext1* context_;
    ConstantBufferStage stage_;
    UINT slot_;
};

} // namespace fo4hdr
