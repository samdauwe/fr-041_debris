# Optional comparisons with the reference implementation. They run in CTest only, never in debris_tests, because they need things
# that are not part of this repository: the reference checkout (C++ sources of the original, the JavaScript port, the tools that
# generate the fixtures), a C++ compiler, node and Chrome. Each part is skipped quietly when its prerequisites are missing.
#
#   -DDEBRIS_REFERENCE_DIR=/path/to/fr-041_debris     (found automatically when this project sits inside it)
#
# Included from DebrisTests.cmake.

# ---- Chrome pictures against the player's pictures (needs node; nothing from the reference) ----------------------------------------
find_program(DEBRIS_NODE node)
if(DEBRIS_NODE AND DEBRIS_PLAYER_AVAILABLE AND NOT WIN32)
  foreach(_shape IN ITEMS 320x180 270x270)
    set(_lsan "")
    if(DEBRIS_SANITIZE)
      set(_lsan "-DLSAN_OPTIONS=suppressions=${DEBRIS_TEST_DIR}/lsan.supp")
    endif()
    add_test(NAME golden_${_shape}
      COMMAND ${CMAKE_COMMAND} -DDEBRIS=$<TARGET_FILE:debris> -DNODE=${DEBRIS_NODE}
              -DCOMPARE=${PROJECT_SOURCE_DIR}/tools/compare_goldens.mjs
              -DGOLDENS=${DEBRIS_TEST_DIR}/data/goldens/${_shape} -DOUT=${DEBRIS_TEST_WORK_DIR}/goldens_${_shape}
              -DSIZE=${_shape} -DTEXTURE=-2 ${_lsan} -P ${PROJECT_SOURCE_DIR}/cmake/scripts/run_goldens.cmake)
    set_tests_properties(golden_${_shape} PROPERTIES LABELS "player;golden;node" TIMEOUT 900 SKIP_REGULAR_EXPRESSION "SKIP:")
  endforeach()
endif()

# ---- the reference checkout ---------------------------------------------------------------------------------------------------------
if(NOT DEBRIS_REFERENCE_DIR AND EXISTS ${PROJECT_SOURCE_DIR}/../vendor/v2redux/src/v2core.cpp)
  get_filename_component(DEBRIS_REFERENCE_DIR ${PROJECT_SOURCE_DIR}/.. ABSOLUTE)
endif()
if(NOT DEBRIS_REFERENCE_DIR)
  message(STATUS "debris tests: no DEBRIS_REFERENCE_DIR; the comparisons with the C++ oracles and the JavaScript port are off")
  return()
endif()
set(_ref ${DEBRIS_REFERENCE_DIR})
set(_oracles ${_ref}/tools/oracles)
set(_scripts ${PROJECT_SOURCE_DIR}/cmake/scripts)
set(_work ${DEBRIS_TEST_WORK_DIR})
message(STATUS "debris tests: reference checkout ${_ref}")

# the fixtures of tests/data are copies of what the reference's tools generate: report drift
add_test(NAME fixtures_match_reference
  COMMAND ${CMAKE_COMMAND} -DOURS=${DEBRIS_TEST_DIR}/data -DREFERENCE=${_ref}/native/tests/wz3/fixtures
          -P ${_scripts}/check_fixtures.cmake)
set_tests_properties(fixtures_match_reference PROPERTIES LABELS "oracle" SKIP_REGULAR_EXPRESSION "SKIP:")

# ---- checkers (C): the same libraries, read what an oracle printed ------------------------------------------------------------------------
# These are ordinary executables with their own copy of the implementation, not part of the runner.
function(debris_oracle_checker name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE debris::headers)
  target_include_directories(${name} PRIVATE ${DEBRIS_TEST_DIR})
  target_compile_definitions(${name} PRIVATE
    DEBRIS_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets" DEBRIS_TEST_DATA_DIR="${DEBRIS_TEST_DIR}/data" ${ARGN})
  if(NOT MSVC)
    target_compile_definitions(${name} PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(${name} PRIVATE -Wno-unused-function)
  endif()
  debris_configure_target(${name})
  set_target_properties(${name} PROPERTIES FOLDER tests/oracle)
endfunction()
# the dxt5 and text suites double as checkers when they are given the output of the reference: compile them with a main()
debris_oracle_checker(check_wz3 ${DEBRIS_TEST_DIR}/oracle/wz3_oracle_check.c)
debris_oracle_checker(check_shadow ${DEBRIS_TEST_DIR}/oracle/wz3_shadow_oracle_check.c)
debris_oracle_checker(check_dxt5 ${DEBRIS_TEST_DIR}/suites/wz3_dxt5.c suite_wz3_dxt5=main)
debris_oracle_checker(check_text ${DEBRIS_TEST_DIR}/suites/wz3_text.c suite_wz3_text=main)
debris_oracle_checker(check_detmath ${DEBRIS_TEST_DIR}/oracle/wz3_detmath_check.c)
debris_oracle_checker(player_pcm ${DEBRIS_TEST_DIR}/oracle/player_pcm.c)

# ---- the JavaScript port and Chrome (need node) -------------------------------------------------------------------------------------
if(DEBRIS_NODE)
  add_test(NAME detmath_v8
    COMMAND ${CMAKE_COMMAND} -DNODE=${DEBRIS_NODE} -DSCRIPT=${_ref}/tools/dump_math_sweep.mjs
            -DCHECKER=$<TARGET_FILE:check_detmath> -DWORK=${_work} -P ${_scripts}/run_detmath.cmake)
  set_tests_properties(detmath_v8 PROPERTIES LABELS "oracle;node")
  find_program(DEBRIS_CHROME NAMES google-chrome chromium chromium-browser chrome)
  if(DEBRIS_CHROME AND NOT WIN32)
    foreach(_offset IN ITEMS -2 0)
      add_test(NAME text_browser_${_offset}
        COMMAND ${CMAKE_COMMAND} -E env CHROME=${DEBRIS_CHROME}
                ${CMAKE_COMMAND} -DNODE=${DEBRIS_NODE} -DSCRIPT=${_ref}/tools/dump_text_reference.mjs
                -DCHECKER=$<TARGET_FILE:check_text> -DASSETS=${PROJECT_SOURCE_DIR}/assets -DFIXTURES=${DEBRIS_TEST_DIR}/data
                -DWORK=${_work} -DOFFSET=${_offset} -P ${_scripts}/run_text_browser.cmake)
      set_tests_properties(text_browser_${_offset} PROPERTIES LABELS "oracle;browser;node" TIMEOUT 300)
    endforeach()
  endif()
else()
  message(STATUS "debris tests: node not found; the comparisons with the JavaScript port are off")
endif()

# ---- the C++ sources of the original (need a C++ compiler) ----------------------------------------------------------------------------
include(CheckLanguage)
check_language(CXX)
if(NOT CMAKE_CXX_COMPILER)
  message(STATUS "debris tests: no C++ compiler; the comparisons with the C++ oracles are off")
  return()
endif()
enable_language(CXX)

# an oracle is only a reference if it keeps strict binary32 arithmetic
function(debris_cxx_oracle name)
  add_executable(${name} ${ARGN})
  target_compile_features(${name} PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(${name} PRIVATE /fp:precise)
  else()
    target_compile_options(${name} PRIVATE -O2 -ffp-contract=off)
  endif()
  set_target_properties(${name} PROPERTIES FOLDER tests/oracle)
endfunction()

# bitmap operators, legacy lookup textures
foreach(_entry IN ITEMS blur:bitmap_blur_oracle cell:bitmap_cell_oracle perlin:perlin_oracle misc:bitmap_misc_oracle
                        lookup:legacy_lookup_oracle)
  string(REPLACE ":" ";" _parts ${_entry})
  list(GET _parts 0 _kind)
  list(GET _parts 1 _source)
  debris_cxx_oracle(oracle_${_kind} ${_oracles}/${_source}.cpp)
  add_test(NAME oracle_wz3_${_kind}
    COMMAND ${CMAKE_COMMAND} -DORACLE=$<TARGET_FILE:oracle_${_kind}> -DCHECKER=$<TARGET_FILE:check_wz3>
            -DKIND=${_kind} -DWORK=${_work} -P ${_scripts}/run_oracle.cmake)
  set_tests_properties(oracle_wz3_${_kind} PROPERTIES LABELS oracle)
endforeach()

# BC3 compression (released rygdxt source, unmodified)
debris_cxx_oracle(oracle_dxt5 ${_oracles}/dxt5_oracle.cpp ${_ref}/vendor/wz3/base/_rygdxt.cpp)
target_include_directories(oracle_dxt5 PRIVATE ${_oracles}/rygdxt_shim ${_ref}/vendor/wz3/base)
if(NOT MSVC)
  target_compile_options(oracle_dxt5 PRIVATE -w)
endif()
add_test(NAME oracle_wz3_dxt5
  COMMAND ${CMAKE_COMMAND} -DORACLE=$<TARGET_FILE:oracle_dxt5> -DCHECKER=$<TARGET_FILE:check_dxt5>
          -DFIXTURES=${DEBRIS_TEST_DIR}/data -DWORK=${_work} -P ${_scripts}/run_dxt5.cmake)
set_tests_properties(oracle_wz3_dxt5 PROPERTIES LABELS oracle)

# shadow volumes (EngMesh::UpdateShadowCacheJob)
debris_cxx_oracle(oracle_shadow ${_oracles}/shadow_volume_oracle.cpp)
add_test(NAME oracle_wz3_shadow
  COMMAND ${CMAKE_COMMAND} -DORACLE=$<TARGET_FILE:oracle_shadow> -DCHECKER=$<TARGET_FILE:check_shadow> -DWORK=${_work}
          -P ${_scripts}/run_shadow_oracle.cmake)
set_tests_properties(oracle_wz3_shadow PROPERTIES LABELS oracle)

# the V2 synthesizer: PCM must match v2redux bit for bit, from v2.h directly and through the player's audio layer
set(_v2src ${_ref}/vendor/v2redux/src)
set(_v2_sources ${_oracles}/v2_render_oracle.cpp ${_v2src}/v2core.cpp ${_v2src}/v2seq.cpp ${_v2src}/v2load.cpp ${_v2src}/v2player.cpp)
if(DEBRIS_V2_RONAN)
  list(APPEND _v2_sources ${_v2src}/ronan.cpp)
endif()
debris_cxx_oracle(oracle_v2 ${_v2_sources})
target_include_directories(oracle_v2 PRIVATE ${_v2src})
target_compile_definitions(oracle_v2 PRIVATE V2_RONAN=$<IF:$<BOOL:${DEBRIS_V2_RONAN}>,1,0>)

add_test(NAME oracle_v2_prepare
  COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:v2m_tool> -DKX=${PROJECT_SOURCE_DIR}/assets/debris_party.kx -DWORK=${_work}
          -P ${_scripts}/prepare_songs.cmake)
set_tests_properties(oracle_v2_prepare PROPERTIES FIXTURES_SETUP v2_songs LABELS oracle)

# debris_compare_pcm(<name> <renderer> <song> <frames> [MASK <mask>] [LABELS ...])
function(debris_compare_pcm name renderer song frames)
  cmake_parse_arguments(ARG "" "MASK" "LABELS" ${ARGN})
  add_test(NAME oracle_v2_${name}
    COMMAND ${CMAKE_COMMAND} -DV2_RENDER=$<TARGET_FILE:${renderer}> -DORACLE=$<TARGET_FILE:oracle_v2>
            -DSONG=${song} -DFRAMES=${frames} -DMASK=${ARG_MASK} -DNAME=${name} -DWORK=${_work}
            -P ${_scripts}/compare_pcm.cmake)
  set_tests_properties(oracle_v2_${name} PROPERTIES FIXTURES_REQUIRED v2_songs TIMEOUT 600 LABELS "oracle;${ARG_LABELS}")
endfunction()

set(_assets ${PROJECT_SOURCE_DIR}/assets)
debris_compare_pcm(loader_full v2_render ${_assets}/debris_loader.v2m 2559552)
debris_compare_pcm(loader_masks v2_render ${_assets}/debris_loader.v2m 1000000 MASK 0x0005)
debris_compare_pcm(loader_muted v2_render ${_assets}/debris_loader.v2m 500000 MASK 0)
debris_compare_pcm(party_60s v2_render ${_work}/party.v2m 2646000)
# music routed through channel 15 exercises the Ronan speech synthesizer
debris_compare_pcm(party_ch15 v2_render ${_work}/party_ch15.v2m 2646000 LABELS ronan)
debris_compare_pcm(party_ch15_masked v2_render ${_work}/party_ch15.v2m 1000000 MASK 0x8001 LABELS ronan)
debris_compare_pcm(player_loader player_pcm ${_assets}/debris_loader.v2m 2559552)
debris_compare_pcm(player_party player_pcm ${_work}/party.v2m 2646000)
if(DEBRIS_TEST_SLOW)
  debris_compare_pcm(party_full v2_render ${_work}/party.v2m 18934066 LABELS slow)
endif()
