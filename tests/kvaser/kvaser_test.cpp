#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "drivers/driver.h"
#include "drivers/kvaser_canlib.h"

extern const DriverOps kvaser_driver;

// Without linuxcan's libcanlib the driver must be a quiet no-op: no channels, a failed open, no
// crash. With it (a developer box with the SDK), every function the driver needs must resolve.
TEST_CASE("Kvaser driver with or without libcanlib")
{
    const Canlib* cl = canlib_load();
    CHECK(canlib_load() == cl); // loaded once
    std::vector<IfaceInfo> infos;
    kvaser_driver.enumerate(infos);
    if (cl == nullptr)
    {
        CHECK(infos.empty());
        Iface iface;
        iface.ops = &kvaser_driver;
        iface.info.name = "Kvaser Leaf (ch0)";
        CHECK_FALSE(kvaser_driver.open(iface, IfaceConfig{}));
        return;
    }
    CHECK(cl->canOpenChannel != nullptr);
    CHECK(cl->canSetBusParamsFd != nullptr);
    CHECK(cl->canReadWait != nullptr);
    for (const IfaceInfo& i : infos)
    {
        CHECK(i.name.ends_with(")"));
        CHECK_FALSE(i.bitrates.empty());
    }
}

TEST_CASE("CANlib constants match the public API")
{
    // Spot checks against canlib.h: a wrong value here would open channels with the wrong flags.
    CHECK(canOPEN_ACCEPT_VIRTUAL == 0x20);
    CHECK(canOPEN_CAN_FD == 0x400);
    CHECK(canMSG_EXT == 4);
    CHECK(canFDMSG_FDF == 0x10000);
    CHECK(canBITRATE_500K == -2);
    CHECK(canFD_BITRATE_2M_80P == -1002);
    CHECK(canCHANNELDATA_DEVDESCR_ASCII == 26);
}
