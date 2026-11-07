# Fails when an executable still imports a transcendental libm function. Only the
# IEEE-exact ones (sqrt, floor, ceil, fabs, ldexp, frexp, fmod) may remain.
#   -DNM=<nm> -DEXE=<file>
execute_process(COMMAND "${NM}" -D --undefined-only "${EXE}" OUTPUT_VARIABLE symbols RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nm failed on ${EXE}")
endif()
string(REGEX MATCHALL "[ \t]U[ \t]+(sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|exp|exp2|expm1|log|log2|log10|log1p|pow|cbrt|hypot)f?(@[^\n]*)?\n" hits "${symbols}")
if(hits)
  message(FATAL_ERROR "${EXE} imports transcendental libm functions: ${hits}")
endif()
message(STATUS "${EXE}: no transcendental libm imports")
