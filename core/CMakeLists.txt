add_library(firncore STATIC
  src/image.cpp
  src/document.cpp
  src/commands.cpp
  src/raster.cpp
  src/mask.cpp
  src/blend.cpp
  src/adjust.cpp
  src/effects.cpp
  src/text.cpp
  src/vector.cpp
  src/adjustment.cpp
  src/photo.cpp
  src/effects_geo.cpp
  src/effects_art.cpp
  src/json.cpp
  src/print.cpp
  src/raster16.cpp
  src/io.cpp
  src/io_psp.cpp
)
target_include_directories(firncore PUBLIC include)
target_link_libraries(firncore PRIVATE stb)
# Vendored stb headers are not ours to fix.
if(NOT MSVC)
  set_source_files_properties(src/io.cpp src/io_psp.cpp src/text.cpp PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers;-Wno-unused-function")
endif()
