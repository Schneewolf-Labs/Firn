# Copy runtime dependencies for every executable, including command-line tools
# and tests. A static dependency graph legitimately has no DLLs to copy.
function(firn_copy_runtime_dlls target)
  if(WIN32)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${target}-copy-$<CONFIG>.cmake" CONTENT
"set(dlls \"$<TARGET_RUNTIME_DLLS:${target}>\")
foreach(dll IN LISTS dlls)
  get_filename_component(name \"\${dll}\" NAME)
  file(COPY_FILE \"\${dll}\" \"$<TARGET_FILE_DIR:${target}>/\${name}\" ONLY_IF_DIFFERENT)
endforeach()
")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/${target}-copy-$<CONFIG>.cmake"
      VERBATIM)
  endif()
endfunction()
