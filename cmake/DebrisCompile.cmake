# Compile settings shared by every target of the project.
#
#   debris_configure_target(<target>)
#
# Applies the warning set and, with DEBRIS_SANITIZE, AddressSanitizer + UndefinedBehaviorSanitizer. The numeric requirements
# (no floating-point contraction, no fast-math) travel with debris::headers instead, so that consumers inherit them.

function(debris_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
  endif()
  if(DEBRIS_SANITIZE AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()
