// lin_usb device->host frame -> BusMessage. Flag semantics from lin_usb_protocol.h
// (the firmware's wire contract), not from cangaroo output.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "drivers/linde/linde.h"

TEST_CASE("protected id is masked, subscriber frame is RX")
{
    lin_usb_host_frame_t f{.echo_id = LIN_USB_ECHO_ID_RX, .lin_id = 0x80 | 0x40 | 0x21, .dlc = 2,
                           .flags = LIN_USB_FRAME_FLAG_SUBSCRIBER | LIN_USB_FRAME_FLAG_RESPONDED | LIN_USB_FRAME_FLAG_VALID,
                           .data = {0xAB, 0xCD}};
    BusMessage m;
    linde_frame_to_message(f, m);
    CHECK(m.type == BusType::LIN);
    CHECK(m.id == 0x21);
    CHECK(m.len == 2);
    CHECK(m.data[0] == 0xAB);
    CHECK(m.data[1] == 0xCD);
    CHECK_FALSE(has_flag(m, bus_flag::tx));
    CHECK(m.errors == 0);
}

TEST_CASE("publisher frame is TX, sleep/wakeup flags")
{
    lin_usb_host_frame_t f{.lin_id = 0x3C, .dlc = 8, .flags = LIN_USB_FRAME_FLAG_SLEEP | LIN_USB_FRAME_FLAG_WAKEUP};
    BusMessage m;
    linde_frame_to_message(f, m);
    CHECK(has_flag(m, bus_flag::tx));
    CHECK(has_flag(m, bus_flag::lin_sleep));
    CHECK(has_flag(m, bus_flag::lin_wakeup));
    CHECK(m.len == 8);
}

TEST_CASE("error classification")
{
    BusMessage m;
    lin_usb_host_frame_t f{.flags = LIN_USB_FRAME_FLAG_ERROR};
    linde_frame_to_message(f, m);
    CHECK(m.errors == bus_error::lin_not_responded);

    f.flags = LIN_USB_FRAME_FLAG_ERROR | LIN_USB_FRAME_FLAG_RESPONDED;
    linde_frame_to_message(f, m);
    CHECK(m.errors == bus_error::lin_checksum_error);

    f.flags = LIN_USB_FRAME_FLAG_ERROR | LIN_USB_FRAME_FLAG_RESPONDED | LIN_USB_FRAME_FLAG_VALID;
    linde_frame_to_message(f, m);
    CHECK(m.errors == bus_error::generic);
}

TEST_CASE("dlc beyond 8 is clamped")
{
    lin_usb_host_frame_t f{.dlc = 200};
    BusMessage m;
    linde_frame_to_message(f, m);
    CHECK(m.len == 8);
}
