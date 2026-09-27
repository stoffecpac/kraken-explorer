// core/setup: <setup> XML in the format the Qt MeasurementSetup/Network/Interface wrote.
//
// The fixture is written by hand from the attribute names and defaults in the Qt
// MeasurementInterface.cpp / MeasurementNetwork.cpp / CanDb::saveXML, not from our own
// output, so a renamed attribute fails here.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "core/setup.h"

namespace
{

const std::string dbc_path = std::string(TEST_DIR) + "/setup_test.dbc";

void write_dbc(unsigned dlc)
{
    std::ofstream(dbc_path) << "VERSION \"\"\n\nNS_:\n\nBS_:\n\nBU_: ECU\n\n"
                            << "BO_ 2147484211 Ext: " << dlc << " ECU\n"   // 0x80000233 = extended 0x233
                            << " SG_ Speed : 0|16@1+ (0.1,0) [0|6553.5] \"km/h\" Vector__XXX\n";
}

std::string fixture()
{
    return R"(<setup>
  <network name="Body">
    <interfaces>
      <interface bus-type="can" type="can" driver="SocketCAN" name="vcan0" configure="1"
                 bitrate="250000" sample-point="800" can-fd="1" bitrate-fd="4000000"
                 sample-point-fd="750" listen-only="1" one-shot="0" triple-sampling="1"
                 auto-restart="1" auto-restart-time="250" is-custom-bitrate="1"
                 is-custom-fdbitrate="0" custom-bitrate="144391" custom-fdbitrate="70920"
                 enabled="0" lin-baudrate="19200" lin-protocol="4" lin-node-mode="0"
                 lin-listen-only="0" lin-checksum-classic="0"
                 lin-ldf-path="" lin-schedule-table="" lin-slave-node=""
                 lin-timebase-ms="5" lin-jitter-us="0"/>
      <interface bus-type="lin" type="lin" driver="LindeAPI" name="lin0" lin-baudrate="10417"
                 lin-protocol="1" lin-node-mode="2" lin-checksum-classic="1"
                 lin-ldf-path="/x/y.ldf" lin-schedule-table="Normal" lin-slave-node="Door"
                 lin-timebase-ms="10" lin-jitter-us="50">
        <lin-frame-defaults>
          <frame id="16" data="0A0BFF"/>
          <frame id="33" data=""/>
        </lin-frame-defaults>
      </interface>
    </interfaces>
    <databases>
      <database db-type="dbc" type="dbc" filename=")" + dbc_path + R"("/>
      <database db-type="dbc" type="dbc" filename="/does/not/exist.dbc"/>
      <database db-type="dbc" filename=""/>
    </databases>
  </network>
  <network/>
</setup>)";
}

Setup load(const std::string& xml)
{
    pugi::xml_document doc;
    REQUIRE(doc.load_string(xml.c_str()));
    Setup s;
    setup_load_xml(s, doc.child("setup"));
    return s;
}

std::string save(const Setup& s)
{
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("setup");
    setup_save_xml(s, root);
    std::ostringstream os;
    doc.save(os);
    return os.str();
}

} // namespace

TEST_CASE("load reads every attribute of the Qt format")
{
    write_dbc(8);
    const Setup s = load(fixture());

    REQUIRE(s.networks.size() == 2);
    const SetupNetwork& n = s.networks[0];
    CHECK(n.name == "Body");
    CHECK(s.networks[1].name == "unnamed network");
    REQUIRE(n.interfaces.size() == 2);

    const SetupInterface& c = n.interfaces[0];
    CHECK(c.driver == "SocketCAN");
    CHECK(c.name == "vcan0");
    CHECK(c.iface == -1);
    CHECK(c.bus_type == BusType::CAN);
    CHECK(c.configure);
    CHECK(c.bitrate == 250000);
    CHECK(c.sample_point == 800);
    CHECK(c.can_fd);
    CHECK(c.fd_bitrate == 4000000);
    CHECK(c.fd_sample_point == 750);
    CHECK(c.listen_only);
    CHECK_FALSE(c.one_shot);
    CHECK(c.triple_sampling);
    CHECK(c.auto_restart);
    CHECK(c.auto_restart_ms == 250);
    CHECK(c.is_custom_bitrate);
    CHECK_FALSE(c.is_custom_fd_bitrate);
    CHECK(c.custom_bitrate == 144391);
    CHECK(c.custom_fd_bitrate == 70920);
    CHECK_FALSE(c.enabled);

    const SetupInterface& l = n.interfaces[1];
    CHECK(l.bus_type == BusType::LIN);
    CHECK_FALSE(l.configure);                 // attribute missing -> "0"
    CHECK(l.enabled);                         // attribute missing -> "1"
    CHECK(l.fd_bitrate == 500000);            // loadXML default, not the constructor's
    CHECK(l.lin_baudrate == 10417);
    CHECK(l.lin_protocol == LinProtocolVersion::V2_0);
    CHECK(l.lin_node_mode == LinNodeMode::Slave);
    CHECK(l.lin_checksum_classic);
    CHECK(l.lin_ldf_path == "/x/y.ldf");
    CHECK(l.lin_schedule_table == "Normal");
    CHECK(l.lin_slave_node == "Door");
    CHECK(l.lin_timebase_ms == 10);
    CHECK(l.lin_jitter_us == 50);
    REQUIRE(l.lin_frame_defaults.size() == 2);
    CHECK(l.lin_frame_defaults.at(16) == std::vector<uint8_t>{0x0a, 0x0b, 0xff});
    CHECK(l.lin_frame_defaults.at(33).empty());

    // Missing and empty filenames are logged and skipped.
    REQUIRE(n.can_dbs.size() == 1);
    CHECK(n.can_dbs[0]->path == dbc_path);
}

TEST_CASE("missing attributes take the MeasurementInterface::loadXML defaults")
{
    const Setup s = load(R"(<setup><network name="n"><interfaces><interface/></interfaces></network></setup>)");
    const SetupInterface& i = s.networks.at(0).interfaces.at(0);
    CHECK(i.bus_type == BusType::CAN);
    CHECK(i.bitrate == 500000);
    CHECK(i.sample_point == 875);
    CHECK(i.fd_sample_point == 875);
    CHECK(i.auto_restart_ms == 100);
    CHECK(i.custom_bitrate == 0);
    CHECK(i.lin_baudrate == 19200);
    CHECK(i.lin_protocol == LinProtocolVersion::V2_2A);
    CHECK(i.lin_node_mode == LinNodeMode::Monitor);
    CHECK(i.lin_timebase_ms == 5);
}

TEST_CASE("save writes the Qt attribute set and round-trips")
{
    write_dbc(8);
    const Setup s = load(fixture());
    const std::string xml = save(s);

    pugi::xml_document doc;
    REQUIRE(doc.load_string(xml.c_str()));
    const pugi::xml_node intf = doc.child("setup").child("network").child("interfaces").child("interface");
    std::set<std::string> names;
    for (const pugi::xml_attribute a : intf.attributes()) names.insert(a.name());
    const std::set<std::string> expected{
        "bus-type", "type", "driver", "name", "configure", "bitrate", "sample-point", "can-fd",
        "bitrate-fd", "sample-point-fd", "listen-only", "one-shot", "triple-sampling",
        "auto-restart", "auto-restart-time", "is-custom-bitrate", "is-custom-fdbitrate",
        "custom-bitrate", "custom-fdbitrate", "enabled", "lin-baudrate", "lin-protocol",
        "lin-node-mode", "lin-listen-only", "lin-checksum-classic",
        "lin-ldf-path", "lin-schedule-table", "lin-slave-node", "lin-timebase-ms", "lin-jitter-us"};
    CHECK(names == expected);
    CHECK(std::string(intf.attribute("bitrate").value()) == "250000");
    CHECK(std::string(intf.attribute("can-fd").value()) == "1");
    CHECK_FALSE(intf.child("lin-frame-defaults"));   // omitted when empty, as before

    const pugi::xml_node lin = intf.next_sibling("interface");
    const pugi::xml_node frame = lin.child("lin-frame-defaults").child("frame");
    CHECK(std::string(frame.attribute("id").value()) == "16");
    CHECK(std::string(frame.attribute("data").value()) == "0A0BFF");

    const pugi::xml_node db = doc.child("setup").child("network").child("databases").child("database");
    CHECK(std::string(db.attribute("db-type").value()) == "dbc");
    CHECK(std::string(db.attribute("filename").value()) == dbc_path);

    CHECK(save(load(xml)) == xml);
}

TEST_CASE("cache finds DBC messages by identifier and survives a reload")
{
    write_dbc(8);
    Setup s = load(fixture());

    BusMessage m{.id = 0x233, .flags = bus_flag::extended};
    const CanDbMessage* msg = setup_find_can_message(s, m);
    REQUIRE(msg);
    CHECK(msg->name == "Ext");
    CHECK(msg->dlc == 8);

    BusMessage err = m;
    err.errors = bus_error::ack;
    CHECK(setup_find_can_message(s, err) == nullptr);
    m.type = BusType::LIN;
    CHECK(setup_find_can_message(s, m) == nullptr);
    m.type = BusType::CAN;

    // A copy (setup dialog) shares the databases.
    const Setup copy = s;
    CHECK(copy.networks[0].can_dbs[0] == s.networks[0].can_dbs[0]);

    write_dbc(4);
    const uint64_t gen = s.generation;
    std::vector<std::string> errors;
    CHECK(setup_reload_databases(s, &errors));
    CHECK(errors.empty());
    CHECK(s.generation > gen);
    CHECK(setup_find_can_message(s, m) == msg);   // updated in place
    CHECK(msg->dlc == 4);

    s.networks[0].can_dbs[0]->path = "/does/not/exist.dbc";
    CHECK_FALSE(setup_reload_databases(s, &errors));
    CHECK(errors.size() == 1);
}

TEST_CASE("find network by name")
{
    Setup s = load(fixture());
    CHECK(setup_find_network(s, "Body") == &s.networks[0]);
    CHECK(setup_find_network(s, "nope") == nullptr);
}

TEST_CASE("DBC lookup is per network and tells standard from extended ids")
{
    const std::string a = std::string(TEST_DIR) + "/setup_net_a.dbc";
    const std::string b = std::string(TEST_DIR) + "/setup_net_b.dbc";
    const std::string head = "VERSION \"\"\n\nNS_:\n\nBS_:\n\nBU_: ECU\n\n";
    std::ofstream(a) << head << "BO_ 256 StdA: 8 ECU\n\n"
                     << "BO_ 2147483904 ExtA: 8 ECU\n";   // 0x80000100 = extended 0x100
    std::ofstream(b) << head << "BO_ 256 StdB: 8 ECU\n";
    Setup s = load(R"(<setup>
  <network name="A"><interfaces><interface bus-type="can" driver="SocketCAN" name="vcan0"/></interfaces>
    <databases><database db-type="dbc" filename=")" + a + R"("/></databases></network>
  <network name="B"><interfaces><interface bus-type="can" driver="SocketCAN" name="vcan1"/></interfaces>
    <databases><database db-type="dbc" filename=")" + b + R"("/></databases></network>
</setup>)");
    s.networks[0].interfaces[0].iface = 0;
    s.networks[1].interfaces[0].iface = 1;

    CHECK(setup_network_of(s, 0) == 0);
    CHECK(setup_network_of(s, 1) == 1);
    CHECK(setup_network_of(s, 2) == -1);

    const auto name = [&](uint16_t iface, bool ext)
    {
        const BusMessage m{.id = 0x100, .flags = ext ? bus_flag::extended : uint16_t{0}, .iface = iface};
        const CanDbMessage* msg = setup_find_can_message(s, m);
        return msg ? msg->name : std::string("-");
    };
    CHECK(name(0, false) == "StdA");
    CHECK(name(1, false) == "StdB");
    CHECK(name(0, true) == "ExtA");
    CHECK(name(1, true) == "-");       // network B has no extended 0x100
    CHECK(name(2, false) == "StdA");   // no network: search all, first wins
    CHECK(name(2, true) == "ExtA");
}
