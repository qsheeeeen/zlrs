# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
project(zlrs_tests LANGUAGES C)

include(CTest)
add_subdirectory(host)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(BASH_EXECUTABLE bash REQUIRED)

  add_test(NAME architecture COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gates.sh")
  set_tests_properties(architecture PROPERTIES LABELS "architecture" TIMEOUT 30)

  file(GLOB tool_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_*.py")
  foreach(source IN LISTS tool_tests)
    get_filename_component(module "${source}" NAME_WE)
    string(REGEX REPLACE "^test_" "" module "${module}")
    add_test(NAME "tools.${module}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/run.py" --module "${module}")
    set_tests_properties("tools.${module}" PROPERTIES LABELS "tools" TIMEOUT 60)
  endforeach()
endif()
