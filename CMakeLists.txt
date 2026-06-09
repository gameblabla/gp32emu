cmake_minimum_required(VERSION 3.16)
project(gp32emu C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(GP32EMU_BUILD_SDL12 "Build optional SDL 1.2 platform backend and frontend" OFF)
option(GP32EMU_BUILD_SDL3 "Build optional SDL3 platform backend and frontend" OFF)
option(GP32EMU_BUILD_QT6 "Build Qt6/Linux GP32emu UI" OFF)
option(GP32EMU_BUILD_WIN64_FRONTEND "Build Win64 GP32emu UI" OFF)
option(GP32EMU_ENABLE_THREADS "Enable optional worker threads for audio/recording" ON)

add_library(gp32emu
    src/gp32.c
    src/arm920t.c
    src/s3c2400.c
    src/smartmedia.c
    src/smc_direct.c
    src/fxe.c
    src/fpk.c
    src/gp32_zip.c
    src/third_party/zip/zip.c
    src/input_script.c
    src/platform/platform.c
    src/audio/gp32_audio_resampler.c
    src/media/gp32_media.c
    src/media/gp32_video_effects.c
)
target_include_directories(gp32emu PUBLIC include PRIVATE src)
target_compile_definitions(gp32emu PUBLIC GP32EMU_ENABLE_THREADS=$<BOOL:${GP32EMU_ENABLE_THREADS}>)
if (GP32EMU_ENABLE_THREADS AND NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(gp32emu PUBLIC Threads::Threads)
endif()
target_compile_definitions(gp32emu PRIVATE _XOPEN_SOURCE=700 _FILE_OFFSET_BITS=64 ZIP_ENABLE_DEFLATE=0 ZIP_HAVE_SYMLINK=0)

if (NOT MSVC)
    set_source_files_properties(src/third_party/zip/zip.c PROPERTIES COMPILE_OPTIONS "-w")
else()
    set_source_files_properties(src/third_party/zip/zip.c PROPERTIES COMPILE_OPTIONS "/w")
endif()

if (GP32EMU_BUILD_SDL3)
    find_package(SDL3 REQUIRED CONFIG)
    add_library(gp32emu_sdl3
        src/platform/sdl3/common.c
        src/platform/sdl3/video.c
        src/platform/sdl3/audio.c
        src/platform/sdl3/input.c
        src/input/gp32_sdl3_input_shared.c
    )
    target_include_directories(gp32emu_sdl3 PUBLIC include PRIVATE src)
    target_link_libraries(gp32emu_sdl3 PUBLIC gp32emu SDL3::SDL3)
    if (MSVC)
        target_compile_options(gp32emu_sdl3 PRIVATE /W4)
    else()
        target_compile_options(gp32emu_sdl3 PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(gp32_sdl3 src/sdl3_main.c)
    target_include_directories(gp32_sdl3 PRIVATE src)
    target_link_libraries(gp32_sdl3 PRIVATE gp32emu gp32emu_sdl3 SDL3::SDL3)
endif()

if (GP32EMU_BUILD_QT6)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    find_package(SDL3 CONFIG)
    if (NOT TARGET gp32emu_sdl3)
        if (SDL3_FOUND)
            add_library(gp32emu_sdl3
                src/platform/sdl3/common.c
                src/platform/sdl3/video.c
                src/platform/sdl3/audio.c
                src/platform/sdl3/input.c
                src/input/gp32_sdl3_input_shared.c
            )
            target_include_directories(gp32emu_sdl3 PUBLIC include PRIVATE src)
            target_link_libraries(gp32emu_sdl3 PUBLIC gp32emu SDL3::SDL3)
        endif()
    endif()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    add_executable(gp32emu_qt6
        src/qt/main.cpp
        src/qt/GP32Engine.cpp
        src/qt/GP32MainWindow.cpp
        src/qt/GP32VideoWidget.cpp
        resources/gp32emu.qrc
    )
    target_include_directories(gp32emu_qt6 PRIVATE include src)
    target_link_libraries(gp32emu_qt6 PRIVATE gp32emu Qt6::Widgets)
    if (TARGET gp32emu_sdl3)
        target_compile_definitions(gp32emu_qt6 PRIVATE GP32EMU_QT_SDL3_AUDIO=1)
        target_link_libraries(gp32emu_qt6 PRIVATE gp32emu_sdl3 SDL3::SDL3)
    endif()
endif()

if (GP32EMU_BUILD_WIN64_FRONTEND)
    if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "GP32EMU_BUILD_WIN64_FRONTEND is Win64-only; no Win32 frontend is provided.")
    endif()
    find_package(SDL3 REQUIRED CONFIG)
    add_executable(gp32emu_win64 WIN32
        src/win64/gp32_win64_main.c
        src/win64/gp32_win64_video.c
        src/win64/gp32_win64_audio.c
        src/win64/gp32_win64_sdl_input.c
        src/input/gp32_sdl3_input_shared.c
        src/win64/gp32emu.rc
    )
    target_include_directories(gp32emu_win64 PRIVATE include src)
    target_compile_definitions(gp32emu_win64 PRIVATE GP32EMU_WIN64_FRONTEND=1 SDL_MAIN_HANDLED=1)
    target_link_libraries(gp32emu_win64 PRIVATE gp32emu SDL3::SDL3 user32 gdi32 comdlg32 winmm ole32 uuid d3d11 dxgi avrt shell32)
endif()
if (MSVC)
    target_compile_options(gp32emu PRIVATE /W4)
else()
    target_compile_options(gp32emu PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(gp32_headless src/headless_main.c)
target_link_libraries(gp32_headless PRIVATE gp32emu)

if (GP32EMU_BUILD_SDL12)
    find_package(SDL REQUIRED)
    add_library(gp32emu_sdl12
        src/platform/sdl12/common.c
        src/platform/sdl12/video.c
        src/platform/sdl12/audio.c
        src/platform/sdl12/input.c
    )
    target_include_directories(gp32emu_sdl12 PUBLIC include PRIVATE src ${SDL_INCLUDE_DIR})
    target_link_libraries(gp32emu_sdl12 PUBLIC gp32emu ${SDL_LIBRARY})
    if (MSVC)
        target_compile_options(gp32emu_sdl12 PRIVATE /W4)
    else()
        target_compile_options(gp32emu_sdl12 PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(gp32_sdl12 src/sdl12_main.c)
    target_include_directories(gp32_sdl12 PRIVATE src ${SDL_INCLUDE_DIR})
    target_link_libraries(gp32_sdl12 PRIVATE gp32emu gp32emu_sdl12 ${SDL_LIBRARY})
endif()
