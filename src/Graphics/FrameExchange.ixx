// FrameExchange: GPU frame exchange between rendering and composition

module;

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

export module DwmOverlay.Graphics.FrameExchange;

import std;

export namespace DwmOverlay
{
struct DisplayBuffer
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> Mutex;
    HANDLE                                  SharedHandle = nullptr;
    std::atomic<std::uint64_t>              Sequence     = 0;
};

struct DisplaySurface
{
    std::array<DisplayBuffer, 2> Buffers;
    LUID                         Adapter{};
    UINT                         Width  = 0;
    UINT                         Height = 0;
};

struct DisplayFrame
{
    std::shared_ptr<DisplaySurface> Surface;
    DWORD                           Orientation = DMDO_DEFAULT;
};

void SetDisplayAdapter(LUID adapter);
bool GetDisplayAdapter(LUID& adapter);
void PublishDisplayFrame(DisplayFrame frame);
bool ReadDisplayFrame(DisplayFrame& frame);
} // namespace DwmOverlay
