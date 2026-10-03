/*
  Copyright (c) 2026 Schildkroet

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "ui/depth_gauge.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>


namespace
{

// Water colour from the surface (0) to the deep (1).
ImU32 water(float t)
{
    const ImVec4 top{0.10f, 0.83f, 0.77f, 1.0f}; // #19D3C5
    const ImVec4 deep{0.02f, 0.05f, 0.16f, 1.0f};
    return ImGui::ColorConvertFloat4ToU32(
        {top.x + (deep.x - top.x) * t, top.y + (deep.y - top.y) * t, top.z + (deep.z - top.z) * t, 1.0f});
}

// A kraken tentacle along the strip, behind the water: a thick base at the left tapering to a tip
// that curls up and sways, two rows of suckers with white rims on the underside, a dark edge.
void draw_tentacle(ImDrawList* dl, ImVec2 a, ImVec2 b, bool sweeping)
{
    constexpr int segments = 64;
    const float height = b.y - a.y;
    const float phase = sweeping ? static_cast<float>(ImGui::GetTime()) * 1.4f : 0.0f;
    // Dark, muscular limb with pale suckers: it has to read through the water fill.
    const ImU32 skin = IM_COL32(28, 62, 70, 215);
    const ImU32 sucker = IM_COL32(60, 150, 150, 230);
    const ImU32 rim = IM_COL32(240, 250, 248, 200);
    const ImU32 edge = IM_COL32(6, 20, 26, 230);
    const auto centre = [&](float t)
    {
        const float sway = std::sin(t * 7.0f - phase) * (0.06f + 0.22f * t); // the tip swings most
        const float curl = -0.55f * t * t * t * t;                           // the tip curls up
        return ImVec2(a.x + (b.x - a.x) * t, a.y + height * (0.62f + sway + curl));
    };
    const auto half = [&](float t) { return height * 0.42f * std::pow(1.0f - t, 0.6f) + 1.0f; };
    dl->PushClipRect(a, b, true);
    for (int i = 0; i <= segments; ++i)
    {
        const float t = static_cast<float>(i) / segments;
        dl->PathLineTo({centre(t).x, centre(t).y - half(t)});
    }
    for (int i = segments; i >= 0; --i)
    {
        const float t = static_cast<float>(i) / segments;
        dl->PathLineTo({centre(t).x, centre(t).y + half(t)});
    }
    dl->PathFillConcave(skin);
    for (const float side : {-1.0f, 1.0f}) // the two edges, darker: the limb reads as a body
    {
        for (int i = 0; i <= segments; ++i)
        {
            const float t = static_cast<float>(i) / segments;
            dl->PathLineTo({centre(t).x, centre(t).y + side * half(t)});
        }
        dl->PathStroke(edge, 0, 2.5f);
    }
    for (int i = 2; i < segments * 9 / 10; i += 3) // suckers in two staggered rows along the underside
    {
        const float t = static_cast<float>(i) / segments;
        const float row = (i / 3) % 2 == 0 ? 0.55f : 0.15f;
        const ImVec2 c{centre(t).x, centre(t).y + half(t) * row};
        const float r = half(t) * (row > 0.5f ? 0.42f : 0.30f);
        dl->AddCircleFilled(c, r, sucker);
        dl->AddCircle(c, r, rim, 0, std::max(1.5f, r * 0.35f));
        dl->AddCircleFilled(c, r * 0.35f, edge); // the cup
    }
    dl->PopClipRect();
}

} // namespace

void dial_face(ImDrawList* dl, ImVec2 c, float r)
{
    // Deep water: darker toward the rim.
    dl->AddCircleFilled(c, r, IM_COL32(0x03, 0x0c, 0x10, 255), 48);
    dl->AddCircleFilled(c, r * 0.8f, IM_COL32(0x05, 0x15, 0x1a, 255), 48);
    dl->AddCircleFilled(c, r * 0.55f, IM_COL32(0x08, 0x20, 0x27, 255), 48);
}

void dial_ticks(ImDrawList* dl, ImVec2 c, float r, float outer)
{
    for (int i = 0; i <= 10; ++i)
    {
        const float a = dial_start + dial_span * static_cast<float>(i) / 10.0f;
        const float inner = outer - (i % 5 == 0 ? 0.23f : 0.13f);
        dl->AddLine({c.x + std::cos(a) * r * inner, c.y + std::sin(a) * r * inner},
                    {c.x + std::cos(a) * r * outer, c.y + std::sin(a) * r * outer},
                    IM_COL32(0xe6, 0xf4, 0xf1, 0xb0), 1.5f); // the face is dark in both themes
    }
}

void dial_needle(ImDrawList* dl, ImVec2 c, float r, float angle, ImU32 col)
{
    const ImVec2 dir{std::cos(angle), std::sin(angle)};
    const float hw = std::max(1.5f, r * 0.035f);
    dl->AddTriangleFilled({c.x - dir.y * hw, c.y + dir.x * hw}, {c.x + dir.y * hw, c.y - dir.x * hw},
                          {c.x + dir.x * r, c.y + dir.y * r}, col);
    dl->AddLine(c, {c.x - dir.x * r * 0.18f, c.y - dir.y * r * 0.18f}, col, hw * 2.0f); // counterweight
}

float depth_gauge_min_height()
{
    return ImGui::GetFontSize() * 2.8f;
}

void draw_depth_gauge(float fill_from, float fill_to, ImVec2 size, bool sweeping, float* mark_a, float* mark_b)
{
    fill_from = std::clamp(fill_from, 0.0f, 1.0f);
    fill_to = std::clamp(fill_to, fill_from, 1.0f);
    if (size.x <= 0.0f)
    {
        size.x = std::max(ImGui::GetContentRegionAvail().x + size.x, 4.0f);
    }
    size.y = std::max(size.y, depth_gauge_min_height());
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1{p0.x + size.x, p0.y + size.y};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##strip", size); // the item: tooltips, and the marker drag below

    // Markers: on press the nearest one within a thumb's width takes the drag until release.
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID drag_id = ImGui::GetID("##strip_drag");
    if (mark_a != nullptr && mark_b != nullptr)
    {
        const float mouse = (ImGui::GetIO().MousePos.x - p0.x) / size.x;
        if (ImGui::IsItemActivated())
        {
            const float da = std::abs(mouse - *mark_a) * size.x;
            const float db = std::abs(mouse - *mark_b) * size.x;
            const float reach = ImGui::GetFontSize();
            store->SetInt(drag_id, std::min(da, db) > reach ? 0 : da <= db ? 1 : 2);
        }
        const int which = ImGui::IsItemActive() ? store->GetInt(drag_id, 0) : 0;
        if (which == 1)
        {
            *mark_a = std::clamp(mouse, 0.0f, *mark_b);
        }
        else if (which == 2)
        {
            *mark_b = std::clamp(mouse, *mark_a, 1.0f);
        }
    }

    // The sea: dusk at the surface fading to the deep, the played part lit from the left in the
    // theme's bioluminescent teal, bubbles rising while something moves, the tentacle through it.
    const ImU32 surface = IM_COL32(16, 64, 74, 255);
    const ImU32 deep = IM_COL32(4, 14, 20, 255);
    dl->AddRectFilledMultiColor(p0, p1, surface, surface, deep, deep);
    if (fill_to > fill_from)
    {
        const ImVec2 f0{p0.x + size.x * fill_from, p0.y};
        const ImVec2 f1{p0.x + size.x * fill_to, p1.y};
        dl->AddRectFilledMultiColor(f0, f1, water(fill_from), water(fill_to), water(fill_to), water(fill_from));
        dl->AddRectFilledMultiColor(f0, f1, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), deep & 0x00FFFFFF | 0x80000000u,
                                    deep & 0x00FFFFFF | 0x80000000u); // darker towards the bottom, as water is
    }
    draw_tentacle(dl, p0, p1, sweeping);
    const double now = sweeping ? ImGui::GetTime() : 0.0;
    for (int i = 0; i < 14; ++i) // bubbles: fixed columns, rising on a loop
    {
        const float u = static_cast<float>((i * 37 + 11) % 100) / 100.0f;
        const float rise = static_cast<float>(std::fmod(now * (0.08 + 0.04 * (i % 3)) + i * 0.17, 1.0));
        const float r = 1.0f + static_cast<float>(i % 3);
        const ImVec2 c{p0.x + size.x * u, p1.y - size.y * rise};
        dl->AddCircle(c, r, IM_COL32(160, 230, 225, 70), 8, 1.0f);
    }
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), 3.0f);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    if (mark_a != nullptr && mark_b != nullptr)
    {
        const float h = size.y * 0.32f; // brass marker buoys, as the dials' bezels
        for (const auto [m, label] : {std::pair{*mark_a, "A"}, std::pair{*mark_b, "B"}})
        {
            const float x = p0.x + size.x * m;
            dl->AddLine({x, p0.y}, {x, p1.y}, dial_brass, 1.5f);
            dl->AddTriangleFilled({x - h * 0.6f, p0.y}, {x + h * 0.6f, p0.y}, {x, p0.y + h}, dial_brass);
            const float lx = std::min(x + 4.0f, p1.x - ImGui::GetFontSize() - 2.0f); // B at the end stays inside
            dl->AddText({lx, p1.y - ImGui::GetFontSize() - 2.0f}, dial_brass, label);
        }
    }
    if (fill_to > fill_from) // the percent only while something plays or loads
    {
        const std::string pct = std::format("{:.0f} %", fill_to * 100.0f);
        const ImVec2 tw = ImGui::CalcTextSize(pct.c_str());
        dl->AddText({p1.x - tw.x - ImGui::GetFontSize() * 1.2f, p0.y + ImGui::GetFontSize() * 0.3f}, text, pct.c_str());
    }
    ImGui::Spacing();
}
