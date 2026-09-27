#pragma once

// GPIO Control window (the old GpioControlWindow): one panel per GPIO source, i.e. every
// aiode USB device (scanned independently of a measurement) and the GrIP of the running
// measurement. A source is a GpioProvider table of free functions (the old GpioProvider
// QObject); the panel logic below it is UI-free so it is unit-tested against a fake.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct App;
struct GripDevice;

struct GpioState
{
    uint16_t pins = 0;                 // digital levels, bit set = high
    std::array<uint16_t, 16> analog{}; // GpioProvider::analog_pins entries valid
    uint64_t reports = 0;              // 0 = nothing received yet
};

struct GpioProvider
{
    std::string name;
    int digital_pins = 0;
    int analog_pins = 0;
    int min_cycle_ms = 5;
    int max_cycle_ms = 255;
    const char* analog_unit = ""; // "mV" (GrIP) or "" (raw)
    void* user = nullptr;
    void (*config)(void* user, bool enable, uint16_t cycle_ms, uint16_t dir_mask) = nullptr;
    void (*output)(void* user, uint16_t mask) = nullptr;
    GpioState (*state)(void* user) = nullptr;
    void (*close)(void* user) = nullptr; // releases user
};

struct GpioPanel
{
    GpioProvider provider;
    bool grip = false;    // replaced when the measurement starts/stops
    bool enabled = false; // reporting on; direction and cycle are frozen meanwhile
    int cycle_ms = 50;
    uint16_t dir_mask = 0; // bit set = output
    uint16_t out_mask = 0; // requested output levels
    GpioState last;
};

// Enable: latches dir_mask/cycle and starts reporting; disable: config(false).
void gpio_panel_set_enabled(GpioPanel& p, bool enabled);
void gpio_panel_set_direction(GpioPanel& p, int pin, bool output);
// Flips the requested level of an output pin and pushes the mask.
void gpio_panel_toggle_output(GpioPanel& p, int pin);
// Level shown in the Digital column: outputs show the requested level, inputs the last report.
[[nodiscard]] bool gpio_pin_high(const GpioPanel& p, int pin) noexcept;
// Stops reporting if enabled and releases the provider.
void gpio_panel_close(GpioPanel& p);

struct GpioControl
{
    bool open = false;
    bool scanned = false; // aiode scan done once the window was first shown
    std::vector<GpioPanel> panels;
    std::weak_ptr<GripDevice> grip_seen; // the GrIP panel follows grip_open_device()
};

// Providers over the drivers.
[[nodiscard]] GpioProvider gpio_provider_grip(std::shared_ptr<GripDevice> dev, std::string name);
// Every connected aiode device; wake is called after each report (glfwPostEmptyEvent).
[[nodiscard]] std::vector<GpioProvider> gpio_providers_aiode(void (*wake)());

// Closes every panel; call before glfwTerminate (the aiode poll threads call wake).
void gpio_control_close_all(GpioControl& gc);
// Closes and re-scans the aiode devices (Rescan button) and syncs the GrIP panel.
void gpio_control_rescan(GpioControl& gc, void (*wake)());
// Adds/removes the GrIP panel when the measurement's device changed. Every frame.
void gpio_control_sync_grip(GpioControl& gc);

// The "GPIO Control" window while gc.open.
void draw_gpio_control(App& app, GpioControl& gc);
