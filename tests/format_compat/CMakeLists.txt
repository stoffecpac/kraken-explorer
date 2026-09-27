# Sample generator for validate.py, not a unit test: the format-compat CI job runs
# it and validate.py, ctest does not.
add_executable(trace_format_samples main.cpp)
target_link_libraries(trace_format_samples PRIVATE kraken_core)
