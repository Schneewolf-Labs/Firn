add_executable(firn-convert convert.cpp)
target_link_libraries(firn-convert PRIVATE firncore)

# The command line client talks to the in-app driver over a Unix socket, so
# it builds where that does.
if(NOT WIN32)
  add_executable(firn-cli cli.cpp)
  target_link_libraries(firn-cli PRIVATE firncore)
  install(TARGETS firn-cli RUNTIME DESTINATION bin)
endif()
