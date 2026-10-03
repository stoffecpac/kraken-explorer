#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "db/dbc/dbc_parser.h"
#include "db/dbc/dbc_writer.h"
#include "db/dbf/dbf.h"

// BUSMASTER's Tests/AutoIt/Module_AutomationV2/TestData/StdExtDB.dbf, verbatim.
constexpr const char* std_ext_db = R"(//******************************BUSMASTER Messages and signals Database ******************************//

[DATABASE_VERSION] 1.3

[PROTOCOL] CAN

[BUSMASTER_VERSION] [1.6.5]

[NUMBER_OF_MESSAGES] 4

[START_MSG] MsgStdLil,257,8,1,1,S
[START_SIGNALS] Sigstd1,64,1,0,U,-1,0,1,0.000000,1.000000,,
[END_MSG]

[START_MSG] MsgStdBig,258,8,1,1,S
[START_SIGNALS] Sigstd2,64,8,0,I,9223372036854775807,-9223372036854775808,0,3.100000,1.000000,,
[END_MSG]

[START_MSG] MsgExtLil,259,8,1,1,X
[START_SIGNALS] SigExt1,1,1,0,B,1,0,1,0.000000,1.000000,,
[END_MSG]

[START_MSG] MsgExtBig,260,8,1,1,X
[START_SIGNALS] SigExt2,64,8,0,I,9223372036854775807,-9223372036854775808,0,1.800000,1.000000,,
[END_MSG]
)";

TEST_CASE("BUSMASTER's own sample: ids, frame formats, byte orders")
{
    CanDb db;
    std::string error;
    REQUIRE(dbf_parse(std_ext_db, db, &error));
    REQUIRE(db.messages.size() == 4);
    const CanDbMessage* big = can_db_find_message(db, 258);
    REQUIRE(big != nullptr);
    CHECK(big->signals[0].big_endian);
    CHECK(big->signals[0].start_bit == 0); // 64-bit Motorola: LSB in byte 8 -> MSB is the first bit
    CHECK(big->signals[0].offset == doctest::Approx(3.1));
    CHECK(can_db_find_message(db, 259 | 0x80000000u) != nullptr); // X = extended
    const CanDbSignal& b = can_db_find_message(db, 259 | 0x80000000u)->signals[0];
    CHECK(b.is_unsigned);
    CHECK(b.length == 1);
    CHECK(b.max == 1.0);
}

TEST_CASE("app-style value descriptions and older versions")
{
    CanDb db;
    REQUIRE(dbf_parse("[DATABASE_VERSION] 1.3\n[NUMBER_OF_MESSAGES] 1\n[START_MSG] Msg1,85,8,1,1,S\n"
                      "[START_SIGNALS] Sig1,8,1,0,I,127,-128,1,0.000000,1.000000,,\n[VALUE_DESCRIPTION] Test1,22\n"
                      "[VALUE_DESCRIPTION] \"ideal,1\",1\n[END_MSG]\n",
                      db));
    const CanDbSignal& s = db.messages.at(85).signals[0];
    CHECK(s.value_table.at(22) == "Test1");
    CHECK(s.value_table.at(1) == "ideal,1");
    CHECK(s.min == -128.0);
    CHECK(s.max == 127.0);
    // Version 1.2: BUSMASTER loads every signal as Intel.
    REQUIRE(dbf_parse("[DATABASE_VERSION] 1.2\n[START_MSG] M,1,8,1,1,S\n[START_SIGNALS] S,8,2,0,U,255,0,0,0,1,,\n[END_MSG]\n", db));
    CHECK_FALSE(db.messages.at(1).signals[0].big_endian);
}

constexpr const char* dbc = R"(VERSION ""

BU_: ECU Tester

BO_ 291 Status: 8 ECU
 SG_ Speed : 7|12@0+ (0.1,0) [0|409.5] "km/h" Tester
 SG_ Mode : 1|6@0+ (1,0) [0|63] "" Tester
 SG_ Temp : 16|8@1- (0.5,-40) [-104|23.5] "degC" Tester
 SG_ Ratio : 32|32@1- (1,0) [0|0] "" Tester

BO_ 2147484160 Ext: 8 Tester
 SG_ Sel M : 0|8@1+ (1,0) [0|255] "" ECU
 SG_ A m1 : 8|16@1+ (0.001,0) [0|65.535] "V" ECU
 SG_ B m2 : 15|16@0- (1,10) [-32758|32777] "" ECU

CM_ BO_ 291 "Vehicle status";
CM_ SG_ 291 Speed "Wheel speed, filtered";
SIG_VALTYPE_ 291 Ratio : 1;
VAL_ 291 Mode 0 "Off" 1 "On, normal" 2 "Service" ;
)";

TEST_CASE("DBC -> DBF -> CanDb keeps every signal decoding the same")
{
    CanDb a;
    REQUIRE(dbc_parse(dbc, a));
    std::ostringstream out;
    dbf_write(a, out);
    CanDb b;
    std::string error;
    REQUIRE(dbf_parse(out.str(), b, &error));
    REQUIRE(b.messages.size() == a.messages.size());
    for (const auto& [id, ma] : a.messages)
    {
        const CanDbMessage& mb = b.messages.at(id);
        CHECK(mb.name == ma.name);
        CHECK(mb.dlc == ma.dlc);
        CHECK(mb.sender == ma.sender);
        CHECK(mb.comment == ma.comment);
        CHECK(mb.muxer == ma.muxer);
        REQUIRE(mb.signals.size() == ma.signals.size());
        for (std::size_t i = 0; i < ma.signals.size(); ++i)
        {
            const CanDbSignal& sa = ma.signals[i];
            const CanDbSignal& sb = mb.signals[i];
            INFO(sa.name);
            CHECK(sb.name == sa.name);
            CHECK(sb.start_bit == sa.start_bit);
            CHECK(sb.length == sa.length);
            CHECK(sb.big_endian == sa.big_endian);
            CHECK(sb.is_unsigned == sa.is_unsigned);
            CHECK(sb.value_type == sa.value_type);
            CHECK(sb.factor == sa.factor);
            CHECK(sb.offset == sa.offset);
            CHECK(sb.unit == sa.unit);
            CHECK(sb.is_muxer == sa.is_muxer);
            CHECK(sb.is_muxed == sa.is_muxed);
            CHECK(sb.mux_value == sa.mux_value);
            CHECK(sb.comment == sa.comment);
            CHECK(sb.value_table == sa.value_table);
            if (sa.min != 0.0 || sa.max != 0.0)
            {
                CHECK(sb.min == doctest::Approx(sa.min));
                CHECK(sb.max == doctest::Approx(sa.max));
            }
            // Same bits out of a frame.
            BusMessage m{.len = 8};
            for (int k = 0; k < 8; ++k)
            {
                m.data[k] = static_cast<uint8_t>(0x3C + 37 * k);
            }
            CHECK(can_signal_extract_raw(sb, m) == can_signal_extract_raw(sa, m));
        }
    }
    // BUSMASTER's DBC->DBF placement of a Motorola signal: DBC start 1, length 6 -> byte 2, bit 4
    // (its LSB), checked by hand against DBFSignal.cpp.
    CHECK(out.str().find("[START_SIGNALS] Mode,6,2,4,U,63,0,0,0,1,,,") != std::string::npos);
    CHECK(out.str().find("[START_SIGNALS] Speed,12,2,4,U,4095,0,0,0,0.1,km/h,,") != std::string::npos);
    CHECK(out.str().find("[NUMBER_OF_MESSAGES] 2") != std::string::npos);
    CHECK(out.str().find("[START_MSG] Ext,512,8,3,1,X,Tester") != std::string::npos);
}
