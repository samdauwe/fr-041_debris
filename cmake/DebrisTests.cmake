# The test runner (tests/test_main.c + tests/suites/*.c) and its registration with CTest. Included from the top-level
# CMakeLists.txt when DEBRIS_BUILD_TESTS is ON.
#
#   ./debris_tests            runs every suite, a few at a time, and prints one line per suite
#   ctest                     the same suites, one CTest test each (tests/suites.def is the single list)
#   cmake --build . --target check
#
# The comparisons with the reference implementation (C++ oracles, the JavaScript port, Chrome) are optional: see DebrisOracles.cmake.

set(DEBRIS_TEST_DIR ${PROJECT_SOURCE_DIR}/tests)
set(DEBRIS_TEST_WORK_DIR ${PROJECT_BINARY_DIR}/tests/work)
file(MAKE_DIRECTORY ${DEBRIS_TEST_WORK_DIR})

file(GLOB _suite_sources CONFIGURE_DEPENDS ${DEBRIS_TEST_DIR}/suites/*.c)

# The suites that draw through a real OpenGL ES driver need EGL; without it they are replaced by stubs that skip.
find_library(DEBRIS_EGL_LIBRARY EGL)
find_library(DEBRIS_GLES_LIBRARY GLESv2)
find_path(DEBRIS_EGL_INCLUDE_DIR EGL/egl.h)
find_path(DEBRIS_GLES_INCLUDE_DIR GLES3/gl3.h)
set(_have_egl OFF)
if(DEBRIS_EGL_LIBRARY AND DEBRIS_GLES_LIBRARY AND DEBRIS_EGL_INCLUDE_DIR AND DEBRIS_GLES_INCLUDE_DIR AND NOT MSVC)
  set(_have_egl ON)
else()
  message(STATUS "debris tests: EGL/GLES development files not found; the OpenGL ES driver suites will skip")
  list(FILTER _suite_sources EXCLUDE REGEX "/(glr_egl|glr_egl_frames|player_scene_egl)\\.c$")
  list(APPEND _suite_sources ${DEBRIS_TEST_DIR}/support/egl_unavailable.c)
endif()

add_executable(debris_tests ${DEBRIS_TEST_DIR}/test_main.c ${_suite_sources})
target_include_directories(debris_tests PRIVATE ${DEBRIS_TEST_DIR} ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(debris_tests PRIVATE debris::headers)
debris_configure_target(debris_tests)
set_target_properties(debris_tests PROPERTIES FOLDER tests)

# Every suite gets its own copy of the libraries (static functions), so that white-box suites can use the internals and no two
# suites share state. The linkage suite undefines these to test the shared layout.
target_compile_definitions(debris_tests PRIVATE
  DEBRIS_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
  DEBRIS_TEST_DATA_DIR="${DEBRIS_TEST_DIR}/data"
  DEBRIS_TEST_WORK_DIR="${DEBRIS_TEST_WORK_DIR}"
  V2_STATIC WZ3_STATIC GLR_STATIC
  PLAYER_CORE_STATIC PLAYER_ARGS_STATIC PLAYER_CREDITS_STATIC PLAYER_GLCHECK_STATIC
  PLAYER_LOAD_STATIC PLAYER_REPLAY_STATIC PLAYER_SCENE_STATIC PLAYER_TEXT_STATIC)
if(NOT DEBRIS_TEST_FULL_SIZE)
  target_compile_definitions(debris_tests PRIVATE DEBRIS_TEST_SKIP_FULL_SIZE)
endif()
if(DEBRIS_TEST_SOAK)
  target_compile_definitions(debris_tests PRIVATE DEBRIS_TEST_SOAK)
endif()
if(NOT MSVC)
  # a suite includes only what it needs of the libraries, so some static functions are unused in it
  target_compile_options(debris_tests PRIVATE -Wno-unused-function)
  target_compile_definitions(debris_tests PRIVATE _POSIX_C_SOURCE=200809L)
endif()
if(DEBRIS_SANITIZE)
  target_compile_definitions(debris_tests PRIVATE DEBRIS_TEST_LSAN_SUPP="${DEBRIS_TEST_DIR}/lsan.supp" DEBRIS_TEST_SLOW_BUILD)
endif()
if(_have_egl)
  target_include_directories(debris_tests PRIVATE ${DEBRIS_EGL_INCLUDE_DIR} ${DEBRIS_GLES_INCLUDE_DIR})
  target_link_libraries(debris_tests PRIVATE ${DEBRIS_EGL_LIBRARY} ${DEBRIS_GLES_LIBRARY})
endif()
if(DEBRIS_PLAYER_AVAILABLE)
  target_compile_definitions(debris_tests PRIVATE DEBRIS_PLAYER_EXE="$<TARGET_FILE:debris>")
  add_dependencies(debris_tests debris)
endif()

add_custom_target(check COMMAND debris_tests DEPENDS debris_tests USES_TERMINAL
  COMMENT "Running every test suite")

# ---- CTest: one test per entry of suites.def ---------------------------------------------------------------------------------------
file(STRINGS ${DEBRIS_TEST_DIR}/suites.def _suite_lines REGEX "^SUITE\\(")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DEBRIS_TEST_DIR}/suites.def)
set(_timeout_scale 1)
if(DEBRIS_SANITIZE)
  set(_timeout_scale 4)
endif()
foreach(_line IN LISTS _suite_lines)
  if(NOT _line MATCHES "^SUITE\\(\"([a-z0-9_]+)\", *[a-z0-9_]+, *([0-9]+), *([A-Z_0-9]+)")
    message(FATAL_ERROR "tests/suites.def: cannot read the entry: ${_line}")
  endif()
  set(_name ${CMAKE_MATCH_1})
  set(_limit ${CMAKE_MATCH_2})
  set(_flags ${CMAKE_MATCH_3})
  if(_name STREQUAL "player_soak_long" AND NOT DEBRIS_TEST_SOAK)
    continue()
  endif()
  math(EXPR _limit "${_limit} * ${_timeout_scale} + 60")
  add_test(NAME ${_name} COMMAND debris_tests ${_name})
  set(_labels unit)
  if(_name MATCHES "egl")
    set(_labels gl)
  elseif(_name MATCHES "^player_" AND _line MATCHES "suite_player_cli")
    set(_labels player)
  endif()
  set_tests_properties(${_name} PROPERTIES
    TIMEOUT ${_limit} LABELS "${_labels}" SKIP_RETURN_CODE 77
    ENVIRONMENT "DEBRIS_TEST_TIMEOUT_SCALE=${_timeout_scale}")
  if(_flags MATCHES "SUITE_SERIAL")
    set_tests_properties(${_name} PROPERTIES RUN_SERIAL TRUE)
  endif()
endforeach()

# ---- static checks on the sources -----------------------------------------------------------------------------------------------------
# no mutable global state in the headers (the libraries are reentrant: every instance owns its state)
file(GLOB_RECURSE _library_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/debris/core/*.h ${PROJECT_SOURCE_DIR}/include/debris/core/*.inc)
list(JOIN _library_headers "|" _library_headers_arg)
add_test(NAME headers_no_mutable_statics
  COMMAND ${CMAKE_COMMAND} "-DHEADERS=${_library_headers_arg}" -P ${PROJECT_SOURCE_DIR}/cmake/scripts/check_no_mutable_statics.cmake)
set_tests_properties(headers_no_mutable_statics PROPERTIES LABELS unit)

# the libraries do their own transcendental math (bit-exact against the reference): the executable must not import any from libm
find_program(DEBRIS_NM nm)
if(DEBRIS_NM AND NOT WIN32 AND NOT APPLE)
  add_test(NAME no_libm_imports
    COMMAND ${CMAKE_COMMAND} -DNM=${DEBRIS_NM} -DEXE=$<TARGET_FILE:debris_tests> -P ${PROJECT_SOURCE_DIR}/cmake/scripts/check_no_libm.cmake)
  set_tests_properties(no_libm_imports PROPERTIES LABELS unit)
endif()

include(DebrisOracles)
