/*
  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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
#include "app.h"
#include "core/log.h"
#include "core/png.h"
#include "ui/icons.h"
#include "ui/settings.h"
#include "ui/theme.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <implot.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h> // SettingsDirtyTimer

namespace
{

// ponytail: fixed 60 fps cap on event-driven frames (RX, tasks, input); vsync still applies on top.
// Follow the monitor refresh rate instead if 120 Hz+ displays should get smoother input.
constexpr double min_frame_interval = 1.0 / 60.0;

// ponytail: RX-only frames (no user input for input_hold seconds, no replay animating) are capped at
// 30 fps, since with traffic every frame costs a lot. Make it a setting if 60 fps RX redraw is wanted.
constexpr double rx_frame_interval = 1.0 / 30.0;
constexpr double input_hold = 0.5;

// One glfwPostEmptyEvent per rendered frame: RX threads post only while no wake-up is pending.
// Cleared right before a frame drains the inboxes and tasks, so nothing delivered after it is missed.
std::atomic<bool> wake_pending{false};

void wake_main_loop()
{
    if (!wake_pending.exchange(true, std::memory_order_acq_rel))
    {
        glfwPostEmptyEvent();
    }
}

// How long the idle loop may sleep when no event arrives. A replay loading or playing animates the
// depth gauge sweep (~30 fps); a script or a pending ini save keeps the 4 Hz tick. So do the 2 s
// after any wakeup, for ImGui's hover/tooltip delays and window settling, and an active text field
// (cursor blink). A measurement needs no tick of its own: every RX batch wakes the loop, and the 2 s
// tail drains the recorder queue (250 ms) and lets CAN Status / frames/s / bus load fall to zero.
// ponytail: counters that change without a frame (sysfs link state, error counters when error frames
// are masked) refresh only on the next wakeup; add a slow (1 s) tick while measuring if that matters.
bool replay_animating(const App& app)
{
    return std::ranges::any_of(app.replays, [](const auto& kv)
                               { return kv.second.open && (kv.second.loader.joinable() || kv.second.running); });
}

double wait_timeout(const App& app, double since_wake)
{
    if (replay_animating(app))
    {
        return 1.0 / 30.0;
    }
    const bool busy = app.python.running || ImGui::GetCurrentContext()->SettingsDirtyTimer > 0.0f
                      || ImGui::GetIO().WantTextInput || since_wake < 2.0;
    return busy ? 0.25 : -1.0;
}

} // namespace

int main(int argc, char** argv)
{
    // --smoke N: render N frames, then exit (headless CI / sanitizer runs).
    // --measure: start a measurement on the default/loaded setup right away.
    // --record: arm recording (Ctrl+R) right away.
    // --setup: open the measurement setup dialog right away.
    // --replay FILE: open a Replay View with FILE, autoplay on (plays with --measure).
    // --gateway: open the CAN Gateway window.
    // --workspace FILE: load a .kraken workspace before anything starts.
    // --script FILE: load a Python script into the script window and run it; under --smoke
    //   its console is printed to stderr at exit.
    long smoke_frames = -1;
    bool setup = false;
    bool measure = false;
    bool record = false;
    const char* replay_file = nullptr;
    bool gateway = false;
    const char* workspace = nullptr;
    const char* script = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--smoke") == 0 && i + 1 < argc)
        {
            smoke_frames = std::strtol(argv[++i], nullptr, 10);
        }
        else if (std::strcmp(argv[i], "--workspace") == 0 && i + 1 < argc)
        {
            workspace = argv[++i];
        }
        else if (std::strcmp(argv[i], "--script") == 0 && i + 1 < argc)
        {
            script = argv[++i];
        }
        else if (std::strcmp(argv[i], "--measure") == 0)
        {
            measure = true;
        }
        else if (std::strcmp(argv[i], "--record") == 0)
        {
            record = true;
        }
        else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc)
        {
            replay_file = argv[++i];
        }
        else if (std::strcmp(argv[i], "--gateway") == 0)
        {
            gateway = true;
        }
        else if (std::strcmp(argv[i], "--setup") == 0)
        {
            setup = true;
        }
    }

    glfwSetErrorCallback([](int code, const char* desc) { std::fprintf(stderr, "GLFW error %d: %s\n", code, desc); });
    // GLFW cannot place windows on Wayland, so imgui_impl_glfw turns multi-viewports off there.
    // Prefer X11 (XWayland) whenever an X display exists so floating docks become OS windows.
    // ponytail: XWayland scales blurry on fractional HiDPI; drop this if that matters more.
    if (smoke_frames >= 0 || std::getenv("DISPLAY") != nullptr)
    {
        // Smoke runs are headless under xvfb-run, which leaves WAYLAND_DISPLAY set: without
        // this GLFW would open on the real Wayland desktop (and pull in libdecor's GTK plugin).
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    }
    if (!glfwInit())
    {
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1280, 800, "Kraken Explorer: Day of the N2K Tentacle " VERSION_STRING " — Deeper than a Peak. Wireshark is stuck in shallow waters.", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    // Floating docks become OS windows, except in a Wayland session: through XWayland the
    // compositor (sway) tiles them and reports window positions that put the mouse off by the
    // frame height, so clicks miss. ponytail: no setting for it; add one if someone wants
    // multi-viewport on Wayland with a floating-window rule in their compositor.
    if (std::getenv("WAYLAND_DISPLAY") == nullptr)
    {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    io.IniFilename = nullptr; // settings_init/settings_save handle the ini under $XDG_CONFIG_HOME
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    float dpi_scale = 1.0f;
    glfwGetWindowContentScale(window, &dpi_scale, nullptr);
    icons_init(std::max(dpi_scale, 2.0f)); // 2x cells: stay sharp at font scales up to 175 %
    theme_logo_init(window, 256); // also the About logo, drawn at ~10x font size

    App app;
    app.tasks.wake = wake_main_loop; // RX threads and posted tasks end glfwWaitEventsTimeout at once
    settings_init(app); // before the interfaces: CANblaster, recording folder, tabs + layout
    settings_apply_theme(app);
    app.fonts = theme_load_fonts(15.0f * dpi_scale); // ui/font_scale multiplies it via style.FontScaleMain;
    app_init_interfaces(app);
    if (workspace != nullptr)
    {
        workspace_load(app, workspace); // outside a frame, before the measurement starts
    }
    if (record)
    {
        recorder_set_armed(app.recorder, true, false);
    }
    if (replay_file != nullptr)
    {
        if (app.workspace.tabs.empty())
        {
            workspace_add_tab(app.workspace);
        }
        Replay& r = app.replays[workspace_current(app.workspace)->uid];
        r.open = true;
        r.autoplay = true;
        replay_load(app, r, replay_file);
    }
    app.gateway.open = gateway;
    if (measure)
    {
        app_measurement_start(app);
    }
    if (setup && !measure)
    {
        setup_dialog_open(app, app.setup_dialog);
    }
    if (script != nullptr && script_window_load_file(app.script, script))
    {
        app.script.was_measuring = app.measuring; // no AutoRun edge for the start above
        script_window_run(app, app.script);
    }
    const double loop_start = glfwGetTime();
    long frame = 0;
    double last_wake = loop_start;
    double last_frame = loop_start;
    double last_input = loop_start;
    ImVec2 last_display_size = io.DisplaySize;
    for (; !app.quit && !glfwWindowShouldClose(window); ++frame)
    {
        if (smoke_frames >= 0 && frame >= smoke_frames)
        {
            break;
        }
        if (smoke_frames >= 0)
        {
            glfwPollEvents();
        }
        else
        {
            // Idle: 0 % CPU; input, RX and posted tasks (glfwPostEmptyEvent) wake us immediately.
            const double t0 = glfwGetTime();
            const double timeout = wait_timeout(app, t0 - last_wake);
            timeout < 0.0 ? glfwWaitEvents() : glfwWaitEventsTimeout(timeout);
            if (const double t1 = glfwGetTime(); timeout < 0.0 || t1 - t0 < timeout * 0.9)
            {
                last_wake = t1; // woken early: by an event, not the timeout
            }
            // Coalesce everything arriving within one frame interval of the last frame into one frame.
            // Input queued by the backend's GLFW callbacks during the wait switches to the 60 fps cap.
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            const bool fast = glfwGetTime() - last_input < input_hold || replay_animating(app);
            for (double left = 0.0;; glfwWaitEventsTimeout(left))
            {
                const double interval =
                    fast || !g.InputEventsQueue.empty() ? min_frame_interval : rx_frame_interval;
                left = last_frame + interval - glfwGetTime();
                if (left <= 0.0)
                {
                    break;
                }
            }
            wake_pending.store(false, std::memory_order_release);
            last_frame = glfwGetTime();
        }

        app_before_frame(app);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        // Mouse, keys, wheel, focus (all ImGui input events of this frame) or a resize: user input.
        if (!ImGui::GetCurrentContext()->InputEventsTrail.empty() || io.DisplaySize.x != last_display_size.x
            || io.DisplaySize.y != last_display_size.y)
        {
            last_input = glfwGetTime();
            last_display_size = io.DisplaySize;
        }
        app_frame(app);
        ImGui::Render();

        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (app.screenshot)
        {
            // Framebuffer pixels of the requested rectangle: ImGui screen coords -> main viewport
            // -> framebuffer scale (HiDPI), y flipped for GL, rows flipped back for the PNG.
            int ww = 0;
            int wh = 0;
            glfwGetWindowSize(window, &ww, &wh);
            const ImVec2 vp = ImGui::GetMainViewport()->Pos;
            const float sx = ww > 0 ? static_cast<float>(w) / static_cast<float>(ww) : 1.0f;
            const float sy = wh > 0 ? static_cast<float>(h) / static_cast<float>(wh) : 1.0f;
            const int px = std::clamp(static_cast<int>((app.screenshot->x - vp.x) * sx), 0, w);
            const int py = std::clamp(static_cast<int>((app.screenshot->y - vp.y) * sy), 0, h);
            const int pw = std::clamp(static_cast<int>(app.screenshot->w * sx), 0, w - px);
            const int ph = std::clamp(static_cast<int>(app.screenshot->h * sy), 0, h - py);
            if (pw > 0 && ph > 0)
            {
                std::vector<uint8_t> rgba(static_cast<std::size_t>(pw) * ph * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(px, h - py - ph, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                const std::size_t stride = static_cast<std::size_t>(pw) * 4;
                for (int row = 0; row < ph / 2; ++row) // GL rows are bottom-up
                {
                    std::swap_ranges(rgba.begin() + static_cast<std::ptrdiff_t>(row * stride),
                                     rgba.begin() + static_cast<std::ptrdiff_t>((row + 1) * stride),
                                     rgba.begin() + static_cast<std::ptrdiff_t>((ph - 1 - row) * stride));
                }
                std::string error;
                if (png_write_file(app.screenshot->path, pw, ph, rgba, &error))
                {
                    log_info(std::format("Graph exported to {} ({}x{})", app.screenshot->path, pw, ph));
                }
                else
                {
                    log_error("Graph: " + error);
                }
            }
            app.screenshot.reset();
        }
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(window); // the platform windows switched the GL context
        }
        glfwSwapBuffers(window);
        settings_save_if_wanted(app);
    }

    const double loop_seconds = glfwGetTime() - loop_start;
    uint64_t rx_overruns = 0; // read before app_measurement_stop: stats are only there while open
    for (Iface& i : app.ifaces)
    {
        IfaceStats st;
        if (iface_stats(i, st))
        {
            rx_overruns += st.rx_overruns;
        }
    }

    settings_save(app);
    app.replays.clear(); // joins the replay players; they post tasks (glfwPostEmptyEvent) too
    gpio_control_close_all(app.gpio); // aiode poll threads call glfwPostEmptyEvent too
    python_shutdown(app, app.python); // before the interfaces close: the script may still send
    app_measurement_stop(app); // RX threads call glfwPostEmptyEvent: stop them before glfwTerminate
    if (smoke_frames >= 0)
    {
        std::fprintf(stderr, "smoke: %zu interfaces, %llu frames in trace\n", app.ifaces.size(),
                     static_cast<unsigned long long>(app.trace.end));
        std::fprintf(stderr, "smoke: %ld frames in %.2f s, %.1f fps, %llu rx_overruns\n", frame, loop_seconds,
                     loop_seconds > 0 ? static_cast<double>(frame) / loop_seconds : 0.0,
                     static_cast<unsigned long long>(rx_overruns));
        if (script != nullptr)
        {
            for (const PyConsoleRun& run : app.python.console)
            {
                std::fprintf(stderr, "%s%s", run.error ? "[stderr] " : "", run.text.c_str());
            }
        }
    }
    icons_shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
