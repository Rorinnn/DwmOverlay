// Compositor: D3D11 state isolation and overlay composition

module;

#include <d3d11.h>

export module DwmOverlay.Graphics.Compositor;

export namespace DwmOverlay
{
void CompositeFrame(ID3D11Texture2D* texture);
} // namespace DwmOverlay
