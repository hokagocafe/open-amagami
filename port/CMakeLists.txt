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

# A personal utility, never part of a release: a window with the text on the game's screen (docs/technical/personal.md).
option(AMG_PERSONAL "Build the personal text window (src/personal/)" OFF)

# Vendored third-party decoder (third_party/clhca/README.md): built as it is, without the project's warnings.
set_source_files_properties(src/third_party/clhca/clhca.c PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
file(GLOB ATRAC9_SOURCES src/third_party/libatrac9/*.c)  # LibAtrac9 (third_party/libatrac9/README.md), also built as it is
set_source_files_properties(${ATRAC9_SOURCES} PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")

# The script machine: loader, interpreter, built-in classes. No game data in the repository.
add_library(amagami_vm STATIC src/vm.cpp src/loader.cpp src/natives.cpp src/gfx.cpp src/audio.cpp src/cpk.cpp
            src/hca.cpp src/at9.cpp src/skins.cpp src/menu_pointer.cpp src/mouse_menus.cpp src/cursor_art.cpp src/port_settings.cpp src/paths.cpp src/ui_text.cpp src/third_party/clhca/clhca.c ${ATRAC9_SOURCES})
target_include_directories(amagami_vm PUBLIC src)
target_link_libraries(amagami_vm PUBLIC Threads::Threads)
if(AMG_PERSONAL)
  target_compile_definitions(amagami_vm PUBLIC AMG_PERSONAL=1)  # DrawItem carries the character code
  target_sources(amagami_vm PRIVATE src/personal/text_extract.cpp)
endif()

add_executable(amagami_vm_cli src/main.cpp)
target_link_libraries(amagami_vm_cli PRIVATE amagami_vm)
set_target_properties(amagami_vm_cli PROPERTIES OUTPUT_NAME amagami_vm)

if(WIN32)
  # The playable front end: a Win32 window, no other dependency.
  add_executable(amagami WIN32 src/win_main.cpp src/d3d_present.cpp src/game_import.cpp src/input.cpp src/data_check.cpp src/launcher.cpp src/launcher_dialogs.cpp src/updater.cpp src/shortcut.cpp src/update_check.cpp)
  # The release this build is ("v0.1.3", given by the release workflow); anything else is a development build, which never
  # asks for updates.
  set(AMG_VERSION "dev" CACHE STRING "The release tag this build is")
  target_compile_definitions(amagami PRIVATE AMG_VERSION="${AMG_VERSION}")
  if(AMG_PERSONAL)
    target_sources(amagami PRIVATE src/personal/text_window.cpp)
  endif()
  # The program file's icon: our mascot (art/amagami.ico, made by `tools/hokago.py make_ico` from art/icon.svg rendered at
  # 512 pixels). The windows and the shortcuts show the game's own icon instead, taken from the player's copy at run time
  # (shortcut.cpp).
  set(AMG_ICON "${CMAKE_CURRENT_SOURCE_DIR}/art/amagami.ico")
  if(EXISTS "${AMG_ICON}")
    enable_language(RC)
    configure_file(res/amagami.rc.in "${CMAKE_CURRENT_BINARY_DIR}/amagami.rc" @ONLY)
    target_sources(amagami PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/amagami.rc")
    target_compile_definitions(amagami PRIVATE AMG_HAS_ICON=1)
  endif()
  if(MSVC)
    # UTF-8 as the code page and long paths (res/amagami.manifest): folder names with accents or Japanese letters, deep folders.
    target_sources(amagami PRIVATE res/amagami.manifest)
  endif()
  target_link_libraries(amagami PRIVATE amagami_vm user32 gdi32 winmm shell32 ole32 uuid comctl32 winhttp d3d11 dxgi d3dcompiler bcrypt)
  # SDL3, for gamepads only (src/third_party/sdl3/README.md, D-025): the library of this build's size (x86 or x64) and its DLL
  # next to the program.
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(AMG_ARCH x64)
  else()
    set(AMG_ARCH x86)
  endif()
  set(AMG_SDL "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/sdl3")
  if(NOT EXISTS "${AMG_SDL}/lib/${AMG_ARCH}/SDL3.lib")
    # The public repository does not carry the library: it is downloaded once, checked against a fixed hash.
    set(AMG_SDL_ZIP "${CMAKE_BINARY_DIR}/SDL3-devel-3.4.18-VC.zip")
    file(DOWNLOAD "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-VC.zip" "${AMG_SDL_ZIP}"
         EXPECTED_HASH SHA256=78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd SHOW_PROGRESS)
    file(ARCHIVE_EXTRACT INPUT "${AMG_SDL_ZIP}" DESTINATION "${CMAKE_BINARY_DIR}/sdl3-download")
    set(AMG_SDL "${CMAKE_BINARY_DIR}/sdl3-download/SDL3-3.4.18")
  endif()
  target_include_directories(amagami PRIVATE "${AMG_SDL}/include")
  target_link_libraries(amagami PRIVATE "${AMG_SDL}/lib/${AMG_ARCH}/SDL3.lib")
  add_custom_command(TARGET amagami POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AMG_SDL}/lib/${AMG_ARCH}/SDL3.dll" "$<TARGET_FILE_DIR:amagami>"
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
