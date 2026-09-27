#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "app.h"
#include "ui_test.h"
#include "ui/workspace_tabs.h"

static void frame(App& app)
{
    ImGui::NewFrame();
    if (WorkspaceTab* tab = draw_workspace(app))
    {
        draw_trace_window(app, app.trace_windows[tab->uid], *tab);
        draw_tx_generator(app, *tab, app.tx_generators[tab->uid]); // Generator View + Message View
        draw_log_window(app.log_window, *tab);
        draw_can_status(app, app.can_status, *tab);
        draw_graph_windows(app, tab);
        draw_script_window(app, app.script, *tab);
    }
    ImGui::EndFrame();
    app.menu.pending.reset();
}

static ImGuiWindow* window(const WorkspaceTab& tab, const char* title)
{
    return ImGui::FindWindowByName(workspace_window_name(tab, title).c_str());
}

TEST_CASE("default layout of createTraceWindow, one dockspace per tab")
{
    const UiTest ui({1280, 800}, true, true); // ImPlot: Graph window (draw_graph_windows)

    App app;
    for (int i = 0; i < 4; ++i)
    {
        frame(app);
    }
    REQUIRE(app.workspace.tabs.size() == 1);
    const WorkspaceTab first = app.workspace.tabs[0]; // copy: the vector grows below
    ImGuiWindow* trace = window(first, "Trace");
    ImGuiWindow* status = window(first, "CAN Status");
    ImGuiWindow* log = window(first, "Log");
    ImGuiWindow* gen = window(first, "Generator View");
    ImGuiWindow* script = window(first, "Python Script");
    ImGuiWindow* graph = window(first, "Graph");
    REQUIRE(trace != nullptr);
    REQUIRE(trace->DockNode != nullptr);
    REQUIRE(graph != nullptr);
    REQUIRE(graph->DockNode != nullptr);
    // Graph on the right, Trace on top of the left column, one tabbed node (Log, Generator, Message,
    // Python) under it, CAN Status in full column width at the bottom.
    CHECK(graph->DockNode->Pos.x > trace->DockNode->Pos.x);
    // Size to fit: the left column is as wide as CAN Status needs (capped at 70 % of the viewport).
    REQUIRE(app.can_status.fit_width > 0.0f);
    CHECK(trace->DockNode->Size.x == doctest::Approx(std::min(app.can_status.fit_width, 1280.0f * 0.7f)).epsilon(0.02));
    CHECK(graph->DockNode->Size.y == doctest::Approx(trace->DockNode->Size.y + log->DockNode->Size.y + status->DockNode->Size.y).epsilon(0.02)); // + splitters
    CHECK(trace->DockNode->Size.x == doctest::Approx(log->DockNode->Size.x).epsilon(0.01));
    CHECK(status->DockNode->Size.x == doctest::Approx(trace->DockNode->Size.x).epsilon(0.01));
    CHECK(status->DockNode != log->DockNode);
    CHECK(gen->DockNode == script->DockNode);
    CHECK(log->DockNode == gen->DockNode);
    CHECK(log->DockNode->Pos.y > trace->DockNode->Pos.y);
    CHECK(status->DockNode->Pos.y > log->DockNode->Pos.y);
    // Graph is the central node: it takes up window size changes, the column keeps its width (T58).
    CHECK(graph->DockNode->IsCentralNode());
    // Front tabs as in docs/view.png.
    CHECK(gen->DockNode->TabBar->SelectedTabId == gen->TabId);

    // Window > New Trace View opens a second tab with its own dockspace and makes it current.
    app.menu.pending.set(static_cast<std::size_t>(Command::NewTraceView));
    for (int i = 0; i < 4; ++i)
    {
        frame(app);
    }
    REQUIRE(app.workspace.tabs.size() == 2);
    CHECK(app.workspace.current == 1);
    const WorkspaceTab& second = app.workspace.tabs[1];
    CHECK(second.dockspace != first.dockspace);
    REQUIRE(window(second, "Trace") != nullptr);
    CHECK(window(second, "Trace")->DockNode != nullptr);
    // The first tab's windows are not drawn but stay docked (kept alive).
    CHECK_FALSE(trace->Active);
    CHECK(trace->DockId != 0);

}

TEST_CASE("new tabs get unique names; closing drops the tab's window state; the last tab stays")
{
    const UiTest ui({1280, 800}, true, true);
    {
        App app;
        frame(app);
        for (int n = 0; n < 2; ++n)
        {
            app.menu.pending.set(static_cast<std::size_t>(Command::NewTraceView));
            for (int i = 0; i < 3; ++i)
            {
                frame(app);
            }
        }
        REQUIRE(app.workspace.tabs.size() == 3);
        CHECK(app.workspace.tabs[0].title == "Trace");
        CHECK(app.workspace.tabs[1].title == "Tab 2");
        CHECK(app.workspace.tabs[2].title == "Tab 3");
        CHECK(app.workspace.current == 2);

        const unsigned uid = app.workspace.tabs[1].uid;
        REQUIRE(app.trace_windows.contains(uid));
        REQUIRE(app.tx_generators.contains(uid));
        app.lin_controls[uid].open = true;
        workspace_close_tab(app, uid);
        REQUIRE(app.workspace.tabs.size() == 2);
        CHECK(app.workspace.tabs[1].title == "Tab 3");
        CHECK(app.workspace.current == 1); // still on "Tab 3"
        CHECK_FALSE(app.trace_windows.contains(uid));
        CHECK_FALSE(app.tx_generators.contains(uid));
        CHECK_FALSE(app.lin_controls.contains(uid));
        for (int i = 0; i < 3; ++i)
        {
            frame(app);
        }
        // A name taken by a renamed tab is skipped.
        app.workspace.tabs[0].title = "Tab 3";
        CHECK(workspace_unique_title(app.workspace) == "Tab 4");

        workspace_close_tab(app, app.workspace.tabs[1].uid); // the current one
        REQUIRE(app.workspace.tabs.size() == 1);
        CHECK(app.workspace.current == 0);
        workspace_close_tab(app, app.workspace.tabs[0].uid); // the last tab is not closable
        CHECK(app.workspace.tabs.size() == 1);
        for (int i = 0; i < 3; ++i)
        {
            frame(app);
        }
        CHECK(window(app.workspace.tabs[0], "Trace")->Active);
    }
}

// The workspace tab bar: the one with the trailing "+" (dock nodes have tab bars too).
static ImGuiTabBar* workspace_bar()
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int n = 0; n < g.TabBars.GetMapSize(); ++n)
    {
        if (ImGuiTabBar* b = g.TabBars.TryGetMapData(n);
            b != nullptr && std::ranges::any_of(b->Tabs, [](const ImGuiTabItem& t) { return (t.Flags & ImGuiTabItemFlags_Trailing) != 0; }))
        {
            return b;
        }
    }
    return nullptr;
}

static ImVec2 tab_center(const ImGuiTabBar& bar, const ImGuiTabItem& t)
{
    return {bar.BarRect.Min.x + t.Offset + t.Width * 0.5f, (bar.BarRect.Min.y + bar.BarRect.Max.y) * 0.5f};
}

static void mouse_button(App& app, bool down)
{
    ImGui::GetIO().AddMouseButtonEvent(0, down);
    frame(app);
}

TEST_CASE("a double click on + adds one tab and closes none (the first tab's X appears under it)")
{
    const UiTest ui({1280, 800}, true, true);
    App app;
    for (int i = 0; i < 3; ++i)
    {
        frame(app);
    }
    ImGuiTabBar* bar = workspace_bar();
    REQUIRE(bar != nullptr);
    const auto plus = std::ranges::find_if(bar->Tabs, [](const ImGuiTabItem& t) { return (t.Flags & ImGuiTabItemFlags_Trailing) != 0; });
    const ImVec2 at = tab_center(*bar, *plus);
    ImGui::GetIO().AddMousePosEvent(at.x, at.y);
    frame(app);
    for (int click = 0; click < 2; ++click)
    {
        mouse_button(app, true);
        mouse_button(app, false);
        for (int i = 0; i < 5; ++i) // the layout settles; still within the double-click time
        {
            frame(app);
        }
    }
    REQUIRE(app.workspace.tabs.size() == 2);
    CHECK(app.workspace.tabs[0].title == "Trace");
    CHECK(app.workspace.hold_close);
    ImGui::GetIO().AddMousePosEvent(at.x, 400.0f); // off the bar: close buttons come back
    frame(app);
    CHECK_FALSE(app.workspace.hold_close);
}

TEST_CASE("dragging a tab reorders the workspace tabs: numbers and digit keys follow")
{
    const UiTest ui({1280, 800}, true, true);
    App app;
    frame(app);
    app.menu.pending.set(static_cast<std::size_t>(Command::NewTraceView));
    for (int i = 0; i < 3; ++i)
    {
        frame(app);
    }
    REQUIRE(app.workspace.tabs.size() == 2);
    ImGuiTabBar* bar = workspace_bar();
    REQUIRE(bar != nullptr);
    std::vector<ImVec2> centers;
    for (const ImGuiTabItem& t : bar->Tabs)
    {
        if ((t.Flags & ImGuiTabItemFlags_Trailing) == 0)
        {
            centers.push_back(tab_center(*bar, t));
        }
    }
    REQUIRE(centers.size() == 2);
    std::ranges::sort(centers, {}, &ImVec2::x);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(centers[1].x, centers[1].y); // "2 Tab 2"
    frame(app);
    mouse_button(app, true);
    for (float x = centers[1].x; x > centers[0].x - 30.0f; x -= 10.0f)
    {
        io.AddMousePosEvent(x, centers[1].y);
        frame(app);
    }
    mouse_button(app, false);
    frame(app);
    REQUIRE(app.workspace.tabs.size() == 2);
    CHECK(app.workspace.tabs[0].title == "Tab 2");
    CHECK(app.workspace.tabs[1].title == "Trace");
    CHECK(app.workspace.tabs[static_cast<std::size_t>(app.workspace.current)].title == "Tab 2");

    io.AddInputCharacter('2'); // the second tab shown is "Trace" now
    frame(app);
    frame(app);
    CHECK(app.workspace.tabs[static_cast<std::size_t>(app.workspace.current)].title == "Trace");
}
