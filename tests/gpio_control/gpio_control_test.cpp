// ui/gpio_control: the panel logic (direction mask latched on enable, output toggling, the
// level shown per pin, config(false) before close) against a fake GpioProvider.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "ui/gpio_control.h"

namespace
{

struct FakeDevice
{
    std::vector<bool> config_enable;
    std::vector<uint16_t> config_dir;
    std::vector<uint16_t> config_cycle;
    std::vector<uint16_t> outputs;
    GpioState state;
    bool closed = false;
};

GpioProvider fake_provider(FakeDevice& dev)
{
    return {
        .name = "Fake",
        .digital_pins = 4,
        .analog_pins = 2,
        .user = &dev,
        .config =
            [](void* u, bool enable, uint16_t cycle_ms, uint16_t dir_mask)
        {
            auto& d = *static_cast<FakeDevice*>(u);
            d.config_enable.push_back(enable);
            d.config_cycle.push_back(cycle_ms);
            d.config_dir.push_back(dir_mask);
        },
        .output = [](void* u, uint16_t mask) { static_cast<FakeDevice*>(u)->outputs.push_back(mask); },
        .state = [](void* u) { return static_cast<FakeDevice*>(u)->state; },
        .close = [](void* u) { static_cast<FakeDevice*>(u)->closed = true; },
    };
}

} // namespace

TEST_CASE("enable pushes the latched direction mask and cycle; outputs toggle the mask")
{
    FakeDevice dev;
    GpioPanel p{.provider = fake_provider(dev), .cycle_ms = 20};
    gpio_panel_set_direction(p, 1, true);
    gpio_panel_set_direction(p, 3, true);
    gpio_panel_set_direction(p, 3, false);
    CHECK(p.dir_mask == 0b0010);
    CHECK(dev.config_enable.empty()); // nothing until Enable

    gpio_panel_set_enabled(p, true);
    REQUIRE(dev.config_enable.size() == 1);
    CHECK(dev.config_enable[0]);
    CHECK(dev.config_dir[0] == 0b0010);
    CHECK(dev.config_cycle[0] == 20);

    gpio_panel_toggle_output(p, 1);
    CHECK(p.out_mask == 0b0010);
    CHECK(dev.outputs == std::vector<uint16_t>{0b0010});
    gpio_panel_toggle_output(p, 1);
    CHECK(p.out_mask == 0);
    CHECK(dev.outputs.back() == 0);

    gpio_panel_set_enabled(p, false);
    CHECK(dev.config_enable.back() == false);
}

TEST_CASE("Digital column: outputs show the requested level, inputs the last report")
{
    FakeDevice dev;
    GpioPanel p{.provider = fake_provider(dev)};
    gpio_panel_set_direction(p, 0, true);
    p.last.pins = 0b0011; // device says both high
    CHECK_FALSE(gpio_pin_high(p, 0)); // output, not set yet
    CHECK(gpio_pin_high(p, 1));       // input from the report
    gpio_panel_toggle_output(p, 0);
    CHECK(gpio_pin_high(p, 0));
    p.last.pins = 0;
    CHECK(gpio_pin_high(p, 0)); // still the requested level
    CHECK_FALSE(gpio_pin_high(p, 1));
}

TEST_CASE("close stops an enabled panel first, then releases the provider")
{
    FakeDevice dev;
    GpioControl gc;
    gc.panels.push_back({.provider = fake_provider(dev)});
    gpio_panel_set_enabled(gc.panels[0], true);
    gpio_control_close_all(gc);
    CHECK(gc.panels.empty());
    CHECK(dev.config_enable == std::vector<bool>{true, false});
    CHECK(dev.closed);

    FakeDevice idle;
    GpioPanel p{.provider = fake_provider(idle)};
    gpio_panel_close(p);
    CHECK(idle.config_enable.empty()); // was never enabled: no config(false)
    CHECK(idle.closed);
    CHECK(p.provider.close == nullptr);
}

TEST_CASE("no GrIP measurement: sync adds no panel")
{
    GpioControl gc;
    gpio_control_sync_grip(gc);
    CHECK(gc.panels.empty());
}
