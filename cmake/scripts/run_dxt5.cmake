# Runs the rygdxt oracle and then the BC3 comparison against its output.
#   -DORACLE=<exe> -DCHECKER=<test_wz3_dxt5 exe> -DFIXTURES=<dir> -DWORK=<dir>
file(MAKE_DIRECTORY "${WORK}")
set(OUT "${WORK}/oracle_dxt5.txt")
execute_process(COMMAND "${ORACLE}" OUTPUT_FILE "${OUT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "oracle ${ORACLE} failed (${rc})")
endif()
execute_process(COMMAND "${CHECKER}" "${FIXTURES}" "${OUT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "wz3.h differs from the rygdxt oracle")
endif()
