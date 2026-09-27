// LIN Description File parser (src/db/ldf/ldf_parser.h) and the LinDb built from it.
//
// Header-only hand-written recursive descent with unit-suffix handling (ms/us,
// kbps, k/M multipliers) and brace-aware skipping of everything LinDb does not
// consume -- plenty of room for quiet misparses.
//
// Was a Qt Test; doctest now with the same vectors.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "db/ldf/ldf_parser.h"
#include "db/model/lin_db.h"

namespace
{

// A small but broad LDF: every section the parser stores plus the ones it must
// skip (Channel_name, Event_triggered_frames, Diagnostic_frames, array init
// values, subscriber lists, configuration commands), with deliberately varied
// number formats and comment styles.
const std::string sample_ldf = R"(LIN_description_file;
LIN_protocol_version = "2.1";
LIN_language_version = "2.1";
LIN_speed = 19.2 kbps;
Channel_name = "TestChannel";

Nodes {
    Master: MasterNode, 5 ms, 0.1 ms ;
    Slaves: SlaveA, SlaveB, SlaveC ;
}

Signals {
    // scalar init value
    MotorSpeed: 16, 0, MasterNode, SlaveA, SlaveB;
    Switch:      1, 1, SlaveA, MasterNode;
    HexInit:     8, 0x2A, SlaveA, MasterNode;
    /* array init value */
    Payload:    32, {0x11, 0x22, 0x33, 0x44}, SlaveB, MasterNode;
}

Frames {
    MasterFrame: 0x10, MasterNode, 4 {
        MotorSpeed, 0;
        Switch, 16;
    }
    SlaveFrame: 0x20, SlaveA, 8 {
        Payload, 0;
    }
}

Sporadic_frames {
    SporadicGroup: MasterFrame;
}

Event_triggered_frames {
    EventFrame: SchedTable, 0x30, SlaveFrame;
}

Diagnostic_frames {
    MasterReq: 0x3C {
        MotorSpeed, 0;
    }
    SlaveResp: 0x3D {
        Switch, 0;
    }
}

Schedule_tables {
    SchedTable {
        MasterFrame delay 10 ms;
        SlaveFrame delay 20 ms;
        MasterReq delay 10 ms;
        SlaveResp delay 10 ms;
        AssignNAD { SlaveA } delay 15 ms;
        AssignFrameId { SlaveA, SlaveFrame } delay 15 ms;
    }
    SecondTable {
        MasterFrame delay 5 ms;
    }
}

Signal_encoding_types {
    SwitchEncoding {
        logical_value, 0, "off";
        logical_value, 1, "on";
    }
    SpeedEncoding {
        physical_value, 0, 65535, 0.25, -100, "rpm";
    }
}

Signal_representation {
    SwitchEncoding: Switch;
    SpeedEncoding: MotorSpeed;
}
)";

template <class T>
[[nodiscard]] const T* find_by_name(const std::vector<T>& v, std::string_view name)
{
    const auto it = std::ranges::find(v, name, &T::name);
    return it != v.end() ? &*it : nullptr;
}

} // namespace

TEST_CASE("parses header")
{
    const auto result = ldf::parse(sample_ldf);
    REQUIRE(result.has_value());

    CHECK(result->lin_protocol_version == std::string("2.1"));
    CHECK(result->lin_speed_bps == doctest::Approx(19200.0));
}

TEST_CASE("parses nodes")
{
    const auto result = ldf::parse(sample_ldf);
    REQUIRE(result.has_value());

    CHECK(result->nodes.master == std::string("MasterNode"));
    CHECK(result->nodes.slaves.size() == size_t(3));
    CHECK(result->nodes.slaves[0] == std::string("SlaveA"));
    CHECK(result->nodes.slaves[2] == std::string("SlaveC"));

    // "5 ms" and "0.1 ms" are stored in seconds.
    CHECK(result->nodes.master_time_base_s == doctest::Approx(0.005));
    CHECK(result->nodes.master_jitter_s == doctest::Approx(0.0001));
}

TEST_CASE("parses signals")
{
    const auto result = ldf::parse(sample_ldf);
    REQUIRE(result.has_value());
    CHECK(result->signals.size() == size_t(4));

    const ldf::Signal *speed = find_by_name(result->signals, "MotorSpeed");
    REQUIRE(speed != nullptr);
    CHECK(speed->bit_length == 16u);
    CHECK(speed->init_value == 0ULL);
    CHECK(speed->publisher == std::string("MasterNode"));

    // Hexadecimal init values must be read as hex, not decimal.
    const ldf::Signal *hexInit = find_by_name(result->signals, "HexInit");
    REQUIRE(hexInit != nullptr);
    CHECK(hexInit->init_value == 42ULL);
}

// Array init values and subscriber lists are consumed but not stored; the
// fields after them must still parse.
TEST_CASE("skips array init value")
{
    const auto result = ldf::parse(sample_ldf);
    REQUIRE(result.has_value());

    const ldf::Signal *payload = find_by_name(result->signals, "Payload");
    REQUIRE(payload != nullptr);
    CHECK(payload->bit_length == 32u);
    CHECK(payload->init_value == 0ULL);
    CHECK(payload->publisher == std::string("SlaveB"));
}

TEST_CASE("parses frames and signal offsets")
{
    const auto result = ldf::parse(sample_ldf);
    REQUIRE(result.has_value());
    CHECK(result->frames.size() == size_t(2));

    const ldf::Frame *master = find_by_name(result->frames, "MasterFrame");
    REQUIRE(master != nullptr);
    CHECK(master->id == uint8_t(0x10));
    CHECK(master->publisher == std::string("MasterNode"));
    CHECK(master->length == uint8_t(4));
    CHECK(master->signals.size() == size_t(2));
    CHECK(master->signals[0].signal_name == std::string("MotorSpeed"));
    CHECK(master->signals[0].bit_offset == 0u);
    CHECK(master->signals[1].signal_name == std::string("Switch"));
    CHECK(master->signals[1].bit_offset == 16u);

    const ldf::Frame *slave = find_by_name(result->frames, "SlaveFrame");
    REQUIRE(slave != nullptr);
    CHECK(slave->id == uint8_t(0x20));
    CHECK(slave->length == uint8_t(8));
}

// Event_triggered_frames and Diagnostic_frames in the sample are skipped as
// unknown sections (nested braces included) without disturbing what follows.
TEST_CASE("parses sporadic frames")
{
    const auto result = ldf::parse(sample_ldf);
    REQUIRE(result.has_value());

    CHECK(result->sporadic_frames.size() == size_t(1));
    CHECK(result->sporadic_frames[0].name == std::string("SporadicGroup"));
    CHECK(result->sporadic_frames[0].frames.size() == size_t(1));
    CHECK(result->sporadic_frames[0].frames[0] == std::string("MasterFrame"));
}

// Frame entries keep their name; MasterReq / SlaveResp and "Cmd { args }"
// configuration commands keep only the delay and an empty frame_name.
TEST_CASE("parses schedule tables")
{
    const auto result = ldf::parse(sample_ldf);
    REQUIRE(result.has_value());
    CHECK(result->schedule_tables.size() == size_t(2));

    const ldf::ScheduleTable &table = result->schedule_tables[0];
    CHECK(table.name == std::string("SchedTable"));
    REQUIRE(table.entries.size() == size_t(6));

    CHECK(table.entries[0].frame_name == std::string("MasterFrame"));
    CHECK(table.entries[0].delay_s == doctest::Approx(0.010));
    CHECK(table.entries[1].frame_name == std::string("SlaveFrame"));
    CHECK(table.entries[1].delay_s == doctest::Approx(0.020));

    CHECK(table.entries[2].frame_name.empty()); // MasterReq
    CHECK(table.entries[3].frame_name.empty()); // SlaveResp
    CHECK(table.entries[4].frame_name.empty()); // AssignNAD { SlaveA }
    CHECK(table.entries[4].delay_s == doctest::Approx(0.015));
    CHECK(table.entries[5].frame_name.empty()); // AssignFrameId { SlaveA, SlaveFrame }
    CHECK(table.entries[5].delay_s == doctest::Approx(0.015));

    CHECK(result->schedule_tables[1].name == std::string("SecondTable"));
    CHECK(result->schedule_tables[1].entries.size() == size_t(1));
}

TEST_CASE("parses logical encoding")
{
    const auto result = ldf::parse(sample_ldf);
    REQUIRE(result.has_value());
    CHECK(result->signal_encoding_types.size() == size_t(2));

    const ldf::SignalEncodingType &enc = result->signal_encoding_types[0];
    CHECK(enc.name == std::string("SwitchEncoding"));
    CHECK(enc.values.size() == size_t(2));

    CHECK(std::holds_alternative<ldf::LogicalValue>(enc.values[0]));
    const auto &off = std::get<ldf::LogicalValue>(enc.values[0]);
    CHECK(off.signal_value == 0u);
    CHECK(off.text == std::string("off"));

    const auto &on = std::get<ldf::LogicalValue>(enc.values[1]);
    CHECK(on.signal_value == 1u);
    CHECK(on.text == std::string("on"));
}

TEST_CASE("parses physical encoding")
{
    const auto result = ldf::parse(sample_ldf);
    REQUIRE(result.has_value());

    const ldf::SignalEncodingType &enc = result->signal_encoding_types[1];
    CHECK(enc.name == std::string("SpeedEncoding"));
    CHECK(enc.values.size() == size_t(1));

    CHECK(std::holds_alternative<ldf::PhysicalRange>(enc.values[0]));
    const auto &range = std::get<ldf::PhysicalRange>(enc.values[0]);
    CHECK(range.min_value == 0.0);
    CHECK(range.max_value == 65535.0);
    CHECK(range.scale == 0.25);
    CHECK(range.offset == -100.0);
    CHECK(range.unit == std::string("rpm"));
}

TEST_CASE("parses signal representation")
{
    const auto result = ldf::parse(sample_ldf);
    REQUIRE(result.has_value());

    CHECK(result->signal_representation.size() == size_t(2));
    CHECK(result->signal_representation.count("SwitchEncoding") == 1);
    CHECK(result->signal_representation.at("SwitchEncoding").size() == size_t(1));
    CHECK(result->signal_representation.at("SwitchEncoding")[0] == std::string("Switch"));
}

TEST_CASE("speed units")
{
    struct Row
    {
        const char* literal;
        double expected_bps;
    };
    constexpr Row rows[] = {
        {"19.2 kbps", 19200.0},   // kbps decimal
        {"20 kbps", 20000.0},     // kbps integer
        {"9600 bps", 9600.0},     // bps
    };

    for (const Row& r : rows)
    {
        CAPTURE(r.literal);
        const std::string source =
            "LIN_description_file;\nLIN_protocol_version = \"2.1\";\nLIN_speed = " + std::string(r.literal) + ";\n";

        const auto result = ldf::parse(source);
        REQUIRE(result.has_value());
        CHECK(result->lin_speed_bps == doctest::Approx(r.expected_bps));
    }
}

// Times are stored in seconds regardless of the suffix used in the file.
TEST_CASE("time units are normalised to seconds")
{
    const std::string source = R"(LIN_description_file;
LIN_protocol_version = "2.1";
Nodes {
    Master: M, 1000 us, 500 us ;
    Slaves: S ;
}
)";

    const auto result = ldf::parse(source);
    REQUIRE(result.has_value());
    CHECK(result->nodes.master_time_base_s == doctest::Approx(0.001));
    CHECK(result->nodes.master_jitter_s == doctest::Approx(0.0005));
}

// Unknown top-level "key = value;" lines and "{ ... }" sections (nested braces
// included) are skipped; bcd_value / ascii_value encodings are skipped too.
TEST_CASE("tolerates comments and unknown sections")
{
    const std::string source = R"(LIN_description_file;
// line comment
/* block
   comment */
LIN_protocol_version = "2.1";
Some_future_key = "value";
Some_future_section {
    whatever { nested }
}
Signals {
    A: 8, 0, M, S;
}
Signal_encoding_types {
    Enc {
        bcd_value;
        ascii_value;
        logical_value, 1, "one";
    }
}
)";

    const auto result = ldf::parse(source);
    REQUIRE(result.has_value());
    CHECK(result->signals.size() == size_t(1));
    CHECK(result->signals[0].name == std::string("A"));
    REQUIRE(result->signal_encoding_types.size() == size_t(1));
    REQUIRE(result->signal_encoding_types[0].values.size() == size_t(1));
    CHECK(std::holds_alternative<ldf::LogicalValue>(result->signal_encoding_types[0].values[0]));
}

TEST_CASE("rejects file without magic header")
{
    const auto result = ldf::parse("Signals { A: 8, 0, M, S; }\n");
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
}

TEST_CASE("rejects truncated file")
{
    const auto result = ldf::parse(R"(LIN_description_file;
Signals {
    A: 8, 0, M
)");
    CHECK_FALSE(result.has_value());
}

// The parser must report failures through std::expected rather than letting an
// exception escape into the caller.
TEST_CASE("reports error instead of throwing")
{
    bool threw = false;
    try
    {
        const auto result = ldf::parse("LIN_description_file;\nNodes { Master: ");
        CHECK_FALSE(result.has_value());
    }
    catch (...)
    {
        threw = true;
    }

    CHECK_FALSE(threw);
}

// LinDb (db/model/lin_db): the LDF flattened into what the LIN windows use.
// Expected values are read off sample_ldf above, not from parser output.
namespace
{

[[nodiscard]] bool load(const std::string& text, LinDb& db)
{
    const auto path = std::filesystem::temp_directory_path() / "cangaroo_ldf_parser_test.ldf";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    const bool ok = lin_db_load(db, path.string());
    std::filesystem::remove(path);
    return ok;
}

} // namespace

TEST_CASE("lin_db_load flattens frames, signals and encodings")
{
    LinDb db;
    REQUIRE(load(sample_ldf, db));

    CHECK(db.protocol_version == "2.1");
    CHECK(db.speed_bps == doctest::Approx(19200.0));
    CHECK(db.master_node == "MasterNode");
    CHECK(db.slave_nodes.size() == 3);
    CHECK(db.master_timebase_ms == doctest::Approx(5.0));
    CHECK(db.master_jitter_ms == doctest::Approx(0.1));
    CHECK(db.frames.size() == 2);

    const LinFrame* master = lin_db_find_frame(db, "MasterFrame");
    REQUIRE(master != nullptr);
    CHECK(master->id == 0x10);
    CHECK(master->length == 4);
    CHECK(master->publisher == "MasterNode");
    REQUIRE(master->signals.size() == 2);
    CHECK(lin_db_find_frame(db, "NoSuchFrame") == nullptr);

    const LinSignal* speed = lin_frame_find_signal(*master, "MotorSpeed");
    REQUIRE(speed != nullptr);
    CHECK(speed->bit_offset == 0);
    CHECK(speed->bit_length == 16);
    CHECK(speed->publisher == "MasterNode");
    CHECK(speed->factor == doctest::Approx(0.25));
    CHECK(speed->offset == doctest::Approx(-100.0));
    CHECK(speed->max == doctest::Approx(65535.0));
    CHECK(speed->unit == "rpm");

    const LinSignal* sw = lin_frame_find_signal(*master, "Switch");
    REQUIRE(sw != nullptr);
    CHECK(sw->bit_offset == 16);
    CHECK(sw->bit_length == 1);
    CHECK(sw->init_value == 1);
    CHECK(lin_signal_value_name(*sw, 0) == "off");
    CHECK(lin_signal_value_name(*sw, 1) == "on");
    CHECK(lin_signal_value_name(*sw, 2).empty());

    // Little-endian: MotorSpeed = 0x0028 = 40 -> 40 * 0.25 - 100, Switch = bit 16.
    constexpr std::array<uint8_t, 4> payload = {0x28, 0x00, 0x01, 0x00};
    CHECK(lin_signal_extract_raw(*speed, payload) == 40);
    CHECK(lin_signal_extract_physical(*speed, payload) == doctest::Approx(-90.0));
    CHECK(lin_signal_extract_raw(*sw, payload) == 1);
    // Bits past the payload read as 0.
    CHECK(lin_signal_extract_raw(*speed, std::span<const uint8_t>(payload).first(1)) == 0x28);

    // No Node_attributes in the sample: defaults.
    CHECK(lin_db_node_nad(db, "SlaveA") == 0);
    CHECK(lin_db_diag_timing(db, "SlaveA").p2_min_ms == 25);
}

// Only unconditional frames reach the schedule; diagnostic and configuration
// commands are dropped.
TEST_CASE("lin_db_load resolves schedule entries")
{
    LinDb db;
    REQUIRE(load(sample_ldf, db));
    REQUIRE(db.schedule_tables.size() == 2);

    const LinScheduleTable& t = db.schedule_tables[0];
    CHECK(t.name == "SchedTable");
    REQUIRE(t.entries.size() == 2);
    CHECK(t.entries[0].frame_name == "MasterFrame");
    CHECK(t.entries[0].frame_id == 0x10);
    CHECK(t.entries[0].dlc == 4);
    CHECK(t.entries[0].delay_ms == 10);
    CHECK(t.entries[0].is_master_publisher);
    CHECK_FALSE(t.entries[0].is_sporadic);
    CHECK(t.entries[1].frame_name == "SlaveFrame");
    CHECK(t.entries[1].frame_id == 0x20);
    CHECK(t.entries[1].dlc == 8);
    CHECK(t.entries[1].delay_ms == 20);
    CHECK(t.entries[1].publisher_name == "SlaveA");
    CHECK_FALSE(t.entries[1].is_master_publisher);

    CHECK(db.schedule_tables[1].name == "SecondTable");
    CHECK(db.schedule_tables[1].entries.size() == 1);
}

// product_id, response_error and the brace-delimited configurable_frames are
// skipped without swallowing the timing fields that follow them.
TEST_CASE("lin_db_load expands sporadic groups and reads node attributes")
{
    const std::string source = R"(LIN_description_file;
LIN_protocol_version = "2.1";
LIN_speed = 19.2 kbps;
Nodes {
    Master: M, 10 ms, 0 ms ;
    Slaves: S ;
}
Signals {
    A: 8, 0, M, S;
    B: 8, 0, S, M;
}
Frames {
    FA: 0x01, M, 1 { A, 0; }
    FB: 0x02, S, 1 { B, 0; }
}
Sporadic_frames {
    Group: FA, FB;
}
Node_attributes {
    S {
        LIN_protocol = "2.1";
        configured_NAD = 0x05;
        product_id = 0x1234, 0x5678, 1;
        response_error = B;
        configurable_frames {
            FB = 0x02;
            FA;
        }
        P2_min = 50 ms;
        ST_min = 10 ms;
        N_As_timeout = 1000 ms;
        N_Cr_timeout = 2000 ms;
    }
}
Schedule_tables {
    T {
        Group delay 15 ms;
    }
}
)";

    LinDb db;
    REQUIRE(load(source, db));
    REQUIRE(db.schedule_tables.size() == 1);
    const auto& entries = db.schedule_tables[0].entries;
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].frame_name == "FA");
    CHECK(entries[0].frame_id == 0x01);
    CHECK(entries[0].is_sporadic);
    CHECK(entries[0].is_master_publisher);
    CHECK(entries[0].delay_ms == 15);
    CHECK(entries[1].frame_name == "FB");
    CHECK(entries[1].frame_id == 0x02);
    CHECK(entries[1].is_sporadic);
    CHECK_FALSE(entries[1].is_master_publisher);

    CHECK(lin_db_node_nad(db, "S") == 0x05);
    CHECK(lin_db_diag_timing(db, "S").p2_min_ms == 50);
    CHECK(lin_db_diag_timing(db, "S").st_min_ms == 10);
    CHECK(lin_db_diag_timing(db, "S").n_as_ms == 1000);
    CHECK(lin_db_diag_timing(db, "S").n_cr_ms == 2000);
}

TEST_CASE("lin_db_load keeps the database on failure")
{
    LinDb db;
    REQUIRE(load(sample_ldf, db));

    CHECK_FALSE(lin_db_load(db, "/nonexistent/cangaroo.ldf"));
    CHECK_FALSE(db.last_error.empty());
    CHECK(db.frames.size() == 2);
    CHECK(db.master_node == "MasterNode");
}
