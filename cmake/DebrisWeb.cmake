# The browser build of the player (Emscripten): WebAssembly, WebGL 2 and the single page in web/index.html.
#
#   debris_configure_web(<target>)
#
# <build>/dist receives everything a static host serves: index.html, index.js, index.wasm. The two data files of the production are
# linked into the module, so the page makes three requests and nothing else. There are no pthreads: a page can only share memory
# between threads with COOP/COEP headers and static hosting (GitHub Pages) does not send them, so the player loads in time slices
# on the main thread instead (--single-thread, see src/player/main.c).

function(debris_configure_web target)
  set(dist ${PROJECT_BINARY_DIR}/dist)
  set(data ${PROJECT_SOURCE_DIR}/assets/debris_party.kx ${PROJECT_SOURCE_DIR}/assets/debris_loader.v2m)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME index SUFFIX ".js" RUNTIME_OUTPUT_DIRECTORY ${dist})

  target_link_options(${target} PRIVATE
    -sMODULARIZE=1 -sEXPORT_NAME=createDebris
    -sENVIRONMENT=web
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sMAXIMUM_MEMORY=${DEBRIS_WEB_MAX_MEMORY} -sSTACK_SIZE=1MB
    $<$<CONFIG:Debug>:-sASSERTIONS=1>)
  foreach(file IN LISTS data)
    get_filename_component(name ${file} NAME)
    target_link_options(${target} PRIVATE "SHELL:--embed-file ${file}@/assets/${name}")
  endforeach()
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${data})

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/web/index.html ${dist}/index.html)
endfunction()
