#include "ui/gpio_control.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <format>

#include <imgui.h>

#include "app.h"
#include "drivers/aiode/aiode.h"
#include "drivers/grip.h"

namespace
{

// --- providers -------------------------------------------------------------

GpioState aiode_provider_state(void* user)
{
    const AiodeState s = aiode_state(*static_cast<Aiode*>(user));
    GpioState out{.pins = s.pins, .reports = s.reports};
    out.analog = s.analog;
    return out;
}

GpioState grip_provider_state(void* user)
{
    const GripGpio& g = (*static_cast<std::shared_ptr<GripDevice>*>(user))->gpio;
    GpioState out{.pins = g.pins, .reports = g.reports};
    std::ranges::copy(g.mv, out.analog.begin());
    return out;
}

void panel_config(GpioPanel& p, bool enable)
{
    if (p.provider.config != nullptr)
    {
        p.provider.config(p.provider.user, enable, static_cast<uint16_t>(p.cycle_ms), p.dir_mask);
    }
}

// --- window ----------------------------------------------------------------

void draw_panel(GpioPanel& p)
{
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::BeginDisabled(p.enabled);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Update interval (ms):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * px);
    if (ImGui::InputInt("##cycle", &p.cycle_ms, 5, 50))
    {
        p.cycle_ms = std::clamp(p.cycle_ms, p.provider.min_cycle_ms, p.provider.max_cycle_ms);
    }
    ImGui::SetItemTooltip("How often the device reports GPIO state (minimum %d ms)", p.provider.min_cycle_ms);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(p.enabled ? "Disable" : "Enable", ImVec2(80.0f * px, 0.0f)))
    {
        gpio_panel_set_enabled(p, !p.enabled);
    }
    if (p.enabled)
    {
        p.last = p.provider.state(p.provider.user);
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##pins", 5, flags))
    {
        return;
    }
    ImGui::TableSetupColumn("Pin", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Direction");
    ImGui::TableSetupColumn("Digital", ImGuiTableColumnFlags_WidthFixed);
    const std::string analog = std::strlen(p.provider.analog_unit) == 0 ? "Analog" : std::format("Voltage ({})", p.provider.analog_unit);
    ImGui::TableSetupColumn(analog.c_str());
    ImGui::TableSetupColumn("Output");
    ImGui::TableHeadersRow();
    const ImVec4 high(0.0f, 0.8f, 0.0f, 1.0f);
    const ImVec4 low(0.8f, 0.0f, 0.0f, 1.0f);
    for (int pin = 0; pin < std::min(16, p.provider.digital_pins); ++pin)
    {
        ImGui::PushID(pin);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d", pin);
        ImGui::TableNextColumn();
        const bool output = (p.dir_mask >> pin) & 1u;
        ImGui::BeginDisabled(p.enabled);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##dir", output ? "Output" : "Input"))
        {
            if (ImGui::Selectable("Input", !output))
            {
                gpio_panel_set_direction(p, pin, false);
            }
            if (ImGui::Selectable("Output", output))
            {
                gpio_panel_set_direction(p, pin, true);
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        if (!output && p.last.reports == 0)
        {
            ImGui::TextUnformatted("--");
        }
        else
        {
            const bool level = gpio_pin_high(p, pin);
            ImGui::TextColored(level ? high : low, level ? "HIGH" : "LOW");
        }
        ImGui::TableNextColumn();
        if (pin >= p.provider.analog_pins)
        {
            ImGui::TextDisabled("N/A");
        }
        else if (p.last.reports == 0)
        {
            ImGui::TextUnformatted("--");
        }
        else
        {
            ImGui::Text("%u %s", p.last.analog[static_cast<std::size_t>(pin)], p.provider.analog_unit);
        }
        ImGui::TableNextColumn();
        if (output)
        {
            if (ImGui::Button(gpio_pin_high(p, pin) ? "Set LOW" : "Set HIGH", ImVec2(-FLT_MIN, 0.0f)))
            {
                gpio_panel_toggle_output(p, pin);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

} // namespace

void gpio_panel_set_enabled(GpioPanel& p, bool enabled)
{
    p.enabled = enabled;
    panel_config(p, enabled);
}

void gpio_panel_set_direction(GpioPanel& p, int pin, bool output)
{
    if (output)
    {
        p.dir_mask |= static_cast<uint16_t>(1u << pin);
    }
    else
    {
        p.dir_mask &= static_cast<uint16_t>(~(1u << pin));
    }
}

void gpio_panel_toggle_output(GpioPanel& p, int pin)
{
    p.out_mask ^= static_cast<uint16_t>(1u << pin);
    if (p.provider.output != nullptr)
    {
        p.provider.output(p.provider.user, p.out_mask);
    }
}

bool gpio_pin_high(const GpioPanel& p, int pin) noexcept
{
    const bool output = (p.dir_mask >> pin) & 1u;
    return ((output ? p.out_mask : p.last.pins) >> pin) & 1u;
}

void gpio_panel_close(GpioPanel& p)
{
    if (p.enabled)
    {
        gpio_panel_set_enabled(p, false);
    }
    if (p.provider.close != nullptr)
    {
        p.provider.close(p.provider.user);
    }
    p.provider = {};
}

GpioProvider gpio_provider_grip(std::shared_ptr<GripDevice> dev, std::string name)
{
    return {
        .name = std::move(name),
        .digital_pins = grip_gpio_digital_pins,
        .analog_pins = grip_gpio_analog_pins,
        .max_cycle_ms = grip_gpio_max_cycle_ms,
        .analog_unit = "mV",
        .user = new std::shared_ptr<GripDevice>(std::move(dev)),
        .config = [](void* u, bool enable, uint16_t cycle_ms, uint16_t dir_mask)
        { grip_gpio_config(**static_cast<std::shared_ptr<GripDevice>*>(u), enable, static_cast<uint8_t>(std::min<int>(cycle_ms, 255)), dir_mask); },
        .output = [](void* u, uint16_t mask) { grip_gpio_output(**static_cast<std::shared_ptr<GripDevice>*>(u), mask); },
        .state = grip_provider_state,
        .close = [](void* u) { delete static_cast<std::shared_ptr<GripDevice>*>(u); },
    };
}

std::vector<GpioProvider> gpio_providers_aiode(void (*wake)())
{
    std::vector<GpioProvider> out;
    for (auto& a : aiode_scan())
    {
        a->wake = wake;
        out.push_back({
            .name = a->name,
            .digital_pins = aiode_digital_pin_count(*a),
            .analog_pins = aiode_analog_pin_count(*a),
            .min_cycle_ms = aiode_min_cycle_ms,
            .max_cycle_ms = aiode_max_cycle_ms,
            .user = a.release(),
            .config = [](void* u, bool enable, uint16_t cycle_ms, uint16_t dir_mask) { aiode_set_config(*static_cast<Aiode*>(u), enable, cycle_ms, dir_mask); },
            .output = [](void* u, uint16_t mask) { aiode_set_output(*static_cast<Aiode*>(u), mask); },
            .state = aiode_provider_state,
            .close =
                [](void* u)
            {
                auto* a = static_cast<Aiode*>(u);
                aiode_close(*a);
                delete a;
            },
        });
    }
    return out;
}

void gpio_control_close_all(GpioControl& gc)
{
    for (GpioPanel& p : gc.panels)
    {
        gpio_panel_close(p);
    }
    gc.panels.clear();
    gc.grip_seen.reset();
}

void gpio_control_rescan(GpioControl& gc, void (*wake)())
{
    gpio_control_close_all(gc);
    for (GpioProvider& prov : gpio_providers_aiode(wake))
    {
        gc.panels.push_back({.provider = std::move(prov)});
    }
    gc.scanned = true;
    gpio_control_sync_grip(gc);
}

void gpio_control_sync_grip(GpioControl& gc)
{
    // The panel's own shared_ptr keeps a closed device alive, so grip_seen still resolves to
    // it and differs from the (now null or new) open device.
    std::shared_ptr<GripDevice> dev = grip_open_device();
    if (dev == gc.grip_seen.lock())
    {
        return;
    }
    for (GpioPanel& p : gc.panels)
    {
        if (p.grip)
        {
            gpio_panel_close(p);
        }
    }
    std::erase_if(gc.panels, [](const GpioPanel& p) { return p.grip; });
    gc.grip_seen = dev;
    if (dev != nullptr)
    {
        gc.panels.push_back({.provider = gpio_provider_grip(std::move(dev), "GrIP Device 1"), .grip = true});
    }
}

void draw_gpio_control(App& app, GpioControl& gc)
{
    if (!gc.open)
    {
        return;
    }
    const float px = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(560.0f * px, 480.0f * px), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("GPIO Control", &gc.open))
    {
        ImGui::End();
        return;
    }
    const bool rescan = ImGui::Button("Rescan devices");
    if (rescan || !gc.scanned)
    {
        gpio_control_rescan(gc, app.tasks.wake);
    }
    gpio_control_sync_grip(gc);
    if (gc.panels.empty())
    {
        ImGui::TextDisabled("No GPIO devices found");
    }
    else if (ImGui::BeginTabBar("##devices"))
    {
        for (std::size_t i = 0; i < gc.panels.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::BeginTabItem(gc.panels[i].provider.name.c_str()))
            {
                if (ImGui::BeginChild("##page"))
                {
                    draw_panel(gc.panels[i]);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
