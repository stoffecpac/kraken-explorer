#include "ui/main_menu.h"

#include <algorithm>
#include <array>
#include <cfloat> // FLT_MAX
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h> // BeginViewportSideBar, GetKeyChordName

#include "app.h"
#include "ui/help_overlay.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace
{

struct CommandInfo
{
    const char* label;
    ImGuiKeyChord chord = ImGuiKey_None;
};

// Labels and shortcuts of mainwindow.ui / mainwindow.cpp, in enum order.
constexpr std::array<CommandInfo, static_cast<std::size_t>(Command::Count)> command_info = {{
    {"New Workspace...", ImGuiMod_Ctrl | ImGuiKey_N},
    {"Open Workspace...", ImGuiMod_Ctrl | ImGuiKey_O},
    {"Open Recent"},
    {"Save Workspace", ImGuiMod_Ctrl | ImGuiKey_S},
    {"Save Workspace As...", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S},
    {"Settings..."},
    {"Exit", ImGuiMod_Alt | ImGuiKey_F4},
    {"Start Measurement", ImGuiKey_F5},
    {"Stop Measurement", ImGuiMod_Shift | ImGuiKey_F5},
    {"Record Trace to File", ImGuiMod_Ctrl | ImGuiKey_R},
    {"Recording Options..."},
    {"Open Recording Folder"},
    {"Setup...", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_S},
    {"Reload Interfaces"},
    {"Clear", ImGuiKey_Escape},
    {"Save Trace to file..."},
    {"Export full trace"},
    {"Import full trace"},
    {"Tab", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T},
    {"Graph View", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G},
    {"Graph View##widget"},
    {"Replay View"},
    {"LIN Control"},
    {"Instrument Panel"},
    {"DBC Editor"},
    {"GPIO Control"},
    {"Standalone Graph", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B},
    {"Gateway"},
    {"Conditional Logging..."},
    {"Find Signal...", ImGuiMod_Ctrl | ImGuiKey_P},
    {"About"},
}};

const CommandInfo& info(Command cmd)
{
    return command_info[static_cast<std::size_t>(cmd)];
}

bool enabled(const App& app, Command cmd)
{
    switch (cmd)
    {
    case Command::MeasurementStart:
    case Command::Setup:
        return !app.measuring;
    case Command::MeasurementStop:
        return app.measuring;
    case Command::Gateway: // two CAN interfaces in the setup
    {
        int can = 0;
        for (const SetupNetwork& net : app.setup.networks)
        {
            can += static_cast<int>(std::ranges::count(net.interfaces, BusType::CAN, &SetupInterface::bus_type));
        }
        return can >= 2;
    }
    default:
        return true;
    }
}

void run(App& app, Command cmd)
{
    switch (cmd)
    {
    case Command::Exit:
        app.quit = true;
        break;
    case Command::TraceClear:
        trace_clear(app.trace);
        break;
    case Command::Record:
        app.menu.record_armed = !app.menu.record_armed;
        break;
    default:
        break;
    }
    app.menu.pending.set(static_cast<std::size_t>(cmd));
}

void menu_item(App& app, Command cmd)
{
    const CommandInfo& ci = info(cmd);
    const int chord = command_chord(app.menu, cmd);
    const char* shortcut = chord != ImGuiKey_None ? ImGui::GetKeyChordName(chord) : nullptr;
    const bool selected = cmd == Command::Record && app.menu.record_armed;
    if (ImGui::MenuItem(ci.label, shortcut, selected, enabled(app, cmd)))
    {
        run(app, cmd);
    }
}

void poll_shortcuts(App& app)
{
    // Esc (Trace Clear) must not fire while it closes a popup or menu.
    const bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (app.menu.capturing_shortcut)
    {
        return; // the key being recorded must not run a command
    }
    for (std::size_t i = 0; i < command_info.size(); ++i)
    {
        const auto cmd = static_cast<Command>(i);
        const ImGuiKeyChord chord = command_chord(app.menu, cmd);
        if (chord == ImGuiKey_None || (chord == ImGuiKey_Escape && popup_open) || !enabled(app, cmd))
        {
            continue;
        }
        if (ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal))
        {
            run(app, cmd);
        }
    }
}

void draw_menu_bar(App& app)
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }
    theme_logo(ImGui::GetFontSize() * 1.2f);
    if (ImGui::BeginMenu("File"))
    {
        menu_item(app, Command::WorkspaceNew);
        ImGui::Separator();
        menu_item(app, Command::WorkspaceOpen);
        if (ImGui::BeginMenu("Open Recent", !app.menu.recent_files.empty()))
        {
            for (const auto& path : app.menu.recent_files)
            {
                const auto slash = path.find_last_of("/\\");
                ImGui::PushID(path.c_str());
                if (ImGui::MenuItem(slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1))
                {
                    app.menu.recent_path = path;
                    run(app, Command::WorkspaceOpenRecent);
                }
                ImGui::SetItemTooltip("%s", path.c_str());
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Recent Files"))
            {
                app.menu.recent_files.clear();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        menu_item(app, Command::WorkspaceSave);
        menu_item(app, Command::WorkspaceSaveAs);
        ImGui::Separator();
        menu_item(app, Command::Settings);
        ImGui::Separator();
        menu_item(app, Command::Exit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Measurement"))
    {
        menu_item(app, Command::MeasurementStart);
        menu_item(app, Command::MeasurementStop);
        ImGui::Separator();
        menu_item(app, Command::Record);
        menu_item(app, Command::RecordingOptions);
        ImGui::Separator();
        menu_item(app, Command::Setup);
        menu_item(app, Command::ReloadInterfaces);
        menu_item(app, Command::ConditionalLogging);
        if (ImGui::BeginMenu("Driver"))
        {
            ImGui::MenuItem("CANblaster", nullptr, &app.menu.canblaster);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Trace"))
    {
        menu_item(app, Command::TraceClear);
        ImGui::Separator();
        menu_item(app, Command::TraceSave);
        menu_item(app, Command::TraceExportFull);
        menu_item(app, Command::TraceImportFull);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window"))
    {
        if (ImGui::BeginMenu("New"))
        {
            for (auto cmd = Command::NewTraceView; cmd <= Command::NewGpioControl;
                 cmd = static_cast<Command>(static_cast<int>(cmd) + 1))
            {
                if (cmd != Command::NewGraphWidget) // same window as Graph View; listing both showed it twice
                {
                    menu_item(app, cmd);
                }
            }
            ImGui::EndMenu();
        }
        menu_item(app, Command::StandaloneGraph);
        menu_item(app, Command::FindSignal);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem("Keyboard shortcuts", "?"))
        {
            help_overlay_open();
        }
        menu_item(app, Command::About);
        ImGui::EndMenu();
    }
    draw_menu_status(app, app.status_bar); // "Connected" + frames/s in the right corner
    ImGui::EndMainMenuBar();
}

// Start/Stop: theme colours (ThemeButton) with the theme's rounding; disabled they fall back to
// the theme's normal disabled button.
void pill_button(App& app, const char* label, Command cmd, const ThemeButton& c, float border, float px)
{
    const bool on = enabled(app, cmd);
    if (on)
    {
        theme_push_button(c);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(15.0f * px, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, on ? border : ImGui::GetStyle().FrameBorderSize);
    ImGui::BeginDisabled(!on);
    if (ImGui::Button(label))
    {
        run(app, cmd);
    }
    ImGui::EndDisabled();
    ImGui::PopStyleVar(2);
    if (on)
    {
        theme_pop_button();
    }
}

void command_tooltip(Command cmd)
{
    const CommandInfo& ci = info(cmd);
    const char* label = ci.label;
    const auto hidden = std::string_view(label).find("##"); // "Graph View##widget"
    const int len = static_cast<int>(hidden == std::string_view::npos ? std::strlen(label) : hidden);
    if (ci.chord == ImGuiKey_None)
    {
        ImGui::SetItemTooltip("%.*s", len, label);
    }
    else
    {
        ImGui::SetItemTooltip("%.*s (%s)", len, label, ImGui::GetKeyChordName(ci.chord));
    }
}

void draw_record_button(App& app)
{
    const bool armed = app.menu.record_armed;
    if (armed)
    {
        theme_push_button(theme_stop_button()); // armed: the coral Stop style
    }
    command_button(app, Command::Record, Icon::Record, "Record");
    ImGui::SameLine(0.0f, 0.0f);
    if (ImGui::ArrowButton("##record_menu", ImGuiDir_Down))
    {
        ImGui::OpenPopup("##record_popup");
    }
    ImGui::SetItemTooltip("Recording options");
    if (armed)
    {
        theme_pop_button();
    }
    if (ImGui::BeginPopup("##record_popup"))
    {
        menu_item(app, Command::RecordingOptions);
        menu_item(app, Command::RecordingOpenFolder);
        ImGui::EndPopup();
    }
}

// REC line of the recorder, or a hint while recording is armed.
void draw_record_status(const App& app)
{
    const bool armed = app.menu.record_armed;
    const bool status = !app.menu.record_status.empty();
    if (status || armed)
    {
        const char* text = status ? app.menu.record_status.c_str() : "Recording armed, starts with measurement";
        same_line_or_wrap(ImGui::CalcTextSize(text).x);
        ImGui::AlignTextToFramePadding();
        if (status)
        {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme_text(ThemeText::rec)), "%s", text);
        }
        else
        {
            ImGui::TextUnformatted(text);
        }
    }
}

constexpr const char* kraken_label = "Release the Kraken";

// same_line_or_wrap() with a wider gap, for the start of a button group.
void group_gap(float next_width, float gap)
{
    ImGui::SameLine(0.0f, gap);
    if (ImGui::GetContentRegionAvail().x < next_width)
    {
        ImGui::NewLine();
    }
}

// Control bar: workspace | measurement | setup, record, trace, windows. Wraps on narrow windows;
// the bar takes the height its content needed last frame.
void draw_control_bar(App& app)
{
    const float px = ImGui::GetFontSize() / 15.0f; // theme_load_fonts(15 * dpi)
    const ImVec2 pad(8.0f * px, 4.0f * px);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 5.0f * px));
    constexpr const char* name = "##control_bar";
    const ImGuiWindow* prev = ImGui::FindWindowByName(name);
    // Last frame's cursor extent: ContentSize is only refreshed inside Begin(), one frame later.
    const float used = prev != nullptr ? prev->DC.CursorMaxPos.y - prev->DC.CursorStartPos.y : 0.0f;
    const float content = used > 0.0f ? used : ImGui::GetFrameHeight();
    const float height = content + pad.y * 2.0f;
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
                                       | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar(name, ImGui::GetMainViewport(), ImGuiDir_Up, height, flags))
    {
        const float group = 20.0f * px; // gap between button groups
        // The Start pill is 8 px taller: centre the normal buttons of its row on it.
        const float dy = 4.0f * px;
        const float row_y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(row_y + dy);
        command_button(app, Command::WorkspaceOpen, Icon::DocumentOpen, "Open");
        same_line_or_wrap(command_button_width("Save"));
        command_button(app, Command::WorkspaceSave, Icon::DocumentSave, "Save");
        group_gap(ImGui::CalcTextSize(kraken_label).x + 30.0f * px, group);
        if (ImGui::GetCursorPosY() == row_y + dy)
        {
            ImGui::SetCursorPosY(row_y); // the pill stayed on the first row
        }
        // Measurement Start/Stop: taller than every other button (frame padding 9 px instead of 5 px).
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 9.0f * px));
        pill_button(app, kraken_label, Command::MeasurementStart, theme_kraken_button(), 2.0f * px, px);
        ImGui::PopStyleVar();
        command_tooltip(Command::MeasurementStart);
        const float pill_y = ImGui::GetItemRectMin().y - ImGui::GetWindowPos().y + ImGui::GetScrollY();
        same_line_or_wrap(ImGui::CalcTextSize("Stop").x + 30.0f * px); // pill padding is 15 px
        if (ImGui::GetCursorPosY() == pill_y)
        {
            ImGui::SetCursorPosY(pill_y + dy); // the rest of the row follows Stop's y through SameLine
        }
        pill_button(app, "Stop", Command::MeasurementStop, theme_stop_button(), 1.5f * px, px);
        command_tooltip(Command::MeasurementStop);

        const char* setup = "Setup Interface...";
        group_gap(command_button_width(setup), group);
        command_button(app, Command::Setup, Icon::PreferencesSystem, setup);
        same_line_or_wrap(command_button_width("Record") + ImGui::GetFrameHeight());
        draw_record_button(app);
        same_line_or_wrap(command_button_width("Clear"));
        command_button(app, Command::TraceClear, Icon::EditClear, "Clear");
        same_line_or_wrap(button_width("Gateway"));
        ImGui::BeginDisabled(!enabled(app, Command::Gateway));
        if (ImGui::Button("Gateway"))
        {
            run(app, Command::Gateway);
        }
        ImGui::EndDisabled();
        command_tooltip(Command::Gateway);
        draw_record_status(app);

        // Sea level: a faint teal swell along the bottom edge.
        const ImVec2 wp = ImGui::GetWindowPos();
        const float y = wp.y + ImGui::GetWindowHeight() - 2.5f * px;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float step = 6.0f * px;
        for (float x = wp.x; x < wp.x + ImGui::GetWindowWidth(); x += step)
        {
            dl->PathLineTo(ImVec2(x, y + std::sin(x / (14.0f * px)) * 1.5f * px));
        }
        dl->PathStroke(ImGui::GetColorU32(ImGuiCol_CheckMark, 0.45f), ImDrawFlags_None, 1.5f * px);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// Help > About: name, version and how fast the app redraws, updated live (T57).
void draw_about(App& app)
{
    constexpr const char* title = "About Kraken Explorer";
    if (menu_take(app.menu, Command::About))
    {
        ImGui::OpenPopup(title);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 44.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX)); // wide, not tall
    bool open = true; // title-bar X: ImGui closes the modal itself when it clears this
    if (ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const float fps = ImGui::GetIO().Framerate;
        const float font = ImGui::GetFontSize();
        ImGui::Spacing();
        theme_logo(font * 6.0f);
        ImGui::SameLine(0.0f, font * 1.5f);
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, font * 1.8f);
        ImGui::TextUnformatted("Kraken Explorer: Day of the N2K Tentacle");
        ImGui::PopFont();
        ImGui::TextDisabled("Deeper than a Peak. Wireshark is stuck in shallow waters.");
        ImGui::Spacing();
        ImGui::Text("Version " VERSION_STRING "   \xc2\xb7   %.1f fps (%.2f ms/frame)", fps,
                    fps > 0.0f ? 1000.0f / fps : 0.0f);
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace

bool menu_take(MainMenu& menu, Command cmd) noexcept
{
    const auto i = static_cast<std::size_t>(cmd);
    const bool set = menu.pending.test(i);
    menu.pending.reset(i);
    return set;
}

void command_button(App& app, Command cmd, Icon icon, const char* label)
{
    ImGui::BeginDisabled(!enabled(app, cmd));
    const bool pressed = icon_text_button(label, icon);
    ImGui::EndDisabled();
    command_tooltip(cmd);
    if (pressed)
    {
        run(app, cmd);
    }
}

float command_button_width(const char* label)
{
    return icon_text_button_width(label);
}

void menu_add_recent(MainMenu& menu, std::string path)
{
    std::erase(menu.recent_files, path);
    menu.recent_files.insert(menu.recent_files.begin(), std::move(path));
    if (menu.recent_files.size() > max_recent_files)
    {
        menu.recent_files.resize(max_recent_files);
    }
}

const char* command_label(Command cmd) noexcept
{
    return info(cmd).label;
}

const char* command_shortcut(const MainMenu& menu, Command cmd)
{
    const int chord = command_chord(menu, cmd);
    return chord != ImGuiKey_None ? ImGui::GetKeyChordName(chord) : "";
}

int command_chord(const MainMenu& menu, Command cmd) noexcept
{
    const int user = menu.chords[static_cast<std::size_t>(cmd)];
    return user != chord_default ? user : static_cast<int>(info(cmd).chord);
}

std::string chord_name(int chord)
{
    return chord != ImGuiKey_None ? ImGui::GetKeyChordName(chord) : "None";
}

int chord_parse(std::string_view text)
{
    if (text.empty() || text == "None")
    {
        return ImGuiKey_None;
    }
    int chord = 0;
    for (std::size_t pos = 0; pos <= text.size();)
    {
        const std::size_t plus = text.find('+', pos);
        const std::string_view part = text.substr(pos, plus == std::string_view::npos ? std::string_view::npos : plus - pos);
        const bool last = plus == std::string_view::npos;
        if (!last && part == "Ctrl") { chord |= ImGuiMod_Ctrl; }
        else if (!last && part == "Shift") { chord |= ImGuiMod_Shift; }
        else if (!last && part == "Alt") { chord |= ImGuiMod_Alt; }
        else if (!last && part == "Super") { chord |= ImGuiMod_Super; }
        else if (last)
        {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
            {
                if (part == ImGui::GetKeyName(static_cast<ImGuiKey>(k)))
                {
                    return chord | k;
                }
            }
            return -1;
        }
        else
        {
            return -1;
        }
        pos = plus + 1;
    }
    return -1;
}

Command command_by_label(std::string_view label) noexcept
{
    for (std::size_t i = 0; i < command_info.size(); ++i)
    {
        const std::string_view l = command_info[i].label;
        if (l.substr(0, l.find("##")) == label)
        {
            return static_cast<Command>(i);
        }
    }
    return Command::Count;
}

void draw_main_menu(App& app)
{
    poll_shortcuts(app);
    draw_menu_bar(app);
    draw_control_bar(app);
    draw_about(app);
}
