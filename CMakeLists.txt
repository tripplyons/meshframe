cmake_minimum_required(VERSION 3.28)
project(meshframe VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(MESHFRAME_TESTS "Build tests" ON)

find_package(PkgConfig REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LUAJIT REQUIRED IMPORTED_TARGET luajit)
pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale libswresample)

set(MF_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

file(GLOB_RECURSE MF_CORE_SOURCES CONFIGURE_DEPENDS
  src/core/*.cpp src/model/*.cpp src/engine/*.cpp src/script/*.cpp src/media/*.cpp
  src/render/*.cpp src/audio/*.cpp src/exporter/*.cpp src/session/*.cpp)
add_library(meshframe_core STATIC ${MF_CORE_SOURCES})
target_include_directories(meshframe_core PUBLIC src)
target_compile_options(meshframe_core PRIVATE ${MF_WARNINGS})
target_link_libraries(meshframe_core PUBLIC
  SDL3::SDL3 fmt::fmt nlohmann_json::nlohmann_json Freetype::Freetype
  PkgConfig::LUAJIT PkgConfig::LIBXML2 PkgConfig::FFMPEG Threads::Threads)

file(GLOB_RECURSE MF_UI_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp)
add_library(meshframe_ui STATIC ${MF_UI_SOURCES})
target_compile_options(meshframe_ui PRIVATE ${MF_WARNINGS})
target_link_libraries(meshframe_ui PUBLIC meshframe_core)

add_executable(meshframe src/app/main.cpp)
target_compile_options(meshframe PRIVATE ${MF_WARNINGS})
target_link_libraries(meshframe PRIVATE meshframe_ui)

add_executable(meshframe-cli src/cli/main.cpp)
target_compile_options(meshframe-cli PRIVATE ${MF_WARNINGS})
target_link_libraries(meshframe-cli PRIVATE meshframe_core)

# Renders the projects in bench/projects and reports deterministic work counts.
option(MESHFRAME_BENCH "Build the render benchmark" OFF)
if(MESHFRAME_BENCH)
  add_executable(meshframe_bench tools/bench_render.cpp)
  target_compile_options(meshframe_bench PRIVATE ${MF_WARNINGS})
  target_link_libraries(meshframe_bench PRIVATE meshframe_core)
endif()

if(MESHFRAME_TESTS)
  find_package(GTest CONFIG REQUIRED)
  enable_testing()
  file(GLOB MF_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(meshframe_tests ${MF_TEST_SOURCES})
  target_compile_options(meshframe_tests PRIVATE ${MF_WARNINGS})
  target_link_libraries(meshframe_tests PRIVATE meshframe_ui GTest::gtest_main)
  # The bridge tests run the CLI binary.
  add_dependencies(meshframe_tests meshframe-cli)
  target_compile_definitions(meshframe_tests PRIVATE
    MF_TEST_BIN_DIR="$<TARGET_FILE_DIR:meshframe-cli>")
  include(GoogleTest)
  gtest_discover_tests(meshframe_tests DISCOVERY_MODE PRE_TEST)
endif()
