# drivers: DriverOps tables + Iface/listener threads. A new driver adds its .cpp with
# target_sources and its DriverOps to the list in driver.cpp.
add_library(kraken_drivers STATIC driver.cpp)
target_include_directories(kraken_drivers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "drivers/driver.h"
target_link_libraries(kraken_drivers PUBLIC kraken_setup Threads::Threads)

target_sources(kraken_drivers PRIVATE slcan.cpp)
target_sources(kraken_drivers PRIVATE grip.cpp)
target_link_libraries(kraken_drivers PRIVATE kraken_tasks) # grip: GPIO/caps via core/tasks
target_link_libraries(kraken_drivers PRIVATE kraken_serial)

target_sources(kraken_drivers PRIVATE canblast.cpp)

target_sources(kraken_drivers PRIVATE socketcan.cpp)
target_link_libraries(kraken_drivers PRIVATE PkgConfig::LIBNL)

# Vendor interfaces of the 1d50:606f composite adapter: lin_usb (LindeAPI) + aio_usb (aiode GPIO)
target_sources(kraken_drivers PRIVATE linde/linde.cpp aiode/aiode.cpp usb_vendor/usb_vendor_libusb.cpp)
target_link_libraries(kraken_drivers PRIVATE PkgConfig::LIBUSB)

# Kvaser (linuxcan CANlib), opt-in because the SDK is not redistributable.
option(KRAKEN_KVASER "Kvaser driver (needs linuxcan's canlib.h + libcanlib)" OFF)

if(KRAKEN_KVASER)
    find_path(KVASER_INCLUDE canlib.h REQUIRED)
    find_library(KVASER_LIB canlib REQUIRED)
    target_sources(kraken_drivers PRIVATE kvaser.cpp)
    target_include_directories(kraken_drivers PRIVATE ${KVASER_INCLUDE})
    target_link_libraries(kraken_drivers PRIVATE ${KVASER_LIB})
    target_compile_definitions(kraken_drivers PRIVATE KRAKEN_KVASER)
endif()
