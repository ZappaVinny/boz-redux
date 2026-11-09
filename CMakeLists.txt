cmake_minimum_required(VERSION 3.20)
project(boz_redux_client VERSION 0.3.1 LANGUAGES C CXX)
set(BOZ_VERSION ${PROJECT_VERSION})
if(WIN32)
  enable_language(RC)
endif()

# The ARM CPU: Unicorn on 32-bit x86 hosts (src/arm_emu.c), the Dynarmic JIT on 64-bit hosts
# (src/arm_emu_dynarmic.cpp, x86-64 and AArch64), and on 32-bit ARM hosts (Android armeabi-v7a
# TVs) none at all: the game's code runs natively (src/arm_emu_native.c). All run the same client;
# see docs/runtime.md. -DBOZ_CPU=unicorn picks Unicorn on a 32-bit ARM host too.
set(BOZ_CPU "" CACHE STRING "ARM CPU: unicorn, dynarmic or native (empty: by host)")
if(NOT BOZ_CPU)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(BOZ_CPU dynarmic)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7)")
    set(BOZ_CPU native)
  else()
    set(BOZ_CPU unicorn)
  endif()
endif()
message(STATUS "ARM CPU: ${BOZ_CPU}")
# The game's 4 GiB at an offset in the host (guest_memory.h): required on Android and the Switch,
# where the low 4 GiB of the process are not ours; on a 64-bit PC it tests that path.
option(BOZ_GUEST_BASE "Map the game's memory at a base offset (64-bit only)" OFF)
# 32-bit Android (armeabi-v7a TVs): every host address fits the game's 32 bits, as on 32-bit x86,
# so Unicorn maps the game where it lives; Gradle passes BOZ_GUEST_BASE=ON for all ABIs.
if(ANDROID AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(BOZ_GUEST_BASE OFF CACHE BOOL "" FORCE)
endif()
if(BOZ_GUEST_BASE AND NOT BOZ_CPU STREQUAL "dynarmic")
  message(FATAL_ERROR "BOZ_GUEST_BASE needs a 64-bit build")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(BOZ_CPU STREQUAL "dynarmic")
  set(CMAKE_CXX_STANDARD 20) # Dynarmic's headers use concepts
else()
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(BOZ_CPU STREQUAL "unicorn")
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
# Linux: Unicorn and the loader without position-independent code. On 32-bit x86, PIC costs a
# get_pc_thunk call in every function, and the JIT calls Unicorn's helpers constantly: the game
# ran 15-20% faster without it (scripts/bench.sh). Windows code is not PIC to begin with.
if(NOT WIN32 AND NOT ANDROID) # Android's shared libraries must be PIC
  foreach(target unicorn unicorn-common arm-softmmu)
    target_compile_options(${target} PRIVATE -fno-pic -fno-pie)
  endforeach()
endif()
set(BOZ_CPU_SOURCES src/arm_emu.c)
set(BOZ_CPU_LIBS unicorn)
elseif(BOZ_CPU STREQUAL "native")
  set(BOZ_CPU_SOURCES src/arm_emu_native.c)
  set(BOZ_CPU_LIBS)
else()
  # Dynarmic: only the A32 frontend, its bundled externals (fmt, mcl, xbyak/oaknut, zydis) and
  # Boost's headers (MSYS2: mingw-w64-x86_64-boost; Debian: libboost-dev).
  set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
  set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
  set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
  set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  set(DYNARMIC_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)
  set(BOZ_CPU_SOURCES src/arm_emu_dynarmic.cpp)
  set(BOZ_CPU_LIBS dynarmic merry::mcl)
  # arm_emu_dynarmic.cpp uses Dynarmic's IR headers, which include Boost (Android: Gradle passes
  # Boost_INCLUDE_DIR; MSYS2 and Linux find it in the system include path).
  if(Boost_INCLUDE_DIR)
    include_directories(SYSTEM ${Boost_INCLUDE_DIR})
  endif()
endif()

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

# Branding (branding/): the logo, window icon and fonts are built into the programs; Windows
# executables also get the icon and version information as resources.
set(BOZ_BRANDING_FILES
  BRAND_LOGO=${CMAKE_SOURCE_DIR}/branding/boz-redux-logo.png
  BRAND_ICON=${CMAKE_SOURCE_DIR}/branding/icons/boz-redux-256.png
  BRAND_FONT_REGULAR=${CMAKE_SOURCE_DIR}/branding/fonts/Barlow-Regular.ttf
  BRAND_FONT_SEMIBOLD=${CMAKE_SOURCE_DIR}/branding/fonts/Barlow-SemiBold.ttf
  BRAND_FONT_HEADING=${CMAKE_SOURCE_DIR}/branding/fonts/BarlowCondensed-Bold.ttf)
set(BOZ_BRANDING_DEPENDS)
foreach(entry IN LISTS BOZ_BRANDING_FILES)
  string(REGEX REPLACE "^[^=]*=" "" path "${entry}")
  list(APPEND BOZ_BRANDING_DEPENDS "${path}")
endforeach()
file(WRITE ${CMAKE_BINARY_DIR}/generated/branding_files.txt "${BOZ_BRANDING_FILES}")
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/generated/branding_data.c
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${CMAKE_BINARY_DIR}/generated/branding_data.c
          -DLIST=${CMAKE_BINARY_DIR}/generated/branding_files.txt -P ${CMAKE_SOURCE_DIR}/cmake/embed_files.cmake
  DEPENDS ${BOZ_BRANDING_DEPENDS} ${CMAKE_SOURCE_DIR}/cmake/embed_files.cmake
  COMMENT "Embedding the branding")
add_library(boz_branding STATIC src/branding/brand_image.c ${CMAKE_BINARY_DIR}/generated/branding_data.c)
target_include_directories(boz_branding PUBLIC include PRIVATE third_party/stb)

# boz_windows_resources(target "Description"): the icon, version information and a manifest
# that runs the program as the user (avoid "install"/"setup" in descriptions all the same).
function(boz_windows_resources target description)
  if(NOT WIN32)
    return()
  endif()
  set(BOZ_RC_ICON "${CMAKE_SOURCE_DIR}/branding/boz-redux.ico")
  set(BOZ_RC_MANIFEST "${CMAKE_SOURCE_DIR}/packaging/windows/app.manifest")
  set(BOZ_RC_NAME "${target}")
  set(BOZ_RC_DESCRIPTION "${description}")
  string(REPLACE "." "," BOZ_RC_VERSION_COMMAS "${BOZ_VERSION}")
  configure_file(${CMAKE_SOURCE_DIR}/packaging/windows/app.rc.in
                 ${CMAKE_BINARY_DIR}/generated/${target}.rc @ONLY)
  target_sources(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated/${target}.rc)
endfunction()

add_library(boz_project INTERFACE)
target_include_directories(boz_project INTERFACE include src third_party/lzma)
target_compile_definitions(boz_project INTERFACE _GNU_SOURCE $<$<BOOL:${BOZ_GUEST_BASE}>:BOZ_GUEST_BASE>)
target_compile_options(boz_project INTERFACE ${BOZ_WARNINGS})

if(WIN32)
  set(BOZ_PLATFORM_SOURCES src/platform/platform_win32.c src/platform/gl_probe_win32.c)
  set(BOZ_ZEROCONF_PLATFORM src/zeroconf_platform_win32.c)
else()
  set(BOZ_PLATFORM_SOURCES src/platform/platform_posix.c)
  set(BOZ_ZEROCONF_PLATFORM src/zeroconf_platform_posix.c)
endif()

add_library(boz_common STATIC src/str_util.c)
target_link_libraries(boz_common PUBLIC boz_project)
# The guest heap (64-bit hosts: memory the game sees has to sit below 4 GiB).
add_library(boz_guest_heap STATIC src/guest_heap.c src/host_signatures.c ${BOZ_PLATFORM_SOURCES})
target_link_libraries(boz_guest_heap PUBLIC boz_project)
target_link_libraries(boz_common PUBLIC boz_guest_heap)

# Android: the game is libmain.so, which SDL's Java activity loads and runs (SDL_main); the guest
# base map is required there (packaging/android).
if(ANDROID AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(NOT BOZ_GUEST_BASE)
    message(FATAL_ERROR "64-bit Android builds need -DBOZ_GUEST_BASE=ON")
  endif()
endif()
if(ANDROID)
  add_library(codboz_s3e_loader SHARED)
  set_target_properties(codboz_s3e_loader PROPERTIES OUTPUT_NAME main)
else()
  add_executable(codboz_s3e_loader)
endif()
target_sources(codboz_s3e_loader PRIVATE
  ${BOZ_PLATFORM_SOURCES}
  ${BOZ_CPU_SOURCES}
  src/client_config.c
  src/game_patches/crash_recovery.c
  src/game_patches/frame_interpolation.c
  src/game_patches/hud_sticks.c
  src/game_patches/prompt_button.c
  src/game_patches/player_name.c
  src/main.c
  src/mods.c
  src/gamedef.c
  src/game_image.c
  src/lua_runtime.c
  src/lua_hooks.c
  src/lua_settings.c
  src/lua_assets.c
  src/native_input.c
  src/touch_controls.c
  src/overlay.cpp
  src/branding/brand_imgui.cpp
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
  src/s3e_symbols.c
  src/s3e_timer.c
  src/s3e_socket.c
  src/mdns_wire.c
  ${BOZ_ZEROCONF_PLATFORM}
  src/s3e_zeroconf.c)
if(WIN32)
  set(BOZ_SYSTEM_LIBS ws2_32 iphlpapi winmm bcrypt)
  set(BOZ_SYSTEM_LINK_OPTIONS -static -static-libgcc -Wl,--large-address-aware)
elseif(ANDROID)
  set(BOZ_SYSTEM_LIBS ${CMAKE_DL_LIBS} log m)
  set(BOZ_SYSTEM_LINK_OPTIONS)
else()
  set(BOZ_SYSTEM_LIBS ${CMAKE_DL_LIBS} pthread m)
  set(BOZ_SYSTEM_LINK_OPTIONS)
endif()
target_link_libraries(boz_guest_heap PUBLIC ${BOZ_SYSTEM_LIBS})
# Programs that run game code on a 64-bit host: guest code calls host functions at their own
# address, so the executable sits at a fixed base below 4 GiB (Linux: -no-pie, below).
set(BOZ_GUEST_LINK_OPTIONS)
if(WIN32 AND BOZ_CPU STREQUAL "dynarmic")
  set(BOZ_SYSTEM_LINK_OPTIONS -static -static-libgcc -static-libstdc++)
  if(NOT BOZ_GUEST_BASE)
    # Guest-base builds call every host function through a slot, so the executable may sit
    # anywhere; leaving it high also makes any host pointer handed to the game stand out.
    set(BOZ_GUEST_LINK_OPTIONS -Wl,--image-base=0x400000 -Wl,--disable-dynamicbase
        -Wl,--disable-high-entropy-va)
  endif()
endif()
target_link_libraries(codboz_s3e_loader PRIVATE boz_project boz_common boz_branding ${BOZ_CPU_LIBS} lua imgui_core ${BOZ_SYSTEM_LIBS})
boz_windows_resources(codboz_s3e_loader "BOZ Redux game")
target_link_options(codboz_s3e_loader PRIVATE ${BOZ_SYSTEM_LINK_OPTIONS} ${BOZ_GUEST_LINK_OPTIONS})
if(NOT WIN32 AND NOT ANDROID)
  target_link_options(codboz_s3e_loader PRIVATE -static-libgcc -static-libstdc++)
endif()
target_compile_options(codboz_s3e_loader PRIVATE -falign-functions=16)
if(NOT WIN32 AND NOT ANDROID)
  target_compile_options(codboz_s3e_loader PRIVATE -fno-pie)
  target_link_options(codboz_s3e_loader PRIVATE -no-pie)
endif()
set_target_properties(codboz_s3e_loader PROPERTIES ENABLE_EXPORTS ON BUILD_RPATH "$ORIGIN")
add_dependencies(codboz_s3e_loader SDL2 SDL2_mixer)

if(ANDROID)
  # The launcher on phones and TVs: the desktop launcher as libboz_launcher.so, run by its own
  # SDL activity (packaging/android LauncherActivity) in the app's main process; the game keeps
  # its own process. Android starts no programs, so the APK extractor is built in.
  add_library(boz_launcher SHARED
    src/launcher/main.cpp
    src/branding/brand_imgui.cpp
    src/launcher/game_files.cpp
    src/launcher/ini_file.cpp
    src/launcher/os.cpp
    src/launcher/os_android.cpp
    src/launcher/sha256.cpp
    src/client_config.c
    src/mods.c
    tools/apk_extract/codboz_apk_extract.c
    tools/apk_extract/apk_extract_inprocess.c
    third_party/lzma/LzmaDec.c
    third_party/imgui/backends/imgui_impl_sdl2.cpp
    third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp)
  set_source_files_properties(tools/apk_extract/codboz_apk_extract.c PROPERTIES
    COMPILE_DEFINITIONS "main=boz_apk_extract_main;exit=boz_apk_extract_exit")
  target_include_directories(boz_launcher PRIVATE include third_party/imgui/backends)
  target_compile_definitions(boz_launcher PRIVATE BOZ_VERSION="${BOZ_VERSION}")
  target_link_libraries(boz_launcher PRIVATE imgui_core boz_common boz_branding SDL2::SDL2 log)
  return() # the desktop launcher, the extractor program and the tests are desktop programs
endif()

# Launcher: Dear ImGui on SDL's 2D renderer (Direct3D on Windows, OpenGL or software on Linux).
add_library(imgui STATIC
  third_party/imgui/backends/imgui_impl_sdl2.cpp
  third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp)
target_include_directories(imgui PUBLIC third_party/imgui/backends)
target_link_libraries(imgui PUBLIC imgui_core SDL2::SDL2)

add_executable(boz-redux
  src/launcher/main.cpp
  src/branding/brand_imgui.cpp
  src/launcher/game_files.cpp
  src/launcher/ini_file.cpp
  src/launcher/os.cpp
  src/launcher/sha256.cpp
  src/client_config.c
  src/mods.c)
target_include_directories(boz-redux PRIVATE include)
target_compile_options(boz-redux PRIVATE ${BOZ_WARNINGS})
target_link_libraries(boz-redux PRIVATE imgui boz_common boz_branding)
target_compile_definitions(boz-redux PRIVATE BOZ_VERSION="${BOZ_VERSION}")
boz_windows_resources(boz-redux "BOZ Redux launcher")
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
boz_windows_resources(codboz_apk_extract "BOZ Redux APK extractor")

enable_testing()

function(boz_test name)
  cmake_parse_arguments(T "" "" "SOURCES;DEFINES;INCLUDES" ${ARGN})
  add_executable(${name} tests/${name}.c ${T_SOURCES} tests/test_support.c)
  target_link_libraries(${name} PRIVATE boz_project boz_common ${BOZ_SYSTEM_LIBS})
  target_link_options(${name} PRIVATE ${BOZ_SYSTEM_LINK_OPTIONS} ${BOZ_GUEST_LINK_OPTIONS})
  target_compile_definitions(${name} PRIVATE ${T_DEFINES})
  target_compile_options(${name} PRIVATE -UNDEBUG)
  target_include_directories(${name} PRIVATE ${T_INCLUDES})
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

boz_test(s3e_socket_test SOURCES src/s3e_socket.c src/s3e_config.c)
boz_test(s3e_config_test SOURCES src/s3e_config.c src/game_patches/player_name.c)
boz_test(mdns_wire_test SOURCES src/mdns_wire.c)
boz_test(hud_sticks_test SOURCES src/game_patches/hud_sticks.c)
boz_test(prompt_button_test SOURCES src/game_patches/prompt_button.c)
boz_test(crash_recovery_test SOURCES src/game_patches/crash_recovery.c)
boz_test(str_util_test)
boz_test(s3e_zeroconf_test SOURCES tests/zeroconf_platform_fake.c src/s3e_zeroconf.c src/mdns_wire.c
         INCLUDES tests)
boz_test(s3e_timer_test SOURCES src/s3e_timer.c)
boz_test(client_config_test SOURCES src/client_config.c)
boz_test(mods_test SOURCES src/mods.c src/client_config.c)
boz_test(gamedef_test SOURCES src/gamedef.c)
boz_test(lua_runtime_test SOURCES src/lua_runtime.c src/lua_settings.c src/lua_assets.c src/mods.c src/client_config.c src/gamedef.c
         src/game_image.c ${BOZ_PLATFORM_SOURCES})
target_link_libraries(lua_runtime_test PRIVATE lua ${CMAKE_DL_LIBS})
boz_test(s3e_memory_test SOURCES src/s3e_memory.c)
boz_test(device_id_test SOURCES src/device_id.c ${BOZ_PLATFORM_SOURCES})
boz_test(s3e_audio_test SOURCES src/s3e_audio.c
         DEFINES dlsym=s3e_audio_test_dlsym dlclose=s3e_audio_test_dlclose
                 plat_lib_symbol=s3e_audio_test_dlsym plat_lib_close=s3e_audio_test_dlclose)
boz_test(s3e_audio_unit_test SOURCES src/s3e_audio_unit.c)
boz_test(s3e_input_test SOURCES src/s3e_input.c src/touch_controls.c src/client_config.c
         DEFINES dlsym=s3e_input_test_dlsym dlclose=s3e_input_test_dlclose
                 plat_lib_symbol=s3e_input_test_dlsym plat_lib_close=s3e_input_test_dlclose)

add_executable(arm_emu_test tests/arm_emu_test.c ${BOZ_CPU_SOURCES} ${BOZ_PLATFORM_SOURCES})
target_link_libraries(arm_emu_test PRIVATE boz_project boz_guest_heap ${BOZ_CPU_LIBS} ${BOZ_SYSTEM_LIBS})
if(NOT WIN32)
  target_link_options(arm_emu_test PRIVATE -no-pie)  # Unicorn is built without PIC
endif()
target_link_options(arm_emu_test PRIVATE ${BOZ_SYSTEM_LINK_OPTIONS} ${BOZ_GUEST_LINK_OPTIONS})
target_compile_options(arm_emu_test PRIVATE -falign-functions=16 -UNDEBUG)
set_target_properties(arm_emu_test PROPERTIES ENABLE_EXPORTS ON
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME arm_emu_test COMMAND arm_emu_test)

add_executable(lua_hooks_test tests/lua_hooks_test.c src/lua_hooks.c ${BOZ_CPU_SOURCES} ${BOZ_PLATFORM_SOURCES})
target_link_libraries(lua_hooks_test PRIVATE boz_project boz_common ${BOZ_CPU_LIBS} lua ${BOZ_SYSTEM_LIBS})
if(NOT WIN32)
  target_link_options(lua_hooks_test PRIVATE -no-pie)  # Unicorn is built without PIC
endif()
target_link_options(lua_hooks_test PRIVATE ${BOZ_SYSTEM_LINK_OPTIONS} ${BOZ_GUEST_LINK_OPTIONS})
target_compile_options(lua_hooks_test PRIVATE -falign-functions=16 -UNDEBUG)
set_target_properties(lua_hooks_test PROPERTIES ENABLE_EXPORTS ON
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME lua_hooks_test COMMAND lua_hooks_test)
