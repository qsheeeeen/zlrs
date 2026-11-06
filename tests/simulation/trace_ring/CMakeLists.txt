# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trace_ring)
get_target_property(trace_sources zephyr SOURCES)
list(FILTER trace_sources EXCLUDE REGEX "/tracing_backend_ram\\.c$")
set_property(TARGET zephyr PROPERTY SOURCES "${trace_sources}")
target_sources(app PRIVATE test_ring.c ../../support/rolling_trace_ram.c)
