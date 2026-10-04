cmake_minimum_required(VERSION 3.20)
project(boz_redux_client C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "The client must be built as a 32-bit program; use a preset (cmake --preset linux-x86)")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB BOZ_UNICORN_PATCHES ${CMAKE_SOURCE_DIR}/third_party/patches/unicorn-*.patch)
foreach(patch IN LISTS BOZ_UNICORN_PATCHES)
  execute_process(COMMAND git apply --reverse --check ${patch}
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/unicorn
                  RESULT_VARIABLE already_applied OUTPUT_QUIET ERROR_QUIET)
  if(NOT already_applied EQUAL 0)
    execute_process(COMMAND git apply ${patch}
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/unicorn
                    RESULT_VARIABLE patch_result)
    if(NOT patch_result EQUAL 0)
      message(FATAL_ERROR "Unicorn patch failed to apply: ${patch}")
    endif()
    message(STATUS "Applied ${patch}")
  endif()
endforeach()

set(UNICORN_ARCH "arm" CACHE STRING "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/unicorn EXCLUDE_FROM_ALL)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL2 EXCLUDE_FROM_ALL)

# SDL2_mixer for music and sound effects: built-in decoders only (minimp3 for the game's MP3
# music, WAVE), so the library has no external dependencies such as FluidSynth.
set(BUILD_SHARED_LIBS ON)
set(SDL2MIXER_VENDORED OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MP3 ON CACHE BOOL "" FORCE)
set(SDL2MIXER_MP3_MINIMP3 ON CACHE BOOL "" FORCE)
set(SDL2MIXER_MP3_MPG123 OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_WAVE ON CACHE BOOL "" FORCE)
foreach(format CMD FLAC GME MOD MIDI OPUS WAVPACK)
  set(SDL2MIXER_${format} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL2MIXER_VORBIS "" CACHE STRING "" FORCE)
add_subdirectory(third_party/SDL2_mixer EXCLUDE_FROM_ALL)
unset(BUILD_SHARED_LIBS)
if(NOT WIN32)
  set_target_properties(SDL2_mixer PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

set(BOZ_WARNINGS -Wall -Wextra -Werror -Wno-format-truncation)

# Lua 5.4 for code mods: the core and standard libraries (no interpreter or test files).
file(GLOB BOZ_LUA_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lua/l*.c)
list(REMOVE_ITEM BOZ_LUA_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lua/lua.c
     ${CMAKE_SOURCE_DIR}/third_party/lua/ltests.c)
add_library(lua STATIC ${BOZ_LUA_SOURCES})
target_include_directories(lua PUBLIC third_party/lua)
if(NOT WIN32)
  target_compile_definitions(lua PRIVATE LUA_USE_POSIX)
  target_link_libraries(lua PUBLIC m)
endif()

# Dear ImGui core, shared by the launcher (with its SDL backends) and the game's mod overlay.
add_library(imgui_core STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp)
target_include_directories(imgui_core PUBLIC third_party/imgui)

add_library(boz_project INTERFACE)
target_include_directories(boz_project INTERFACE include src third_party/lzma)
target_compile_definitions(boz_project INTERFACE _GNU_SOURCE)
target_compile_options(boz_project INTERFACE ${BOZ_WARNINGS})

if(WIN32)
  set(BOZ_PLATFORM_SOURCES src/platform/platform_win32.c src/platform/gl_probe_win32.c)
  set(BOZ_ZEROCONF_PLATFORM src/zeroconf_platform_win32.c)
else()
  set(BOZ_PLATFORM_SOURCES src/platform/platform_posix.c)
  set(BOZ_ZEROCONF_PLATFORM src/zeroconf_platform_posix.c)
endif()

add_executable(codboz_s3e_loader
  ${BOZ_PLATFORM_SOURCES}
  src/arm_emu.c
  src/codboz_crash_recovery.c
  src/client_config.c
  src/codboz_assets.c
  src/codboz_frame_interpolation.c
  src/main.c
  src/mods.c
  src/gamedef.c
  src/game_image.c
  src/lua_runtime.c
  src/lua_hooks.c
  src/overlay.cpp
  src/s3e_config.c
  src/s3e_audio.c
  src/s3e_audio_unit.c
  src/s3e_egl.c
  src/s3e_file.c
  src/s3e_gl.c
  src/s3e_host.c
  src/s3e_image.c
  src/s3e_input.c
  src/s3e_memory.c
  src/device_id.c
  src/s3e_runtime.c
  src/s3e_timer.c
  src/s3e_socket.c
  src/mdns_wire.c
  ${BOZ_ZEROCONF_PLATFORM}
  src/s3e_zeroconf.c)
if(WIN32)
  set(BOZ_SYSTEM_LIBS ws2_32 iphlpapi winmm bcrypt)
  set(BOZ_SYSTEM_LINK_OPTIONS -static -static-libgcc -Wl,--large-address-aware)
else()
  set(BOZ_SYSTEM_LIBS ${CMAKE_DL_LIBS} pthread m)
  set(BOZ_SYSTEM_LINK_OPTIONS)
endif()
target_link_libraries(codboz_s3e_loader PRIVATE boz_project unicorn lua imgui_core ${BOZ_SYSTEM_LIBS})
target_link_options(codboz_s3e_loader PRIVATE ${BOZ_SYSTEM_LINK_OPTIONS})
if(NOT WIN32)
  target_link_options(codboz_s3e_loader PRIVATE -static-libgcc -static-libstdc++)
endif()
target_compile_options(codboz_s3e_loader PRIVATE -falign-functions=16)
set_target_properties(codboz_s3e_loader PROPERTIES ENABLE_EXPORTS ON BUILD_RPATH "$ORIGIN")
add_dependencies(codboz_s3e_loader SDL2 SDL2_mixer)

# Launcher: Dear ImGui on SDL's 2D renderer (Direct3D on Windows, OpenGL or software on Linux).
add_library(imgui STATIC
  third_party/imgui/backends/imgui_impl_sdl2.cpp
  third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp)
target_include_directories(imgui PUBLIC third_party/imgui/backends)
target_link_libraries(imgui PUBLIC imgui_core SDL2::SDL2)

add_executable(boz-redux
  src/launcher/main.cpp
  src/launcher/game_files.cpp
  src/launcher/ini_file.cpp
  src/launcher/os.cpp
  src/launcher/sha256.cpp
  src/client_config.c
  src/mods.c)
target_include_directories(boz-redux PRIVATE include)
target_compile_options(boz-redux PRIVATE ${BOZ_WARNINGS})
target_link_libraries(boz-redux PRIVATE imgui)
if(WIN32)
  target_link_libraries(boz-redux PRIVATE comdlg32 shell32 ole32)
  target_link_options(boz-redux PRIVATE -static -static-libgcc -static-libstdc++)
  set_target_properties(boz-redux PROPERTIES WIN32_EXECUTABLE ON)
else()
  target_link_libraries(boz-redux PRIVATE pthread ${CMAKE_DL_LIBS})
  target_link_options(boz-redux PRIVATE -static-libgcc -static-libstdc++)
  set_target_properties(boz-redux PROPERTIES BUILD_RPATH "$ORIGIN")
endif()
add_dependencies(boz-redux codboz_s3e_loader codboz_apk_extract)

add_executable(codboz_apk_extract tools/apk_extract/codboz_apk_extract.c third_party/lzma/LzmaDec.c)
target_link_libraries(codboz_apk_extract PRIVATE boz_project)

enable_testing()

function(boz_test name)
  cmake_parse_arguments(T "" "" "SOURCES;DEFINES;INCLUDES" ${ARGN})
  add_executable(${name} tests/${name}.c ${T_SOURCES} tests/test_support.c)
  target_link_libraries(${name} PRIVATE boz_project pthread)
  target_compile_definitions(${name} PRIVATE ${T_DEFINES})
  target_compile_options(${name} PRIVATE -UNDEBUG)
  target_include_directories(${name} PRIVATE ${T_INCLUDES})
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

boz_test(s3e_socket_test SOURCES src/s3e_socket.c src/s3e_config.c)
boz_test(s3e_config_test SOURCES src/s3e_config.c src/codboz_assets.c)
boz_test(mdns_wire_test SOURCES src/mdns_wire.c)
boz_test(s3e_zeroconf_test SOURCES tests/zeroconf_platform_fake.c src/s3e_zeroconf.c src/mdns_wire.c
         INCLUDES tests)
boz_test(s3e_timer_test SOURCES src/s3e_timer.c)
boz_test(client_config_test SOURCES src/client_config.c)
boz_test(mods_test SOURCES src/mods.c src/client_config.c)
boz_test(gamedef_test SOURCES src/gamedef.c)
boz_test(lua_runtime_test SOURCES src/lua_runtime.c src/mods.c src/client_config.c src/gamedef.c
         src/game_image.c ${BOZ_PLATFORM_SOURCES})
target_link_libraries(lua_runtime_test PRIVATE lua ${CMAKE_DL_LIBS})
boz_test(s3e_memory_test SOURCES src/s3e_memory.c)
boz_test(device_id_test SOURCES src/device_id.c ${BOZ_PLATFORM_SOURCES})
boz_test(s3e_audio_test SOURCES src/s3e_audio.c
         DEFINES dlsym=s3e_audio_test_dlsym dlclose=s3e_audio_test_dlclose
                 plat_lib_symbol=s3e_audio_test_dlsym plat_lib_close=s3e_audio_test_dlclose)
boz_test(s3e_audio_unit_test SOURCES src/s3e_audio_unit.c)
boz_test(s3e_input_test SOURCES src/s3e_input.c src/client_config.c
         DEFINES dlsym=s3e_input_test_dlsym dlclose=s3e_input_test_dlclose
                 plat_lib_symbol=s3e_input_test_dlsym plat_lib_close=s3e_input_test_dlclose)

add_executable(arm_emu_test tests/arm_emu_test.c src/arm_emu.c ${BOZ_PLATFORM_SOURCES})
target_link_libraries(arm_emu_test PRIVATE boz_project unicorn ${CMAKE_DL_LIBS} pthread m)
target_compile_options(arm_emu_test PRIVATE -falign-functions=16 -UNDEBUG)
set_target_properties(arm_emu_test PROPERTIES ENABLE_EXPORTS ON
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME arm_emu_test COMMAND arm_emu_test)

add_executable(lua_hooks_test tests/lua_hooks_test.c src/lua_hooks.c src/arm_emu.c ${BOZ_PLATFORM_SOURCES})
target_link_libraries(lua_hooks_test PRIVATE boz_project unicorn lua ${CMAKE_DL_LIBS} pthread m)
target_compile_options(lua_hooks_test PRIVATE -falign-functions=16 -UNDEBUG)
set_target_properties(lua_hooks_test PROPERTIES ENABLE_EXPORTS ON
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME lua_hooks_test COMMAND lua_hooks_test)
