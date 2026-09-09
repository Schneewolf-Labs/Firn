add_library(firncore STATIC
  src/image.cpp
  src/document.cpp
  src/commands.cpp
  src/raster.cpp
  src/io.cpp
)
target_include_directories(firncore PUBLIC include)
target_link_libraries(firncore PRIVATE stb)
# Vendored stb headers are not ours to fix.
set_source_files_properties(src/io.cpp PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers")
