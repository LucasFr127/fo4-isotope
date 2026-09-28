// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
// Copyright (c) 2025 Carlos Lopez Jr.
// Upstream: clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/src/shaders/draw.hlsl
//
// Fallout 4 shader identities and tone/LUT seams use the pinned RenoDX facts.
// The exact-backbuffer FP16 proxy and UI-relative transport adapt
// clshortfuse/renodx@9ac0ad2f0e4a7a28fcdd0c0ece912b5ac4772e27/
// src/games/fallout4/addon.cpp and src/mods/swapchain_v2.hpp.
// RenoDX material retains its MIT permission in THIRD_PARTY_NOTICES.md.
// Native hooks, generation ownership, settings, grading integration and direct
// display output are project implementations of the Fallout 4 and DXGI contracts.

#include "ShaderRegistry.hpp"
#include "GraphicsDiagnostics.hpp"
#include "RenoDxShaderIdentity.hpp"
#include "FO4Isotope/hooks/HookOwnership.hpp"

#if defined(FO4HDR_RUNTIME_1_10_163)
#  include "../../runtime/1.10.163/RuntimeIdentity.generated.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <span>

namespace fo4hdr {
namespace {
constexpr std::size_t kMaximumShaders = 64;
constexpr std::size_t kMaximumObjects = 4;
constexpr std::size_t kRegistryAttempts = 64;
struct ShaderRecord
{
    std::atomic<ID3D11PixelShader*> shader{};
    RenoDxShaderPass pass{};
};
std::array<ShaderRecord, kMaximumShaders> g_shaders{};
std::array<std::atomic<ID3D11PixelShader*>, kMaximumObjects> g_lateGameUiShaders{};
std::array<std::atomic<ID3D11PixelShader*>, kMaximumObjects> g_lateGameUiBackdropShaders{};
std::atomic_flag g_shaderLock = ATOMIC_FLAG_INIT;
}  // namespace

[[nodiscard]] const RenoDxShaderDescriptor* FindShaderDescriptor(
    RenoDxShaderPass pass) noexcept
{
    for (const auto& descriptor : kRenoDxFallout4Shaders)
        if (descriptor.pass == pass) return &descriptor;
    return nullptr;
}

[[nodiscard]] const RenoDxShaderDescriptor* FindShader(
    ID3D11PixelShader* shader) noexcept
{
    if (shader == nullptr) return nullptr;
    for (const auto& entry : g_shaders)
    {
        if (entry.shader.load(std::memory_order_acquire) == shader)
        {
            if (const auto* descriptor = FindShaderDescriptor(entry.pass))
                return descriptor;
        }
    }
    return nullptr;
}

[[nodiscard]] bool MatchesDxbcHash(const void* bytecode,
    std::size_t bytecodeLength,
    const std::array<std::uint32_t, 4>& expectedHash) noexcept
{
    constexpr std::array<std::byte, 4> magic{
        std::byte{0x44}, std::byte{0x58},
        std::byte{0x42}, std::byte{0x43}};
    if (bytecode == nullptr || bytecodeLength < 20) return false;
    const auto bytes = std::span{
        static_cast<const std::byte*>(bytecode), bytecodeLength};
    return std::equal(magic.begin(), magic.end(), bytes.begin()) &&
        std::memcmp(bytes.data() + magic.size(),
            expectedHash.data(), sizeof(expectedHash)) == 0;
}

enum class LateGameUiShaderKind { composite, backdrop };

[[nodiscard]] bool MatchesLateGameUiBytecode(const void* bytecode,
    std::size_t bytecodeLength, LateGameUiShaderKind kind) noexcept
{
#if defined(FO4HDR_RUNTIME_1_10_163)
    const auto& hash = kind == LateGameUiShaderKind::composite ?
        generated::kLateGameUiCompositeDxbcHash : generated::kLateGameUiBackdropDxbcHash;
    return MatchesDxbcHash(bytecode, bytecodeLength, hash);
#else
    (void)bytecode;
    (void)bytecodeLength;
    (void)kind;
    return false;
#endif
}

template <std::size_t Count>
[[nodiscard]] bool ContainsLateGameUiShader(
    const std::array<std::atomic<ID3D11PixelShader*>, Count>& registry,
    ID3D11PixelShader* shader) noexcept
{
    for (const auto& registered : registry)
        if (registered.load(std::memory_order_acquire) == shader) return true;
    return false;
}
[[nodiscard]] bool IsLateGameUiShader(
    ID3D11PixelShader* shader) noexcept
{
    if (shader == nullptr) return false;
    return ContainsLateGameUiShader(g_lateGameUiShaders, shader);
}

[[nodiscard]] bool IsLateGameUiBackdropShader(
    ID3D11PixelShader* shader) noexcept
{
    if (shader == nullptr) return false;
    return ContainsLateGameUiShader(g_lateGameUiBackdropShaders, shader);
}

bool ContainsRegisteredShader(ID3D11PixelShader* shader) noexcept
{
    for (auto& entry : g_shaders)
    {
        if (entry.shader.load(std::memory_order_acquire) == shader) return true;
    }
    return false;
}

void StoreRegisteredShader(ID3D11PixelShader* shader, RenoDxShaderPass pass) noexcept
{
    for (auto& entry : g_shaders)
    {
        if (entry.shader.load(std::memory_order_relaxed) == nullptr)
        {
            entry.pass = pass;
            entry.shader.store(shader, std::memory_order_release);
            return;
        }
    }
    RecordFailure(GraphicsObservationFailure::registryExhausted);
}

void RegisterShader(
    ID3D11PixelShader* shader, RenoDxShaderPass pass) noexcept
{
    if (shader == nullptr) return;
    BoundedAtomicFlagGuard guard(g_shaderLock, kRegistryAttempts);
    if (!guard.OwnsLock())
    {
        RecordFailure(GraphicsObservationFailure::registryBusy);
        return;
    }
    if (ContainsRegisteredShader(shader)) return;
    StoreRegisteredShader(shader, pass);
}

template <std::size_t Count>
void RegisterLateGameUiShader(
    std::array<std::atomic<ID3D11PixelShader*>, Count>& registry,
    ID3D11PixelShader* shader) noexcept
{
    if (shader == nullptr) return;
    BoundedAtomicFlagGuard guard(g_shaderLock, kRegistryAttempts);
    if (!guard.OwnsLock())
    {
        RecordFailure(GraphicsObservationFailure::registryBusy);
        return;
    }
    if (ContainsLateGameUiShader(registry, shader)) return;
    for (auto& entry : registry)
    {
        auto* expected = static_cast<ID3D11PixelShader*>(nullptr);
        if (entry.compare_exchange_strong(expected, shader,
                std::memory_order_acq_rel))
            return;
    }
    RecordFailure(GraphicsObservationFailure::registryExhausted);
}

void ResetShaderRegistry() noexcept
{
    BoundedAtomicFlagGuard guard(g_shaderLock, kRegistryAttempts);
    if (!guard.OwnsLock()) return;
    for (auto& entry : g_shaders)
    {
        entry.shader.store(nullptr, std::memory_order_release);
        entry.pass = RenoDxShaderPass::toneMapA;
    }
    for (auto& entry : g_lateGameUiShaders)
        entry.store(nullptr, std::memory_order_release);
    for (auto& entry : g_lateGameUiBackdropShaders)
        entry.store(nullptr, std::memory_order_release);
}

bool CreatedShaderIdentity::PipboyHashMismatch(PipboyScreenShader role,
    const void* bytecode, std::size_t length)
{
    switch (role)
    {
    case PipboyScreenShader::producer:
        return !MatchesDxbcHash(bytecode, length,
            {0x28465710, 0x966a0df5, 0xd2c8c2d7, 0x56e8e66d});
    case PipboyScreenShader::consumer:
        return !MatchesDxbcHash(bytecode, length,
            {0x2ff863b6, 0xf45e6855, 0x9304fad4, 0xeb237c12});
    default: return false;
    }
}

void CreatedShaderIdentity::RecordKnownShader(std::uint32_t crc32) const
{
    if (descriptor == nullptr)
    {
        g_evidence.unknownPixelShaderCreateCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_evidence.knownPixelShaderCreateCount.fetch_add(1, std::memory_order_relaxed);
    g_evidence.knownShaderMask.fetch_or(RenoDxShaderBit(descriptor->pass), std::memory_order_relaxed);
    g_evidence.lastKnownShaderCrc32.store(crc32, std::memory_order_release);
}

void CreatedShaderIdentity::Observe(const void* bytecode, std::size_t length, bool developerMode)
{
    if (bytecode == nullptr || length == 0) return;
    lateGameUiComposite = MatchesLateGameUiBytecode(bytecode, length, LateGameUiShaderKind::composite);
    lateGameUiBackdrop = MatchesLateGameUiBytecode(bytecode, length, LateGameUiShaderKind::backdrop);
    const auto crc32 = ComputeRenoDxShaderCrc32(
        std::span<const std::byte>(static_cast<const std::byte*>(bytecode), length));
    descriptor = FindRenoDxFallout4Shader(crc32);
    if (developerMode)
    {
        pipboyRole = IdentifyPipboyScreenShader(crc32, length);
        if (PipboyHashMismatch(pipboyRole, bytecode, length))
            pipboyRole = PipboyScreenShader::unknown;
    }
    RecordKnownShader(crc32);
}

bool CreatedShaderIdentity::CreatedSuccessfully(HRESULT result, ID3D11PixelShader** shader)
{
    return SUCCEEDED(result) && shader != nullptr && *shader != nullptr;
}

void CreatedShaderIdentity::AttachPipboyRole(HRESULT result, ID3D11PixelShader** shader) const
{
    if (CreatedSuccessfully(result, shader) && pipboyRole != PipboyScreenShader::unknown)
        (void)(*shader)->SetPrivateData(kPipboyScreenShaderRole, sizeof(pipboyRole), &pipboyRole);
}

void CreatedShaderIdentity::Register(HRESULT result, ID3D11PixelShader** shader) const
{
    AttachPipboyRole(result, shader);
    if (CreatedSuccessfully(result, shader) && descriptor != nullptr)
        RegisterShader(*shader, descriptor->pass);
    if (CreatedSuccessfully(result, shader) && lateGameUiComposite)
        RegisterLateGameUiShader(g_lateGameUiShaders, *shader);
    if (CreatedSuccessfully(result, shader) && lateGameUiBackdrop)
        RegisterLateGameUiShader(g_lateGameUiBackdropShaders, *shader);
}

}  // namespace fo4hdr