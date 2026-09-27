#pragma once

// LIN Control window (the old LinControlWindow): one row per LIN interface of the setup with
// schedule table, Sleep and Wakeup, plus a list of diagnostic requests sent on double-click.

#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "ui/diag_dialog.h"

struct App;
struct Iface;
struct WorkspaceTab;
namespace pugi
{
class xml_node;
}

// Driver LIN calls guarded like iface_send: shared lock on io_mutex, only while the interface
// is open and the driver supports the call. False when nothing was sent.
bool lin_send_sleep_wakeup(Iface& iface, bool wakeup);
bool lin_send_set_schedule(Iface& iface, uint8_t table);
bool lin_send_diag_request(Iface& iface, uint8_t nad, std::span<const uint8_t> data);

// Combo of the setup's LIN interfaces ("<network>: <interface>"); iface = App::ifaces index.
bool draw_lin_iface_combo(const char* id, App& app, uint16_t& iface);

// State of one tab's LIN Control window.
struct LinControl
{
    bool open = false;
    std::vector<LinDiagRequest> requests;
    int selected = -1;
    DiagDialogState dialog;
};

// The "LIN Control" window of `tab` while lc.open.
void draw_lin_control(App& app, const WorkspaceTab& tab, LinControl& lc);

// Workspace persistence: one <DiagRequest name driver interface nad data/> per request, the
// interface resolved by driver + name against ifaces on load (UINT16_MAX when missing).
void lin_control_save_xml(const LinControl& lc, const std::deque<Iface>& ifaces, pugi::xml_node el);
void lin_control_load_xml(LinControl& lc, const std::deque<Iface>& ifaces, pugi::xml_node el);
