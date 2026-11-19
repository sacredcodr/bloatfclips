cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
# Static CRT so the shipped exe runs without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(clipper LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(clipper_core STATIC
  src/config.cpp
  src/clip_library.cpp
  src/ring_buffer.cpp
  src/mp4_muxer.cpp
  src/video_pipeline.cpp
  src/audio_engine.cpp
  src/recorder_engine.cpp
)

target_include_directories(clipper_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvcodec
)

target_compile_definitions(clipper_core
  PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

target_compile_options(clipper_core
  PUBLIC
    /W4
    /EHsc
    /permissive-
    /utf-8
    $<$<CONFIG:Release>:/O2>
)

target_link_libraries(clipper_core
  PUBLIC
    d3d11
    dxgi
    dxguid
    ole32
    mfplat
    mfuuid
    wmcodecdspuuid
    avrt
    winmm
    user32
)

add_executable(clipper WIN32
  src/main.cpp
  src/settings_window.cpp
  src/app.rc
)
set_target_properties(clipper PROPERTIES OUTPUT_NAME bloatfclips)

target_link_libraries(clipper
  PRIVATE
    clipper_core
    shell32
    gdi32
    dwmapi
    shcore
    d2d1
    dwrite
    windowscodecs
    mfreadwrite
)

add_executable(clipper_tests
  tests/unit_tests.cpp
)

target_link_libraries(clipper_tests
  PRIVATE
    clipper_core
    mfreadwrite
)

add_executable(clipper_bench
  tools/clipper_bench.cpp
)

target_link_libraries(clipper_bench
  PRIVATE
    clipper_core
    mfreadwrite
    psapi
    d3dcompiler
)
