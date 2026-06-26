// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestHarness.hpp"
#include "DisplayDiscovery.hpp"
#include "UnitTestSuite.hpp"
#include "Fallout4HDR/ui/HdrSettingsInput.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

using Microsoft::WRL::ComPtr;
constexpr wchar_t windowClassName[] = L"Fallout4HDRPresentationCharacterization";
constexpr UINT imageSize = 64;
fo4hdr::HdrSettingsInput* windowInput{};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
    if (windowInput != nullptr)
    {
        windowInput->Observe(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct OwnedWindow
{
    HINSTANCE module{};
    ATOM registered{};
    HWND window{};
    fo4hdr::HdrSettingsInput input;
    bool inputAttached{};

    ~OwnedWindow()
    {
        if (inputAttached)
        {
            windowInput = nullptr;
            FO4HDR_CHECK(input.Stop());
        }
        if (window != nullptr)
        {
            const auto destroyed = DestroyWindow(window);
            FO4HDR_CHECK(destroyed != FALSE);
            if (destroyed == FALSE)
            {
                return;
            }
        }
        if (registered != 0)
        {
            FO4HDR_CHECK(UnregisterClassW(windowClassName, module) != FALSE);
        }
    }

    bool Create()
    {
        module = GetModuleHandleW(nullptr);
        FO4HDR_CHECK(module != nullptr);
        if (module == nullptr)
        {
            return false;
        }
        WNDCLASSW description{};
        description.lpfnWndProc = WindowProcedure;
        description.hInstance = module;
        description.lpszClassName = windowClassName;
        registered = RegisterClassW(&description);
        FO4HDR_CHECK(registered != 0);
        if (registered == 0)
        {
            return false;
        }
        window = CreateWindowExW(0, windowClassName, L"Presentation characterization",
            WS_OVERLAPPEDWINDOW, 0, 0, imageSize, imageSize, nullptr, nullptr, module, nullptr);
        FO4HDR_CHECK(window != nullptr);
        if (window == nullptr) return false;
        inputAttached = input.Initialize(window);
        FO4HDR_CHECK(inputAttached);
        if (inputAttached) windowInput = &input;
        return inputAttached;
    }
};

void CheckOwnedInput(OwnedWindow& window)
{
    static_cast<void>(SendMessageW(window.window, WM_KEYDOWN, VK_F10, 1));
    static_cast<void>(SendMessageW(window.window, WM_KEYDOWN, VK_F10,
        LPARAM{1} | (LPARAM{1} << 30)));
    static_cast<void>(SendMessageW(window.window, WM_KILLFOCUS, 0, 0));
    const auto lost = window.input.Drain();
    FO4HDR_CHECK(lost.focusLost && lost.toggles == 0);
    static_cast<void>(SendMessageW(window.window, WM_KEYDOWN, VK_F10,
        LPARAM{1} | (LPARAM{1} << 30)));
    static_cast<void>(SendMessageW(window.window, WM_KEYUP, VK_F10, 1));
    static_cast<void>(SendMessageW(window.window, WM_KEYDOWN, VK_F10, 1));
    static_cast<void>(SendMessageW(window.window, WM_KEYUP, VK_F10, 1));
    const auto toggled = window.input.Drain();
    FO4HDR_CHECK(!toggled.focusLost && toggled.toggles == 1);
    FO4HDR_CHECK(window.input.Drain().toggles == 0);
}

struct NativePresentation
{
    ComPtr<ID3D11Device> device;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11DeviceContext> context;
};

void ReportResult(std::string_view operation, HRESULT result)
{
    std::cout << operation << " HRESULT=" << static_cast<std::uint32_t>(result) << '\n';
}

bool CheckResult(std::string_view operation, HRESULT result)
{
    ReportResult(operation, result);
    FO4HDR_CHECK(SUCCEEDED(result));
    return SUCCEEDED(result);
}

bool CreatePresentation(NativePresentation& native, HWND window)
{
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = imageSize;
    description.BufferDesc.Height = imageSize;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    constexpr auto requested = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL selected{};
    const auto result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP,
        nullptr, 0, &requested, 1, D3D11_SDK_VERSION, &description,
        native.chain.GetAddressOf(), native.device.GetAddressOf(), &selected,
        native.context.GetAddressOf());
    if (!CheckResult("D3D11CreateDeviceAndSwapChain", result))
    {
        return false;
    }
    FO4HDR_CHECK(native.device.Get() != nullptr);
    FO4HDR_CHECK(native.context.Get() != nullptr);
    FO4HDR_CHECK(native.chain.Get() != nullptr);
    FO4HDR_CHECK(selected == requested);
    return native.device.Get() != nullptr && native.context.Get() != nullptr &&
           native.chain.Get() != nullptr && selected == requested;
}

bool CheckDescription(IDXGISwapChain& chain, HWND window)
{
    DXGI_SWAP_CHAIN_DESC actual{};
    if (!CheckResult("GetDesc", chain.GetDesc(&actual)))
    {
        return false;
    }
    const auto before = fo4hdr::test::failures;
    FO4HDR_CHECK(actual.BufferDesc.Width == imageSize);
    FO4HDR_CHECK(actual.BufferDesc.Height == imageSize);
    FO4HDR_CHECK(actual.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    FO4HDR_CHECK(actual.SampleDesc.Count == 1);
    FO4HDR_CHECK(actual.SampleDesc.Quality == 0);
    FO4HDR_CHECK(actual.BufferUsage == DXGI_USAGE_RENDER_TARGET_OUTPUT);
    FO4HDR_CHECK(actual.BufferCount == 1);
    FO4HDR_CHECK(actual.OutputWindow == window);
    FO4HDR_CHECK(actual.Windowed == TRUE);
    FO4HDR_CHECK(actual.SwapEffect == DXGI_SWAP_EFFECT_DISCARD);
    FO4HDR_CHECK(actual.Flags == 0);
    std::cout << "descriptor width=" << actual.BufferDesc.Width
              << " height=" << actual.BufferDesc.Height
              << " format=" << actual.BufferDesc.Format
              << " samples=" << actual.SampleDesc.Count
              << " quality=" << actual.SampleDesc.Quality
              << " usage=" << actual.BufferUsage
              << " buffers=" << actual.BufferCount
              << " windowed=" << actual.Windowed
              << " effect=" << actual.SwapEffect << " flags=" << actual.Flags << '\n';
    return fo4hdr::test::failures == before;
}

void ReportOutput(IDXGISwapChain& chain)
{
    std::cout << "output capability unavailable unless GetDesc1 succeeds\n";
    ComPtr<IDXGIOutput> output;
    const auto containing = chain.GetContainingOutput(output.GetAddressOf());
    ReportResult("GetContainingOutput", containing);
    if (FAILED(containing) || output.Get() == nullptr)
    {
        return;
    }
    ComPtr<IDXGIOutput6> output6;
    const auto queried = output->QueryInterface(__uuidof(IDXGIOutput6),
        reinterpret_cast<void**>(output6.GetAddressOf()));
    ReportResult("QueryInterface(IDXGIOutput6)", queried);
    if (FAILED(queried) || output6.Get() == nullptr)
    {
        return;
    }
    DXGI_OUTPUT_DESC1 description{};
    const auto described = output6->GetDesc1(&description);
    ReportResult("GetDesc1", described);
    if (FAILED(described))
    {
        return;
    }
    std::cout << "SDK output colorSpace=" << description.ColorSpace
              << " minLuminance=" << description.MinLuminance
              << " maxLuminance=" << description.MaxLuminance
              << " maxFullFrameLuminance=" << description.MaxFullFrameLuminance << '\n';
}


void ReportWindowDisplay(HWND window)
{
    const auto unavailable = fo4hdr::QueryDisplayCapabilities(nullptr);
    FO4HDR_CHECK(!unavailable.known);
    FO4HDR_CHECK(unavailable.monitorDevicePath.front() == L'\0');
    const auto display = fo4hdr::QueryDisplayCapabilities(window);
    std::cout << "window output known=" << display.known
              << " monitor identity available=" << (display.monitorDevicePath.front() != L'\0')
              << " SDK SDR white nits=" << display.sdrWhiteNits << '\n';
}

bool ClearFrame(NativePresentation& native)
{
    // Reverse destruction releases the view before the direct backbuffer reference.
    ComPtr<ID3D11Texture2D> buffer;
    ComPtr<ID3D11RenderTargetView> view;
    const auto acquired = native.chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(buffer.GetAddressOf()));
    if (!CheckResult("GetBuffer(0)", acquired))
    {
        return false;
    }
    FO4HDR_CHECK(buffer.Get() != nullptr);
    if (buffer.Get() == nullptr)
    {
        return false;
    }
    const auto created = native.device->CreateRenderTargetView(buffer.Get(), nullptr,
        view.GetAddressOf());
    if (!CheckResult("CreateRenderTargetView", created))
    {
        return false;
    }
    FO4HDR_CHECK(view.Get() != nullptr);
    if (view.Get() == nullptr)
    {
        return false;
    }
    constexpr float black[] = {0.0f, 0.0f, 0.0f, 1.0f};
    native.context->ClearRenderTargetView(view.Get(), black);
    std::cout << "one black SDR clear issued; completion unobserved\n";
    return true;
}

}  // namespace

void fo4hdr::tests::RunPresentationCharacterizationTests()
{
    std::cout << "WARP hidden-window presentation characterization\n";
    OwnedWindow window;
    if (!window.Create())
    {
        return;
    }
    CheckOwnedInput(window);
    // Native owners retire before their HWND; this path stays on the calling thread.
    NativePresentation native;
    if (!CreatePresentation(native, window.window))
    {
        return;
    }
    if (!CheckDescription(*native.chain.Get(), window.window))
    {
        return;
    }
    ReportOutput(*native.chain.Get());
    ReportWindowDisplay(window.window);
    if (!ClearFrame(native))
    {
        return;
    }
    const auto presented = native.chain->Present(0, 0);
    CheckResult("Present(0,0)", presented);
    if (presented == S_OK)
    {
        std::cout << "Present returned S_OK\n";
    }
    else if (presented == DXGI_STATUS_OCCLUDED)
    {
        std::cout << "Present returned OCCLUDED\n";
    }
    std::cout << "presentation attempted; displayed pixels, HDR and brightness unverified\n";
}
