cmake_minimum_required(VERSION 3.16)
project(open_amagami_port C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The C runtime is linked into the program, so that nothing has to be installed beside it (docs/DECISIONS.md D-027).
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Threads REQUIRED)

# Vendored third-party decoder (third_party/clhca/README.md): built as it is, without the project's warnings.
set_source_files_properties(src/third_party/clhca/clhca.c PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
file(GLOB ATRAC9_SOURCES src/third_party/libatrac9/*.c)  # LibAtrac9 (third_party/libatrac9/README.md), also built as it is
set_source_files_properties(${ATRAC9_SOURCES} PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")

# The script machine: loader, interpreter, built-in classes. No game data in the repository.
add_library(amagami_vm STATIC src/vm.cpp src/loader.cpp src/natives.cpp src/gfx.cpp src/audio.cpp src/ahx.cpp src/cpk.cpp
            src/hca.cpp src/at9.cpp src/skins.cpp src/menu_pointer.cpp src/port_settings.cpp src/paths.cpp src/third_party/clhca/clhca.c ${ATRAC9_SOURCES})
target_include_directories(amagami_vm PUBLIC src)
target_link_libraries(amagami_vm PUBLIC Threads::Threads)

add_executable(amagami_vm_cli src/main.cpp)
target_link_libraries(amagami_vm_cli PRIVATE amagami_vm)
set_target_properties(amagami_vm_cli PROPERTIES OUTPUT_NAME amagami_vm)

if(WIN32)
  # The playable front end: a Win32 window, no other dependency.
  add_executable(amagami WIN32 src/win_main.cpp src/d3d_present.cpp src/game_import.cpp src/input.cpp src/data_check.cpp src/launcher.cpp)
  # The program's icon: the game's own if it is there (assets/icon/amagami.ico, made by `tools/hokago.py make_ico`
  # from assets/icon/game-icon.jpg; assets/ is not tracked, D-012), otherwise our mascot (art/amagami.ico, made the
  # same way from art/icon.svg rendered at 512 pixels), which is what the public builds carry.
  set(AMG_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../assets/icon/amagami.ico")
  if(NOT EXISTS "${AMG_ICON}")
    set(AMG_ICON "${CMAKE_CURRENT_SOURCE_DIR}/art/amagami.ico")
  endif()
  if(EXISTS "${AMG_ICON}")
    enable_language(RC)
    configure_file(res/amagami.rc.in "${CMAKE_CURRENT_BINARY_DIR}/amagami.rc" @ONLY)
    target_sources(amagami PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/amagami.rc")
    target_compile_definitions(amagami PRIVATE AMG_HAS_ICON=1)
  endif()
  target_link_libraries(amagami PRIVATE amagami_vm user32 gdi32 winmm shell32 ole32 comctl32 d3d11 dxgi d3dcompiler bcrypt)
  # SDL3, for gamepads only (src/third_party/sdl3/README.md, D-025): the 32-bit library and its DLL next to the program.
  set(AMG_SDL "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/sdl3")
  if(NOT EXISTS "${AMG_SDL}/lib/x86/SDL3.lib")
    # The public repository does not carry the library: it is downloaded once, checked against a fixed hash.
    set(AMG_SDL_ZIP "${CMAKE_BINARY_DIR}/SDL3-devel-3.4.18-VC.zip")
    file(DOWNLOAD "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-VC.zip" "${AMG_SDL_ZIP}"
         EXPECTED_HASH SHA256=78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd SHOW_PROGRESS)
    file(ARCHIVE_EXTRACT INPUT "${AMG_SDL_ZIP}" DESTINATION "${CMAKE_BINARY_DIR}/sdl3-download")
    set(AMG_SDL "${CMAKE_BINARY_DIR}/sdl3-download/SDL3-3.4.18")
  endif()
  target_include_directories(amagami PRIVATE "${AMG_SDL}/include")
  target_link_libraries(amagami PRIVATE "${AMG_SDL}/lib/x86/SDL3.lib")
  add_custom_command(TARGET amagami POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AMG_SDL}/lib/x86/SDL3.dll" "$<TARGET_FILE_DIR:amagami>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/skins" "$<TARGET_FILE_DIR:amagami>/skins"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/script" "$<TARGET_FILE_DIR:amagami>/script"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/art" "$<TARGET_FILE_DIR:amagami>/art"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:amagami>/data")
endif()

if(MSVC)
  target_compile_options(amagami_vm PRIVATE /W3 /fp:precise)
else()
  # No fused multiply-add: frames are compared pixel by pixel with the reference model.
  target_compile_options(amagami_vm PRIVATE -Wall -Wextra -ffp-contract=off)
endif()
