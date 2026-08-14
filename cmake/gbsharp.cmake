# Copyright (C) 2026 GB# contributors
#
# This software may be modified and distributed under the terms
# of the MIT license.  See the LICENSE file for details.
#
# The GB# runtime: the emulator core plus src/gbsharp.c, as a library, with no
# SDL, no OpenGL and no imgui anywhere in the link.
#
# Two flavours ship, exporting the identical ABI:
#
#   gbsharp_emulator         emulator.c, no instrumentation
#   gbsharp_emulator_debug   emulator-debug.c, which #includes emulator.c and
#                            compiles it a second time with hooks enabled
#
# Instrumentation is a compile time choice upstream, not a runtime one, which
# is why it is two files rather than one flag. The C# side loads whichever it
# wants and asks gbsharp_has_debug_support() which one it got.
#
# Included from the bottom of the top level CMakeLists.txt, so that upstream
# can rewrite that file freely and the only thing to reapply is one line.

set(GBSHARP_CORE_SOURCES
  ${PROJECT_SOURCE_DIR}/src/memory.c
  ${PROJECT_SOURCE_DIR}/src/common.c
  ${PROJECT_SOURCE_DIR}/src/gbsharp.c
)

# Names the artifacts, exports only the facade, and refuses to link if anything
# in the sources reached for a symbol we did not intend to depend on.
function (gbsharp_configure_library name kind)
  target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/src)
  set_property(TARGET ${name} PROPERTY C_STANDARD 11)
  set_property(TARGET ${name} PROPERTY POSITION_INDEPENDENT_CODE ON)

  if (kind STREQUAL "SHARED")
    target_compile_definitions(${name} PRIVATE GBSHARP_BUILD_SHARED)

    # Everything that is not marked GBSHARP_API stays internal, so the ABI is
    # the header and not whatever emulator.c happens to leave non-static.
    set_property(TARGET ${name} PROPERTY C_VISIBILITY_PRESET hidden)
    set_property(TARGET ${name} PROPERTY VISIBILITY_INLINES_HIDDEN ON)

    # A shared library on ELF may leave symbols undefined and discover the
    # problem at load time. Windows and macOS already refuse. Refusing
    # everywhere is what turns "the core must not link SDL" into a build error
    # the first time somebody adds host.c to these sources, rather than a
    # convention in a document.
    if (UNIX AND NOT APPLE)
      target_link_options(${name} PRIVATE "LINKER:--no-undefined")
    endif ()
  endif ()
endfunction ()

# gbsharp_emulator, gbsharp_emulator_static: the fast flavour.
add_library(gbsharp_emulator SHARED
  ${GBSHARP_CORE_SOURCES}
  ${PROJECT_SOURCE_DIR}/src/emulator.c
)
gbsharp_configure_library(gbsharp_emulator SHARED)

add_library(gbsharp_emulator_static STATIC
  ${GBSHARP_CORE_SOURCES}
  ${PROJECT_SOURCE_DIR}/src/emulator.c
)
gbsharp_configure_library(gbsharp_emulator_static STATIC)

# gbsharp_emulator_debug, gbsharp_emulator_debug_static: the hooked flavour.
# emulator-debug.c includes emulator.c, so emulator.c must not also be listed
# here or every symbol in it lands in the link twice.
add_library(gbsharp_emulator_debug SHARED
  ${GBSHARP_CORE_SOURCES}
  ${PROJECT_SOURCE_DIR}/src/emulator-debug.c
)
target_compile_definitions(gbsharp_emulator_debug PRIVATE GBSHARP_DEBUG_FLAVOUR)
gbsharp_configure_library(gbsharp_emulator_debug SHARED)

add_library(gbsharp_emulator_debug_static STATIC
  ${GBSHARP_CORE_SOURCES}
  ${PROJECT_SOURCE_DIR}/src/emulator-debug.c
)
target_compile_definitions(gbsharp_emulator_debug_static PRIVATE GBSHARP_DEBUG_FLAVOUR)
gbsharp_configure_library(gbsharp_emulator_debug_static STATIC)

# On Windows a SHARED target's import library and a STATIC target's archive are
# both called <name>.lib, so the static flavours keep the suffix in their file
# name on every platform rather than only where it is forced.
set_target_properties(gbsharp_emulator_static PROPERTIES
  OUTPUT_NAME gbsharp_emulator_static)
set_target_properties(gbsharp_emulator_debug_static PROPERTIES
  OUTPUT_NAME gbsharp_emulator_debug_static)

# The layout tools/get-emulator.ps1 expects to find inside a release archive:
# the loadable libraries next to each other, and the one header that describes
# them.
#
# Its own component, so that "cmake --install out --component gbsharp" produces
# exactly the runtime. A plain install would also carry upstream's binjgb
# executables, which a GB# release archive has no use for and which would make
# every contributor download an SDL emulator they did not ask for.
install(TARGETS
    gbsharp_emulator
    gbsharp_emulator_debug
    gbsharp_emulator_static
    gbsharp_emulator_debug_static
  COMPONENT gbsharp
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION bin
  ARCHIVE DESTINATION lib
)
install(FILES ${PROJECT_SOURCE_DIR}/src/gbsharp.h
  COMPONENT gbsharp
  DESTINATION include
)

# ---------------------------------------------------------------------------
# The GB# Player: what a published game is.
#
# Links SDL2 and the runtime, and reaches the emulator only through gbsharp.h,
# the same ABI the test harness and the web runtime use. It could link the core
# directly, being in the same repository, and deliberately does not: the
# boundary is worth more when the thing users actually run is on the far side
# of it.
#
# Built only when SDL2 is present, so that the runtime, which is what almost
# everybody consumes, still builds on a machine with no SDL at all.
# ---------------------------------------------------------------------------
if (SDL2_FOUND)
  add_executable(gbsharp-player WIN32
    ${PROJECT_SOURCE_DIR}/player/main.c
    ${PROJECT_SOURCE_DIR}/player/config.c
    ${PROJECT_SOURCE_DIR}/player/payload.c
  )

  set_property(TARGET gbsharp-player PROPERTY C_STANDARD 11)
  target_include_directories(gbsharp-player PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/player
  )

  # The static flavour, so a published game is one file rather than a file plus
  # a runtime library sitting next to it that a player could delete.
  target_link_libraries(gbsharp-player gbsharp_emulator_static SDL2::SDL2
                        SDL2::SDL2main)

  install(TARGETS gbsharp-player COMPONENT gbsharp-player DESTINATION bin)
  if (SDL2_DYNAMIC)
    install(FILES ${SDL2_RUNTIME_LIBRARY} COMPONENT gbsharp-player DESTINATION bin)
  endif ()
endif ()
