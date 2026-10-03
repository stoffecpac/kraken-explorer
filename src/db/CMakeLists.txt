# db: description databases (CanDb/LinDb model, DBC + PCAN .sym parsers, header-only LDF parser).
add_library(kraken_db STATIC model/can_db.cpp model/lin_db.cpp dbc/dbc_parser.cpp dbc/dbc_writer.cpp dbc/dbc_check.cpp
                        sym/sym_parser.cpp dbf/dbf.cpp)
target_include_directories(kraken_db PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..) # #include "db/model/can_db.h"
target_link_libraries(kraken_db PUBLIC kraken_core)
target_compile_features(kraken_db PUBLIC cxx_std_23) # std::expected in db/ldf/ldf_parser.h
