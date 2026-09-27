#pragma once

#include <vector>

struct GLFWwindow;
struct ImFont;

struct ThemeFonts
{
    ImFont* ui = nullptr;   // Noto Sans, the default font
    ImFont* mono = nullptr; // Noto Sans Mono, for hex/data columns
};

// U+1F991 squid: both fonts draw it from the kraken logo (no emoji font), in the text colour.
inline constexpr unsigned theme_squid_codepoint = 0x1F991;

// Adds the embedded Noto Sans (default, with Noto Sans Mono merged in for the symbols it lacks,
// e.g. ≈) and Noto Sans Mono to the ImGui font atlas.
// Call once after ImGui::CreateContext(), before the first frame.
ThemeFonts theme_load_fonts(float size_px);

// Dark: "abyss" deep-sea palette. Light: "shallow water" sand/sea. Both share the
// bioluminescent teal accent, also used as the first ImPlot colormap colour (when an ImPlot
// context exists). Shared metrics: 5 px rounding, 6 px dock separators.
void theme_apply(bool dark);

// Colours (0xRRGGBB) of a control-bar button that stands out from the theme's normal buttons.
struct ThemeButton
{
    unsigned fill, hover, active, border, text;
};

// "Release the Kraken" (petrol fill, brass rim) and "Stop" (coral outline) of the theme last
// passed to theme_apply(); the dark set before the first call.
[[nodiscard]] ThemeButton theme_kraken_button() noexcept;
[[nodiscard]] ThemeButton theme_stop_button() noexcept;

// Button, hovered, active, border and text colours of c for the next ImGui buttons; pop undoes it.
void theme_push_button(const ThemeButton& c);
void theme_pop_button();

// 0xRRGGBB -> opaque ImU32 (IM_COL32 byte order).
[[nodiscard]] constexpr unsigned theme_u32(unsigned rgb) noexcept
{
    return 0xff000000u | ((rgb & 0xffu) << 16) | (rgb & 0xff00u) | ((rgb >> 16) & 0xffu);
}

// Status text: error coral, warning brass, recording = the Stop button's coral.
enum class ThemeText
{
    error,
    warn,
    rec,
};

// Status text colour (ImU32) of the theme last passed to theme_apply(), readable on WindowBg.
[[nodiscard]] unsigned theme_text(ThemeText t) noexcept;

// Categorical colour i (ImU32, cycles through 8) of the theme's teal-first graph colormap.
[[nodiscard]] unsigned theme_signal_color(unsigned i) noexcept;
// A colour of either theme's signal palette -> the same slot in the current theme; others unchanged.
[[nodiscard]] unsigned theme_signal_color_remap(unsigned color) noexcept;

// The Kraken app icon (assets/kraken.svg) rasterised to size x size RGBA8. False if the SVG
// fails to parse.
bool theme_app_icon(std::vector<unsigned char>& rgba, int size = 64);

// The kraken logo as a GL texture (size px square) and as the window icon (X11/XWayland; Wayland
// ignores it, the .desktop file applies instead); needs a current GL context. theme_logo() draws
// it as an ImGui item, or an empty item of the same size before init (headless tests).
void theme_logo_init(GLFWwindow* window, int size);
void theme_logo(float size);

// Desktop color-scheme preference from xdg-desktop-portal. False (light) when it cannot
// be determined; Settings can override (T24).
bool theme_os_prefers_dark();
