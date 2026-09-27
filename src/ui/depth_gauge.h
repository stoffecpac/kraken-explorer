#pragma once

#include <numbers>

#include <imgui.h>

struct Stats;

// Dial parts shared with the instrument panel's gauge and knob: the sea-at-night face (same in
// both themes), an 11-tick scale over 270 degrees from 7:30 clockwise to 4:30, a brass bezel.
// The caller layers rings / arcs / needle between the calls.
inline constexpr ImU32 dial_brass = IM_COL32(0xe0, 0xa8, 0x4a, 255);
inline constexpr float dial_start = 0.75f * std::numbers::pi_v<float>;
inline constexpr float dial_span = 1.5f * std::numbers::pi_v<float>;
void dial_face(ImDrawList* dl, ImVec2 c, float r);
// Ticks end at r * outer; major ones every fifth tick.
void dial_ticks(ImDrawList* dl, ImVec2 c, float r, float outer);
void dial_bezel(ImDrawList* dl, ImVec2 c, float r, float thickness);

// "Depth Gauge": progress as a dive into the Mariana Trench, 0 m to Challenger Deep (100 %).
inline constexpr float challenger_deep_m = 10994.0f;

// GL-free helpers (unit tested). fraction is clamped to [0, 1].
float depth_m(float fraction);
// Pelagic zone name for a depth in metres: Epipelagic .. Hadal.
const char* depth_zone(float depth);

// Echo-sounder instrument: round dial with a sonar sweep, a large "4 213 m" and percent readout,
// zone / fathoms line and a depth strip with landmarks (Titanic, Challenger Deep). size.x <= 0
// fills the available width; the height is at least depth_gauge_min_height() so the text stays
// legible. Submits one item (tooltips work on it).
float depth_gauge_min_height();
// gaps (inter-frame time in seconds, optional) adds a statistics line under the instrument.
// sweeping rotates the sonar sweep (while loading / playing); the caller must keep frames coming
// then (main.cpp wait_timeout). Otherwise the sweep is drawn parked.
void draw_depth_gauge(float fraction, ImVec2 size, const Stats* gaps = nullptr, bool sweeping = false);
