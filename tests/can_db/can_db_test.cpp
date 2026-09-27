// CanDb container behaviour, in particular can_db_update_from().
//
// can_db_update_from() is how a DBC reload lands in a database that the
// measurement setup already points at, so it must reproduce everything the parser
// establishes on a fresh load -- not just the scalar signal attributes. It
// previously copied the per-signal is_muxer flag but never re-established the
// message's muxer, which is what decides whether muxed signals are decoded at all.
//
// Was a Qt Test; doctest now with the same vectors.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "core/bus_message.h"
#include "db/dbc/dbc_parser.h"
#include "db/model/can_db.h"

namespace
{

constexpr std::string_view preamble = R"(VERSION "1.0"

NS_:

BS_:

BU_: ECU_A ECU_B

)";

constexpr std::string_view mux_dbc = R"(BO_ 100 MuxMsg: 8 ECU_A
 SG_ Mode M : 0|4@1+ (1,0) [0|15] "" ECU_B
 SG_ ValueA m0 : 8|8@1+ (1,0) [0|255] "" ECU_B
 SG_ ValueB m1 : 8|8@1+ (1,0) [0|255] "" ECU_B
)";

constexpr std::string_view plain_dbc = R"(BO_ 100 MuxMsg: 8 ECU_A
 SG_ Mode : 0|4@1+ (1,0) [0|15] "" ECU_B
 SG_ ValueA : 8|8@1+ (1,0) [0|255] "" ECU_B
)";

[[nodiscard]] bool parse_into(std::string_view body, CanDb& db)
{
    return dbc_parse(std::string(preamble) + std::string(body), db);
}

// Builds the frame a muxed signal needs in order to be considered present.
[[nodiscard]] BusMessage mux_message(uint32_t raw_id, uint8_t mode)
{
    BusMessage m{.id = raw_id};
    set_length(m, 8);
    m.data[0] = mode;
    return m;
}

[[nodiscard]] CanDbMessage& message(CanDb& db, uint32_t raw_id)
{
    CanDbMessage* msg = can_db_find_message(db, raw_id);
    REQUIRE(msg != nullptr);
    return *msg;
}

[[nodiscard]] CanDbSignal& signal(CanDbMessage& msg, std::string_view name)
{
    CanDbSignal* sig = can_db_find_signal(msg, name);
    REQUIRE(sig != nullptr);
    return *sig;
}

} // namespace

TEST_CASE("update_from copies messages into an empty database")
{
    CanDb source;
    REQUIRE(parse_into(mux_dbc, source));

    CanDb target;
    can_db_update_from(target, source);

    CHECK(target.messages.size() == 1);
    CanDbMessage& msg = message(target, 100);
    CHECK(msg.name == "MuxMsg");
    CHECK(msg.dlc == 8);
    CHECK(msg.signals.size() == 3);
}

TEST_CASE("update_from copies signal attributes")
{
    CanDb source;
    REQUIRE(parse_into(R"(BO_ 200 M: 8 ECU_A
 SG_ Temp : 7|16@0- (0.1,-40) [-40|100] "degC" ECU_B
)", source));

    CanDb target;
    can_db_update_from(target, source);

    const CanDbSignal& sig = signal(message(target, 200), "Temp");
    CHECK(sig.start_bit == signal(message(source, 200), "Temp").start_bit);
    CHECK(sig.length == 16);
    CHECK(sig.factor == doctest::Approx(0.1));
    CHECK(sig.offset == doctest::Approx(-40.0));
    CHECK(sig.unit == "degC");
    CHECK(sig.big_endian);
    CHECK_FALSE(sig.is_unsigned);
}

// Regression: the muxer must be established, not just the is_muxer flag.
TEST_CASE("update_from wires up the muxer on first load")
{
    CanDb source;
    REQUIRE(parse_into(mux_dbc, source));

    CanDb target;
    can_db_update_from(target, source);

    CanDbMessage& msg = message(target, 100);
    const CanDbSignal& mode = signal(msg, "Mode");
    CHECK(mode.is_muxer);
    CHECK(can_db_muxer(msg) == &mode);
}

// A reload where the DBC gained a multiplexer: the message is reused, so the
// muxer has to be attached to the reused signal.
TEST_CASE("update_from wires up a newly added muxer")
{
    CanDb target;
    REQUIRE(parse_into(plain_dbc, target));
    CHECK(can_db_muxer(message(target, 100)) == nullptr);

    CanDb reloaded;
    REQUIRE(parse_into(mux_dbc, reloaded));
    can_db_update_from(target, reloaded);

    CanDbMessage& msg = message(target, 100);
    const CanDbSignal& mode = signal(msg, "Mode");
    CHECK(mode.is_muxer);
    CHECK(can_db_muxer(msg) == &mode);
}

// The reverse: a reload that removed the multiplexer must not leave the message
// pointing at a signal that is no longer a muxer.
TEST_CASE("update_from clears the muxer when it disappears")
{
    CanDb target;
    REQUIRE(parse_into(mux_dbc, target));
    CHECK(can_db_muxer(message(target, 100)) != nullptr);

    CanDb reloaded;
    REQUIRE(parse_into(plain_dbc, reloaded));
    can_db_update_from(target, reloaded);

    CanDbMessage& msg = message(target, 100);
    CHECK_FALSE(signal(msg, "Mode").is_muxer);
    CHECK(can_db_muxer(msg) == nullptr);
}

// The user-visible consequence: without the muxer every muxed signal is reported
// absent and silently vanishes from the trace after a reload.
TEST_CASE("update_from keeps muxed signals decodable")
{
    CanDb source;
    REQUIRE(parse_into(mux_dbc, source));

    CanDb target;
    can_db_update_from(target, source);

    CanDbMessage& msg = message(target, 100);
    const CanDbSignal& value_a = signal(msg, "ValueA");
    const CanDbSignal& value_b = signal(msg, "ValueB");
    CHECK(value_a.is_muxed);
    CHECK(value_b.is_muxed);

    const BusMessage mode0 = mux_message(100, 0);
    CHECK(can_signal_present(msg, value_a, mode0));
    CHECK_FALSE(can_signal_present(msg, value_b, mode0));

    const BusMessage mode1 = mux_message(100, 1);
    CHECK_FALSE(can_signal_present(msg, value_a, mode1));
    CHECK(can_signal_present(msg, value_b, mode1));
}

// Signals are updated in place rather than replaced, which is what lets the
// measurement setup keep raw pointers to them across a reload.
TEST_CASE("update_from reuses existing signal objects")
{
    CanDb target;
    REQUIRE(parse_into(mux_dbc, target));

    const CanDbSignal* before = &signal(message(target, 100), "ValueA");
    const size_t signal_count = message(target, 100).signals.size();

    CanDb reloaded;
    REQUIRE(parse_into(mux_dbc, reloaded));
    can_db_update_from(target, reloaded);

    CHECK(&signal(message(target, 100), "ValueA") == before);
    CHECK(message(target, 100).signals.size() == signal_count);
}

TEST_CASE("find_message returns null for an unknown id")
{
    CanDb db;
    REQUIRE(parse_into(mux_dbc, db));

    CHECK(can_db_find_message(db, 0x999) == nullptr);
}
