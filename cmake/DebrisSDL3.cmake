# Provides SDL3::SDL3: an installed SDL3 (>= 3.2) when there is one, otherwise a static build fetched from the pinned tag.
#
#   debris_find_sdl3(<result variable>)     sets the variable to ON when SDL3::SDL3 exists

function(debris_find_sdl3 result)
  set(${result} OFF PARENT_SCOPE)
  find_package(SDL3 3.2 QUIET CONFIG)
  if(NOT SDL3_FOUND)
    if(NOT DEBRIS_FETCH_SDL3)
      message(STATUS "debris: SDL3 not found and DEBRIS_FETCH_SDL3=OFF: the player is not built")
      return()
    endif()
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    # optional X11 extensions whose -devel packages are often missing; Wayland and core X11 stay available
    set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
      GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
      GIT_TAG ${DEBRIS_SDL3_TAG}
      GIT_SHALLOW ON
      SYSTEM)
    FetchContent_MakeAvailable(SDL3)
  endif()
  set(${result} ON PARENT_SCOPE)
endfunction()
