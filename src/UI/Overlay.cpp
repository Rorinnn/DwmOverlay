// Overlay: Interactive ImGui window with keyboard input

module;

#include <imgui.h>

export module DwmOverlay.UI.Overlay;

import std;

export namespace DwmOverlay
{
void DrawOverlay()
{
    static std::array<char, 256> inputText{};
    static std::array<char, 256> submittedText{};

    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440.0f, 280.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("DwmOverlay"))
    {
        ImGui::End();
        return;
    }

    const auto& io = ImGui::GetIO();
    ImGui::Text("%.0f FPS  |  %.0f x %.0f", static_cast<double>(io.Framerate), static_cast<double>(io.DisplaySize.x), static_cast<double>(io.DisplaySize.y));
    ImGui::TextUnformatted("Home: show / hide    End: restart DWM");
    ImGui::Spacing();

    ImGui::TextUnformatted("Keyboard input");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    const bool shouldSubmit = ImGui::InputTextWithHint("##Message", "Type here and press Enter", inputText.data(), inputText.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Submit") || shouldSubmit)
        submittedText = inputText;
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        inputText.fill('\0');
        submittedText.fill('\0');
    }
    ImGui::TextDisabled("Tab: next control    Shift+Tab: previous control");
    ImGui::Spacing();

    if (submittedText[0] != '\0')
    {
        ImGui::TextUnformatted("Submitted text");
        ImGui::TextWrapped("%s", submittedText.data());
    }
    ImGui::End();
}
} // namespace DwmOverlay
