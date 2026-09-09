add_library(psp9core STATIC
  src/image.cpp
  src/document.cpp
  src/commands.cpp
  src/io.cpp
)
target_include_directories(psp9core PUBLIC include)
target_link_libraries(psp9core PRIVATE stb)
# Vendored stb headers are not ours to fix.
set_source_files_properties(src/io.cpp PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers")
