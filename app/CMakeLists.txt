find_package(SDL2 REQUIRED)
# SDL2 creates the context through legacy libGL; link the same library so
# our direct GL calls (texture upload) share its dispatch. GLVND libOpenGL
# silently no-ops here.
set(OpenGL_GL_PREFERENCE LEGACY)
find_package(OpenGL REQUIRED)
include(${CMAKE_SOURCE_DIR}/cmake/imgui.cmake)

add_executable(firn
  src/main.cpp
  src/App.cpp
  src/ui/Canvas.cpp
  src/ui/Palettes.cpp
  src/ui/Menu.cpp
  src/ui/FileDialog.cpp
  src/ui/Adjust.cpp
  src/tools/Tools.cpp
)
target_include_directories(firn PRIVATE src)
target_link_libraries(firn PRIVATE firncore imgui SDL2::SDL2 OpenGL::GL)
