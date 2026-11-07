// WindowInput: Win32 input window and ImGui event queue

module;

#include <Windows.h>

export module DwmOverlay.Input.WindowInput;

import std;

export namespace DwmOverlay
{
bool             InitializeWindowInput();
void             AttachWindowInput();
HDESK            GetInputDesktop();
std::bitset<256> GetWindowInputPressedKeys();
bool             ProcessWindowInput(RECT& bounds, DWORD& orientation);
bool             SetInputWindowVisible(bool visible);
} // namespace DwmOverlay
