// FrameRuntime: Input processing, offscreen rendering, and display synchronization

module;

#include <Windows.h>
#include <dwmapi.h>
#include <imgui.h>

module DwmOverlay.Core.FrameRuntime;

import std;

import DwmOverlay.Core.Runtime;
import DwmOverlay.Graphics.FrameExchange;
import DwmOverlay.Graphics.OverlayRenderer;
import DwmOverlay.Input.WindowInput;
import DwmOverlay.UI.Overlay;

namespace DwmOverlay
{
#pragma region PublicFunctions

bool RunFrameLoop()
{
    if (!SetThreadDesktop(GetInputDesktop()))
        return false;
    if (!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return false;

    LUID adapter{};
    while (!GetDisplayAdapter(adapter))
    {
        if (GetRuntimeStatus() == RuntimeStatus::Failed)
            return false;
        Sleep(100);
    }
    if (!InitializeOverlayRenderer(adapter))
        return false;

    bool isOverlayVisible = true;
    auto previousTime     = std::chrono::steady_clock::now();
    for (;;)
    {
        if (GetRuntimeStatus() == RuntimeStatus::Failed)
            return false;

        const auto currentAdapter = GetOverlayAdapter();
        if (GetDisplayAdapter(adapter) &&
            (adapter.HighPart != currentAdapter.HighPart || adapter.LowPart != currentAdapter.LowPart))
            if (!InitializeOverlayRenderer(adapter))
                return false;

        RECT  bounds{};
        DWORD orientation = DMDO_DEFAULT;
        if (!ProcessWindowInput(bounds, orientation))
            return false;
        const auto pressed = GetWindowInputPressedKeys();
        if (pressed[VK_HOME])
            isOverlayVisible = !isOverlayVisible;
        if (pressed[VK_END])
            return TerminateProcess(GetCurrentProcess(), 0) != FALSE;

        const LONG width  = bounds.right - bounds.left;
        const LONG height = bounds.bottom - bounds.top;
        if (width <= 0 || height <= 0)
            return false;
        const auto  now       = std::chrono::steady_clock::now();
        const float deltaTime = std::max(std::chrono::duration<float>(now - previousTime).count(), 0.000001f);
        previousTime          = now;
        if (!BeginOverlayFrame(static_cast<UINT>(width), static_cast<UINT>(height), deltaTime))
            return false;
        if (isOverlayVisible)
            DrawOverlay();
        ImGui::Render();
        if (!SetInputWindowVisible(isOverlayVisible) || !RenderOverlayFrame(orientation))
            return false;
        if (FAILED(DwmFlush()))
            return false;
    }
}

#pragma endregion
} // namespace DwmOverlay
