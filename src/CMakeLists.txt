foreach(m core db decoders drivers ui)
    add_subdirectory(${m})
endforeach()

add_executable(kraken-explorer main.cpp app.cpp)
target_link_libraries(kraken-explorer PRIVATE imgui kraken_ui kraken_core kraken_tasks kraken_setup kraken_drivers)
