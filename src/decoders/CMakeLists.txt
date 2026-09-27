# decoders: UDS (ISO-TP, LIN diagnostics) and J1939 protocol decoders.
add_library(cangaroo_decoders STATIC uds.cpp j1939.cpp protocol.cpp)
target_include_directories(cangaroo_decoders PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "decoders/decoders.h"
target_link_libraries(cangaroo_decoders PUBLIC cangaroo_core)
