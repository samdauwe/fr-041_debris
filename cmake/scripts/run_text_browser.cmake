# Renders the Debris Text operators with headless Chrome (the production JS canvas adapter, no kerning,
# no hinting) and compares the native rasterizer against it within the calibrated tolerance.
#   -DNODE=<node> -DSCRIPT=<dump_text_reference.mjs> -DCHECKER=<test_wz3_text exe>
#   -DASSETS=<dir> -DFIXTURES=<dir> -DWORK=<dir> -DOFFSET=<-2|0>
file(MAKE_DIRECTORY "${WORK}")
set(REFERENCE "${WORK}/text_reference_${OFFSET}.bin")
execute_process(COMMAND "${NODE}" "${SCRIPT}" "${OFFSET}" "${REFERENCE}" --plain RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the browser reference could not be generated (${rc})")
endif()
execute_process(COMMAND "${CHECKER}" "${ASSETS}" "${FIXTURES}" "${OFFSET}" "${REFERENCE}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "native text differs from the browser canvas beyond the calibrated tolerance")
endif()
