// OverlayRenderer: D3D11 offscreen resources and ImGui frame submission

module;

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <backends/imgui_impl_dx11.h>
#include <imgui.h>

module DwmOverlay.Graphics.OverlayRenderer;

import std;

import DwmOverlay.Graphics.FrameExchange;
import DwmOverlay.Input.WindowInput;

namespace DwmOverlay
{
namespace
{
#pragma region Types

using Microsoft::WRL::ComPtr;

#pragma endregion

#pragma region RuntimeState

bool                                          isRendererBackendInitialized = false;
ComPtr<ID3D11Device>                          device;
ComPtr<ID3D11DeviceContext>                   context;
std::shared_ptr<DisplaySurface>               surface;
std::array<ComPtr<ID3D11RenderTargetView>, 2> surfaceTargets;
std::uint64_t                                 presentationSequence = 0;
LUID                                          graphicsAdapter{};

#pragma endregion

#pragma region HelperDeclarations

bool ResizeSurface(UINT width, UINT height);

#pragma endregion
} // namespace

#pragma region PublicFunctions

bool InitializeOverlayRenderer(const LUID& adapterID)
{
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    for (UINT i = 0;; ++i)
    {
        adapter.Reset();
        if (FAILED(factory->EnumAdapters1(i, &adapter)))
            return false;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)))
            return false;
        if (description.AdapterLuid.HighPart == adapterID.HighPart &&
            description.AdapterLuid.LowPart == adapterID.LowPart)
            break;
    }
    ComPtr<ID3D11Device>        nextDevice;
    ComPtr<ID3D11DeviceContext> nextContext;
    if (FAILED(D3D11CreateDevice(adapter.Get(),
                                 D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr,
                                 0,
                                 D3D11_SDK_VERSION,
                                 &nextDevice,
                                 nullptr,
                                 &nextContext)))
        return false;
    DwmOverlay::PublishDisplayFrame({});
    if (isRendererBackendInitialized)
        ImGui_ImplDX11_Shutdown();
    isRendererBackendInitialized = false;
    if (context)
        context->ClearState();
    surfaceTargets = {};
    surface.reset();
    context.Reset();
    device.Reset();
    device          = std::move(nextDevice);
    context         = std::move(nextContext);
    graphicsAdapter = adapterID;
    if (!ImGui::GetCurrentContext())
    {
        ImGui::CreateContext();
        auto& io                   = ImGui::GetIO();
        io.IniFilename             = nullptr;
        io.LogFilename             = nullptr;
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        AttachWindowInput();
        ImGui::StyleColorsDark();
    }
    isRendererBackendInitialized = ImGui_ImplDX11_Init(device.Get(), context.Get());
    return isRendererBackendInitialized;
}

LUID GetOverlayAdapter()
{
    return graphicsAdapter;
}

bool BeginOverlayFrame(UINT width, UINT height, float deltaTime)
{
    if (!isRendererBackendInitialized || !width || !height || deltaTime <= 0.0f || !ResizeSurface(width, height))
        return false;

    ImGui::GetIO().DeltaTime = deltaTime;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    return true;
}

bool RenderOverlayFrame(DWORD orientation)
{
    if (!isRendererBackendInitialized || !surface)
        return false;
    std::array<std::size_t, 2> order{0, 1};
    if (surface->Buffers[0].Sequence.load(std::memory_order_acquire) >
        surface->Buffers[1].Sequence.load(std::memory_order_acquire))
        std::swap(order[0], order[1]);
    for (const auto index : order)
    {
        auto&         buffer   = surface->Buffers[index];
        const HRESULT acquired = buffer.Mutex->AcquireSync(0, 0);
        if (acquired == WAIT_TIMEOUT)
            continue;
        if (acquired != S_OK)
            return false;
        ID3D11RenderTargetView* target = surfaceTargets[index].Get();
        constexpr float         Transparent[]{0, 0, 0, 0};
        context->ClearRenderTargetView(target, Transparent);
        context->OMSetRenderTargets(1, &target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        context->Flush();
        buffer.Sequence.store(++presentationSequence, std::memory_order_release);
        if (FAILED(buffer.Mutex->ReleaseSync(0)))
            return false;
        break;
    }
    DwmOverlay::PublishDisplayFrame({surface, orientation});
    return SUCCEEDED(device->GetDeviceRemovedReason());
}

#pragma endregion

namespace
{
#pragma region Helpers

bool ResizeSurface(UINT width, UINT height)
{
    if (surface && surface->Width == width && surface->Height == height)
        return true;
    auto                 next = std::make_shared<DisplaySurface>();
    D3D11_TEXTURE2D_DESC description{};
    description.Width            = width;
    description.Height           = height;
    description.MipLevels        = 1;
    description.ArraySize        = 1;
    description.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage            = D3D11_USAGE_DEFAULT;
    description.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> targets;
    for (std::size_t i = 0; i < next->Buffers.size(); ++i)
    {
        auto&                 buffer = next->Buffers[i];
        ComPtr<IDXGIResource> resource;
        if (FAILED(device->CreateTexture2D(&description, nullptr, &buffer.Texture)) ||
            FAILED(buffer.Texture.As(&buffer.Mutex)) || FAILED(buffer.Texture.As(&resource)) ||
            FAILED(resource->GetSharedHandle(&buffer.SharedHandle)) ||
            FAILED(device->CreateRenderTargetView(buffer.Texture.Get(), nullptr, &targets[i])))
            return false;
    }
    next->Width   = width;
    next->Height  = height;
    next->Adapter = graphicsAdapter;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    surfaceTargets = std::move(targets);
    surface        = std::move(next);
    return true;
}

#pragma endregion
} // namespace
} // namespace DwmOverlay
