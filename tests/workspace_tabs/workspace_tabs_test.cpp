#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

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
