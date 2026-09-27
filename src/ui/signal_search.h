// fzf-style search over every DBC / SYM / LDF signal of the setup: the Graph search box,
// the Instrument Panel signal picker and the Ctrl+P "Find signal" palette.
// No <imgui.h> here: graph.h (and so app.h) includes this header.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ui/vim_nav.h"

struct CanDbMessage;
struct CanDbSignal;
struct LinFrame;
struct LinSignal;
struct Setup;

struct SignalEntry
{
    std::string label;                 // "Message.Signal", what the pattern is matched against
    std::size_t network = 0;           // Setup::networks index
    uint32_t raw_id = 0;               // CAN: CanDb::messages key
    const CanDbMessage* can_msg = nullptr; // CAN, else null
    const CanDbSignal* can_sig = nullptr;
    const LinFrame* lin_frame = nullptr;   // LIN, else null; valid until Setup::generation changes
    const LinSignal* lin_sig = nullptr;
};

struct SearchHit
{
    int score = 0;
    int entry = 0; // SignalSearch::entries index
};

struct SignalSearch
{
    uint64_t generation = UINT64_MAX; // Setup::generation the entries were built from
    std::string query;                // query `hits` were ranked for
    std::vector<SignalEntry> entries;
    std::vector<SearchHit> hits;      // matches, best first
    int selected = 0;                 // hits index; Up/Down move it, Enter takes it
    bool scroll_to_selected = false;
    std::vector<int> positions;       // scratch: matched chars of the row being drawn
    bool can_only = false;            // Instrument Panel: CAN signals only
    VimNav vim;                       // j/k/gg/G on the focused list, '/' back to the input
    bool focus_input = false;
};

// Rebuilds the entries when the setup changed and re-ranks when the query changed.
// Cheap when neither did, so call it every frame.
void signal_search_update(SignalSearch& s, const Setup& setup, const std::string& query);

// Search input (hint text) bound to `query`. Up/Down while it is active move the selection.
// Returns the selected entry on Enter, nullptr otherwise.
[[nodiscard]] const SignalEntry* signal_search_input(SignalSearch& s, const Setup& setup, const char* hint,
                                                     std::string& query, bool focus = false);

// Ranked flat list, "Message.Signal [unit]" with the matched characters in the accent colour.
// Click / j / k select, double-click or Enter (list focused) returns the entry. Draw it inside a child / popup of your size.
[[nodiscard]] const SignalEntry* signal_search_list(SignalSearch& s);
