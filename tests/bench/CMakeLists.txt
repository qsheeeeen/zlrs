# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
project(zlrs_benchmarks LANGUAGES C)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
add_executable(link_bench link.c
  ../support/sha256_soft.c
  ../../core/proto/crypto.c ../../core/proto/frame.c ../../core/proto/fhss.c
  ../../core/proto/mod.c ../../core/region/region.c
  ../../core/link/link.c ../../core/link/pairing.c
  ../../core/link/session.c ../../core/link/data.c)
target_include_directories(link_bench PRIVATE ../../include ../../core/proto)
target_compile_features(link_bench PRIVATE c_std_11)
target_compile_options(link_bench PRIVATE -Wall -Wextra -Werror)
