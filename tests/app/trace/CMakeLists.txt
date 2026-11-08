# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trace_ring)
get_filename_component(zlrs_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
get_target_property(trace_sources zephyr SOURCES)
list(FILTER trace_sources EXCLUDE REGEX "/tracing_backend_ram\\.c$")
set_property(TARGET zephyr PROPERTY SOURCES "${trace_sources}")
target_sources(app PRIVATE
  "${zlrs_root}/tests/app/trace/test_ring.c"
  "${zlrs_root}/tests/app/support/rolling_trace_ram.c"
)
