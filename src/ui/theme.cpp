#include "ui/theme.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <implot.h>
#include <nanosvg.h>
#include <nanosvgrast.h>

#include "NotoSans.h"
#include "NotoSansMono.h"
#include "kraken_svg.inc"

// 0xRRGGBB -> ImVec4 with alpha
static ImVec4 rgb(unsigned hex, float alpha = 1.0f)
{
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(theme_u32(hex));
    c.w = alpha;
    return c;
}

ThemeFonts theme_load_fonts(float size_px)
{
    ImGuiIO& io = ImGui::GetIO();
    return {
        .ui = io.Fonts->AddFontFromMemoryCompressedTTF(NotoSans_compressed_data, NotoSans_compressed_size, size_px),
        .mono = io.Fonts->AddFontFromMemoryCompressedTTF(NotoSansMono_compressed_data, NotoSansMono_compressed_size, size_px),
    };
}

// Both palettes fill every slot through one table so light/dark cannot drift apart.
struct Palette
{
    unsigned text, text_dim, bg, deep, alt, popup, frame, frame_hov, frame_act, title, title_act;
    unsigned button, border, grid, accent, accent_hi, select, select_hov;
    ThemeButton kraken_button, stop_button;
    unsigned text_error, text_warn;
};

// "Abyss": deep blue-teal water, bioluminescent teal accent, sea-foam text.
static constexpr Palette abyss = {
    .text = 0xd8f3ef, .text_dim = 0x5e8a8f, .bg = 0x0a1f26, .deep = 0x061418, .alt = 0x0c262e,
    .popup = 0x08191e, .frame = 0x10303a, .frame_hov = 0x16414d, .frame_act = 0x1b5260,
    .title = 0x08191e, .title_act = 0x0e2e36, .button = 0x123843, .border = 0x1e4a55,
    .grid = 0x143640, .accent = 0x19d3c5, .accent_hi = 0x5ff0e4, .select = 0x0f6b73,
    .select_hov = 0x14434e,
    .kraken_button = {.fill = 0x0f6b73, .hover = 0x138690, .active = 0x0a4f56, .border = 0xe0a84a, .text = 0xf2fffd},
    .stop_button = {.fill = 0x123843, .hover = 0x3a3434, .active = 0x5a3a30, .border = 0xff7a59, .text = 0xff9a7f},
    .text_error = 0xff7a59, .text_warn = 0xe0a84a,
};

// "Shallow water": pale sand and sea glass, same teal (darkened a step so it reads on white).
static constexpr Palette shallows = {
    .text = 0x0b2a30, .text_dim = 0x7a8f8c, .bg = 0xf3eee2, .deep = 0xfdfbf6, .alt = 0xeef6f3,
    .popup = 0xfdfbf6, .frame = 0xfdfbf6, .frame_hov = 0xdcf2ee, .frame_act = 0xbfe8e3,
    .title = 0xe6dfcf, .title_act = 0xd6e9e4, .button = 0xe9f1ec, .border = 0xb3c6c1,
    .grid = 0xd9e3df, .accent = 0x0fa3b1, .accent_hi = 0x0b7f8a, .select = 0x19d3c5,
    .select_hov = 0x19d3c5,
    .kraken_button = {.fill = 0x0b6f79, .hover = 0x0e8591, .active = 0x085760, .border = 0xc08a2e, .text = 0xffffff},
    .stop_button = {.fill = 0xfdf3ee, .hover = 0xfbe0d6, .active = 0xf6c4b3, .border = 0xd4502f, .text = 0xb8401f},
    .text_error = 0xc0392b, .text_warn = 0x8a5a00,
};

static void apply_palette(ImVec4* c, bool dark, const Palette& p)
{
    c[ImGuiCol_Text] = rgb(p.text);
    c[ImGuiCol_TextDisabled] = rgb(p.text_dim);
    c[ImGuiCol_WindowBg] = rgb(p.bg);
    c[ImGuiCol_ChildBg] = rgb(p.bg, 0.0f);
    c[ImGuiCol_PopupBg] = rgb(p.popup);
    c[ImGuiCol_Border] = rgb(p.border);
    c[ImGuiCol_BorderShadow] = rgb(0, 0.0f);
    c[ImGuiCol_FrameBg] = rgb(p.frame);
    c[ImGuiCol_FrameBgHovered] = rgb(p.frame_hov);
    c[ImGuiCol_FrameBgActive] = rgb(p.frame_act);
    c[ImGuiCol_TitleBg] = rgb(p.title);
    c[ImGuiCol_TitleBgActive] = rgb(p.title_act);
    c[ImGuiCol_TitleBgCollapsed] = rgb(p.title);
    c[ImGuiCol_MenuBarBg] = rgb(p.title);
    c[ImGuiCol_ScrollbarBg] = rgb(p.bg);
    c[ImGuiCol_ScrollbarGrab] = rgb(p.border);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(p.accent, 0.6f);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(p.accent);
    c[ImGuiCol_CheckMark] = rgb(p.accent);
    c[ImGuiCol_SliderGrab] = rgb(p.accent);
    c[ImGuiCol_SliderGrabActive] = rgb(p.accent_hi);
    c[ImGuiCol_Button] = rgb(p.button);
    c[ImGuiCol_ButtonHovered] = rgb(p.frame_hov);
    c[ImGuiCol_ButtonActive] = rgb(p.accent, 0.7f);
    // Selection stays translucent in light mode so dark text on it keeps its contrast.
    c[ImGuiCol_Header] = rgb(p.select, dark ? 1.0f : 0.45f);
    c[ImGuiCol_HeaderHovered] = rgb(p.select_hov, dark ? 1.0f : 0.25f);
    c[ImGuiCol_HeaderActive] = rgb(p.accent, dark ? 0.8f : 0.6f);
    c[ImGuiCol_Separator] = rgb(p.border);
    c[ImGuiCol_SeparatorHovered] = rgb(p.accent);
    c[ImGuiCol_SeparatorActive] = rgb(p.accent_hi);
    c[ImGuiCol_ResizeGrip] = rgb(p.border, 0.4f);
    c[ImGuiCol_ResizeGripHovered] = rgb(p.accent, 0.6f);
    c[ImGuiCol_ResizeGripActive] = rgb(p.accent);
    c[ImGuiCol_InputTextCursor] = rgb(p.accent_hi);
    c[ImGuiCol_Tab] = rgb(p.title);
    c[ImGuiCol_TabHovered] = rgb(p.frame_hov);
    c[ImGuiCol_TabSelected] = rgb(p.deep);
    c[ImGuiCol_TabSelectedOverline] = rgb(p.accent);
    c[ImGuiCol_TabDimmed] = rgb(p.title);
    c[ImGuiCol_TabDimmedSelected] = rgb(p.alt);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(p.accent, 0.5f);
    c[ImGuiCol_DockingPreview] = rgb(p.accent, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = rgb(p.bg);
    c[ImGuiCol_PlotLines] = rgb(p.accent);
    c[ImGuiCol_PlotLinesHovered] = rgb(p.accent_hi);
    c[ImGuiCol_PlotHistogram] = rgb(p.accent);
    c[ImGuiCol_PlotHistogramHovered] = rgb(p.accent_hi);
    c[ImGuiCol_TableHeaderBg] = rgb(p.title_act);
    c[ImGuiCol_TableBorderStrong] = rgb(p.border);
    c[ImGuiCol_TableBorderLight] = rgb(p.grid);
    c[ImGuiCol_TableRowBg] = rgb(p.deep);
    c[ImGuiCol_TableRowBgAlt] = rgb(p.alt);
    c[ImGuiCol_TextLink] = rgb(p.accent);
    c[ImGuiCol_TextSelectedBg] = rgb(p.accent, 0.35f);
    c[ImGuiCol_TreeLines] = rgb(p.border);
    c[ImGuiCol_DragDropTarget] = rgb(p.accent);
    c[ImGuiCol_DragDropTargetBg] = rgb(0, 0.0f);
    c[ImGuiCol_UnsavedMarker] = rgb(p.text);
    c[ImGuiCol_NavCursor] = rgb(p.accent);
    c[ImGuiCol_NavWindowingHighlight] = rgb(p.accent, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = rgb(0x000000, dark ? 0.3f : 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(p.deep, dark ? 0.55f : 0.25f);
}

// Teal-first qualitative colormaps for graphs: teal, coral, sand gold, sky, violet, kelp, pink, ice.
static constexpr ImU32 abyss_map[] = {
    IM_COL32(0x19, 0xd3, 0xc5, 255), IM_COL32(0xff, 0x7a, 0x59, 255), IM_COL32(0xf6, 0xc8, 0x5f, 255),
    IM_COL32(0x7f, 0xb3, 0xff, 255), IM_COL32(0xc3, 0x8b, 0xff, 255), IM_COL32(0x9b, 0xe5, 0x64, 255),
    IM_COL32(0xff, 0x5c, 0x8a, 255), IM_COL32(0xb8, 0xf2, 0xff, 255),
};
static constexpr ImU32 shallows_map[] = {
    IM_COL32(0x0f, 0xa3, 0xb1, 255), IM_COL32(0xe0, 0x5a, 0x38, 255), IM_COL32(0xc0, 0x8a, 0x10, 255),
    IM_COL32(0x2f, 0x6f, 0xd6, 255), IM_COL32(0x8a, 0x4f, 0xd8, 255), IM_COL32(0x4c, 0x9a, 0x2a, 255),
    IM_COL32(0xd6, 0x33, 0x6c, 255), IM_COL32(0x0b, 0x5f, 0x6a, 255),
};

static void apply_plot_colormap(bool dark)
{
    if (!ImPlot::GetCurrentContext()) // headless UI tests without ImPlot
    {
        return;
    }
    const char* name = dark ? "Kraken Abyss" : "Kraken Shallows";
    ImPlotColormap map = ImPlot::GetColormapIndex(name);
    if (map == -1)
    {
        map = dark ? ImPlot::AddColormap(name, abyss_map, IM_ARRAYSIZE(abyss_map))
                   : ImPlot::AddColormap(name, shallows_map, IM_ARRAYSIZE(shallows_map));
    }
    ImPlot::GetStyle().Colormap = map;
    // Plot area a shade deeper than the window, grid in the palette's grid colour; the rest
    // (axis text, legend, frame) stays ImPlot's auto colours derived from the ImGui style.
    const Palette& p = dark ? abyss : shallows;
    ImPlot::GetStyle().Colors[ImPlotCol_PlotBg] = rgb(p.deep);
    ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid] = rgb(p.border, 0.8f);
}

static const Palette* g_palette = &abyss;

ThemeButton theme_kraken_button() noexcept
{
    return g_palette->kraken_button;
}

ThemeButton theme_stop_button() noexcept
{
    return g_palette->stop_button;
}

void theme_push_button(const ThemeButton& c)
{
    ImGui::PushStyleColor(ImGuiCol_Button, theme_u32(c.fill));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme_u32(c.hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme_u32(c.active));
    ImGui::PushStyleColor(ImGuiCol_Border, theme_u32(c.border));
    ImGui::PushStyleColor(ImGuiCol_Text, theme_u32(c.text));
}

void theme_pop_button()
{
    ImGui::PopStyleColor(5);
}

unsigned theme_text(ThemeText t) noexcept
{
    const Palette& p = *g_palette;
    switch (t)
    {
    case ThemeText::error:
        return theme_u32(p.text_error);
    case ThemeText::warn:
        return theme_u32(p.text_warn);
    case ThemeText::rec:
        break;
    }
    return theme_u32(p.stop_button.text);
}

unsigned theme_signal_color(unsigned i) noexcept
{
    return (g_palette == &abyss ? abyss_map : shallows_map)[i % 8];
}

void theme_apply(bool dark)
{
    g_palette = dark ? &abyss : &shallows;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.ChildRounding = 5.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;
    // Flat: no frame around buttons and fields, a little more air (T79h, "looked like Qt").
    style.FrameBorderSize = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.TabBarBorderSize = 0.0f;
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.DockingSeparatorSize = 4.0f;
    style.ScrollbarSize = 10.0f;
    apply_palette(style.Colors, dark, dark ? abyss : shallows);
    apply_plot_colormap(dark);
}

bool theme_app_icon(std::vector<unsigned char>& rgba, int size)
{
    std::string text = kraken_svg; // nsvgParse writes into its input
    NSVGimage* img = nsvgParse(text.data(), "px", 96.0f);
    if (!img || img->width <= 0.0f)
    {
        nsvgDelete(img);
        return false;
    }
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    rgba.assign(static_cast<size_t>(size) * size * 4, 0);
    nsvgRasterize(rast, img, 0, 0, static_cast<float>(size) / img->width, rgba.data(), size, size, size * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    return true;
}

static GLuint g_logo = 0;

void theme_logo_init(GLFWwindow* window, int size)
{
    std::vector<unsigned char> rgba;
    if (!theme_app_icon(rgba, size))
    {
        return;
    }
    const GLFWimage img{size, size, rgba.data()};
    glfwSetWindowIcon(window, 1, &img);
    glGenTextures(1, &g_logo);
    glBindTexture(GL_TEXTURE_2D, g_logo);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void theme_logo(float size)
{
    if (g_logo == 0) // headless tests: keep the layout, draw nothing
    {
        ImGui::Dummy(ImVec2(size, size));
        return;
    }
    ImGui::Image(static_cast<ImTextureID>(g_logo), ImVec2(size, size));
}

bool theme_os_prefers_dark()
{
    // org.freedesktop.appearance color-scheme: 0 no preference, 1 dark, 2 light.
    // ponytail: gdbus subprocess instead of linking libdbus; one call at startup, no live updates. Portal signal via libdbus if live switching is wanted.
    FILE* p = popen("gdbus call --session --timeout 1 --dest org.freedesktop.portal.Desktop"
                    " --object-path /org/freedesktop/portal/desktop"
                    " --method org.freedesktop.portal.Settings.ReadOne"
                    " org.freedesktop.appearance color-scheme 2>/dev/null",
                    "r");
    if (!p)
    {
        return false;
    }
    char out[128] = {};
    const size_t n = std::fread(out, 1, sizeof(out) - 1, p);
    pclose(p);
    const char* v = std::strstr(out, "uint32 ");
    return n > 0 && v && v[7] == '1';
}
