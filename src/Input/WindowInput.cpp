// WindowInput: Transparent input window and ImGui event forwarding

module;

#include <Windows.h>
#include <process.h>
#include <imgui.h>

module DwmOverlay.Input.WindowInput;

import std;

import DwmOverlay.Core.Runtime;

#pragma region ExternalInterfaces

// Version dependency: the ImGui 1.92.9 Win32 backend defines this symbol without declaring it in its header.
extern "C++" ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM, LPARAM);

#pragma endregion

namespace DwmOverlay
{
namespace
{
#pragma region Constants

constexpr UINT UpdateWindowMessage = WM_APP + 1;

#pragma endregion

#pragma region Types

enum class InputEventType
{
    MousePosition,
    MouseButton,
    MouseWheel,
    Key,
    CharacterUTF16,
    Focus
};

struct InputEvent
{
    InputEventType Type;
    ImVec2         Position{};
    ImGuiKey       Key       = ImGuiKey_None;
    int            NativeKey = 0;
    int            ScanCode  = 0;
    unsigned int   Character = 0;
    bool           IsDown    = false;
};

struct InputThreadState
{
    HWND                      Window = nullptr;
    std::array<ImGuiKey, 256> KeysDown{};
    RECT                      DesktopRectangle{};
    std::array<BYTE, 256>     KeyboardState{};
};

#pragma endregion

#pragma region RuntimeState

HDESK                   inputDesktop                   = nullptr;
std::atomic<HWND>       inputWindow                    = nullptr;
std::atomic<bool>       isInputThreadRunning           = false;
std::mutex              inputMutex;
RECT                    monitorBounds{};
DWORD                   displayOrientation = DMDO_DEFAULT;
std::vector<InputEvent> pendingEvents;
std::vector<InputEvent> frameEvents;
std::bitset<256>        inputKeyStates;
std::bitset<256>        pendingPressedKeys;
std::bitset<256>        framePressedKeys;
std::atomic<bool>       isInputWindowVisible = false;

#pragma endregion

#pragma region HelperDeclarations

LRESULT CALLBACK       InputWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
unsigned int __stdcall RunInputThread(void* startupEvent) noexcept;
bool                   QueueRawInput(InputThreadState& state, HRAWINPUT input);
void                   QueueMousePosition(POINT position);
void                   QueueModifiers(InputThreadState& state);
void                   ReleaseInput(InputThreadState& state);

#pragma endregion

} // namespace

#pragma region PublicFunctions

bool InitializeWindowInput()
{
    const HANDLE startupEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!startupEvent)
        return false;
    isInputWindowVisible.store(false);
    const HANDLE inputThread =
        reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, RunInputThread, startupEvent, 0, nullptr));
    if (!inputThread)
    {
        CloseHandle(startupEvent);
        return false;
    }
    const HANDLE handles[]{startupEvent, inputThread};
    const bool   hasStarted =
        WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 && isInputThreadRunning.load();
    CloseHandle(inputThread);
    CloseHandle(startupEvent);
    return hasStarted;
}

void AttachWindowInput()
{
    auto& io                = ImGui::GetIO();
    io.BackendPlatformName  = "DwmOverlay.WindowInput";
    io.BackendFlags        &= ~(ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_HasSetMousePos);
    io.ConfigFlags         |= ImGuiConfigFlags_NavEnableKeyboard;
    io.AddFocusEvent(true);
    ImGui::GetMainViewport()->PlatformHandle     = inputWindow.load();
    ImGui::GetMainViewport()->PlatformHandleRaw  = inputWindow.load();
    ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
}

HDESK GetInputDesktop()
{
    return inputDesktop;
}

std::bitset<256> GetWindowInputPressedKeys()
{
    std::lock_guard lock(inputMutex);
    return framePressedKeys;
}

bool ProcessWindowInput(RECT& bounds, DWORD& orientation)
{
    if (!isInputThreadRunning.load())
        return false;
    auto& io = ImGui::GetIO();
    {
        std::lock_guard lock(inputMutex);
        frameEvents.clear();
        frameEvents.swap(pendingEvents);
        framePressedKeys = pendingPressedKeys;
        pendingPressedKeys.reset();
        bounds      = monitorBounds;
        orientation = displayOrientation;
        io.DisplaySize =
            ImVec2(static_cast<float>(bounds.right - bounds.left), static_cast<float>(bounds.bottom - bounds.top));
    }
    for (const auto& event : frameEvents)
    {
        switch (event.Type)
        {
            case InputEventType::MousePosition:
                io.AddMousePosEvent(event.Position.x - bounds.left, event.Position.y - bounds.top);
                break;
            case InputEventType::MouseButton:
                io.AddMouseButtonEvent(event.NativeKey, event.IsDown);
                break;
            case InputEventType::MouseWheel:
                io.AddMouseWheelEvent(event.Position.x, event.Position.y);
                break;
            case InputEventType::Key:
                if (event.Key != ImGuiKey_None)
                {
                    io.AddKeyEvent(event.Key, event.IsDown);
                    if (event.NativeKey)
                        io.SetKeyEventNativeData(event.Key, event.NativeKey, event.ScanCode);
                }
                break;
            case InputEventType::CharacterUTF16:
                io.AddInputCharacterUTF16(static_cast<ImWchar16>(event.Character));
                break;
            case InputEventType::Focus:
                io.AddFocusEvent(event.IsDown);
                break;
        }
    }
    POINT position{};
    if (GetCursorPos(&position))
        io.AddMousePosEvent(static_cast<float>(position.x - bounds.left), static_cast<float>(position.y - bounds.top));
    return true;
}

bool SetInputWindowVisible(bool visible)
{
    if (!isInputThreadRunning.load())
        return false;
    if (isInputWindowVisible.exchange(visible) == visible)
        return true;
    return PostMessageW(inputWindow.load(), UpdateWindowMessage, visible, 0) != FALSE;
}

#pragma endregion

namespace
{
#pragma region Helpers

void FailInput()
{
    isInputThreadRunning.store(false);
    SetRuntimeStatus(RuntimeStatus::Failed);
}

void QueueEvent(const InputEvent& event)
{
    std::lock_guard lock(inputMutex);
    int             code = 0;
    if (event.Type == InputEventType::MouseButton)
    {
        constexpr int Codes[]{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        code = Codes[event.NativeKey];
    }
    else if (event.Type == InputEventType::Key)
    {
        if (event.NativeKey)
            code = event.NativeKey;
        else if (event.Key == ImGuiMod_Shift)
            code = VK_SHIFT;
        else if (event.Key == ImGuiMod_Ctrl)
            code = VK_CONTROL;
        else if (event.Key == ImGuiMod_Alt)
            code = VK_MENU;
    }
    if (code > 0 && code < inputKeyStates.size())
    {
        if (event.IsDown && !inputKeyStates[code])
            pendingPressedKeys[code] = true;
        inputKeyStates[code] = event.IsDown;
    }
    if (event.Type == InputEventType::MousePosition && !pendingEvents.empty() &&
        pendingEvents.back().Type == InputEventType::MousePosition)
        pendingEvents.back() = event;
    else
        pendingEvents.push_back(event);
}

BOOL CALLBACK AccumulateMonitorBounds(HMONITOR, HDC, LPRECT monitorBounds, LPARAM parameter) noexcept
{
    auto& bounds = *reinterpret_cast<RECT*>(parameter);
    UnionRect(&bounds, &bounds, monitorBounds);
    return TRUE;
}

bool RefreshMonitors(InputThreadState& state)
{
    state.DesktopRectangle = {};
    if (!EnumDisplayMonitors(
            nullptr, nullptr, AccumulateMonitorBounds, reinterpret_cast<LPARAM>(&state.DesktopRectangle)) ||
        IsRectEmpty(&state.DesktopRectangle))
        return false;
    MONITORINFOEXW monitor{sizeof(monitor)};
    DEVMODEW       mode{};
    mode.dmSize = sizeof(mode);
    if (!GetMonitorInfoW(MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY), reinterpret_cast<MONITORINFO*>(&monitor)) ||
        !EnumDisplaySettingsExW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) ||
        !(mode.dmFields & DM_DISPLAYORIENTATION) || mode.dmDisplayOrientation > DMDO_270)
        return false;
    if (state.Window)
    {
        const auto& rectangle = state.DesktopRectangle;
        if (!SetWindowPos(state.Window,
                          nullptr,
                          rectangle.left,
                          rectangle.top,
                          rectangle.right - rectangle.left,
                          rectangle.bottom - rectangle.top,
                          SWP_NOACTIVATE | SWP_NOZORDER))
            return false;
    }
    std::lock_guard lock(inputMutex);
    monitorBounds      = monitor.rcMonitor;
    displayOrientation = mode.dmDisplayOrientation;
    return true;
}

bool InitializeInputWindow(InputThreadState& state)
{
    inputDesktop =
        OpenDesktopW(L"Default", 0, FALSE, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
    if (!inputDesktop)
        return false;
    if (!SetThreadDesktop(inputDesktop))
        return false;
    if (!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return false;
    if (!RefreshMonitors(state))
        return false;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const auto className = std::format(L"DwmOverlay.Input.{:X}", reinterpret_cast<std::uintptr_t>(&inputWindow));
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc   = InputWindowProcedure;
    windowClass.hInstance     = instance;
    windowClass.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = className.c_str();
    if (!RegisterClassExW(&windowClass))
        return false;
    const auto& rectangle = state.DesktopRectangle;
    state.Window          = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                                                WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
                                            className.c_str(),
                                            L"DwmOverlay",
                                            WS_POPUP,
                                            rectangle.left,
                                            rectangle.top,
                                            rectangle.right - rectangle.left,
                                            rectangle.bottom - rectangle.top,
                                            nullptr,
                                            nullptr,
                                            instance,
                                            &state);
    if (!state.Window)
        return false;
    if (!SetLayeredWindowAttributes(state.Window, 0, 255, LWA_ALPHA))
        return false;
    // Receive HID Generic Desktop mouse and keyboard events in the background while preserving foreground input.
    const RAWINPUTDEVICE devices[]{{0x01, 0x02, RIDEV_INPUTSINK, state.Window},
                                   {0x01, 0x06, RIDEV_INPUTSINK, state.Window}};
    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)))
        return false;
    for (const int key : {VK_CAPITAL, VK_NUMLOCK, VK_SCROLL})
        state.KeyboardState[key] |= GetKeyState(key) & 1;
    return true;
}

unsigned int __stdcall RunInputThread(void* startupEvent) noexcept
{
    bool hasReportedStartup = false;
    try
    {
        InputThreadState state;
        if (InitializeInputWindow(state))
        {
            MSG message{};
            inputWindow.store(state.Window);
            isInputThreadRunning.store(true);
            SetEvent(startupEvent);
            hasReportedStartup = true;
            for (;;)
            {
                const BOOL result = GetMessageW(&message, nullptr, 0, 0);
                if (result <= 0)
                {
                    FailInput();
                    break;
                }
                DispatchMessageW(&message);
            }
        }
        else
            FailInput();
    }
    catch (...)
    {
        FailInput();
    }
    isInputThreadRunning.store(false);
    inputWindow.store(nullptr);
    if (!hasReportedStartup)
        SetEvent(startupEvent);
    return 0;
}

void QueueMousePosition(POINT position)
{
    QueueEvent({.Type     = InputEventType::MousePosition,
                .Position = ImVec2(static_cast<float>(position.x), static_cast<float>(position.y))});
}

void QueueModifiers(InputThreadState& state)
{
    state.KeyboardState[VK_SHIFT]   = (state.KeyboardState[VK_LSHIFT] | state.KeyboardState[VK_RSHIFT]) & 0x80;
    state.KeyboardState[VK_CONTROL] = (state.KeyboardState[VK_LCONTROL] | state.KeyboardState[VK_RCONTROL]) & 0x80;
    state.KeyboardState[VK_MENU]    = (state.KeyboardState[VK_LMENU] | state.KeyboardState[VK_RMENU]) & 0x80;
    QueueEvent(
        {.Type = InputEventType::Key, .Key = ImGuiMod_Ctrl, .IsDown = (state.KeyboardState[VK_CONTROL] & 0x80) != 0});
    QueueEvent(
        {.Type = InputEventType::Key, .Key = ImGuiMod_Shift, .IsDown = (state.KeyboardState[VK_SHIFT] & 0x80) != 0});
    QueueEvent(
        {.Type = InputEventType::Key, .Key = ImGuiMod_Alt, .IsDown = (state.KeyboardState[VK_MENU] & 0x80) != 0});
    QueueEvent({.Type   = InputEventType::Key,
                .Key    = ImGuiMod_Super,
                .IsDown = ((state.KeyboardState[VK_LWIN] | state.KeyboardState[VK_RWIN]) & 0x80) != 0});
}

void ReleaseInput(InputThreadState& state)
{
    for (int i = 0; i < 5; ++i)
        QueueEvent({.Type = InputEventType::MouseButton, .NativeKey = i, .IsDown = false});
    for (std::size_t i = 0; i < state.KeysDown.size(); ++i)
        if (state.KeyboardState[i] & 0x80)
            QueueEvent({.Type      = InputEventType::Key,
                        .Key       = state.KeysDown[i],
                        .NativeKey = static_cast<int>(i),
                        .IsDown    = false});
    state.KeysDown.fill(ImGuiKey_None);
    for (auto& key : state.KeyboardState)
        key &= 1;
    for (const auto key : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super})
        QueueEvent({.Type = InputEventType::Key, .Key = key, .IsDown = false});
    QueueEvent({.Type = InputEventType::Focus, .IsDown = true});
}

bool QueueRawInput(InputThreadState& state, HRAWINPUT handle)
{
    RAWINPUT   input{};
    UINT       size     = sizeof(input);
    const UINT readSize = GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
    if (readSize == static_cast<UINT>(-1))
        return false;
    if (readSize < sizeof(RAWINPUTHEADER))
        return false;
    if (input.header.dwType == RIM_TYPEMOUSE)
    {
        if (readSize < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE))
            return false;
        POINT position{};
        if (GetCursorPos(&position))
            QueueMousePosition(position);
        const auto&      mouse = input.data.mouse;
        constexpr USHORT DownFlags[]{RI_MOUSE_LEFT_BUTTON_DOWN,
                                     RI_MOUSE_RIGHT_BUTTON_DOWN,
                                     RI_MOUSE_MIDDLE_BUTTON_DOWN,
                                     RI_MOUSE_BUTTON_4_DOWN,
                                     RI_MOUSE_BUTTON_5_DOWN};
        constexpr USHORT UpFlags[]{RI_MOUSE_LEFT_BUTTON_UP,
                                   RI_MOUSE_RIGHT_BUTTON_UP,
                                   RI_MOUSE_MIDDLE_BUTTON_UP,
                                   RI_MOUSE_BUTTON_4_UP,
                                   RI_MOUSE_BUTTON_5_UP};
        for (int i = 0; i < 5; ++i)
        {
            if (mouse.usButtonFlags & DownFlags[i])
                QueueEvent({.Type = InputEventType::MouseButton, .NativeKey = i, .IsDown = true});
            if (mouse.usButtonFlags & UpFlags[i])
                QueueEvent({.Type = InputEventType::MouseButton, .NativeKey = i, .IsDown = false});
        }
        const float distance = static_cast<float>(static_cast<SHORT>(mouse.usButtonData)) / WHEEL_DELTA;
        if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
            QueueEvent({.Type = InputEventType::MouseWheel, .Position = ImVec2(0.0f, distance)});
        if (mouse.usButtonFlags & RI_MOUSE_HWHEEL)
            QueueEvent({.Type = InputEventType::MouseWheel, .Position = ImVec2(-distance, 0.0f)});
        return true;
    }
    if (input.header.dwType != RIM_TYPEKEYBOARD)
        return true;
    if (readSize < sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD))
        return false;
    const auto& keyboard = input.data.keyboard;
    if (keyboard.VKey == 255)
        return true;
    const bool isDown     = (keyboard.Flags & RI_KEY_BREAK) == 0;
    const bool isExtended = (keyboard.Flags & RI_KEY_E0) != 0;
    UINT       scanCode   = keyboard.MakeCode;
    WPARAM     nativeKey  = keyboard.VKey;
    if (nativeKey == VK_SHIFT)
        nativeKey = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
    else if (nativeKey == VK_CONTROL)
        nativeKey = isExtended ? VK_RCONTROL : VK_LCONTROL;
    else if (nativeKey == VK_MENU)
        nativeKey = isExtended ? VK_RMENU : VK_LMENU;
    if (nativeKey >= state.KeyboardState.size())
        return true;
    if (!scanCode)
        scanCode = MapVirtualKeyW(static_cast<UINT>(nativeKey), MAPVK_VK_TO_VSC_EX) & 0xFF;
    auto& keyState = state.KeyboardState[nativeKey];
    if (isDown && !(keyState & 0x80) && (nativeKey == VK_CAPITAL || nativeKey == VK_NUMLOCK || nativeKey == VK_SCROLL))
        keyState ^= 1;
    keyState = static_cast<BYTE>((keyState & 1) | (isDown ? 0x80 : 0));
    QueueModifiers(state);
    const LPARAM keyData =
        (static_cast<LPARAM>(scanCode) << 16) | (isExtended ? static_cast<LPARAM>(KF_EXTENDED) << 16 : 0);
    const ImGuiKey key        = ImGui_ImplWin32_KeyEventToImGuiKey(nativeKey, keyData);
    state.KeysDown[nativeKey] = isDown ? key : ImGuiKey_None;
    if (key == ImGuiKey_PrintScreen && !isDown)
        QueueEvent({.Type      = InputEventType::Key,
                    .Key       = key,
                    .NativeKey = static_cast<int>(nativeKey),
                    .ScanCode  = static_cast<int>(scanCode),
                    .IsDown    = true});
    QueueEvent({.Type      = InputEventType::Key,
                .Key       = key,
                .NativeKey = static_cast<int>(nativeKey),
                .ScanCode  = static_cast<int>(scanCode),
                .IsDown    = isDown});
    if (isDown)
    {
        const HWND foreground = GetForegroundWindow();
        const HKL  layout     = GetKeyboardLayout(foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0);
        WCHAR      characters[16]{};
        // On Windows 10 1607 and later, flag 4 preserves foreground dead key state during character conversion.
        const int count = ToUnicodeEx(keyboard.VKey,
                                      scanCode,
                                      state.KeyboardState.data(),
                                      characters,
                                      static_cast<int>(std::size(characters)),
                                      4,
                                      layout);
        for (int i = 0; i < count; ++i)
            if (characters[i] >= 0x20)
                QueueEvent(
                    {.Type = InputEventType::CharacterUTF16, .Character = static_cast<unsigned int>(characters[i])});
    }
    return true;
}

LRESULT HandleWindowMessage(InputThreadState& state, UINT message, WPARAM wParam, LPARAM lParam)
{
    const HWND window = state.Window;
    switch (message)
    {
        case UpdateWindowMessage:
            ShowWindow(window, wParam ? SW_SHOWNOACTIVATE : SW_HIDE);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            ReleaseInput(state);
            if (!RefreshMonitors(state))
                FailInput();
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_INPUT:
            if (!QueueRawInput(state, reinterpret_cast<HRAWINPUT>(lParam)))
                FailInput();
            break;
        case WM_CANCELMODE:
            ReleaseInput(state);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_KEYMENU)
                return 0;
            break;
        case WM_CLOSE:
            if (!TerminateProcess(GetCurrentProcess(), 0))
                FailInput();
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK InputWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    auto* state = reinterpret_cast<InputThreadState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        state         = static_cast<InputThreadState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        state->Window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(window, message, wParam, lParam);
    try
    {
        return HandleWindowMessage(*state, message, wParam, lParam);
    }
    catch (...)
    {
        FailInput();
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

#pragma endregion
} // namespace
} // namespace DwmOverlay
