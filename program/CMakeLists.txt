cmake_minimum_required(VERSION 3.21)
project(SBPC VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Repository layout: external code in ../libs (SDL3 and TinyCC submodules); the built programs go to
# ../builds (not committed: releases ship them).
set(SBPC_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libs")
set(SBPC_BUILDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds")
set(SBPC_SDL_DIR "${SBPC_LIBS_DIR}/SDL")
if(EXISTS "${SBPC_SDL_DIR}/CMakeLists.txt")
  set(_sbpc_sdl_default ON)
else()
  set(_sbpc_sdl_default OFF)
endif()
option(SBPC_WITH_SDL "Build the SDL3 GUI launcher/runtime" ${_sbpc_sdl_default})
option(SBPC_BUILD_TESTS "Build unit tests" ON)
option(SBPC_COPY_TO_SOURCE "Copy the built executables (and sbpc-tcc with its headers) to ../builds" ON)

# ---- warnings -----------------------------------------------------------------
option(SBPC_WERROR "Treat warnings in SBPC's own code as errors (CI)" OFF)
if(MSVC)
  set(SBPC_WARNINGS /W4 /utf-8 /wd4996)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
  if(SBPC_WERROR)
    list(APPEND SBPC_WARNINGS /WX)
  endif()
else()
  set(SBPC_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wno-unused-parameter)
  if(SBPC_WERROR)
    list(APPEND SBPC_WARNINGS -Werror)
  endif()
endif()

# ---- core + disc library (no SDL) ---------------------------------------------
add_library(sbpc_core STATIC
  src/core/fs.c
  src/core/log.c
  src/core/paths.c
  src/core/config.c
  src/core/sha1.c
  src/core/png.c
  src/core/proc.c
  src/disc/cue.c
  src/disc/disc.c
  src/disc/chd.c
  src/disc/inflate.c
  src/disc/lzma.c
  src/disc/flac.c
  src/disc/saturn_header.c
  src/disc/identify.c
  src/disc/versions.c
  src/disc/iso9660.c
  src/sh2/sh2_decode.c
  src/sh2/sh2_interp.c
  src/m68k/m68k.c
  src/analysis/image.c
  src/analysis/boot.c
  src/analysis/analyse.c
  src/codegen/codegen.c
  src/runtime/dispatch.c
  src/runtime/loader.c
  src/runtime/tcc_build.c
  src/saturn/saturn.c
  src/saturn/saturn_scu.c
  src/saturn/saturn_smpc.c
  src/saturn/saturn_rec.c
  src/saturn/saturn_cdb.c
  src/saturn/saturn_scsp.c
  src/saturn/saturn_bios.c
  src/saturn/saturn_bup.c
  src/saturn/saturn_vdp1.c
  src/saturn/saturn_vdp2.c
  src/launcher/scan.c
  src/launcher/overlay.c
)
target_include_directories(sbpc_core PUBLIC src)
target_link_libraries(sbpc_core PUBLIC ${CMAKE_DL_LIBS})
if(UNIX)
  target_link_libraries(sbpc_core PUBLIC m)   # pow() for the SCSP level tables
endif()
target_compile_options(sbpc_core PRIVATE ${SBPC_WARNINGS})
target_compile_definitions(sbpc_core PUBLIC SBPC_VERSION_STRING="${PROJECT_VERSION}")
if(NOT WIN32)
  target_compile_definitions(sbpc_core PUBLIC _FILE_OFFSET_BITS=64 _POSIX_C_SOURCE=200809L)
endif()

# ---- SDL3 -----------------------------------------------------------------------
if(SBPC_WITH_SDL)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory("${SBPC_SDL_DIR}" libs/SDL EXCLUDE_FROM_ALL)
endif()

# ---- TinyCC (sbpc-tcc) -----------------------------------------------------------------
# The launcher's Build step compiles the generated C into one library per module with TinyCC.
# It is built here as its own program (sbpc-tcc), so the LGPL code stays a separate executable.
set(SBPC_TCC_DIR "${SBPC_LIBS_DIR}/tinycc")
if(EXISTS "${SBPC_TCC_DIR}/tcc.c")
  file(STRINGS "${SBPC_TCC_DIR}/VERSION" _tcc_version LIMIT_COUNT 1)
  set(_tcc_cfg "${CMAKE_CURRENT_BINARY_DIR}/tcc_config")
  file(WRITE "${_tcc_cfg}/config.h" "#define TCC_VERSION \"${_tcc_version}\"\n#define CONFIG_TCC_PREDEFS 0\n")
  add_executable(sbpc_tcc "${SBPC_TCC_DIR}/tcc.c")
  set_target_properties(sbpc_tcc PROPERTIES OUTPUT_NAME sbpc-tcc)
  target_include_directories(sbpc_tcc PRIVATE "${_tcc_cfg}" "${SBPC_TCC_DIR}")
  target_compile_definitions(sbpc_tcc PRIVATE ONE_SOURCE=1)
  if(WIN32)
    target_compile_definitions(sbpc_tcc PRIVATE TCC_TARGET_PE TCC_TARGET_X86_64 _CRT_SECURE_NO_WARNINGS)
  else()
    target_link_libraries(sbpc_tcc PRIVATE m ${CMAKE_DL_LIBS} pthread)
  endif()
  if(MSVC)
    target_compile_options(sbpc_tcc PRIVATE /W0)
  else()
    target_compile_options(sbpc_tcc PRIVATE -w)
  endif()
  # Next to the compiler: tcc/include (TinyCC's own headers + our tiny stdint.h) and tcc/sbpc
  # (the SH-2 headers generated code includes).
  set(SBPC_TCC_FILES
    "${SBPC_TCC_DIR}/include/tccdefs.h" "${SBPC_TCC_DIR}/include/stddef.h" "${SBPC_TCC_DIR}/include/stdarg.h"
    "${SBPC_TCC_DIR}/include/stdbool.h" "${SBPC_TCC_DIR}/include/float.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/tcc_include/stdint.h")
  set(SBPC_SDK_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sh2/sh2_state.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/sh2/sh2_ops.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sh2/sbpc_rt.h")
  # Copied next to sbpc-tcc in the build folder (tests) and in ../builds.
  set(_tcc_dests "$<TARGET_FILE_DIR:sbpc_tcc>/tcc")
  if(SBPC_COPY_TO_SOURCE)
    list(APPEND _tcc_dests "${SBPC_BUILDS_DIR}/tcc")
  endif()
  foreach(_d ${_tcc_dests})
    add_custom_command(TARGET sbpc_tcc POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_d}/include" "${_d}/sbpc"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SBPC_TCC_FILES} "${_d}/include/"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SBPC_SDK_FILES} "${_d}/sbpc/")
  endforeach()
  if(SBPC_COPY_TO_SOURCE)
    add_custom_command(TARGET sbpc_tcc POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sbpc_tcc> "${SBPC_BUILDS_DIR}/")
  endif()
else()
  message(WARNING "libs/tinycc is missing: Build & Play will run the game on the interpreter only")
endif()

# ---- sbpc executable --------------------------------------------------------------
set(SBPC_EXE_SOURCES src/main.c src/launcher/launcher_cli.c)
if(SBPC_WITH_SDL)
  list(APPEND SBPC_EXE_SOURCES src/launcher/launcher_gui.c src/launcher/play.c src/launcher/input.c src/launcher/ui_font.c)
endif()
if(WIN32)
  list(APPEND SBPC_EXE_SOURCES res/sbpc.rc)   # the program icon (res/icon.png, see scripts/icon_to_c.py)
endif()

if(WIN32 AND SBPC_WITH_SDL)
  add_executable(sbpc WIN32 ${SBPC_EXE_SOURCES})
else()
  add_executable(sbpc ${SBPC_EXE_SOURCES})
endif()
target_link_libraries(sbpc PRIVATE sbpc_core)
target_compile_options(sbpc PRIVATE ${SBPC_WARNINGS})
if(SBPC_WITH_SDL)
  target_link_libraries(sbpc PRIVATE SDL3::SDL3-static)
  target_compile_definitions(sbpc PRIVATE SBPC_HAVE_SDL=1)
  target_include_directories(sbpc PRIVATE "${SBPC_LIBS_DIR}/half-eighties" res)   # the launcher font (OFL), the icon
endif()

# The launcher is copied to ../builds. From there it scans ../games by default and keeps built games,
# settings, saves and logs in ../recompiled (see core/paths.c).
if(SBPC_COPY_TO_SOURCE)
  add_custom_command(TARGET sbpc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SBPC_BUILDS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sbpc> "${SBPC_BUILDS_DIR}/"
    COMMENT "Copying sbpc to ${SBPC_BUILDS_DIR}")
endif()

# On Windows the GUI build uses the WINDOWS subsystem, so the shell doesn't wait for it and
# its output arrives after the prompt. sbpc-cli.exe is the same program as a console app.
if(WIN32 AND SBPC_WITH_SDL)
  add_executable(sbpc_cli ${SBPC_EXE_SOURCES})
  set_target_properties(sbpc_cli PROPERTIES OUTPUT_NAME sbpc-cli)
  target_link_libraries(sbpc_cli PRIVATE sbpc_core SDL3::SDL3-static)
  target_compile_options(sbpc_cli PRIVATE ${SBPC_WARNINGS})
  target_compile_definitions(sbpc_cli PRIVATE SBPC_HAVE_SDL=1 SBPC_CONSOLE=1)
  target_include_directories(sbpc_cli PRIVATE "${SBPC_LIBS_DIR}/half-eighties" res)
  set(SBPC_CLI_TARGET sbpc_cli)
  if(SBPC_COPY_TO_SOURCE)
    add_custom_command(TARGET sbpc_cli POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${SBPC_BUILDS_DIR}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sbpc_cli> "${SBPC_BUILDS_DIR}/")
  endif()
else()
  set(SBPC_CLI_TARGET sbpc)
endif()

# ---- tests --------------------------------------------------------------------------
if(SBPC_BUILD_TESTS)
  enable_testing()
  foreach(t sha1 cue saturn_header identify iso9660 sh2_decode sh2_interp analyse saturn video m68k bup overlay scsp chd export rec)
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE sbpc_core)
    target_compile_options(test_${t} PRIVATE ${SBPC_WARNINGS})
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()

  # Differential test: random SH-2 programs -> analyser -> code generator -> compiled here, then
  # compared against the interpreter. diffgen runs at build time.
  add_executable(diffgen tests/diffgen.c)
  target_link_libraries(diffgen PRIVATE sbpc_core)
  set(DIFF_DIR ${CMAKE_CURRENT_BINARY_DIR}/diffgen_out)
  set(DIFF_OUT ${DIFF_DIR}/diff_prog.h ${DIFF_DIR}/gen_diff.h ${DIFF_DIR}/gen_diff_000.c ${DIFF_DIR}/gen_diff_table.c)
  add_custom_command(OUTPUT ${DIFF_OUT}
    COMMAND diffgen ${DIFF_DIR}
    DEPENDS diffgen
    COMMENT "Generating differential test programs")
  add_executable(test_diff tests/test_diff.c ${DIFF_DIR}/gen_diff_000.c ${DIFF_DIR}/gen_diff_table.c)
  target_include_directories(test_diff PRIVATE ${DIFF_DIR} src/sh2)
  target_link_libraries(test_diff PRIVATE sbpc_core)
  # Generated code has labels that only the entry switch uses, and that's fine.
  if(MSVC)
    set_source_files_properties(${DIFF_DIR}/gen_diff_000.c ${DIFF_DIR}/gen_diff_table.c
      PROPERTIES COMPILE_OPTIONS "/wd4102;/wd4100")
  else()
    set_source_files_properties(${DIFF_DIR}/gen_diff_000.c ${DIFF_DIR}/gen_diff_table.c
      PROPERTIES COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-parameter")
  endif()
  add_test(NAME diff COMMAND test_diff)

  # Smoke tests of the real executable, no disc needed: it starts, prints help, scans an empty folder.
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/smoke_empty_games)
  add_test(NAME cli_help COMMAND ${SBPC_CLI_TARGET} --portable --cli help)
  add_test(NAME cli_scan_empty COMMAND ${SBPC_CLI_TARGET} --portable --cli scan ${CMAKE_CURRENT_BINARY_DIR}/smoke_empty_games)
  set_tests_properties(cli_scan_empty PROPERTIES PASS_REGULAR_EXPRESSION "no disc images found")

  # TinyCC end to end (no disc): the same generated C, built by the bundled sbpc-tcc into a module and
  # loaded at run time, as Build & Play does. Runs next to sbpc-tcc and its tcc/ folder.
  if(TARGET sbpc_tcc)
  add_executable(test_tcc tests/test_tcc.c ${DIFF_DIR}/diff_prog.h)
  target_include_directories(test_tcc PRIVATE ${DIFF_DIR} src/sh2)
  target_link_libraries(test_tcc PRIVATE sbpc_core)
  target_compile_options(test_tcc PRIVATE ${SBPC_WARNINGS})
  target_compile_definitions(test_tcc PRIVATE SBPC_DIFF_DIR="${DIFF_DIR}")
  add_dependencies(test_tcc sbpc_tcc)
  set_target_properties(test_tcc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:sbpc_tcc>")
  add_test(NAME tcc COMMAND test_tcc WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  endif()

  # Local-only check against the developer's own dump. Skipped when not set.
  set(SBPC_LOCAL_GAMES_DIR "" CACHE PATH "Folder with your own disc dumps (local tests only)")
  if(SBPC_LOCAL_GAMES_DIR)
    add_test(NAME local_scan COMMAND ${SBPC_CLI_TARGET} --cli scan "${SBPC_LOCAL_GAMES_DIR}" --strict)
  endif()
endif()
