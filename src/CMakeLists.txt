foreach(m core db decoders drivers ui)
    add_subdirectory(${m})
endforeach()

# app.cpp (per-frame flow, measurement start/stop) and core/rest_api.cpp (HTTP control, calls it):
# a library so the tests reach them too.
add_library(kraken_app STATIC app.cpp core/rest_api.cpp)
target_link_libraries(kraken_app PUBLIC kraken_ui kraken_python)

add_executable(kraken-explorer main.cpp)
target_link_libraries(kraken-explorer PRIVATE imgui kraken_app)
