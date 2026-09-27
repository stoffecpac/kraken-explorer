#pragma once

#include <cstdint>

#include "ui/settings.h"

struct App;

// File > Settings (old SettingsDialog). Edits copies; OK writes them back to where T24's
// settings.cpp reads and saves them. Opens on Command::Settings. Theme and text size are edited
// in app.settings directly (live preview); Cancel restores the saved_* values.
struct SettingsDialogState
{
    bool canblaster = false;
    int max_trace_size = 500000;
    ThemeMode saved_theme = ThemeMode::System; // app.settings when the dialog opened
    int saved_font_scale_pct = 110;
    int capture = -1; // Command whose shortcut is being recorded, -1 = none
};

void draw_settings_dialog(App& app, SettingsDialogState& s);
