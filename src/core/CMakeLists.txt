# core/serial: termios wrapper (SLCAN, GrIP).
add_library(cangaroo_serial STATIC serial.cpp)
target_include_directories(cangaroo_serial PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "core/serial.h"

# core: BusMessage POD + log ring buffer. Later core modules add to it with target_sources.
add_library(cangaroo_core STATIC bus_message.cpp log.cpp)
target_include_directories(cangaroo_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "core/bus_message.h"
target_link_libraries(cangaroo_core PUBLIC Threads::Threads)

# core/tasks: main-thread queue. std::move_only_function needs C++23, so only this
# target and its users are raised from the project-wide C++20.
add_library(cangaroo_tasks STATIC tasks.cpp)
target_include_directories(cangaroo_tasks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "core/tasks.h"
target_compile_features(cangaroo_tasks PUBLIC cxx_std_23)
target_link_libraries(cangaroo_tasks PUBLIC Threads::Threads)

# core trace formats: text lines, pcapng blocks, whole export files (std::ostream).
target_sources(cangaroo_core PRIVATE trace_line_format.cpp pcapng.cpp trace_file_writer.cpp)

# core/trace: chunked trace store + per-interface RX inbox (main-thread drain).
target_sources(cangaroo_core PRIVATE trace.cpp png.cpp)

# core/trace_recorder: streaming recording to ASC/candump/pcapng while armed.
target_sources(cangaroo_core PRIVATE trace_recorder.cpp)

# core/setup: networks/interfaces/databases + <setup> XML (pugixml). Separate target
# because it needs cangaroo_db, which itself links cangaroo_core.
add_library(cangaroo_setup STATIC setup.cpp)
target_link_libraries(cangaroo_setup PUBLIC cangaroo_db pugixml::pugixml)

# core/python_engine: embedded Python (pybind11) scripting, the `cangaroo` module. Own
# target: it includes app.h (through the include root) and links the Python runtime.
add_library(cangaroo_python STATIC python_engine.cpp)
target_link_libraries(cangaroo_python PUBLIC cangaroo_tasks cangaroo_drivers PRIVATE pybind11::embed)
