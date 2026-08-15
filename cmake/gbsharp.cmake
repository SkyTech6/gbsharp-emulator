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

# ---------------------------------------------------------------------------
# The web runtime: the same facade, the same emulator.c, through emcc.
#
# One ABI, two hosts. The exported function list below is the same fifteen
# entry points gbsharp.h declares, so web/gbsharp.js can mirror the C ABI name
# for name and a reader of one host can follow the other.
#
# malloc and free are exported alongside them because a ROM has to be copied
# into the module's heap before gbsharp_load_rom can be handed a pointer to it.
# That is the browser's version of "a ROM arrives as bytes, never as a path":
# there is no filesystem here at all, and FILESYSTEM=0 makes sure none is
# linked in to pretend otherwise.
# ---------------------------------------------------------------------------
if (EMSCRIPTEN)
  add_executable(gbsharp_web
    ${GBSHARP_CORE_SOURCES}
    ${PROJECT_SOURCE_DIR}/src/emulator.c
  )

  target_include_directories(gbsharp_web PRIVATE ${PROJECT_SOURCE_DIR}/src)
  set_property(TARGET gbsharp_web PROPERTY C_STANDARD 11)

  set(GBSHARP_WEB_EXPORTS
    _gbsharp_abi_version
    _gbsharp_has_debug_support
    _gbsharp_create
    _gbsharp_destroy
    _gbsharp_load_rom
    _gbsharp_reset
    _gbsharp_run_frame
    _gbsharp_get_framebuffer
    _gbsharp_get_audio
    _gbsharp_set_button
    _gbsharp_read_memory
    _gbsharp_write_memory
    _gbsharp_get_pc
    _gbsharp_get_rom_bank
    _gbsharp_get_ram_bank
    _gbsharp_get_rom_size
    _gbsharp_get_registers
    _gbsharp_step
    _gbsharp_add_breakpoint
    _gbsharp_remove_breakpoint
    _gbsharp_clear_breakpoints
    _gbsharp_set_profiling_enabled
    _gbsharp_get_profiling_enabled
    _gbsharp_clear_profile
    _gbsharp_read_profile
    _gbsharp_set_rom_usage_enabled
    _gbsharp_get_rom_usage_enabled
    _gbsharp_clear_rom_usage
    _gbsharp_read_rom_usage
    _gbsharp_save_ram_size
    _gbsharp_read_save_ram
    _gbsharp_write_save_ram
    _malloc
    _free
  )
  string(REPLACE ";" "','" GBSHARP_WEB_EXPORTS_JS "${GBSHARP_WEB_EXPORTS}")

  set(GBSHARP_WEB_LINK_FLAGS
    "-sEXPORTED_FUNCTIONS=['${GBSHARP_WEB_EXPORTS_JS}']"
    "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32','HEAP16']"
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=GBSharpRuntime
    # web and node: the browser is what this is for, and node is what lets CI
    # run the compatibility suite against the wasm build rather than trusting
    # that it behaves like the native one.
    -sENVIRONMENT=web,node
    -sFILESYSTEM=0
    -sEXIT_RUNTIME=0
    -sALLOW_MEMORY_GROWTH=1
    -sMALLOC=emmalloc
    -sASSERTIONS=0
    # One .wasm beside one .js, which is what `gbsharp publish web` emits and
    # what --single-file then inlines.
    -sWASM=1
  )
  string(REPLACE ";" " " GBSHARP_WEB_LINK_FLAGS_STRING "${GBSHARP_WEB_LINK_FLAGS}")

  set_target_properties(gbsharp_web PROPERTIES
    OUTPUT_NAME gbsharp
    SUFFIX ".js"
    LINK_FLAGS "${GBSHARP_WEB_LINK_FLAGS_STRING}"
  )

  # The module, the wasm beside it, and the hand-written wrapper that gives the
  # three of them a shape a host can use.
  install(FILES
      ${CMAKE_CURRENT_BINARY_DIR}/gbsharp.js
      ${CMAKE_CURRENT_BINARY_DIR}/gbsharp.wasm
      ${PROJECT_SOURCE_DIR}/web/gbsharp-runtime.js
      ${PROJECT_SOURCE_DIR}/src/gbsharp.h
    COMPONENT gbsharp-web
    DESTINATION web
  )

  # Nothing below this line applies to a browser.
  return ()
endif ()

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
