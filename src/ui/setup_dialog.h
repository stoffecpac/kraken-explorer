// Measurement setup dialog: network tree on the left, the selected item's page on the right
// (network name, interface list, databases, or the CAN / LIN interface page). Edits a copy
// of App::setup, applied on OK.
// Replaces SetupDialog, GenericCanSetupPage, GenericLinSetupPage and LinFrameDefaultsDialog.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/setup.h"
#include "ui/file_dialog.h"

struct App;
struct IfaceInfo;

enum class SetupSel : uint8_t
{
    None,
    Network,
    Interfaces,
    Interface,
    Databases,
    CanDb,
    LinDb,
};

// "LIN Frame Default Data" popup of the LIN page; edits SetupInterface::lin_frame_defaults.
struct LinFrameDefaultsState
{
    bool open_request = false; // OpenPopup on the next draw
    std::string node;          // publisher filter, empty = all frames
    int frame = 0;             // index into the filtered frame list
};

struct SetupDialogState
{
    bool open_request = false; // OpenPopup on the next draw
    Setup work;                // edited copy of App::setup
    SetupSel sel = SetupSel::None;
    int net = -1;              // selected network
    int item = -1;             // interface / database index for SetupSel::Interface, CanDb, LinDb
    int row = -1;              // selected row in the Interfaces / Databases page
    bool add_request = false;  // tree "Add..." -> open the Add Interfaces popup on the page
    std::vector<int> add_candidates; // Add Interface popup: App::ifaces indices
    std::vector<char> add_checked;
    LinFrameDefaultsState frame_defaults;
    std::string message;       // last error, shown under the page
    FileDialog db_dialog;      // "Add Database...": files go to network db_net
    int db_net = -1;
    bool db_lin = false;
};

// Copies App::setup into the dialog and opens it (Command::Setup).
void setup_dialog_open(App& app, SetupDialogState& s);
// Draws the modal while open; OK writes the edited setup back to App::setup.
void draw_setup_dialog(App& app, SetupDialogState& s);

// Logic behind the pages (testable without ImGui).
// The choices the interface offers for the CAN page's combo boxes, given the current selection.
struct CanTimingLists
{
    std::vector<unsigned> bitrates;
    std::vector<unsigned> sample_points;
    std::vector<unsigned> fd_bitrates;
    std::vector<unsigned> fd_sample_points;
};
// Snaps bitrate / sample points / FD bitrate to entries the interface offers, as the Qt
// combo boxes did; untouched (empty lists) when the interface lists no bitrates. Sets can_fd.
CanTimingLists can_setup_snap(const IfaceInfo& info, SetupInterface& intf);
// Div+Seg1+Seg2 limits of the custom (FD) bitrate fields.
[[nodiscard]] uint32_t custom_bitrate_clamp(uint32_t v) noexcept;
[[nodiscard]] uint32_t custom_fd_bitrate_clamp(uint32_t v) noexcept;
// LDF-derived settings: path, baud rate, timebase, jitter and protocol version.
void lin_setup_apply_ldf(const LinDb& db, SetupInterface& intf);
// Writes a LIN signal's raw value into frame data (LSB first, bits past data.size() dropped).
void lin_signal_write_raw(std::span<uint8_t> data, const LinSignal& sig, uint64_t raw) noexcept;
// True when some network in `setup` already holds driver/name (Add Interfaces hides it:
// one interface belongs to one network).
[[nodiscard]] bool setup_interface_used(const Setup& setup, std::string_view driver, std::string_view name);
// Frame data of `length` zero bytes with every signal's init value.
[[nodiscard]] std::vector<uint8_t> lin_frame_init_data(const LinFrame& frame);
