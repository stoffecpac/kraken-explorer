#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <imgui.h>

#include "ui_test.h"

// Expected colors are the Kraken palette hex values (abyss / shallow water).
static bool is_rgb(const ImVec4& c, unsigned hex)
{
    return ImGui::ColorConvertFloat4ToU32(c) == ImGui::ColorConvertFloat4ToU32(
               {((hex >> 16) & 0xff) / 255.0f, ((hex >> 8) & 0xff) / 255.0f, (hex & 0xff) / 255.0f, 1.0f});
}

TEST_CASE("themes and embedded fonts")
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    const ThemeFonts fonts = theme_load_fonts(15.0f);
    REQUIRE(fonts.ui != nullptr);
    REQUIRE(fonts.mono != nullptr);
    CHECK(io.Fonts->Fonts.Size == 2);
    // Glyphs are rasterised on demand in 1.92: a frame proves the compressed data decodes.
    io.DisplaySize = {800, 600};
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::NewFrame();
    CHECK(ImGui::CalcTextSize("0x7FF").x > 0.0f);
    ImGui::PushFont(fonts.mono, 0.0f);
    CHECK(ImGui::CalcTextSize("i").x == doctest::Approx(ImGui::CalcTextSize("W").x));
    ImGui::PopFont();
    ImGui::EndFrame();

    const ImVec4* c = ImGui::GetStyle().Colors;
    theme_apply(true);
    CHECK(is_rgb(c[ImGuiCol_WindowBg], 0x0a1f26));
    CHECK(is_rgb(c[ImGuiCol_Text], 0xd8f3ef));
    CHECK(is_rgb(c[ImGuiCol_TableRowBg], 0x061418));
    CHECK(is_rgb(c[ImGuiCol_TabSelectedOverline], 0x19d3c5));
    CHECK(is_rgb(c[ImGuiCol_CheckMark], 0x19d3c5));

    theme_apply(false);
    CHECK(is_rgb(c[ImGuiCol_WindowBg], 0xf3eee2));
    CHECK(is_rgb(c[ImGuiCol_Text], 0x0b2a30));
    CHECK(is_rgb(c[ImGuiCol_TabSelectedOverline], 0x0fa3b1));
    CHECK(ImGui::GetStyle().FrameRounding == 4.0f);
    CHECK(ImGui::GetStyle().DockingSeparatorSize == 4.0f);
    ImGui::DestroyContext();
}

TEST_CASE("kraken app icon rasterises teal on transparent")
{
    std::vector<unsigned char> rgba;
    REQUIRE(theme_app_icon(rgba));
    REQUIRE(rgba.size() == 64u * 64u * 4u);
    CHECK(rgba[3] == 0); // top-left corner is transparent
    const unsigned char* head = &rgba[(12 * 64 + 32) * 4]; // top of the mantle, above the spots and shading: #19D3C5
    CHECK(head[0] == 0x19);
    CHECK(head[1] == 0xd3);
    CHECK(head[2] == 0xc5);
    CHECK(head[3] == 255);
}

// WCAG 2 relative luminance of an ImU32 colour, and the contrast ratio of two colours.
static double luminance(unsigned u32)
{
    const ImVec4 c = ImGui::ColorConvertU32ToFloat4(u32);
    const auto lin = [](float v) { return v <= 0.03928f ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.x) + 0.7152 * lin(c.y) + 0.0722 * lin(c.z);
}

static double contrast(unsigned a, unsigned b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

TEST_CASE("status text colours follow the theme and stay readable")
{
    const UiTest ui;
    const ThemeText all[] = {ThemeText::error, ThemeText::warn, ThemeText::rec};
    unsigned dark[3];
    theme_apply(true);
    const unsigned dark_bg = ImGui::GetColorU32(ImGuiCol_WindowBg);
    for (int i = 0; i < 3; ++i)
    {
        dark[i] = theme_text(all[i]);
        CAPTURE(i);
        CHECK(contrast(dark[i], dark_bg) >= 3.0);
    }
    CHECK(theme_text(ThemeText::rec) == theme_u32(theme_stop_button().text));
    CHECK(theme_signal_color(0) == theme_u32(0x19d3c5)); // teal first
    CHECK(theme_signal_color(8) == theme_signal_color(0));

    theme_apply(false);
    const unsigned light_bg = ImGui::GetColorU32(ImGuiCol_WindowBg);
    for (int i = 0; i < 3; ++i)
    {
        CAPTURE(i);
        CHECK(theme_text(all[i]) != dark[i]);
        CHECK(contrast(theme_text(all[i]), light_bg) >= 3.0);
    }
    CHECK(theme_signal_color(0) == theme_u32(0x0fa3b1));
    CHECK(contrast(theme_u32(0x000000), theme_u32(0xffffff)) == doctest::Approx(21.0));
}
