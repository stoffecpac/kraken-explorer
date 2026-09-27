#pragma once

// "?" help overlay: every global shortcut (from the main menu's command table) and the vim
// motions (ui/vim_nav.h). "?" toggles it (typed character, any keyboard layout) while no text
// field has input and no popup is open; Esc or a click outside closes it. Costs nothing while closed.
// Call once per frame at the root of the frame (app_frame).
struct MainMenu;
void draw_help_overlay(const MainMenu& menu);

// Opens the overlay from anywhere (Help > Keyboard shortcuts).
void help_overlay_open();

[[nodiscard]] bool help_overlay_is_open();
