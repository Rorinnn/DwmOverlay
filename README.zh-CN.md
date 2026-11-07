# DwmOverlay

[English](README.md) | 简体中文

Windows x64 DWM 绘制框架，使用 xmake 和 C++20 Modules，包含 D3D11 合成、Dear ImGui 绘制、输入处理和 [DWM 签名](src/Hook/DWMSignatures.cpp)。

## 构建

需要 Windows 10 1703 或更新版本（x64）、支持 C++20 Modules 的 MSVC、Windows SDK，以及 xmake 3.0.0 或更新版本。

在项目根目录执行：

```powershell
xmake f -p windows -a x64
xmake build DwmOverlay
```

输出为 `build/release/DwmOverlay.dll`。Detours 和 Dear ImGui 由 xmake 管理。

## 使用

DLL 支持通过 Windows 加载器或手动映射加载到 `dwm.exe`，加载后自动初始化。

- **Home**：显示或隐藏画面。
- **End**：重启 DWM。

在 [Overlay.cpp](src/UI/Overlay.cpp) 的 `DwmOverlay::DrawOverlay()` 中修改绘制内容。

## 许可证

[MIT](LICENSE)。Detours 和 Dear ImGui 保留各自的许可证。
