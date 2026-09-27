#include "ui/settings_dialog.h"
#include "ui/main_menu.h"

#include <algorithm>
#include <string_view>
#include <cfloat>
#include <format>
#include <string>

#include <imgui.h>
#include <imgui_internal.h> // MarkIniSettingsDirty

#include "app.h"

namespace
{

constexpr const char* popup = "Settings";

} // namespace

void draw_settings_dialog(App& app, SettingsDialogState& s)
{
    if (menu_take(app.menu, Command::Settings))
    {
        s = {.canblaster = app.menu.canblaster,
             .max_trace_size = static_cast<int>(std::min<uint64_t>(app.trace.max_size, 10000000)),
             .saved_theme = app.settings.theme,
             .saved_font_scale_pct = app.settings.font_scale_pct};
        ImGui::OpenPopup(popup);
    }
    const float em = ImGui::GetFontSize();
    if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    ImGui::SeparatorText("Appearance");
    ImGui::SetNextItemWidth(em * 10.0f);
    int theme = static_cast<int>(app.settings.theme);
    bool changed = ImGui::Combo("Theme", &theme, "System\0Light\0Dark\0");
    app.settings.theme = static_cast<ThemeMode>(theme);
    ImGui::SetNextItemWidth(em * 10.0f);
    const std::string current = std::format("{} %", app.settings.font_scale_pct);
    if (ImGui::BeginCombo("Text size", current.c_str()))
    {
        for (const int pct : {100, 110, 125, 150, 175})
        {
            if (ImGui::Selectable(std::format("{} %", pct).c_str(), pct == app.settings.font_scale_pct))
            {
                app.settings.font_scale_pct = pct;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (changed)
    {
        settings_apply_theme(app); // live preview; Cancel reverts
    }

    ImGui::SeparatorText("Trace Window");
    ImGui::SetNextItemWidth(em * 10.0f);
    ImGui::InputInt("Max trace size (messages)", &s.max_trace_size, 10000, 100000);
    s.max_trace_size = std::clamp(s.max_trace_size, 1000, 10000000);
    ImGui::SetItemTooltip("Maximum number of messages kept in memory. When exceeded, the oldest messages are "
                          "discarded and are missing from saved traces. Larger values use more memory.");

    ImGui::SeparatorText("Drivers");
    ImGui::Checkbox("CANblaster (UDP) driver", &s.canblaster);
    ImGui::SetItemTooltip("Applies on Measurement > Reload Interfaces. Enumeration waits 2 s for servers.");

    ImGui::SeparatorText("Shortcuts");
    ImGui::TextDisabled("Click a shortcut, then press the keys. Backspace clears, Esc keeps the old one.");
    // Changes apply at once and are saved with the ini (no revert on Cancel; Reset restores the defaults).
    if (ImGui::BeginChild("##shortcuts", ImVec2(em * 28.0f, em * 12.0f), ImGuiChildFlags_Borders)
        && ImGui::BeginTable("##shortcut_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        for (int i = 0; i < static_cast<int>(Command::Count); ++i)
        {
            const auto cmd = static_cast<Command>(i);
            if (cmd == Command::WorkspaceOpenRecent)
            {
                continue;
            }
            const std::string_view label = command_label(cmd);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label.data(), label.data() + std::min(label.find("##"), label.size()));
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            const std::string name = s.capture == i ? "Press keys..." : chord_name(command_chord(app.menu, cmd));
            if (ImGui::Button(name.c_str(), ImVec2(-FLT_MIN, 0.0f)))
            {
                s.capture = s.capture == i ? -1 : i;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    if (ImGui::Button("Reset shortcuts"))
    {
        app.menu.chords.fill(chord_default);
        ImGui::MarkIniSettingsDirty();
    }
    app.menu.capturing_shortcut = s.capture >= 0;
    if (s.capture >= 0)
    {
        const ImGuiIO& io = ImGui::GetIO();
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        {
            const auto key = static_cast<ImGuiKey>(k);
            const bool modifier = (key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper) || key >= ImGuiKey_ReservedForModCtrl;
            if (modifier || !ImGui::IsKeyPressed(key, false))
            {
                continue;
            }
            if (key != ImGuiKey_Escape)
            {
                const int mods = (io.KeyCtrl ? ImGuiMod_Ctrl : 0) | (io.KeyShift ? ImGuiMod_Shift : 0)
                                 | (io.KeyAlt ? ImGuiMod_Alt : 0) | (io.KeySuper ? ImGuiMod_Super : 0);
                app.menu.chords[static_cast<std::size_t>(s.capture)] = key == ImGuiKey_Backspace ? ImGuiKey_None : (mods | k);
                ImGui::MarkIniSettingsDirty();
            }
            s.capture = -1;
            app.menu.capturing_shortcut = false;
            break;
        }
    }

    ImGui::Separator();
    if (ImGui::Button("OK", ImVec2(em * 6.0f, 0.0f)))
    {
        app.menu.canblaster = s.canblaster;
        app.trace.max_size = static_cast<uint64_t>(s.max_trace_size);
        ImGui::MarkIniSettingsDirty();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 6.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        app.settings.theme = s.saved_theme;
        app.settings.font_scale_pct = s.saved_font_scale_pct;
        settings_apply_theme(app);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
