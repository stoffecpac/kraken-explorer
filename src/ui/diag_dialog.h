#pragma once

// "Add/Edit Diagnostic Request" modal of the LIN Control window (the old LinDiagRequestDialog).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct App;

// A stored LIN diagnostic request: NAD + service bytes (SID + params, no PCI).
struct LinDiagRequest
{
    std::string name;
    uint16_t iface = UINT16_MAX; // App::ifaces index
    uint8_t nad = 0;
    std::vector<uint8_t> data;
};

struct DiagDialogState
{
    bool open = false;    // popup requested / shown
    bool editing = false; // title: Edit vs. Add
    std::string name;
    uint16_t iface = UINT16_MAX;
    int nad = 0;
    std::string data_hex;
    std::string error;
};

// "22 F1 90" / "22F190" -> bytes; nullopt on a non-hex character or an odd digit count.
[[nodiscard]] std::optional<std::vector<uint8_t>> parse_hex_bytes(std::string_view text);
// Upper-case pairs separated by spaces.
[[nodiscard]] std::string format_hex_bytes(std::span<const uint8_t> data);

// Fills the fields (from `existing` for Edit, defaults for Add) and requests the popup.
void diag_dialog_open(DiagDialogState& d, const LinDiagRequest* existing);

// Draws the modal while d.open. Returns true once when OK validated; `out` is then filled.
bool draw_diag_dialog(App& app, DiagDialogState& d, LinDiagRequest& out);
