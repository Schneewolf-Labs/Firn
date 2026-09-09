cmake_minimum_required(VERSION 3.22)
project(PSP9 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(PSP9_BUILD_APP   "Build the Dear ImGui desktop app" ON)
option(PSP9_BUILD_TESTS "Build core tests" ON)

add_compile_options(-Wall -Wextra -Wpedantic)

# Header-only third party
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

add_subdirectory(core)

if(PSP9_BUILD_APP)
  add_subdirectory(app)
endif()

if(PSP9_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
