#include "ui/depth_gauge.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>

#include "core/stats.h"

namespace
{

struct Landmark
{
    float depth;
    const char* label;
};

// Labelled on the sounder strip; the zone edges are ticks only (the last one is the strip's
// right border, under the Challenger Deep landmark line).
constexpr Landmark landmarks[] = {{3800.0f, "Titanic"}, {6000.0f, "Hadal"}, {challenger_deep_m, "Challenger Deep"}};
constexpr float zone_edges[] = {200.0f, 1000.0f, 4000.0f, 6000.0f, challenger_deep_m};

// Sunlit teal -> hadal navy, blended by depth fraction.
ImU32 water(float t)
{
    const ImVec4 top{0.10f, 0.83f, 0.77f, 1.0f}; // #19D3C5
    const ImVec4 deep{0.02f, 0.05f, 0.16f, 1.0f};
    return ImGui::ColorConvertFloat4ToU32(
        {top.x + (deep.x - top.x) * t, top.y + (deep.y - top.y) * t, top.z + (deep.z - top.z) * t, 1.0f});
}

// 4213 -> "4 213"
std::string group_thousands(int v)
{
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
    {
        s.insert(static_cast<std::size_t>(i), " ");
    }
    return s;
}

// Round echo-sounder dial: brass bezel, ping rings, sweep (rotating while sweeping), needle on a
// 270 degree scale.
void draw_dial(ImDrawList* dl, ImVec2 c, float r, float fraction, bool sweeping)
{
    constexpr float pi = std::numbers::pi_v<float>;
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    dial_face(dl, c, r);
    for (int i = 1; i <= 3; ++i)
    {
        dl->AddCircle(c, r * static_cast<float>(i) / 4.0f, ImGui::GetColorU32(ImGuiCol_CheckMark, 0.25f), 48);
    }
    // Sweep: a fading wedge trailing the beam; parked at 12 o'clock when idle, so an idle frame
    // looks like the last one and nothing needs a redraw.
    const float beam = sweeping ? static_cast<float>(std::fmod(ImGui::GetTime() * 1.5, 2.0 * std::numbers::pi))
                                : -0.5f * pi;
    for (int i = 0; i < 12; ++i)
    {
        const float a0 = beam - static_cast<float>(i + 1) * 0.07f;
        dl->PathLineTo(c);
        dl->PathArcTo(c, r - 1.0f, a0, a0 + 0.07f, 3);
        dl->PathFillConvex(ImGui::GetColorU32(ImGuiCol_CheckMark, 0.30f * (1.0f - static_cast<float>(i) / 12.0f)));
    }
    dial_ticks(dl, c, r, 0.95f); // scale 0..100 %
    const float a = dial_start + dial_span * fraction;
    dl->AddLine(c, {c.x + std::cos(a) * r * 0.85f, c.y + std::sin(a) * r * 0.85f}, accent, 3.0f);
    dl->AddCircleFilled(c, r * 0.08f, dial_brass);
    dial_bezel(dl, c, r, std::max(3.0f, r * 0.09f));
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

void dial_bezel(ImDrawList* dl, ImVec2 c, float r, float thickness)
{
    // Porthole: brass ring with a shadowed inner edge and eight bolts.
    constexpr ImU32 shadow = IM_COL32(0x7a, 0x56, 0x1e, 255);
    dl->AddCircle(c, r, dial_brass, 48, thickness);
    dl->AddCircle(c, r - thickness * 0.5f, shadow, 48, std::max(1.0f, thickness * 0.25f));
    if (thickness >= 4.0f) // bolts are noise on a small dial
    {
        for (int i = 0; i < 8; ++i)
        {
            const float a = (static_cast<float>(i) + 0.5f) * std::numbers::pi_v<float> / 4.0f;
            dl->AddCircleFilled({c.x + std::cos(a) * r, c.y + std::sin(a) * r}, thickness * 0.28f, shadow, 8);
        }
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

float depth_m(float fraction)
{
    return std::clamp(fraction, 0.0f, 1.0f) * challenger_deep_m;
}

const char* depth_zone(float depth)
{
    if (depth < 200.0f)
    {
        return "Epipelagic";
    }
    if (depth < 1000.0f)
    {
        return "Mesopelagic";
    }
    if (depth < 4000.0f)
    {
        return "Bathypelagic";
    }
    if (depth < 6000.0f)
    {
        return "Abyssopelagic";
    }
    return "Hadal";
}

float depth_gauge_min_height()
{
    return ImGui::GetFontSize() * 6.4f;
}

void draw_depth_gauge(float fraction, ImVec2 size, const Stats* gaps, bool sweeping)
{
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    const float fs = ImGui::GetFontSize();
    if (size.x <= 0.0f)
    {
        size.x = std::max(ImGui::GetContentRegionAvail().x + size.x, 4.0f);
    }
    size.y = std::max(size.y, depth_gauge_min_height()); // legible text needs the room
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    const float r = size.y * 0.5f - 2.0f;
    draw_dial(dl, {p0.x + r + 2.0f, p0.y + size.y * 0.5f}, r, fraction, sweeping);

    // Readout: big depth, then zone / fathoms / percent at normal size.
    const float depth = depth_m(fraction);
    const float x = p0.x + 2.0f * r + fs;
    const float right = p0.x + size.x;
    const std::string big = std::format("{} m", group_thousands(static_cast<int>(depth + 0.5f)));
    const float big_fs = fs * 2.2f;
    ImFont* font = ImGui::GetFont();
    dl->AddText(font, big_fs, {x, p0.y}, text, big.c_str());
    const std::string pct = std::format("{:.0f} %", fraction * 100.0f);
    const float pct_w = font->CalcTextSizeA(big_fs, FLT_MAX, 0.0f, pct.c_str()).x;
    if (right - pct_w > x + font->CalcTextSizeA(big_fs, FLT_MAX, 0.0f, big.c_str()).x + fs)
    {
        dl->AddText(font, big_fs, {right - pct_w, p0.y}, ImGui::GetColorU32(ImGuiCol_CheckMark), pct.c_str());
    }
    const std::string sub = std::format("{}  ·  {} fathoms", depth_zone(depth),
                                        group_thousands(static_cast<int>(depth / 1.8288f + 0.5f)));
    dl->AddText({x, p0.y + big_fs + 1.0f}, dim, sub.c_str());

    // Sounder strip: water gradient down to the current depth, landmark ticks, diver marker.
    const float sy0 = p0.y + big_fs + fs + 6.0f;
    const float sy1 = p0.y + size.y - fs - 2.0f;
    const float w = std::max(right - x, 4.0f);
    const float x_at = w / challenger_deep_m;
    const float fill_x = x + w * fraction;
    dl->AddRectFilled({x, sy0}, {right, sy1}, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
    float prev = 0.0f;
    for (const float edge : zone_edges)
    {
        const float a = x + prev * x_at;
        const float b = std::min(x + edge * x_at, fill_x);
        if (a < fill_x)
        {
            const ImU32 ca = water(prev / challenger_deep_m);
            const ImU32 cb = water(edge / challenger_deep_m);
            dl->AddRectFilledMultiColor({a, sy0}, {b, sy1}, ca, cb, cb, ca);
        }
        prev = edge;
    }
    dl->AddRect({x, sy0}, {right, sy1}, ImGui::GetColorU32(ImGuiCol_Border), 3.0f);
    for (const float edge : zone_edges)
    {
        const float tx = x + edge * x_at;
        dl->AddLine({tx, sy1 - (sy1 - sy0) * 0.4f}, {tx, sy1}, dim);
    }
    float label_end = x; // skip a landmark label that would overlap the previous one
    for (const Landmark& m : landmarks)
    {
        const float tx = x + m.depth * x_at;
        const std::string label = std::format("{} {} m", m.label, group_thousands(static_cast<int>(m.depth)));
        const float lw = ImGui::CalcTextSize(label.c_str()).x;
        const float lx = std::clamp(tx - lw * 0.5f, label_end, right - lw);
        dl->AddLine({tx, sy0}, {tx, sy1 + 3.0f}, dial_brass);
        if (lx >= label_end)
        {
            dl->AddText({lx, sy1 + 2.0f}, dim, label.c_str());
            label_end = lx + lw + fs * 0.5f;
        }
    }
    const float h = (sy1 - sy0) * 0.45f;
    dl->AddTriangleFilled({fill_x - h * 0.6f, sy0}, {fill_x + h * 0.6f, sy0}, {fill_x, sy0 + h}, text);

    ImGui::Dummy(size);

    if (gaps != nullptr)
    {
        const auto ms = [](double s) { return s * 1e3; };
        ImGui::Text("Frame gap  min %.3f  max %.3f  mean %.3f  median %.3f ms", ms(gaps->min), ms(gaps->max),
                    ms(gaps->mean), ms(gaps->median));
        if (gaps->mean > 0.0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%.0f frames/s)", 1.0 / gaps->mean);
        }
    }
}
