// SPDX-License-Identifier: LicenseRef-FO4Isotope-GPL-3.0-or-later-with-CommonLibF4-Exceptions

#pragma once

#include "GpuHarness.hpp"
#include "hooks/ContextHooks.hpp"
#include "hooks/DeviceHooks.hpp"
#include "hooks/HookRecords.hpp"
#include "passes/PresentationPass.hpp"
#include "passes/ObservedDraw.hpp"
#include "resources/PresentationResources.hpp"
#include "resources/SceneResources.hpp"
#include "resources/ArmorCompositionResources.hpp"
#include "RendererLifetime.hpp"
#include "PresentFrame.hpp"
#include "SharedUiFrame.hpp"
#include "ShaderRegistry.hpp"
#include "DisplayDiscovery.hpp"
#include "GraphicsDiagnostics.hpp"
#include "LiveSettings.hpp"
#include "resources/ReplacementShaders.hpp"
#include "FO4Isotope/output/Hdr10Color.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace fo4hdr::tests::gpu {

template <class Value, std::size_t Count>
struct ContextAtomicRestore
{
    std::array<std::atomic<Value>*, Count> fields;
    std::array<Value, Count> saved{};

    explicit ContextAtomicRestore(std::array<std::atomic<Value>*, Count> values) : fields(values)
    {
        for (std::size_t index = 0; index < Count; ++index) saved[index] = fields[index]->load();
    }
    ~ContextAtomicRestore()
    {
        for (std::size_t index = 0; index < Count; ++index) fields[index]->store(saved[index]);
    }
};

struct PresentationContextFixture
{
    Device& device;
    RendererDevice savedDevice{g_rendererDevice};
    PresentationResources savedPresentation{g_presentation};
    UiTransportResources savedUi{g_uiTransport};
    SceneResources savedScene{g_scene};
    ArmorCompositionResources savedArmor{g_armor};
    PipelineHeartbeat savedHeartbeat{g_heartbeat};
    PluginConfiguration savedConfiguration{g_configuration};
    HdrSettings savedSettings{g_activeSettings};
    bool savedHealthy{g_referenceHealthy.load()};
    bool savedProductionHealthy{g_productionHealthy.load()};
    bool savedGameplay{g_gameplayReadyApplied.load()};
    ContextRecord* record{};
    PresentationWindow window;
    ComPtr<IDXGISwapChain1> chain;
    Texture output, proxy;

    explicit PresentationContextFixture(Device& value);
    ~PresentationContextFixture();
    void PrepareResources();
    void InstallHooks();
};

struct SharedPresentationContextFixture : PresentationContextFixture
{
    HdrRendererLifecycle savedLifecycle{g_lifecycle};
    GraphicsLifecyclePublication savedGame{ReadGraphicsLifecycle()};
    ReplacementShaderSet savedReplacements{g_replacements};
    HMONITOR savedMonitor{ActiveDisplayMonitor()};
    int savedHdrState{ActiveDisplayHdrState()};
    std::uint64_t savedFrame{g_frame};
    bool savedCorrection{ArmorUiCorrectionEnabled()};
    ContextAtomicRestore<std::uint64_t, 3> generations{std::array{
        &g_evidence.resourceGeneration, &g_evidence.colorDomainGeneration, &g_evidence.gameResetEpoch}};
    ContextAtomicRestore<DWORD, 3> threads{std::array{&g_observedPresentThread,
        &g_currentFrameQualifiedDrawThread, &g_observedQualifiedDrawThread}};
    ContextAtomicRestore<bool, 8> states{std::array{&g_liveSettingsThreadCompatible,
        &g_currentFrameQualifiedDrawThreadConflict, &g_evidence.hdrColorSpaceActive,
        &g_evidence.hdrChainActive, &g_evidence.productionActive,
        &g_evidence.presentationPaused, &g_evidence.heartbeatReady, &g_evidence.gameplayReady}};
    ContextAtomicRestore<std::uint32_t, 2> observations{std::array{
        &g_evidence.colorDomain, &g_evidence.failure}};
    ComPtr<ID3D11PixelShader> hudShader;
    ComPtr<ID3D11Buffer> hudConstants;

    struct HudConstants
    {
        UINT width{kWidth}, height{kHeight}, operation{};
        float uiWhite{203}, sceneWhite{203};
        std::array<float, 3> reserved{};
        Float4 color{0.5F, 0.5F, 0.5F, 1};
        Float4 rectangle{0, 0, kWidth, kHeight};
    };

    explicit SharedPresentationContextFixture(Device& value);
    ~SharedPresentationContextFixture();
    void CreateHudDraw();
    void BindHud();
    void PrepareScene();
    void DrawHud(bool write = true, ArmorUiOwner owner = ArmorUiOwner::hud,
        std::uint32_t targetArgument = 0);
    void CheckEncodedSeed(const Texture& source);
    void PresentEncodedUi(double linear, float white = 203);
    void CheckFrozenWhitePresentation();
    std::array<std::array<std::uint16_t, 4>, kWidth> ReadPublicPixels();
    std::uint32_t ReadPresentationPixel(ID3D11Texture2D* texture);
    void PresentReadable(double linear, float white = 203);
    void PresentBlack(bool hdr = true);
    void CheckOutputCode(std::uint16_t expected, UINT x = 0, UINT y = 0);
};

struct ObservedConstantProbe
{
    ID3D11DeviceContext* context;
    ID3D11Buffer* expected;
    UINT calls{};

    static void Invoke(void* arguments);
};

void PrepareObservedProductionBindings(Device& device);
Device CreateNativeContextDevice(Device& source);

} // namespace fo4hdr::tests::gpu
