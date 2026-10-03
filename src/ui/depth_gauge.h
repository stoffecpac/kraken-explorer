#pragma once

#include <numbers>

#include <imgui.h>


// Dial parts shared with the instrument panel's gauge and knob: the sea-at-night face (same in
// both themes), an 11-tick scale over 270 degrees from 7:30 clockwise to 4:30, a brass bezel.
// The caller layers rings / arcs / needle between the calls.
inline constexpr ImU32 dial_brass = IM_COL32(0xe0, 0xa8, 0x4a, 255);
inline constexpr float dial_start = 0.75f * std::numbers::pi_v<float>;
inline constexpr float dial_span = 1.5f * std::numbers::pi_v<float>;
void dial_face(ImDrawList* dl, ImVec2 c, float r);
// Ticks end at r * outer; major ones every fifth tick.
void dial_ticks(ImDrawList* dl, ImVec2 c, float r, float outer);
// Tapered needle from the centre to radius r at `angle`, with a short counterweight.
void dial_needle(ImDrawList* dl, ImVec2 c, float r, float angle, ImU32 col);

// Progress strip of the Replay window: water filling the strip from `fill_from` to `fill_to`
// (fractions of the width), a kraken tentacle behind it, the percent at the right. mark_a /
// mark_b (fractions, optional): two draggable markers on the strip, the replay's start and end;
// dragging writes them back (A stays left of B). size.x <= 0 fills the available width; the
// height is at least depth_gauge_min_height(). Submits one item (tooltips work on it).
float depth_gauge_min_height();
// sweeping makes the tentacle sway (while loading / playing); the caller keeps frames coming then.
void draw_depth_gauge(float fill_from, float fill_to, ImVec2 size, bool sweeping, float* mark_a = nullptr,
                      float* mark_b = nullptr);
