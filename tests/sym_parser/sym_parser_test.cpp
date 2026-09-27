// PCAN .sym parser, end to end through can_db_parse_file (extension dispatch).
//
// Expected values come from cantools 41 (cantools.database.load_file("test.sym")),
// not from Kraken: decode(bytes.fromhex("3412abcd9c3f00d8")) on Engine gives
// Speed 46.6, Rpm 10995.25, Torque -150.0, Gear 'Drive', Temp -80; Muxed gives
// {Page 1, A 4660} for 0134120000000000 and {Page 2, B -4} for 02fe000000000000.
// cantools 44.1: Floats.encode({F 3.14159, G -273.15}) = d00f4940c40d9333, decoded as
// 3.141590118408203 / -273.1499938964844; Dbl.encode({D 1e10}) = 000000205fa00242.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string_view>

#include "db/model/can_db.h"
#include "db/sym/sym_parser.h"

namespace
{

BusMessage frame(uint32_t id, std::initializer_list<uint8_t> bytes)
{
    BusMessage m{.id = id, .len = static_cast<uint8_t>(bytes.size())};
    std::ranges::copy(bytes, m.data.begin());
    return m;
}

double phys(const CanDbMessage& msg, std::string_view name, const BusMessage& m)
{
    const CanDbSignal* sig = can_db_find_signal(msg, name);
    REQUIRE(sig != nullptr);
    REQUIRE(can_signal_present(msg, *sig, m));
    return can_signal_extract_physical(*sig, m);
}

} // namespace

TEST_CASE("parses a PCAN symbol file like cantools")
{
    CanDb db;
    REQUIRE(can_db_parse_file(SYM_TEST_FILE, db));
    CHECK(db.path == SYM_TEST_FILE);
    REQUIRE(db.messages.size() == 4);

    SUBCASE("standard message: Intel, Motorola, signed, factor/offset, enum, {SIGNALS} reference")
    {
        const CanDbMessage* msg = can_db_find_message(db, 0x123);
        REQUIRE(msg != nullptr);
        CHECK(msg->name == "Engine");
        CHECK(msg->dlc == 8);
        CHECK(msg->comment == "engine status");
        CHECK(msg->muxer == -1);

        const CanDbSignal* speed = can_db_find_signal(*msg, "Speed");
        REQUIRE(speed != nullptr);
        CHECK(speed->unit == "km/h");
        CHECK(speed->max == doctest::Approx(655.35));
        CHECK(can_db_find_signal(*msg, "Rpm")->big_endian);
        CHECK_FALSE(can_db_find_signal(*msg, "Torque")->is_unsigned);
        CHECK(can_db_find_signal(*msg, "Temp")->unit == "C");

        const BusMessage m = frame(0x123, {0x34, 0x12, 0xab, 0xcd, 0x9c, 0x3f, 0x00, 0xd8});
        CHECK(phys(*msg, "Speed", m) == doctest::Approx(46.6));
        CHECK(phys(*msg, "Rpm", m) == doctest::Approx(10995.25));
        CHECK(phys(*msg, "Torque", m) == doctest::Approx(-150.0));
        CHECK(phys(*msg, "Temp", m) == doctest::Approx(-80.0));
        const CanDbSignal* gear = can_db_find_signal(*msg, "Gear");
        CHECK(can_signal_extract_raw(*gear, m) == 3);
        CHECK(can_signal_value_name(*gear, 3) == "Drive");
        CHECK(can_signal_value_name(*gear, 1) == "Reverse");
    }

    SUBCASE("extended multiplexed message spread over two blocks")
    {
        const CanDbMessage* msg = can_db_find_message(db, 0x18FF0010u | 0x80000000u);
        REQUIRE(msg != nullptr);
        CHECK(msg->name == "Muxed");
        const CanDbSignal* page = can_db_muxer(*msg);
        REQUIRE(page != nullptr);
        CHECK(page->name == "Page");

        const BusMessage p1 = frame(0x18FF0010, {0x01, 0x34, 0x12, 0, 0, 0, 0, 0});
        CHECK(phys(*msg, "A", p1) == doctest::Approx(4660));
        CHECK_FALSE(can_signal_present(*msg, *can_db_find_signal(*msg, "B"), p1));

        const BusMessage p2 = frame(0x18FF0010, {0x02, 0xfe, 0, 0, 0, 0, 0, 0});
        CHECK(phys(*msg, "B", p2) == doctest::Approx(-4));
        CHECK_FALSE(can_signal_present(*msg, *can_db_find_signal(*msg, "A"), p2));
    }

    SUBCASE("float (Intel and Motorola) and double signals")
    {
        const CanDbMessage* floats = can_db_find_message(db, 0x200);
        const CanDbMessage* dbl = can_db_find_message(db, 0x201);
        REQUIRE(floats != nullptr);
        REQUIRE(dbl != nullptr);
        CHECK(can_db_find_signal(*floats, "F")->value_type == SignalValueType::float32);
        CHECK(can_db_find_signal(*floats, "G")->value_type == SignalValueType::float32);
        CHECK(can_db_find_signal(*dbl, "D")->value_type == SignalValueType::float64);

        const BusMessage f = frame(0x200, {0xd0, 0x0f, 0x49, 0x40, 0xc4, 0x0d, 0x93, 0x33});
        CHECK(phys(*floats, "F", f) == 3.141590118408203);
        CHECK(phys(*floats, "G", f) == doctest::Approx(-273.1499938964844).epsilon(1e-15));

        BusMessage enc = frame(0x200, {0, 0, 0, 0, 0, 0, 0, 0});
        can_signal_inject_physical(*can_db_find_signal(*floats, "F"), enc, 3.14159);
        can_signal_inject_physical(*can_db_find_signal(*floats, "G"), enc, -273.15);
        CHECK(enc.data == f.data);

        BusMessage d = frame(0x201, {0, 0, 0, 0, 0, 0, 0, 0});
        can_signal_inject_physical(*can_db_find_signal(*dbl, "D"), d, 1e10);
        CHECK(d.data == frame(0x201, {0x00, 0x00, 0x00, 0x20, 0x5f, 0xa0, 0x02, 0x42}).data);
        CHECK(phys(*dbl, "D", d) == 1e10);
    }
}

TEST_CASE("rejects text without FormatVersion")
{
    CanDb db;
    CHECK_FALSE(sym_parse("VERSION \"\"\nBO_ 1 M: 8 X\n", db));
}
