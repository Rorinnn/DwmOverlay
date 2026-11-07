# DwmOverlay

English | [简体中文](README.zh-CN.md)

A Windows x64 DWM overlay framework built with xmake and C++20 modules. Includes D3D11 composition, Dear ImGui rendering, input handling, and [DWM signatures](src/Hook/DWMSignatures.cpp).

## Build

Requires Windows 10 1703 or later (x64), MSVC with C++20 module support, the Windows SDK, and xmake 3.0.0 or later.

Run from the project root:

```powershell
xmake f -p windows -a x64
xmake build DwmOverlay
```

Output: `build/release/DwmOverlay.dll`. xmake manages Detours and Dear ImGui.

## Usage

The DLL initializes automatically when loaded into `dwm.exe` through the Windows loader or manual mapping.

- **Home**: show or hide the overlay.
- **End**: restart DWM.

Customize drawing in `DwmOverlay::DrawOverlay()` in [Overlay.cpp](src/UI/Overlay.cpp).

## License

[MIT](LICENSE). Detours and Dear ImGui retain their respective licenses.
