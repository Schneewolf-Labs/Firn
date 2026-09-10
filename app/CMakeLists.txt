find_package(SDL2 REQUIRED)
# SDL2 creates the context through legacy libGL; link the same library so
# our direct GL calls (texture upload) share its dispatch. GLVND libOpenGL
# silently no-ops here.
if(NOT WIN32)
  set(OpenGL_GL_PREFERENCE LEGACY)
endif()
find_package(OpenGL REQUIRED)
include(${CMAKE_SOURCE_DIR}/cmake/imgui.cmake)

# The window icon, embedded as a byte array so the binary is self-contained.
set(FIRN_ICON_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/IconData.cpp)
add_custom_command(
  OUTPUT ${FIRN_ICON_CPP}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/assets/icon-128.png -DOUTPUT=${FIRN_ICON_CPP}
          -DSYMBOL=kFirnIconPng -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/assets/icon-128.png ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  COMMENT "Embedding the window icon")

# Version, git commit and build date, refreshed on every build.
set(FIRN_VERSION_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.cpp)
add_custom_target(firn_version
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${FIRN_VERSION_CPP} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
          -DVERSION=${PROJECT_VERSION} -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
  BYPRODUCTS ${FIRN_VERSION_CPP}
  COMMENT "Recording the version and commit")

add_executable(firn
  src/main.cpp
  ${FIRN_ICON_CPP}
  ${FIRN_VERSION_CPP}
  src/ui/About.cpp
  src/App.cpp
  src/Vectors.cpp
  src/Drive.cpp
  src/Script.cpp
  src/Config.cpp
  src/ui/Canvas.cpp
  src/ui/Palettes.cpp
  src/ui/Menu.cpp
  src/ui/FileDialog.cpp
  src/ui/Adjust.cpp
  src/ui/TextDialog.cpp
  src/ui/VectorDialog.cpp
  src/ui/AdjustmentLayer.cpp
  src/ui/ImageMenu.cpp
  src/ui/Toolbar.cpp
  src/ui/Icons.cpp
  src/ui/MaterialDialog.cpp
  src/ui/SelectionMenu.cpp
  src/tools/Tools.cpp
  src/tools/VectorTools.cpp
  src/tools/DeformTools.cpp
  src/tools/WarpTools.cpp
)
target_include_directories(firn PRIVATE src)
add_dependencies(firn firn_version)
# Lets a development build find the sample tubes in the (gitignored) backup.
target_compile_definitions(firn PRIVATE FIRN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(firn PRIVATE firncore imgui SDL2::SDL2 OpenGL::GL)
if(WIN32)
  # Plain main(); SDL's own entry point is not used.
  target_compile_definitions(firn PRIVATE SDL_MAIN_HANDLED)
  target_sources(firn PRIVATE ${CMAKE_SOURCE_DIR}/assets/firn.rc)
endif()
