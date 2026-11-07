# Shadow-volume oracle: the checker writes the shadow jobs and the volumes the library extracts, the C++ transcription of
# EngMesh::UpdateShadowCacheJob extracts them again, and the checker compares the two.
#   -DORACLE=<exe> -DCHECKER=<test_wz3_shadow_oracle exe> -DWORK=<dir>
file(MAKE_DIRECTORY "${WORK}")
set(INPUT "${WORK}/shadow_oracle_input.txt")
set(EXPECTED "${WORK}/shadow_oracle_expected.txt")
set(OUT "${WORK}/shadow_oracle_output.txt")
execute_process(COMMAND "${CHECKER}" gen "${INPUT}" "${EXPECTED}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cannot generate the shadow jobs")
endif()
execute_process(COMMAND "${ORACLE}" "${INPUT}" OUTPUT_FILE "${OUT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "oracle ${ORACLE} failed (${rc})")
endif()
execute_process(COMMAND "${CHECKER}" check "${EXPECTED}" "${OUT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "wz3.h differs from the UpdateShadowCacheJob oracle")
endif()
