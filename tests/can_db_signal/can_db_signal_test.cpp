// CanDbSignal: raw <-> physical conversion, sign extension, and the multiplexer
// gate. These decide what number the user actually sees in the trace, so an
// error here is silently wrong output rather than a crash -- the same failure
// mode as the big-endian bug in issue #34.
//
// Was a Qt Test; doctest now with the same vectors.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "core/bus_message.h"
#include "db/model/can_db.h"

namespace
{

// Mirrors the parser's Motorola start-bit conversion (dbc_parser.cpp).
[[nodiscard]] uint16_t motorola_start_bit(uint16_t dbc_start_bit) noexcept
{
    const uint16_t row = dbc_start_bit >> 3;
    const uint16_t column = dbc_start_bit & 0b111;
    return static_cast<uint16_t>((row * 8) + (7 - column));
}

[[nodiscard]] BusMessage frame8(uint32_t id = 0)
{
    BusMessage m{.id = id};
    set_length(m, 8);
    return m;
}

} // namespace

TEST_CASE("unsigned conversion applies factor and offset")
{
    const CanDbSignal sig{.length = 8, .is_unsigned = true, .factor = 0.5, .offset = -40.0};

    CHECK(can_signal_raw_to_physical(sig, 0) == doctest::Approx(-40.0));
    CHECK(can_signal_raw_to_physical(sig, 80) == doctest::Approx(0.0));
    CHECK(can_signal_raw_to_physical(sig, 255) == doctest::Approx(87.5));
}

TEST_CASE("negative factor and offset")
{
    const CanDbSignal sig{.length = 8, .is_unsigned = true, .factor = -2.0, .offset = 10.0};

    CHECK(can_signal_raw_to_physical(sig, 0) == doctest::Approx(10.0));
    CHECK(can_signal_raw_to_physical(sig, 5) == doctest::Approx(0.0));
}

TEST_CASE("sign extension")
{
    struct Row
    {
        const char* name;
        uint16_t length;
        uint64_t raw;
        double expected;
    };
    // For a signed signal of n bits, raw values with the top bit set are negative.
    constexpr Row rows[] = {
        {.name = "1bit -1", .length = 1, .raw = 0x1, .expected = -1.0},
        {.name = "1bit 0", .length = 1, .raw = 0x0, .expected = 0.0},
        {.name = "2bit -1", .length = 2, .raw = 0x3, .expected = -1.0},
        {.name = "2bit -2", .length = 2, .raw = 0x2, .expected = -2.0},
        {.name = "2bit 1", .length = 2, .raw = 0x1, .expected = 1.0},
        {.name = "4bit -8", .length = 4, .raw = 0x8, .expected = -8.0},
        {.name = "4bit 7", .length = 4, .raw = 0x7, .expected = 7.0},
        {.name = "8bit -1", .length = 8, .raw = 0xFF, .expected = -1.0},
        {.name = "8bit -128", .length = 8, .raw = 0x80, .expected = -128.0},
        {.name = "8bit 127", .length = 8, .raw = 0x7F, .expected = 127.0},
        {.name = "12bit -2048", .length = 12, .raw = 0x800, .expected = -2048.0},
        {.name = "16bit -1", .length = 16, .raw = 0xFFFF, .expected = -1.0},
        {.name = "16bit -32768", .length = 16, .raw = 0x8000, .expected = -32768.0},
        {.name = "32bit -1", .length = 32, .raw = 0xFFFFFFFF, .expected = -1.0},
        {.name = "32bit min", .length = 32, .raw = 0x80000000, .expected = -2147483648.0},
        {.name = "63bit -1", .length = 63, .raw = 0x7FFFFFFFFFFFFFFFULL, .expected = -1.0},
    };

    for (const Row& r : rows)
    {
        CAPTURE(r.name);
        CanDbSignal sig{.length = r.length, .is_unsigned = false};
        CHECK(can_signal_raw_to_physical(sig, r.raw) == doctest::Approx(r.expected));

        // The same raw value read as unsigned must stay positive.
        sig.is_unsigned = true;
        CHECK(can_signal_raw_to_physical(sig, r.raw) >= 0.0);
    }
}

TEST_CASE("sign extension at full width")
{
    const CanDbSignal sig{.length = 64, .is_unsigned = false};

    CHECK(can_signal_raw_to_physical(sig, 0xFFFFFFFFFFFFFFFFULL) == doctest::Approx(-1.0));
    CHECK(can_signal_raw_to_physical(sig, 0x8000000000000000ULL)
          == doctest::Approx(static_cast<double>(std::numeric_limits<int64_t>::min())));
    CHECK(can_signal_raw_to_physical(sig, 1) == doctest::Approx(1.0));
}

// A DBC file's signal length is not validated, so 0 and >64 reach this code.
// Sign extension is impossible there (the shift width would be undefined), so
// the value must be read as unsigned instead of invoking undefined behaviour.
TEST_CASE("degenerate length falls back to unsigned")
{
    for (const uint16_t length : {uint16_t(0), uint16_t(65), uint16_t(100)})
    {
        CAPTURE(length);
        const CanDbSignal sig{.length = length, .is_unsigned = false};

        CHECK(can_signal_raw_to_physical(sig, 0) == doctest::Approx(0.0));
        CHECK(can_signal_raw_to_physical(sig, 1) == doctest::Approx(1.0));
        CHECK(can_signal_raw_to_physical(sig, 0xFF) > 0.0);
    }
}

// Encoding a physical value and decoding it again must return the same number.
TEST_CASE("physical round-trips through message")
{
    struct Row
    {
        const char* name;
        uint16_t dbc_start_bit;
        uint16_t length;
        bool big_endian;
        bool is_unsigned;
        double factor;
        double offset;
        double physical;
    };
    constexpr Row rows[] = {
        {"intel u8 scaled", 0, 8, false, true, 0.5, -40.0, 20.0},
        {"intel s16", 8, 16, false, false, 1.0, 0.0, -1234.0},
        {"motorola u12", 7, 12, true, true, 1.0, 0.0, 3000.0},
        {"motorola s12", 7, 12, true, false, 1.0, 0.0, -2000.0},
        {"motorola unaligned", 11, 4, true, true, 1.0, 0.0, 9.0},
        {"motorola scaled", 23, 16, true, true, 0.01, 0.0, 12.34},
        {"intel s32 offset", 0, 32, false, false, 1.0, 100.0, -50.0},
    };

    for (const Row& r : rows)
    {
        CAPTURE(r.name);
        const CanDbSignal sig{
            .start_bit = r.big_endian ? motorola_start_bit(r.dbc_start_bit) : r.dbc_start_bit,
            .length = r.length,
            .is_unsigned = r.is_unsigned,
            .big_endian = r.big_endian,
            .factor = r.factor,
            .offset = r.offset,
        };

        BusMessage msg = frame8();
        can_signal_inject_physical(sig, msg, r.physical);
        CHECK(can_signal_extract_physical(sig, msg) == doctest::Approx(r.physical));
    }
}

// An unsigned signal cannot hold a negative value; it must saturate at zero
// rather than wrapping around to a huge positive number.
TEST_CASE("inject physical clamps unsigned at zero")
{
    const CanDbSignal sig{.start_bit = 0, .length = 8, .is_unsigned = true};

    BusMessage msg = frame8();
    can_signal_inject_physical(sig, msg, -5.0);
    CHECK(can_signal_extract_raw(sig, msg) == 0ULL);
}

TEST_CASE("inject physical rounds rather than truncates")
{
    const CanDbSignal sig{.start_bit = 0, .length = 8, .is_unsigned = true};

    BusMessage msg = frame8();
    can_signal_inject_physical(sig, msg, 9.6);
    CHECK(can_signal_extract_raw(sig, msg) == 10ULL);

    can_signal_inject_physical(sig, msg, 9.4);
    CHECK(can_signal_extract_raw(sig, msg) == 9ULL);
}

// can_signal_present gates whether a muxed signal is decoded at all. If it
// answers wrongly the trace shows a value decoded from unrelated payload bytes.
TEST_CASE("muxed signal is present only for matching mux value")
{
    CanDbMessage db_msg{.raw_id = 0x100, .dlc = 8};
    db_msg.signals.push_back({.name = "Mux", .start_bit = 0, .length = 4, .is_unsigned = true, .is_muxer = true});
    // Adding the signal does not infer this; the message's muxer index is what
    // can_signal_present() consults, and the DBC parser sets it explicitly.
    db_msg.muxer = 0;
    db_msg.signals.push_back(
        {.name = "Muxed", .start_bit = 8, .length = 8, .is_unsigned = true, .is_muxed = true, .mux_value = 2});
    const CanDbSignal& muxer = db_msg.signals[0];
    const CanDbSignal& muxed = db_msg.signals[1];

    BusMessage msg = frame8(0x100);

    can_signal_inject_raw(muxer, msg, 2);
    CHECK(can_signal_present(db_msg, muxed, msg));

    can_signal_inject_raw(muxer, msg, 3);
    CHECK_FALSE(can_signal_present(db_msg, muxed, msg));

    can_signal_inject_raw(muxer, msg, 0);
    CHECK_FALSE(can_signal_present(db_msg, muxed, msg));
}

TEST_CASE("unmuxed signal is always present")
{
    CanDbMessage db_msg{.raw_id = 0x200, .dlc = 8};
    db_msg.signals.push_back({.start_bit = 0, .length = 8});

    const BusMessage msg = frame8(0x200);
    CHECK(can_signal_present(db_msg, db_msg.signals[0], msg));
}

TEST_CASE("value table lookup")
{
    const CanDbSignal sig{.length = 4, .value_table = {{0, "Off"}, {1, "On"}, {15, "Invalid"}}};

    CHECK(can_signal_value_name(sig, 0) == "Off");
    CHECK(can_signal_value_name(sig, 1) == "On");
    CHECK(can_signal_value_name(sig, 15) == "Invalid");
    CHECK(can_signal_value_name(sig, 7).empty());
}

// IEEE-754 signals. Vectors from cantools 44.1 (SIG_VALTYPE_ 1/2 DBC, Message.encode /
// decode), not from Kraken: IntelF 0|32@1 = 3.14159 and MotoF 39|32@0 (0.5,10) = -273.15
// encode to d00f4940 c40d9333 and decode to 3.141590118408203 / -273.1499938964844;
// D 0|64@1 = 1e10 is 000000205fa00242, MD 7|64@0 = -273.15 is c071126666666666.
TEST_CASE("float32 and float64 signals in both byte orders match cantools")
{
    const CanDbSignal intel_f{.start_bit = 0, .length = 32, .value_type = SignalValueType::float32};
    const CanDbSignal moto_f{.start_bit = motorola_start_bit(39), .length = 32, .big_endian = true,
                             .value_type = SignalValueType::float32, .factor = 0.5, .offset = 10.0};
    const CanDbSignal intel_d{.start_bit = 0, .length = 64, .value_type = SignalValueType::float64};
    const CanDbSignal moto_d{.start_bit = motorola_start_bit(7), .length = 64, .big_endian = true,
                             .value_type = SignalValueType::float64};

    const auto bytes = [](const BusMessage& m) { return std::vector<uint8_t>(m.data.begin(), m.data.begin() + 8); };

    BusMessage f = frame8();
    can_signal_inject_physical(intel_f, f, 3.14159);
    can_signal_inject_physical(moto_f, f, -273.15);
    CHECK(bytes(f) == std::vector<uint8_t>{0xd0, 0x0f, 0x49, 0x40, 0xc4, 0x0d, 0x93, 0x33});
    CHECK(can_signal_extract_physical(intel_f, f) == 3.141590118408203);
    CHECK(can_signal_extract_physical(moto_f, f) == doctest::Approx(-273.1499938964844).epsilon(1e-15));

    BusMessage d = frame8();
    can_signal_inject_physical(intel_d, d, 1e10);
    CHECK(bytes(d) == std::vector<uint8_t>{0x00, 0x00, 0x00, 0x20, 0x5f, 0xa0, 0x02, 0x42});
    CHECK(can_signal_extract_physical(intel_d, d) == 1e10);

    BusMessage md = frame8();
    can_signal_inject_physical(moto_d, md, -273.15);
    CHECK(bytes(md) == std::vector<uint8_t>{0xc0, 0x71, 0x12, 0x66, 0x66, 0x66, 0x66, 0x66});
    CHECK(can_signal_extract_physical(moto_d, md) == -273.15);
}

TEST_CASE("float value type with the wrong length reads as integer")
{
    const CanDbSignal sig{.length = 16, .is_unsigned = true, .value_type = SignalValueType::float32};
    CHECK(can_signal_raw_to_physical(sig, 0x4049) == 16457.0);
}
