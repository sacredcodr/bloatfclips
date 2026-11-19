cmake_minimum_required(VERSION 3.20)
project(clipper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(clipper_core STATIC
  src/config.cpp
  src/ring_buffer.cpp
  src/mp4_muxer.cpp
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
    user32
)

add_executable(clipper_tests
  tests/unit_tests.cpp
)

target_link_libraries(clipper_tests
  PRIVATE
    clipper_core
    d3d11
    dxgi
    dxguid
    ole32
    mfplat
    mfreadwrite
    mfuuid
    wmcodecdspuuid
)
