// Compositor: Compose overlay textures into DWM frames

module;

#include <Windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

module DwmOverlay.Graphics.Compositor;

import std;

import DwmOverlay.Core.Runtime;
import DwmOverlay.Graphics.FrameExchange;

namespace DwmOverlay
{
namespace
{
#pragma region Types

using Microsoft::WRL::ComPtr;

struct DeviceStateScope
{
    ID3D11DeviceContext1*          Context;
    ComPtr<ID3DDeviceContextState> PreviousState;

    DeviceStateScope(ID3D11DeviceContext1* deviceContext, ID3DDeviceContextState* state) : Context(deviceContext)
    {
        Context->SwapDeviceContextState(state, &PreviousState);
    }

    DeviceStateScope(const DeviceStateScope&)            = delete;
    DeviceStateScope& operator=(const DeviceStateScope&) = delete;

    ~DeviceStateScope()
    {
        // Release the frame's RTV from the private state so the swap chain can free its old buffers.
        Context->ClearState();
        Context->SwapDeviceContextState(PreviousState.Get(), nullptr);
    }
};

struct CompositeConstants
{
    DWORD                Orientation;
    std::array<DWORD, 3> Padding{};
};

struct RendererState
{
    ComPtr<ID3D11Device>                            SourceDevice;
    ComPtr<ID3D11Device1>                           Device;
    ComPtr<ID3D11DeviceContext1>                    Context;
    ComPtr<ID3DDeviceContextState>                  ContextState;
    ComPtr<ID3D11VertexShader>                      VertexShader;
    ComPtr<ID3D11PixelShader>                       PixelShader;
    ComPtr<ID3D11Buffer>                            ConstantBuffer;
    ComPtr<ID3D11BlendState>                        Blend;
    ComPtr<ID3D11SamplerState>                      Sampler;
    ComPtr<ID3D11RasterizerState>                   Rasterizer;
    std::array<ComPtr<IDXGIKeyedMutex>, 2>          SharedMutexes;
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> SharedViews;
    std::shared_ptr<DisplaySurface>                 Surface;
    std::optional<std::size_t>                      ActiveBuffer;
    LUID                                            Adapter{};
    std::uint64_t                                   Sequence = 0;
};

#pragma endregion

#pragma region RuntimeState

RendererState renderer;

#pragma endregion

#pragma region HelperDeclarations

bool InitializeRenderer(ID3D11Device* device);
bool ReleaseDisplayBuffer();
bool CloseDisplaySurface();
bool OpenDisplaySurface(const std::shared_ptr<DisplaySurface>& surface);

#pragma endregion
} // namespace

#pragma region PublicFunctions

void CompositeFrame(ID3D11Texture2D* texture)
{
    if (!texture || GetRuntimeStatus() == RuntimeStatus::Failed)
        return;
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    if (!device || FAILED(device->GetDeviceRemovedReason()))
    {
        SetRuntimeStatus(RuntimeStatus::Failed);
        return;
    }
    if (renderer.SourceDevice.Get() != device.Get())
    {
        ComPtr<IDXGIDevice>  dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC    description{};
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&description)))
        {
            SetRuntimeStatus(RuntimeStatus::Failed);
            return;
        }
        if (!CloseDisplaySurface())
        {
            SetRuntimeStatus(RuntimeStatus::Failed);
            return;
        }
        renderer = {};
        if (!InitializeRenderer(device.Get()))
        {
            SetRuntimeStatus(RuntimeStatus::Failed);
            return;
        }
        renderer.Adapter = description.AdapterLuid;
        SetDisplayAdapter(renderer.Adapter);
    }
    DisplayFrame frame;
    if (!ReadDisplayFrame(frame))
        return;
    if (!frame.Surface || frame.Surface->Adapter.HighPart != renderer.Adapter.HighPart ||
        frame.Surface->Adapter.LowPart != renderer.Adapter.LowPart)
    {
        if (!CloseDisplaySurface())
            SetRuntimeStatus(RuntimeStatus::Failed);
        return;
    }
    DeviceStateScope deviceScope(renderer.Context.Get(), renderer.ContextState.Get());
    if (renderer.Surface != frame.Surface && !OpenDisplaySurface(frame.Surface))
    {
        SetRuntimeStatus(RuntimeStatus::Failed);
        return;
    }
    std::array<std::size_t, 2> order{0, 1};
    if (frame.Surface->Buffers[0].Sequence.load(std::memory_order_acquire) <
        frame.Surface->Buffers[1].Sequence.load(std::memory_order_acquire))
        std::swap(order[0], order[1]);
    for (const auto index : order)
    {
        const auto& buffer = frame.Surface->Buffers[index];
        if (renderer.ActiveBuffer == index || buffer.Sequence.load(std::memory_order_acquire) <= renderer.Sequence)
            continue;
        const HRESULT acquired = renderer.SharedMutexes[index]->AcquireSync(0, 0);
        if (acquired == WAIT_TIMEOUT)
            continue;
        if (acquired != S_OK)
        {
            SetRuntimeStatus(RuntimeStatus::Failed);
            return;
        }
        if (!ReleaseDisplayBuffer())
        {
            renderer.SharedMutexes[index]->ReleaseSync(0);
            SetRuntimeStatus(RuntimeStatus::Failed);
            return;
        }
        // Hold the displayed buffer until the next frame is acquired so the producer cannot overwrite it.
        renderer.ActiveBuffer = index;
        renderer.Sequence     = buffer.Sequence.load(std::memory_order_acquire);
        break;
    }
    if (!renderer.ActiveBuffer)
        return;
    const CompositeConstants constants{frame.Orientation};
    renderer.Context->UpdateSubresource(renderer.ConstantBuffer.Get(), 0, nullptr, &constants, 0, 0);
    ComPtr<ID3D11RenderTargetView> renderTarget;
    if (FAILED(device->CreateRenderTargetView(texture, nullptr, &renderTarget)))
    {
        SetRuntimeStatus(RuntimeStatus::Failed);
        return;
    }
    const bool                swapsAxes = frame.Orientation == DMDO_90 || frame.Orientation == DMDO_270;
    const D3D11_VIEWPORT      viewport{0.0f,
                                       0.0f,
                                       static_cast<float>(swapsAxes ? frame.Surface->Height : frame.Surface->Width),
                                       static_cast<float>(swapsAxes ? frame.Surface->Width : frame.Surface->Height),
                                       0.0f,
                                       1.0f};
    ID3D11RenderTargetView*   target         = renderTarget.Get();
    ID3D11ShaderResourceView* view           = renderer.SharedViews[*renderer.ActiveBuffer].Get();
    ID3D11Buffer*             constantBuffer = renderer.ConstantBuffer.Get();
    ID3D11SamplerState*       sampler        = renderer.Sampler.Get();
    renderer.Context->OMSetRenderTargets(1, &target, nullptr);
    renderer.Context->OMSetBlendState(renderer.Blend.Get(), nullptr, 0xffffffff);
    renderer.Context->RSSetState(renderer.Rasterizer.Get());
    renderer.Context->RSSetViewports(1, &viewport);
    renderer.Context->IASetInputLayout(nullptr);
    renderer.Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderer.Context->VSSetShader(renderer.VertexShader.Get(), nullptr, 0);
    renderer.Context->PSSetShader(renderer.PixelShader.Get(), nullptr, 0);
    renderer.Context->PSSetConstantBuffers(0, 1, &constantBuffer);
    renderer.Context->PSSetShaderResources(0, 1, &view);
    renderer.Context->PSSetSamplers(0, 1, &sampler);
    renderer.Context->Draw(3, 0);
    SetRuntimeStatus(RuntimeStatus::Rendering);
}

#pragma endregion

namespace
{
#pragma region Helpers

bool InitializeRenderer(ID3D11Device* device)
{
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&renderer.Device))))
        return false;
    renderer.SourceDevice = device;
    renderer.Device->GetImmediateContext1(&renderer.Context);
    // Context state creation accepts feature levels up to 11_1 and must match the device's D3D11_CREATE_DEVICE_SINGLETHREADED flag.
    const D3D_FEATURE_LEVEL featureLevel = std::min(renderer.Device->GetFeatureLevel(), D3D_FEATURE_LEVEL_11_1);
    const UINT              contextFlags = (renderer.Device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                                               ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                                               : 0;
    if (!renderer.Context || FAILED(renderer.Device->CreateDeviceContextState(contextFlags,
                                                                              &featureLevel,
                                                                              1,
                                                                              D3D11_SDK_VERSION,
                                                                              __uuidof(ID3D11Device),
                                                                              nullptr,
                                                                              &renderer.ContextState)))
        return false;
    D3D11_BUFFER_DESC bufferDescription{};
    bufferDescription.ByteWidth = sizeof(CompositeConstants);
    bufferDescription.Usage     = D3D11_USAGE_DEFAULT;
    bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bufferDescription, nullptr, &renderer.ConstantBuffer)))
        return false;

    constexpr char   Shader[] = R"HLSL(
cbuffer CompositeConstants : register(b0)
{
    uint Orientation;
    uint3 Padding;
};
struct Vertex
{
    float4 Position : SV_POSITION;
    float2 Uv : TEXCOORD0;
};
Vertex VertexMain(uint Index : SV_VertexID)
{
    Vertex Result;
    Result.Uv = float2((Index << 1) & 2, Index & 2);
    Result.Position = float4(Result.Uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return Result;
}
Texture2D Image : register(t0);
SamplerState ImageSampler : register(s0);
float4 PixelMain(Vertex Input) : SV_TARGET
{
    // DMDO_* rotations are counterclockwise; compensate for DWM output rotation.
    float2 Uv = Input.Uv;
    if (Orientation == 1)
        Uv = float2(Input.Uv.y, 1.0 - Input.Uv.x);
    else if (Orientation == 2)
        Uv = 1.0 - Input.Uv;
    else if (Orientation == 3)
        Uv = float2(1.0 - Input.Uv.y, Input.Uv.x);
    return Image.Sample(ImageSampler, Uv);
}
)HLSL";
    ComPtr<ID3DBlob> vertexCode;
    ComPtr<ID3DBlob> pixelCode;
    if (FAILED(D3DCompile(Shader,
                          sizeof(Shader) - 1,
                          nullptr,
                          nullptr,
                          nullptr,
                          "VertexMain",
                          "vs_4_0",
                          0,
                          0,
                          &vertexCode,
                          nullptr)) ||
        FAILED(D3DCompile(
            Shader, sizeof(Shader) - 1, nullptr, nullptr, nullptr, "PixelMain", "ps_4_0", 0, 0, &pixelCode, nullptr)))
        return false;
    if (FAILED(renderer.Device->CreateVertexShader(
            vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &renderer.VertexShader)) ||
        FAILED(renderer.Device->CreatePixelShader(
            pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &renderer.PixelShader)))
        return false;

    D3D11_BLEND_DESC blend{};
    auto&            target      = blend.RenderTarget[0];
    target.BlendEnable           = TRUE;
    target.SrcBlend              = D3D11_BLEND_ONE;
    target.DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp               = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha         = D3D11_BLEND_ONE;
    target.DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD                                         = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode        = D3D11_FILL_SOLID;
    rasterizer.CullMode        = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    return SUCCEEDED(renderer.Device->CreateBlendState(&blend, &renderer.Blend)) &&
           SUCCEEDED(renderer.Device->CreateSamplerState(&sampler, &renderer.Sampler)) &&
           SUCCEEDED(renderer.Device->CreateRasterizerState(&rasterizer, &renderer.Rasterizer));
}

bool ReleaseDisplayBuffer()
{
    if (!renderer.ActiveBuffer)
        return true;
    const auto index = *std::exchange(renderer.ActiveBuffer, std::nullopt);
    renderer.Context->Flush();
    return SUCCEEDED(renderer.SharedMutexes[index]->ReleaseSync(0));
}

bool CloseDisplaySurface()
{
    const bool released = ReleaseDisplayBuffer();
    renderer.Surface.reset();
    renderer.SharedMutexes = {};
    renderer.SharedViews   = {};
    renderer.Sequence      = 0;
    return released;
}

bool OpenDisplaySurface(const std::shared_ptr<DisplaySurface>& surface)
{
    if (!CloseDisplaySurface())
        return false;
    for (std::size_t i = 0; i < surface->Buffers.size(); ++i)
    {
        ComPtr<ID3D11Texture2D> sharedTexture;
        if (FAILED(
                renderer.Device->OpenSharedResource(surface->Buffers[i].SharedHandle, IID_PPV_ARGS(&sharedTexture))) ||
            FAILED(sharedTexture.As(&renderer.SharedMutexes[i])) ||
            FAILED(renderer.Device->CreateShaderResourceView(sharedTexture.Get(), nullptr, &renderer.SharedViews[i])))
            return false;
    }
    renderer.Surface = surface;
    return true;
}

#pragma endregion
} // namespace
} // namespace DwmOverlay
