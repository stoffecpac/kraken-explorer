#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <string_view>

#include <imgui.h>
#include <imgui_internal.h> // NavWindow

#include "ui_test.h"
#include "ui/vim_nav.h"

namespace
{

struct List
{
    VimNav vim;
    int selected = 0;
    bool focus_search = false;
    int h = 0;
    bool with_input = false; // an InputText in the window, focused on the first frame
    bool focus_input = false;
    std::array<char, 32> text{};
};

constexpr int rows = 100;
constexpr int page = 20;

void frame(List& l)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({400, 400});
    ImGui::Begin("List");
    vim_nav(l.vim, l.selected, rows, page, l.focus_search, l.h);
    if (l.with_input)
    {
        if (l.focus_input)
        {
            ImGui::SetKeyboardFocusHere();
            l.focus_input = false;
        }
        ImGui::InputText("##filter", l.text.data(), l.text.size());
    }
    ImGui::End();
    ImGui::EndFrame();
}

void type(List& l, ImWchar c)
{
    ImGui::GetIO().AddInputCharacter(c);
    frame(l);
}

void ctrl(List& l, ImGuiKey key)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(key, true);
    frame(l);
    io.AddKeyEvent(key, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame(l);
}

} // namespace

TEST_CASE("list motions: j k gg G Ctrl+d/u/f/b, clamped to the rows")
{
    const UiTest ui;
    List l;
    frame(l); // the new window takes focus
    type(l, 'j');
    CHECK(l.selected == 1);
    type(l, 'j');
    type(l, 'k');
    CHECK(l.selected == 1);
    type(l, 'k');
    type(l, 'k');
    CHECK(l.selected == 0); // clamped
    type(l, 'G');
    CHECK(l.selected == rows - 1);
    type(l, 'j');
    CHECK(l.selected == rows - 1);
    type(l, 'g');
    CHECK(l.selected == rows - 1); // one g waits
    type(l, 'g');
    CHECK(l.selected == 0);
    ctrl(l, ImGuiKey_D);
    CHECK(l.selected == page / 2);
    ctrl(l, ImGuiKey_F);
    CHECK(l.selected == page / 2 + page);
    ctrl(l, ImGuiKey_B);
    ctrl(l, ImGuiKey_U);
    CHECK(l.selected == 0);
    type(l, 'G');
    ctrl(l, ImGuiKey_F);
    CHECK(l.selected == rows - 1);
}

TEST_CASE("gg needs both g within 0.5 s; j between them cancels")
{
    const UiTest ui;
    List l;
    l.selected = 50;
    frame(l);
    type(l, 'g');
    for (int i = 0; i < 40; ++i) // 0.67 s
    {
        frame(l);
    }
    type(l, 'g');
    CHECK(l.selected == 50);
    type(l, 'j');
    type(l, 'g');
    CHECK(l.selected == 51);
    type(l, 'g');
    CHECK(l.selected == 0);
}

TEST_CASE("h / l and / are reported to the caller")
{
    const UiTest ui;
    List l;
    frame(l);
    type(l, 'h');
    CHECK(l.h == -1);
    type(l, 'l');
    type(l, 'l');
    CHECK(l.h == 1);
    CHECK_FALSE(l.focus_search);
    type(l, '/');
    CHECK(l.focus_search);
    CHECK(l.selected == 0);
}

TEST_CASE("nothing happens while a text field has the keyboard")
{
    const UiTest ui;
    List l;
    l.with_input = true;
    l.focus_input = true;
    for (int i = 0; i < 3; ++i)
    {
        frame(l);
    }
    REQUIRE(ImGui::GetIO().WantTextInput);
    type(l, 'j');
    type(l, 'G');
    ctrl(l, ImGuiKey_D);
    CHECK(l.selected == 0);
    CHECK(std::string_view(l.text.data()) == "jG"); // the field got the letters instead
}

TEST_CASE("pick window: 2x2 grid")
{
    // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    const std::array<VimRect, 4> grid = {{
        {.x0 = 0, .y0 = 0, .x1 = 100, .y1 = 100},
        {.x0 = 100, .y0 = 0, .x1 = 200, .y1 = 100},
        {.x0 = 0, .y0 = 100, .x1 = 100, .y1 = 200},
        {.x0 = 100, .y0 = 100, .x1 = 200, .y1 = 200},
    }};
    CHECK(vim_pick_window(grid, 0, VimDir::Right) == 1);
    CHECK(vim_pick_window(grid, 0, VimDir::Down) == 2);
    CHECK(vim_pick_window(grid, 0, VimDir::Left) == -1);
    CHECK(vim_pick_window(grid, 0, VimDir::Up) == -1);
    CHECK(vim_pick_window(grid, 3, VimDir::Up) == 1);
    CHECK(vim_pick_window(grid, 3, VimDir::Left) == 2);
    CHECK(vim_pick_window(grid, 3, VimDir::Next) == 0);
    CHECK(vim_pick_window(grid, -1, VimDir::Right) == 0);
    CHECK(vim_pick_window({}, 0, VimDir::Right) == -1);
}

TEST_CASE("Ctrl+w l focuses the window to the right of the same tab")
{
    const UiTest ui;
    const auto draw = [] {
        ImGui::NewFrame();
        vim_window_nav(*static_cast<VimNav*>(ImGui::GetIO().UserData), 7);
        const auto window = [](const char* name, float x) {
            ImGui::SetNextWindowPos({x, 0});
            ImGui::SetNextWindowSize({100, 100});
            ImGui::Begin(name);
            ImGui::End();
        };
        window("B###B@7", 100);
        window("Other###Other@17", 300); // another tab (uid 17 also ends in 7)
        window("A###A@7", 0);            // last: focused on appearing
        ImGui::EndFrame();
    };
    VimNav v;
    ImGuiIO& io = ImGui::GetIO();
    io.UserData = &v;
    draw();
    draw();
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    REQUIRE(g.NavWindow != nullptr);
    REQUIRE(std::string_view(g.NavWindow->Name) == "A###A@7");
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_W, true);
    draw();
    io.AddKeyEvent(ImGuiKey_W, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    draw();
    io.AddInputCharacter('l');
    draw();
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "B###B@7");
    io.AddKeyEvent(ImGuiMod_Ctrl, true); // Ctrl+w Ctrl+l: nothing further right in this tab
    io.AddKeyEvent(ImGuiKey_W, true);
    draw();
    io.AddKeyEvent(ImGuiKey_W, false);
    io.AddKeyEvent(ImGuiKey_L, true);
    draw();
    io.AddKeyEvent(ImGuiKey_L, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "B###B@7");
}

TEST_CASE("Shift+h / Shift+l focus the window left / right without Ctrl+w")
{
    const UiTest ui;
    const auto draw = [] {
        ImGui::NewFrame();
        vim_window_nav(*static_cast<VimNav*>(ImGui::GetIO().UserData), 7);
        const auto window = [](const char* name, float x) {
            ImGui::SetNextWindowPos({x, 0});
            ImGui::SetNextWindowSize({100, 100});
            ImGui::Begin(name);
            ImGui::End();
        };
        window("B###B@7", 100);
        window("A###A@7", 0); // last: focused on appearing
        ImGui::EndFrame();
    };
    VimNav v;
    ImGuiIO& io = ImGui::GetIO();
    io.UserData = &v;
    draw();
    draw();
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    REQUIRE(std::string_view(g.NavWindow->Name) == "A###A@7");
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddInputCharacter('L');
    draw();
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "B###B@7");
    io.AddInputCharacter('H');
    draw();
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "A###A@7");
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddInputCharacter('L'); // no Shift held (e.g. Caps Lock): not a window move
    draw();
    draw();
    CHECK(std::string_view(g.NavWindow->Name) == "A###A@7");
}
