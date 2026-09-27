// ui/dbc_editor: fuzzy rows over messages / signals / ids, selection helpers, message re-key,
// add / delete, the dirty flag and one headless draw of the window with a selection.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName

#include "app.h"

#include "db/model/can_db.h"
#include "ui/dbc_editor.h"
#include "ui/workspace_tabs.h"
#include "ui_test.h"

namespace
{

// Two messages, three signals: EngineData (0x100: EngineSpeed, EngineTemp) and BrakeStatus (0x200: BrakePressure).
CanDb sample_db()
{
    CanDb db;
    CanDbMessage engine{.name = "EngineData", .raw_id = 0x100, .dlc = 8, .sender = "ECU"};
    engine.signals.push_back({.name = "EngineSpeed", .start_bit = 0, .length = 16, .is_unsigned = true, .factor = 0.25, .unit = "rpm"});
    engine.signals.push_back({.name = "EngineTemp", .start_bit = 16, .length = 8, .is_unsigned = true, .offset = -40.0, .unit = "degC"});
    CanDbMessage brake{.name = "BrakeStatus", .raw_id = 0x200, .dlc = 4, .sender = "BCM"};
    brake.signals.push_back({.name = "BrakePressure", .start_bit = 0, .length = 16, .is_unsigned = true, .factor = 0.1, .unit = "bar"});
    db.messages[engine.raw_id] = engine;
    db.messages[brake.raw_id] = brake;
    return db;
}

DbcEditorState loaded()
{
    DbcEditorState s;
    dbc_editor_load(s, sample_db(), "");
    return s;
}

int row_of(const DbcEditorState& s, uint32_t raw_id, int signal)
{
    const auto it = std::ranges::find_if(s.rows, [&](const DbcEditorRow& r) { return r.raw_id == raw_id && r.signal == signal; });
    return it == s.rows.end() ? -1 : static_cast<int>(it - s.rows.begin());
}

} // namespace

TEST_CASE("an empty query lists every message by id with all its signals")
{
    DbcEditorState s = loaded();
    REQUIRE(s.rows.size() == 5);
    CHECK(s.rows[0].raw_id == 0x100);
    CHECK(s.rows[0].signal == -1);
    CHECK(s.rows[0].label == "0x100  EngineData  [8]");
    CHECK(s.rows[1].signal == 0);
    CHECK(s.rows[2].signal == 1);
    CHECK(s.rows[3].raw_id == 0x200);
    CHECK(s.rows[3].signal == -1);
    CHECK(s.rows[4].label.find("BrakePressure") != std::string::npos);
    CHECK(!s.dirty);
}

TEST_CASE("\"eng\" ranks EngineData and its signals first and drops the brake message")
{
    DbcEditorState s = loaded();
    s.query = "eng";
    dbc_editor_rebuild_rows(s);
    REQUIRE(s.rows.size() >= 2);
    CHECK(s.rows[0].raw_id == 0x100);
    CHECK(s.rows[0].signal == -1);
    CHECK(s.rows[1].raw_id == 0x100);
    CHECK(s.rows[1].signal >= 0);
    CHECK(row_of(s, 0x200, -1) == -1);
    CHECK(row_of(s, 0x200, 0) == -1);

    s.query = "brake.press"; // Message.Signal
    dbc_editor_rebuild_rows(s);
    REQUIRE(s.rows.size() == 2);
    CHECK(s.rows[0].raw_id == 0x200);
    CHECK(s.rows[1].signal == 0);
}

TEST_CASE("ids match as 0x-hex, bare hex and decimal")
{
    DbcEditorState s = loaded();
    s.query = "0x1";
    dbc_editor_rebuild_rows(s);
    REQUIRE(!s.rows.empty());
    CHECK(std::ranges::all_of(s.rows, [](const DbcEditorRow& r) { return r.raw_id == 0x100; }));
    CHECK(s.rows[0].signal == -1);

    s.query = "512"; // 0x200 in decimal
    dbc_editor_rebuild_rows(s);
    CHECK(row_of(s, 0x200, -1) >= 0);
    CHECK(row_of(s, 0x100, -1) == -1);

    s.query = "200"; // bare hex
    dbc_editor_rebuild_rows(s);
    CHECK(row_of(s, 0x200, -1) >= 0);
}

TEST_CASE("selection helpers follow the rows and survive a rebuild")
{
    DbcEditorState s = loaded();
    dbc_editor_select(s, 2); // EngineTemp
    REQUIRE(dbc_editor_selected_message(s) != nullptr);
    CHECK(dbc_editor_selected_message(s)->name == "EngineData");
    REQUIRE(dbc_editor_selected_signal(s) != nullptr);
    CHECK(dbc_editor_selected_signal(s)->name == "EngineTemp");

    s.query = "temp";
    dbc_editor_rebuild_rows(s);
    REQUIRE(s.selected >= 0);
    CHECK(dbc_editor_selected_signal(s)->name == "EngineTemp");

    s.query = "brake"; // the selection is filtered out: nothing selected, no dangling row
    dbc_editor_rebuild_rows(s);
    CHECK(s.selected == -1);
    CHECK(dbc_editor_selected_message(s) == nullptr);

    dbc_editor_select(s, -1);
    CHECK(dbc_editor_selected_signal(s) == nullptr);
}

TEST_CASE("changing a message id re-keys the map entry and keeps the selection")
{
    DbcEditorState s = loaded();
    dbc_editor_select(s, 0);
    CHECK(dbc_editor_set_message_id(s, 0x100, 0x300));
    CHECK(!s.db.messages.contains(0x100));
    REQUIRE(s.db.messages.contains(0x300));
    CHECK(s.db.messages.at(0x300).raw_id == 0x300);
    CHECK(s.db.messages.at(0x300).name == "EngineData");
    CHECK(s.db.messages.at(0x300).signals.size() == 2);
    CHECK(s.dirty);
    REQUIRE(dbc_editor_selected_message(s) != nullptr);
    CHECK(dbc_editor_selected_message(s)->raw_id == 0x300);
    CHECK(s.rows[s.selected].label == "0x300  EngineData  [8]"); // rows follow the id order: 0x200 first
    CHECK(s.rows[0].raw_id == 0x200);

    CHECK(!dbc_editor_set_message_id(s, 0x300, 0x200)); // taken
    CHECK(s.db.messages.contains(0x300));
    CHECK(!dbc_editor_set_message_id(s, 0x777, 0x400)); // no such message
    CHECK(dbc_editor_set_message_id(s, 0x300, 0x300));  // no-op

    // Extended flag = bit 31; the label shows 8 hex digits.
    CHECK(dbc_editor_set_message_id(s, 0x300, 0x300 | 0x80000000u));
    CHECK(s.rows[s.selected].label == "0x00000300  EngineData  [8]");
}

TEST_CASE("add and delete edit the copy, mark it dirty and re-check")
{
    DbcEditorState s = loaded();
    CHECK(s.check_errors.empty());
    const uint32_t id = dbc_editor_add_message(s);
    CHECK(id == 0);
    CHECK(s.dirty);
    REQUIRE(dbc_editor_selected_message(s) != nullptr);
    CHECK(dbc_editor_selected_message(s)->name == "NewMessage");
    CHECK(s.rows.size() == 6);

    REQUIRE(dbc_editor_add_signal(s));
    REQUIRE(dbc_editor_selected_signal(s) != nullptr);
    CHECK(dbc_editor_selected_signal(s)->name == "NewSignal");
    CHECK(s.db.messages.at(0).signals.size() == 1);

    // A second message with the same name is a semantic problem dbc_check reports.
    dbc_editor_selected_message(s)->name = "EngineData";
    dbc_editor_mark(s);
    CHECK(!s.check_errors.empty());

    REQUIRE(dbc_editor_delete_selected(s)); // the signal; the message stays selected
    CHECK(s.db.messages.at(0).signals.empty());
    REQUIRE(dbc_editor_selected_message(s) != nullptr);
    CHECK(dbc_editor_selected_signal(s) == nullptr);
    REQUIRE(dbc_editor_delete_selected(s)); // the message
    CHECK(!s.db.messages.contains(0));
    CHECK(s.selected == -1);
    CHECK(!dbc_editor_delete_selected(s));
    CHECK(s.rows.size() == 5);
}

TEST_CASE("deleting the multiplexer signal clears the message's muxer index")
{
    DbcEditorState s = loaded();
    CanDbMessage& m = s.db.messages.at(0x100);
    m.signals[0].is_muxer = true;
    m.muxer = 0;
    m.signals[1].is_muxed = true;
    dbc_editor_rebuild_rows(s);
    dbc_editor_select(s, 1); // EngineSpeed = the muxer
    REQUIRE(dbc_editor_delete_selected(s));
    CHECK(m.muxer == -1);
    CHECK(m.signals.size() == 1);
}

TEST_CASE("the window draws headless with a message and a signal selected")
{
    const UiTest ui;
    App app;
    const WorkspaceTab& tab = workspace_add_tab(app.workspace);
    DbcEditorState& s = app.dbc_editors[tab.uid];
    dbc_editor_load(s, sample_db(), "");
    s.open = true;
    s.parse_errors.push_back({.line = 3, .message = "unexpected token"});
    for (const int row : {-1, 0, 1})
    {
        dbc_editor_select(s, row);
        for (int i = 0; i < 3; ++i)
        {
            ImGui::NewFrame();
            draw_dbc_editor(app, s, tab);
            ImGui::EndFrame();
        }
    }
    CHECK(ImGui::FindWindowByName(workspace_window_name(tab, "DBC Editor").c_str()) != nullptr);
    CHECK(!s.dirty); // drawing alone never edits
}
