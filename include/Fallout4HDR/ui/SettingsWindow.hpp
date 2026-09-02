// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Fallout4HDR/ui/HdrSettingsInput.hpp"

#include <d3d11_1.h>

#include <cstdint>
#include <memory>

namespace fo4hdr {

class LiveSettings;

enum class SettingsWindowScope { unavailable, qualifiedExclusive };

struct SettingsWindowConfiguration
{
    HWND window{};
    UINT width{};
    UINT height{};
    HdrSettingsInput* input{};
    LiveSettings* live{};
};

struct SettingsWindowOverlay
{
    ID3D11ShaderResourceView* source{};
    std::uint64_t frame{};
    UINT width{};
    UINT height{};
    std::uint32_t calibrationMode{};
};

// Initialize/Draw/Retire and destruction require one renderer thread. Borrowed
// input/live owners outlive this owner; input callbacks never call ImGui.
// Qualified scope excludes active queries, command lists, concurrent context
// use and reentry. Stock backend buffer/texture growth is not allocation-free.
class SettingsWindow
{
public:
    SettingsWindow() noexcept;
    ~SettingsWindow() noexcept;
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    [[nodiscard]] bool Initialize(ID3D11Device* device, ID3D11DeviceContext1* context,
        SettingsWindowConfiguration configuration) noexcept;
    // Returned FP16 RGB is premultiplied authored IEC code, with independent
    // coverage alpha, BT.709/UI-relative. Consume only in the named frame,
    // without externally binding its private view; decode before HDR blend.
    // A hidden calibration control surface can publish a null source with current
    // frame/extent/mode. Mode 0 is normal, 10-14 are diagnostic patterns.
    // Same-frame Draw returns the saved publication without draining input again.
    // A newer-frame Draw or Retire invalidates it. Never render into PQ/R10.
    [[nodiscard]] SettingsWindowOverlay Draw(std::uint64_t frame,
        SettingsWindowScope scope) noexcept;
    // Retire before input Stop/device reset/resize, with callbacks and rendering
    // quiescent. Resize uses a new cold owner with fresh dimensions.
    [[nodiscard]] bool Retire() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace fo4hdr
