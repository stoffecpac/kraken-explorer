// AUTOSAR E2E Profile 2 CRC (CRC-8H2F).
//
// A wrong CRC here is invisible in Kraken Explorer itself -- it only shows up as a real
// ECU silently rejecting every frame the TX generator sends. The anchors are
// therefore external: the check value from the AUTOSAR CRC specification, and a
// bitwise reimplementation that shares no code with the lookup table under test.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string_view>

#include "core/autosar_e2e.h"

namespace
{

// Bitwise CRC-8H2F, deliberately not table driven: poly 0x2F, init 0xFF,
// non-reflected, final xor 0xFF.
[[nodiscard]] uint8_t reference_crc8h2f(std::string_view data)
{
    uint8_t crc = 0xFF;
    for (const char c : data)
    {
        crc ^= static_cast<uint8_t>(c);
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 0x80u) ? static_cast<uint8_t>((crc << 1) ^ 0x2Fu) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc ^ 0xFFu;
}

[[nodiscard]] BusMessage message_from_hex(std::string_view hex)
{
    const auto nibble = [](char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
    BusMessage msg;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        msg.data[i / 2] = static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1]));
    }
    set_length(msg, static_cast<int>(hex.size() / 2));
    return msg;
}

} // namespace

// The AUTOSAR CRC specification gives 0xDF as the CRC-8H2F check value for the
// ASCII string "123456789" (init 0xFF, final xor 0xFF). If the polynomial or the
// table is wrong, this is the assertion that says so.
TEST_CASE("spec check value")
{
    constexpr std::string_view input = "123456789";
    uint8_t crc = 0xFF;
    for (const char c : input)
    {
        crc = crc8h2f_byte(crc, static_cast<uint8_t>(c));
    }
    CHECK(static_cast<uint8_t>(crc ^ 0xFFu) == 0xDF);
    CHECK(reference_crc8h2f(input) == 0xDF);
}

// The header builds its table with consteval; recompute it bitwise here so a
// mistake in the table generator cannot hide behind itself.
TEST_CASE("table matches bitwise implementation")
{
    for (int i = 0; i < 256; ++i)
    {
        auto expected = static_cast<uint8_t>(i);
        for (int bit = 0; bit < 8; ++bit)
        {
            expected = (expected & 0x80u) ? static_cast<uint8_t>((expected << 1) ^ 0x2Fu)
                                          : static_cast<uint8_t>(expected << 1);
        }
        CAPTURE(i);
        // crc8h2f_byte(0, i) is a plain table lookup at index i.
        CHECK(crc8h2f_byte(0, static_cast<uint8_t>(i)) == expected);
    }
}

// Values from an independent bitwise implementation of the Profile 2 input
// sequence: DataID low, DataID high, 0x00, then data[1..length-1].
TEST_CASE("profile 2 crc")
{
    struct Row
    {
        const char* name;
        const char* payload_hex;
        uint16_t data_id;
        uint8_t expected;
    };
    constexpr Row rows[] = {
        {.name = "all zero, id 0", .payload_hex = "0000000000000000", .data_id = 0x0000, .expected = 0xE4},
        {.name = "all ff, id 0", .payload_hex = "FFFFFFFFFFFFFFFF", .data_id = 0x0000, .expected = 0xDD},
        {.name = "counter only", .payload_hex = "0003000000000000", .data_id = 0x0123, .expected = 0xAC},
        {.name = "ramp", .payload_hex = "0001020304050607", .data_id = 0x0234, .expected = 0xB8},
        {.name = "high data id", .payload_hex = "000ADEADBEEF0011", .data_id = 0xBEEF, .expected = 0x8A},
        {.name = "minimum dlc 2", .payload_hex = "0005", .data_id = 0x0042, .expected = 0x33},
    };
    for (const auto& r : rows)
    {
        CAPTURE(r.name);
        CHECK(e2e_p2_compute_crc(message_from_hex(r.payload_hex), r.data_id) == r.expected);
    }
}

// Byte 0 holds the CRC itself, so it must be fed to the CRC as 0x00 regardless
// of what the frame currently carries there.
TEST_CASE("crc ignores byte zero")
{
    const uint8_t crc = e2e_p2_compute_crc(message_from_hex("0001020304050607"), 0x1234);
    CHECK(e2e_p2_compute_crc(message_from_hex("FF01020304050607"), 0x1234) == crc);
    CHECK(e2e_p2_compute_crc(message_from_hex("A501020304050607"), 0x1234) == crc);
}

TEST_CASE("crc depends on counter nibble")
{
    BusMessage msg = message_from_hex("0000000000000000");
    const uint8_t crc0 = e2e_p2_compute_crc(msg, 0x1234);
    msg.data[1] = 0x01;
    CHECK(e2e_p2_compute_crc(msg, 0x1234) != crc0);
}

TEST_CASE("crc depends on data id")
{
    const BusMessage msg = message_from_hex("0001020304050607");
    CHECK(e2e_p2_compute_crc(msg, 0x0000) != e2e_p2_compute_crc(msg, 0x0001));
    // Both DataID bytes must be mixed in, in the documented low-then-high order.
    CHECK(e2e_p2_compute_crc(msg, 0x0100) != e2e_p2_compute_crc(msg, 0x0001));
}

// The CRC runs to the frame's length, so a shorter frame must produce a different
// value even when the underlying buffer bytes are identical.
TEST_CASE("crc depends on length")
{
    BusMessage msg = message_from_hex("0001020304050607");
    const uint8_t crc8 = e2e_p2_compute_crc(msg, 0x1234);
    set_length(msg, 4);
    CHECK(e2e_p2_compute_crc(msg, 0x1234) != crc8);
}

TEST_CASE("crc covers full FD payload")
{
    BusMessage msg;
    set_length(msg, 64);
    for (int i = 1; i < 64; i++)
    {
        msg.data[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    }
    CHECK(e2e_p2_compute_crc(msg, 0x1337) == 0x90);

    // Changing the very last byte must change the CRC: proof the loop reaches it.
    msg.data[63] ^= 0xFF;
    CHECK(e2e_p2_compute_crc(msg, 0x1337) != 0x90);
}

// A len beyond the 64-byte buffer (corrupt input, Python binding) must not read past it.
TEST_CASE("crc stops at the buffer end")
{
    BusMessage msg;
    set_length(msg, 64);
    const uint8_t crc = e2e_p2_compute_crc(msg, 0x1337);
    msg.len = 200;
    CHECK(e2e_p2_compute_crc(msg, 0x1337) == crc);
}
