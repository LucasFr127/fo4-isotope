// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace fo4hdr::game {

[[nodiscard]] bool InitializeNativeUiResources(bool requested, const void* trampolineInterface,
    std::uint32_t pluginHandle) noexcept;

namespace detail {

struct NativeUiRegistration
{
    std::uintptr_t manager{};
    std::uint32_t thread{};
    std::int32_t slot{};
    std::uint8_t mode{};
};

// The native mode-1 registrar copies this invocation-local descriptor before
// creating its dedicated texture and inferred views. Preserve every other byte.
template <class Properties, class Forward>
bool ForwardNativeUiRegistration(const Properties* properties, bool qualified, Forward&& forward)
{
    static_assert(std::is_trivially_copyable_v<Properties>);
    if (!qualified || !properties) {
        forward(properties);
        return false;
    }
    Properties promoted;
    std::memcpy(&promoted, properties, sizeof(promoted));
    promoted.format = static_cast<decltype(promoted.format)>(10);
    forward(&promoted);
    return true;
}

template <class Properties>
[[nodiscard]] bool NativePauseTextureShape(const Properties& properties) noexcept
{
    return properties.width != 0 && properties.height != 0 &&
        static_cast<std::uint32_t>(properties.format) == 24 && properties.multiSample == 0 &&
        properties.mipLevel == -1 && properties.textureTarget == 0;
}
template <class Properties>
[[nodiscard]] bool NativePauseTextureFlags(const Properties& properties) noexcept
{
    return !properties.copyable && properties.supportUnorderedAccess &&
        !properties.allowMipGeneration && !properties.forceLinear && !properties.enableFastClear;
}
template <class Properties>
[[nodiscard]] bool NativePauseProperties(const Properties& properties) noexcept
{
    return NativePauseTextureShape(properties) && NativePauseTextureFlags(properties);
}

// Access is serialized by the allocation callback's nonblocking entry guard.
// A new slot-24 invocation retires readiness before native resources change.
class NativeUiResourcePair
{
    std::uintptr_t manager_{};
    std::uint32_t thread_{};
    bool firstComplete_{};
public:
    [[nodiscard]] bool Begin(const NativeUiRegistration& call) noexcept
    {
        if (call.slot == 24) {
            manager_ = call.manager;
            thread_ = call.thread;
            firstComplete_ = false;
            return manager_ != 0 && thread_ != 0 && call.mode == 1;
        }
        const auto valid = call.slot == 25 && call.mode == 1 && firstComplete_ &&
            call.manager == manager_ && call.thread == thread_;
        firstComplete_ = false;
        return valid;
    }
    [[nodiscard]] bool Complete(std::int32_t slot, bool valid) noexcept
    {
        firstComplete_ = slot == 24 && valid;
        return slot == 25 && valid;
    }
};

}  // namespace detail
}  // namespace fo4hdr::game
