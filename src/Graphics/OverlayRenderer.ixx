// OverlayRenderer: Offscreen rendering and shared texture submission

module;

#include <Windows.h>

export module DwmOverlay.Graphics.OverlayRenderer;

export namespace DwmOverlay
{
bool InitializeOverlayRenderer(const LUID& adapterID);
LUID GetOverlayAdapter();
bool BeginOverlayFrame(UINT width, UINT height, float deltaTime);
bool RenderOverlayFrame(DWORD orientation);
} // namespace DwmOverlay
