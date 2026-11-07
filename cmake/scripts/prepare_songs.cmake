# Prepare the party song inputs for the oracle comparison tests.
foreach(v TOOL KX WORK)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "prepare_songs.cmake: ${v} is not set")
  endif()
endforeach()

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(rc)
    message(FATAL_ERROR "failed: ${ARGN}\n${err}")
  endif()
endfunction()

file(MAKE_DIRECTORY "${WORK}")
run(${TOOL} extract-kx "${KX}" "${WORK}/party.v2m")
run(${TOOL} move-channel "${WORK}/party.v2m" "${WORK}/party_ch15.v2m" 14 15)
