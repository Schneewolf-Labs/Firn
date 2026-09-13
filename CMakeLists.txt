cmake_minimum_required(VERSION 3.22)
project(Firn VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(FIRN_BUILD_APP   "Build the Dear ImGui desktop app" ON)
option(FIRN_BUILD_TESTS "Build core tests" ON)

include(GNUInstallDirs)
include(${CMAKE_SOURCE_DIR}/cmake/runtime.cmake)

if(MSVC)
  add_compile_options(/W3 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Header-only third party
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

include(${CMAKE_SOURCE_DIR}/cmake/webp.cmake)   # WebP codec for core/src/io.cpp

add_subdirectory(core)

add_subdirectory(tools)

if(FIRN_BUILD_APP)
  add_subdirectory(app)
endif()

if(FIRN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Installation: `cmake --install build` (default prefix /usr/local; add
# --prefix ~/.local for a per-user install). Installs the app, the converter,
# a desktop entry and the icon.
install(TARGETS firn-convert firn-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md CHANGELOG.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
if(WIN32)
  install(FILES $<TARGET_RUNTIME_DLLS:firn-convert> $<TARGET_RUNTIME_DLLS:firn-cli>
    DESTINATION ${CMAKE_INSTALL_BINDIR})
  # Ship the redistributable release CRT beside the executables so the ZIP
  # also runs on machines without Visual Studio or the VC++ redistributable.
  if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    include(InstallRequiredSystemLibraries)
  endif()
endif()
if(FIRN_BUILD_APP)
  install(TARGETS firn RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(NOT WIN32)
    install(FILES assets/firn.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    foreach(size 48 128 256)
      install(FILES assets/icon-${size}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME firn.png)
    endforeach()
  else()
    # The DLLs the app links (SDL2 from vcpkg) go next to the executables.
    install(FILES $<TARGET_RUNTIME_DLLS:firn> DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()

# Archives for releases: `cpack -G TGZ` (Linux, macOS) or `cpack -G ZIP` (Windows)
# in the build directory packs the install tree as Firn-<version>-<system>.
set(CPACK_PACKAGE_NAME "Firn")
set(CPACK_PACKAGE_VENDOR "Schneewolf Labs")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Raster image editor")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "Firn-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES TRUE)
set(CPACK_GENERATOR "TGZ")
if(WIN32)
  set(CPACK_GENERATOR "ZIP")
endif()
include(CPack)
