find_package(SDL2 REQUIRED)
# SDL2 creates the context through legacy libGL; link the same library so
# our direct GL calls (texture upload) share its dispatch. GLVND libOpenGL
# silently no-ops here.
if(NOT WIN32)
  set(OpenGL_GL_PREFERENCE LEGACY)
endif()
find_package(OpenGL REQUIRED)
include(${CMAKE_SOURCE_DIR}/cmake/imgui.cmake)

add_executable(firn
  src/main.cpp
  src/App.cpp
  src/Vectors.cpp
  src/Drive.cpp
  src/Config.cpp
  src/ui/Canvas.cpp
  src/ui/Palettes.cpp
  src/ui/Menu.cpp
  src/ui/FileDialog.cpp
  src/ui/Adjust.cpp
  src/ui/TextDialog.cpp
  src/ui/VectorDialog.cpp
  src/ui/AdjustmentLayer.cpp
  src/ui/Toolbar.cpp
  src/tools/Tools.cpp
  src/tools/VectorTools.cpp
)
target_include_directories(firn PRIVATE src)
# Lets a development build find the sample tubes in the (gitignored) backup.
target_compile_definitions(firn PRIVATE FIRN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(firn PRIVATE firncore imgui SDL2::SDL2 OpenGL::GL)
if(WIN32)
  # Plain main(); SDL's own entry point is not used.
  target_compile_definitions(firn PRIVATE SDL_MAIN_HANDLED)
endif()
