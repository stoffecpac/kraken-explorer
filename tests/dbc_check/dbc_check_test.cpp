// dbc_check: semantic problems of an in-memory CanDb, one case per check. Databases are
// built by hand so cases the parser would refuse (length 0, float32 of 16 bits) are
// reachable. Bit positions follow the DBC spec: an Intel signal's start bit is its LSB
// counted LSB-first per byte, a Motorola signal is stored by the parser as a sequential
// MSB-first index (CanDbSignal::start_bit), so byte 0 is Intel bits 0..7 and Motorola
// (internal) bits 0..7 in opposite order.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "db/dbc/dbc_check.h"
#include "db/model/can_db.h"

namespace
{

[[nodiscard]] CanDbSignal sig(std::string name, uint16_t start_bit, uint16_t length, bool big_endian = false)
{
    return CanDbSignal{.name = std::move(name), .start_bit = start_bit, .length = length, .is_unsigned = true,
                       .big_endian = big_endian, .max = 1.0};
}

[[nodiscard]] CanDbSignal muxer(std::string name, uint16_t start_bit, uint16_t length)
{
    CanDbSignal s = sig(std::move(name), start_bit, length);
    s.is_muxer = true;
    return s;
}

[[nodiscard]] CanDbSignal muxed(std::string name, uint16_t start_bit, uint16_t length, uint32_t mux_value)
{
    CanDbSignal s = sig(std::move(name), start_bit, length);
    s.is_muxed = true;
    s.mux_value = mux_value;
    return s;
}

// Wires msg.muxer like the parser does.
[[nodiscard]] CanDbMessage message(std::string name, uint32_t raw_id, std::initializer_list<CanDbSignal> signals,
                                   uint8_t dlc = 8)
{
    CanDbMessage msg{.name = std::move(name), .raw_id = raw_id, .dlc = dlc};
    for (const CanDbSignal& s : signals)
    {
        msg.signals.push_back(s);
        if (s.is_muxer) { msg.muxer = static_cast<int>(msg.signals.size() - 1); }
    }
    return msg;
}

[[nodiscard]] CanDb db_of(std::initializer_list<CanDbMessage> messages)
{
    CanDb db;
    for (const CanDbMessage& m : messages) { db.messages[m.raw_id] = m; }
    return db;
}

// A finding containing `text`; semantic findings are never tied to a line.
[[nodiscard]] bool has(const std::vector<DbcError>& errors, std::string_view text)
{
    for (const DbcError& e : errors)
    {
        if (e.line == 0 && e.message.find(text) != std::string::npos) { return true; }
    }
    return false;
}

// For INFO(): every message on its own line, shown when an assertion fails.
[[nodiscard]] std::string joined(const std::vector<DbcError>& errors)
{
    std::string all;
    for (const DbcError& e : errors) { all += e.message + "\n"; }
    return all;
}

} // namespace

TEST_CASE("a consistent database has no findings")
{
    const CanDb db = db_of({
        message("Plain", 0x100, {sig("A", 0, 16), sig("B", 16, 8, true), sig("C", 24, 8), sig("D", 63, 1, true)}),
        // Muxed signals with different mux values may share bits; the muxer itself may not.
        message("Mux", 0x200, {muxer("Mode", 0, 8), muxed("V0", 8, 16, 0), muxed("V1", 8, 16, 1), muxed("W1", 24, 8, 1)}),
        message("Ext", 0x80000123u, {sig("E", 0, 8)}, 1),
    });
    const auto errors = dbc_check(db);
    INFO(joined(errors));
    CHECK(errors.empty());
}

TEST_CASE("duplicate message names")
{
    const auto errors = dbc_check(db_of({message("Same", 0x100, {}), message("Same", 0x200, {}), message("Other", 0x300, {})}));
    INFO(joined(errors));
    CHECK(errors.size() == 1);
    CHECK(has(errors, "Same: duplicate message name"));
    CHECK(has(errors, "256"));
    CHECK(has(errors, "512"));
}

TEST_CASE("duplicate signal names within a message")
{
    const auto errors = dbc_check(db_of({message("M", 0x100, {sig("X", 0, 8), sig("X", 8, 8)}),
                                         message("N", 0x200, {sig("X", 0, 8)})}));   // same name elsewhere is fine
    INFO(joined(errors));
    CHECK(errors.size() == 1);
    CHECK(has(errors, "M.X: duplicate signal name"));
}

TEST_CASE("signal length 0")
{
    const auto errors = dbc_check(db_of({message("M", 0x100, {sig("Empty", 8, 0)})}));
    INFO(joined(errors));
    CHECK(errors.size() == 1);
    CHECK(has(errors, "M.Empty: length 0"));
}

TEST_CASE("signal bits outside dlc")
{
    SUBCASE("intel")
    {
        const auto errors = dbc_check(db_of({message("M", 0x100, {sig("Late", 60, 8)})}));
        INFO(joined(errors));
        CHECK(errors.size() == 1);
        CHECK(has(errors, "M.Late: bits 60..67 exceed dlc 8"));
    }
    SUBCASE("motorola")
    {
        const auto errors = dbc_check(db_of({message("M", 0x100, {sig("Late", 8, 16, true)}, 2)}));
        INFO(joined(errors));
        CHECK(errors.size() == 1);
        CHECK(has(errors, "M.Late: bits 8..23 exceed dlc 2"));
    }
    SUBCASE("exactly filling the payload is fine")
    {
        CHECK(dbc_check(db_of({message("M", 0x100, {sig("Full", 0, 16), sig("Top", 16, 16, true)}, 4)})).empty());
    }
}

TEST_CASE("overlapping plain signals")
{
    SUBCASE("intel over intel")
    {
        const auto errors = dbc_check(db_of({message("M", 0x100, {sig("A", 0, 16), sig("B", 8, 8)})}));
        INFO(joined(errors));
        CHECK(errors.size() == 1);
        CHECK(has(errors, "M.A: bits 0..15 overlap B (bits 8..15)"));
    }
    SUBCASE("motorola over motorola")
    {
        const auto errors = dbc_check(db_of({message("M", 0x100, {sig("A", 40, 16, true), sig("B", 48, 16, true)})}));
        INFO(joined(errors));
        CHECK(errors.size() == 1);
        CHECK(has(errors, "M.A: bits 40..55 overlap B (bits 48..63)"));
    }
    SUBCASE("intel against motorola in the same byte")
    {
        // Motorola internal 0..3 = DBC 7|4@0 = the high nibble of byte 0; Intel 4|4@1 is the
        // same nibble, Intel 0|4@1 the low nibble.
        const auto clash = dbc_check(db_of({message("M", 0x100, {sig("Hi", 0, 4, true), sig("Also", 4, 4)})}));
        INFO(joined(clash));
        CHECK(clash.size() == 1);
        CHECK(has(clash, "M.Hi: bits 0..3 overlap Also (bits 4..7)"));

        CHECK(dbc_check(db_of({message("M", 0x100, {sig("Hi", 0, 4, true), sig("Lo", 0, 4)})})).empty());
    }
}

TEST_CASE("overlapping multiplexed signals")
{
    SUBCASE("muxed against a plain signal")
    {
        const auto errors = dbc_check(db_of({message("M", 0x100, {muxer("Mode", 0, 8), sig("Plain", 8, 8), muxed("V0", 8, 16, 0)})}));
        INFO(joined(errors));
        CHECK(errors.size() == 1);
        CHECK(has(errors, "M.Plain: bits 8..15 overlap V0 (bits 8..23)"));
    }
    SUBCASE("muxed against the muxer")
    {
        const auto errors = dbc_check(db_of({message("M", 0x100, {muxer("Mode", 0, 8), muxed("V0", 4, 8, 0)})}));
        INFO(joined(errors));
        CHECK(errors.size() == 1);
        CHECK(has(errors, "M.Mode: bits 0..7 overlap V0 (bits 4..11)"));
    }
    SUBCASE("same mux value")
    {
        const auto errors = dbc_check(db_of({message("M", 0x100, {muxer("Mode", 0, 8), muxed("V0", 8, 16, 3), muxed("W0", 16, 8, 3)})}));
        INFO(joined(errors));
        CHECK(errors.size() == 1);
        CHECK(has(errors, "M.V0: bits 8..23 overlap W0 (bits 16..23)"));
    }
    SUBCASE("different mux values may overlap")
    {
        CHECK(dbc_check(db_of({message("M", 0x100, {muxer("Mode", 0, 8), muxed("V0", 8, 16, 0), muxed("V1", 8, 16, 1)})})).empty());
    }
}

TEST_CASE("muxed signal without a multiplexer")
{
    const auto errors = dbc_check(db_of({message("M", 0x100, {sig("Plain", 0, 8), muxed("V2", 8, 8, 2)})}));
    INFO(joined(errors));
    CHECK(errors.size() == 1);
    CHECK(has(errors, "M.V2: multiplexed (m2) but M has no multiplexer"));
}

TEST_CASE("factor 0")
{
    CanDbSignal flat = sig("Flat", 0, 8);
    flat.factor = 0.0;
    const auto errors = dbc_check(db_of({message("M", 0x100, {flat})}));
    INFO(joined(errors));
    CHECK(errors.size() == 1);
    CHECK(has(errors, "M.Flat: factor 0"));
}

TEST_CASE("min greater than max")
{
    CanDbSignal inverted = sig("Inv", 0, 8);
    inverted.min = 10.0;
    inverted.max = 5.0;
    const auto errors = dbc_check(db_of({message("M", 0x100, {inverted})}));
    INFO(joined(errors));
    CHECK(errors.size() == 1);
    CHECK(has(errors, "M.Inv: min 10 > max 5"));

    CanDbSignal equal = sig("Eq", 0, 8);   // min == max (the DBC "unspecified" [0|0]) is fine
    equal.max = 0.0;
    CHECK(dbc_check(db_of({message("M", 0x100, {equal})})).empty());
}

TEST_CASE("float signals of the wrong length")
{
    CanDbSignal f = sig("F", 0, 16);
    f.value_type = SignalValueType::float32;
    CanDbSignal d = sig("D", 16, 32);
    d.value_type = SignalValueType::float64;
    const auto errors = dbc_check(db_of({message("M", 0x100, {f, d})}));
    INFO(joined(errors));
    CHECK(errors.size() == 2);
    CHECK(has(errors, "M.F: float32 needs length 32, has 16"));
    CHECK(has(errors, "M.D: float64 needs length 64, has 32"));

    CanDbSignal ok_f = sig("F", 0, 32);
    ok_f.value_type = SignalValueType::float32;
    CanDbSignal ok_d = sig("D", 0, 64);
    ok_d.value_type = SignalValueType::float64;
    CHECK(dbc_check(db_of({message("A", 0x100, {ok_f}), message("B", 0x200, {ok_d})})).empty());
}
