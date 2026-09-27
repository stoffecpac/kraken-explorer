#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include <imgui.h>
#include <imgui_internal.h> // LogToBuffer, GImGui->LogBuffer
#include <misc/cpp/imgui_stdlib.h>

#include "ui/help_overlay.h"
#include "ui/main_menu.h"
#include "ui_test.h"

namespace
{

std::string field;

// One frame: a window with a text field (focused when focus_field), then the overlay.
// Returns everything the frame rendered as text.
std::string frame(bool focus_field = false)
{
    ImGui::NewFrame();
    ImGui::LogToBuffer();
    ImGui::Begin("host");
    if (focus_field)
    {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##field", &field);
    ImGui::End();
    draw_help_overlay(MainMenu{});
    std::string text = GImGui->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::EndFrame();
    return text;
}

void type(ImWchar c)
{
    ImGui::GetIO().AddInputCharacter(c);
}

} // namespace

TEST_CASE("? opens the shortcut overlay, Esc and ? close it, text fields keep their ?")
{
    const UiTest ui;
    ImGuiIO& io = ImGui::GetIO();
    {
        frame();
        CHECK_FALSE(help_overlay_is_open());

        type('?');
        std::string text = frame();
        REQUIRE(help_overlay_is_open());
        text += frame();
        for (Command cmd : {Command::MeasurementStart, Command::MeasurementStop, Command::Record, Command::FindSignal})
        {
            const std::string keys = command_shortcut(MainMenu{}, cmd);
            REQUIRE_FALSE(keys.empty());
            CHECK_MESSAGE(text.find(keys) != std::string::npos, keys);
            CHECK_MESSAGE(text.find(command_label(cmd)) != std::string::npos, command_label(cmd));
        }
        CHECK(text.find("Shift+F5") != std::string::npos); // Stop Measurement
        CHECK(text.find("gg / G") != std::string::npos);   // vim table

        io.AddKeyEvent(ImGuiKey_Escape, true);
        frame();
        io.AddKeyEvent(ImGuiKey_Escape, false);
        frame();
        CHECK_FALSE(help_overlay_is_open());

        type('?'); // toggles
        frame();
        REQUIRE(help_overlay_is_open());
        type('?');
        frame();
        frame();
        CHECK_FALSE(help_overlay_is_open());

        // With a text field active the ? is typed into it.
        frame(true); // focus lands a frame later, io.WantTextInput one more after that
        frame();
        frame();
        REQUIRE(io.WantTextInput);
        type('?');
        frame();
        frame();
        CHECK_FALSE(help_overlay_is_open());
        CHECK(field == "?");
    }
}
