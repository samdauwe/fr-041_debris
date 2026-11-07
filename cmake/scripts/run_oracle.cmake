# Runs one C++ source oracle (stdout captured to a file) and then the C
# comparison against that output.
#   -DORACLE=<exe> -DCHECKER=<test_wz3_oracle exe> -DKIND=<blur|cell|perlin|misc|lookup> -DWORK=<dir>
file(MAKE_DIRECTORY "${WORK}")
set(OUT "${WORK}/oracle_${KIND}.txt")
execute_process(COMMAND "${ORACLE}" OUTPUT_FILE "${OUT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "oracle ${ORACLE} failed (${rc})")
endif()
execute_process(COMMAND "${CHECKER}" "${KIND}" "${OUT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "wz3.h differs from the ${KIND} oracle")
endif()
