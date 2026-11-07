set_project("DwmOverlay")
set_allowedplats("windows")
set_allowedarchs("x64")
set_toolchains("msvc")
set_languages("cxx20")
set_defaultmode("release")

add_rules("mode.debug", "mode.release")

if is_mode("debug") then
    set_runtimes("MTd")
else
    set_runtimes("MT")
end

add_requires("microsoft-detours")
add_requires("imgui 1.92.9", {configs = {dx11 = true, win32 = true}})

target("DwmOverlay")
    set_kind("shared")
    set_warnings("all")
    set_filename("DwmOverlay.dll")
    set_targetdir("build/$(mode)")
    set_policy("build.c++.modules", true)
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE")
    add_cxxflags("/utf-8", "/permissive-")
    add_files("src/**.ixx", "src/**.cpp")
    add_packages("microsoft-detours", "imgui")
    add_syslinks("d3d11", "d3dcompiler", "dxgi", "dwmapi", "user32", "gdi32", "kernel32")
