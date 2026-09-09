add_library(firncore STATIC
  src/image.cpp
  src/document.cpp
  src/commands.cpp
  src/raster.cpp
  src/mask.cpp
  src/blend.cpp
  src/io.cpp
  src/io_psp.cpp
)
target_include_directories(firncore PUBLIC include)
target_link_libraries(firncore PRIVATE stb)
# Vendored stb headers are not ours to fix.
set_source_files_properties(src/io.cpp src/io_psp.cpp PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers")
