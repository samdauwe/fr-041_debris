# The debris executable. Included from the top-level CMakeLists.txt when DEBRIS_BUILD_PLAYER is ON.

include(DebrisSDL3)

debris_find_sdl3(_have_sdl3)
if(NOT _have_sdl3)
  return()
endif()
set(DEBRIS_PLAYER_AVAILABLE ON)

add_executable(debris src/player/main.c)
target_include_directories(debris PRIVATE ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(debris PRIVATE debris::headers SDL3::SDL3)
debris_configure_target(debris)

# the data the player looks for next to the executable
add_custom_command(TARGET debris POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:debris>/assets
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${PROJECT_SOURCE_DIR}/assets/debris_party.kx ${PROJECT_SOURCE_DIR}/assets/debris_loader.v2m
          $<TARGET_FILE_DIR:debris>/assets)

include(GNUInstallDirs)
install(TARGETS debris RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${PROJECT_SOURCE_DIR}/assets/debris_party.kx ${PROJECT_SOURCE_DIR}/assets/debris_loader.v2m
        DESTINATION ${CMAKE_INSTALL_DATADIR}/debris)
