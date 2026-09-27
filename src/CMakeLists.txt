foreach(m core db decoders drivers ui)
    add_subdirectory(${m})
endforeach()

add_executable(kraken-explorer main.cpp app.cpp)
target_link_libraries(kraken-explorer PRIVATE imgui cangaroo_ui cangaroo_core cangaroo_tasks cangaroo_setup cangaroo_drivers)
