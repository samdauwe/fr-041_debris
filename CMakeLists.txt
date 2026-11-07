cmake_minimum_required(VERSION 3.25)

project(debris
  VERSION 1.0.0
  DESCRIPTION "Debris (Farbrausch fr-041) native player: C99 libraries, SDL3 and OpenGL ES 3"
  LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(EMSCRIPTEN)
  set(CMAKE_C_EXTENSIONS ON) # EM_ASM, and SDL3's own use of it, do not compile in -std=c99
endif()
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- options -----------------------------------------------------------------------------------------------------------------
set(_is_root OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_is_root ON)
endif()
# the tools and the tests are native programs; the browser build is the player only
set(_native_default ${_is_root})
if(EMSCRIPTEN)
  set(_native_default OFF)
endif()

option(DEBRIS_BUILD_PLAYER "Build the SDL3 player (debris)" ON)
option(DEBRIS_BUILD_TOOLS "Build the command-line tools (v2_render, wz3_inventory, v2m_tool)" ${_native_default})
option(DEBRIS_BUILD_TESTS "Build the test runner (debris_tests) and register it with CTest" ${_native_default})
option(DEBRIS_FETCH_SDL3 "Fetch and build SDL3 when no installed SDL3 >= 3.2 is found" ON)
set(DEBRIS_SDL3_TAG "release-3.4.8" CACHE STRING "SDL3 tag used when it is fetched")
set(DEBRIS_WEB_MAX_MEMORY "2GB" CACHE STRING "Emscripten builds: the largest the WebAssembly memory may grow to")
option(DEBRIS_V2_RONAN "Include the Ronan speech synthesizer (V2 channel 15)" ON)
option(DEBRIS_BUILD_FUZZERS "Build the libFuzzer harnesses (Clang); each also replays its seed as a CTest test" OFF)
option(DEBRIS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DEBRIS_TEST_FULL_SIZE "Also run the full-size (texture offset 0) comparisons" ON)
option(DEBRIS_TEST_SLOW "Also compare the full-length (18.9M frame) party song with the V2 oracle (needs DEBRIS_REFERENCE_DIR)" OFF)
option(DEBRIS_TEST_SOAK "Add the long soak test (the whole demo repeatedly, ten minutes of wall time)" OFF)
set(DEBRIS_REFERENCE_DIR "" CACHE PATH
  "Checkout of the reference repository (fr-041_debris): enables the optional comparisons against the C++ oracles and the JavaScript port")

include(DebrisCompile)

# ---- libraries -----------------------------------------------------------------------------------------------------------------
# The libraries are single headers in the style of stb: define <NAME>_IMPLEMENTATION in exactly one translation unit
# (see doc/ARCHITECTURE.md). They only carry usage requirements. Bit-exact results need strict binary32 arithmetic, so
# every consumer is compiled without floating-point contraction.
add_library(debris_headers INTERFACE)
add_library(debris::headers ALIAS debris_headers)
target_include_directories(debris_headers INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(debris_headers INTERFACE c_std_99)
if(NOT DEBRIS_V2_RONAN)
  target_compile_definitions(debris_headers INTERFACE V2_RONAN=0)
endif()
if(MSVC)
  target_compile_options(debris_headers INTERFACE /fp:precise)
else()
  target_compile_options(debris_headers INTERFACE -ffp-contract=off)
  target_link_libraries(debris_headers INTERFACE m)
endif()

# ---- licenses ----------------------------------------------------------------------------------------------------------------
include(EmbedLicenses)
# The notices of everything the player is built from, embedded in the executable (debris --licenses). The header is
# generated into the build tree; the player and its tests include it.
set(_licenses ${PROJECT_SOURCE_DIR}/third_party/licenses)
debris_embed_licenses(${PROJECT_BINARY_DIR}/generated/licenses_data.h
  "werkkzeug3 operator library (Farbrausch fr_public), BSD license" ${_licenses}/werkkzeug3-LICENSE.txt
  "V2 synthesizer (v2redux, after the original Farbrausch V2), public domain" ${_licenses}/v2redux-LICENSE.txt
  "GLU tessellator (libtess.js, SGI sample implementation), SGI Free Software License B" ${_licenses}/libtess-LICENSE.txt
  "Arimo and Gelasio outlines (text and Font3D glyph tables), SIL Open Font License 1.1" ${_licenses}/font3d-LICENSE.txt
  "Production data and music, Creative Commons Attribution 3.0" ${PROJECT_SOURCE_DIR}/assets/ATTRIBUTION.md
  "SDL3, zlib license" ${_licenses}/SDL3-NOTICE.txt)

# ---- player --------------------------------------------------------------------------------------------------------------------
set(DEBRIS_PLAYER_AVAILABLE OFF)
if(DEBRIS_BUILD_PLAYER)
  include(DebrisPlayer)
endif()

# ---- tools ---------------------------------------------------------------------------------------------------------------------
if(DEBRIS_BUILD_TOOLS OR DEBRIS_BUILD_TESTS)
  foreach(tool IN ITEMS v2_render wz3_inventory v2m_tool)
    add_executable(${tool} tools/${tool}.c)
    target_link_libraries(${tool} PRIVATE debris::headers)
    debris_configure_target(${tool})
    set_target_properties(${tool} PROPERTIES FOLDER tools)
  endforeach()
endif()

# ---- tests ---------------------------------------------------------------------------------------------------------------------
if(DEBRIS_BUILD_TESTS)
  enable_testing()
  include(DebrisTests)
endif()

if(DEBRIS_BUILD_FUZZERS)
  enable_testing()
  include(DebrisFuzzers)
endif()

# ---- formatting ----------------------------------------------------------------------------------------------------------------
# `cmake --build <dir> --target format` applies .clang-format; `format-check` fails when a file does not conform. The libraries of
# include/debris/core keep the layout of their reference (see the .clang-format next to them) and are left alone.
find_program(DEBRIS_CLANG_FORMAT clang-format)
if(DEBRIS_CLANG_FORMAT AND _is_root)
  file(GLOB_RECURSE _formatted CONFIGURE_DEPENDS
    include/debris/*.h src/*.c tests/*.c tests/*.h tools/*.c)
  add_custom_target(format COMMAND ${DEBRIS_CLANG_FORMAT} -i ${_formatted}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} COMMENT "Formatting the sources" VERBATIM)
  add_custom_target(format-check COMMAND ${DEBRIS_CLANG_FORMAT} --dry-run -Werror ${_formatted}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} COMMENT "Checking the formatting of the sources" VERBATIM)
endif()

# ---- install -------------------------------------------------------------------------------------------------------------------
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h" PATTERN "*.inc")
if(TARGET v2_render)
  install(TARGETS v2_render RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
