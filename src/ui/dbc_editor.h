// DBC Editor window: edits a copy of a CAN database (messages, signals, value tables) with a
// fuzzy-searched message / signal list, a form for the selection and a Problems panel fed by the
// parser (syntax) and dbc_check (semantics). Plain data plus free functions. No <imgui.h> here:
// app.h includes this header.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "db/dbc/dbc_parser.h" // DbcError
#include "db/model/can_db.h"
#include "ui/file_dialog.h"
#include "ui/vim_nav.h"

struct App;
struct WorkspaceTab;

// One line of the left-hand list: a message, or (signal >= 0) a signal of it.
struct DbcEditorRow
{
    uint32_t raw_id = 0; // CanDb::messages key
    int signal = -1;     // CanDbMessage::signals index, -1 = the message row
    int score = 0;       // fuzzy score of the query (0 when the query is empty)
    std::string label;   // "0x123  Name  [8]" or the indented signal name
};

struct DbcEditorState
{
    bool open = false;
    CanDb db;             // the editor's own copy; the setup's databases are never edited in place
    std::string path;     // file the copy came from / was saved to; empty = unsaved new database
    bool dirty = false;
    std::string query;    // search field
    std::string applied_query; // query the rows were built for
    std::vector<DbcEditorRow> rows;
    int selected = -1;         // rows index of the selection, -1 = none
    uint32_t sel_id = UINT32_MAX; // the selection itself (survives a rows rebuild); UINT32_MAX = none
    int sel_signal = -1;
    bool scroll_to_selected = false;
    VimNav vim;
    bool focus_search = false;
    std::vector<DbcError> parse_errors; // from the last Open / Check of the file on disk
    std::vector<DbcError> check_errors; // dbc_check of the editor's copy
    FileDialog dialog;
    // Message id field: edited as text so a half-typed id does not re-key the message on every keystroke.
    std::string id_text;
    uint32_t id_text_for = UINT32_MAX;
    bool id_conflict = false;
    // Value table "add row" inputs of the signal form.
    uint64_t vt_value = 0;
    std::string vt_name;
};

// Replaces the editor's database with a copy of `db` (path = `path`), clears the selection and
// the parse errors, runs dbc_check.
void dbc_editor_load(DbcEditorState& s, const CanDb& db, std::string path);
// Parses the file into the editor (whatever parsed is kept on a syntax error, so the window stays
// usable) and stores the parse errors. False when the parser reported errors.
bool dbc_editor_open_file(DbcEditorState& s, const std::filesystem::path& path);
// Re-parses s.path from disk for syntax errors and runs dbc_check on the editor's copy.
void dbc_editor_check(DbcEditorState& s);
// After any edit of s.db: dirty, dbc_check re-run, rows rebuilt (labels may have changed).
void dbc_editor_mark(DbcEditorState& s);

// Rows for s.query. Empty query: every message by id with all its signals. Otherwise a message is
// listed when its name / id ("0x123", "123", "291") or one of its "Message.Signal" labels matches,
// groups sorted by their best score, only the matching signals under each. Restores `selected`
// from sel_id / sel_signal.
void dbc_editor_rebuild_rows(DbcEditorState& s);
// Selects row `row` (-1 clears); keeps sel_id / sel_signal in step.
void dbc_editor_select(DbcEditorState& s, int row);
[[nodiscard]] CanDbMessage* dbc_editor_selected_message(DbcEditorState& s);
[[nodiscard]] CanDbSignal* dbc_editor_selected_signal(DbcEditorState& s);

// Moves the message keyed old_raw to new_raw (raw_id included). False (nothing changed) when
// new_raw is taken or old_raw does not exist.
bool dbc_editor_set_message_id(DbcEditorState& s, uint32_t old_raw, uint32_t new_raw);
// New message at the lowest free standard id, selected. Returns its raw id.
uint32_t dbc_editor_add_message(DbcEditorState& s);
// Appends "NewSignal" to the selected message and selects it. False without a selection.
bool dbc_editor_add_signal(DbcEditorState& s);
// Removes the selected signal, or the selected message with its signals. False without a selection.
bool dbc_editor_delete_selected(DbcEditorState& s);

// Writes with dbc_write_file; on success clears dirty, remembers the path and reloads the setup's
// databases when one of them is that file. Errors go to the log.
bool dbc_editor_save(App& app, DbcEditorState& s, const std::filesystem::path& path);

// The "DBC Editor" window of `tab` while s.open.
void draw_dbc_editor(App& app, DbcEditorState& s, const WorkspaceTab& tab);
