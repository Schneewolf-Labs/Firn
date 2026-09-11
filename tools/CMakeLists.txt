add_executable(firn-convert convert.cpp)
target_link_libraries(firn-convert PRIVATE firncore)

# The command line client talks to the in-app driver: a Unix socket, or on
# Windows the loopback port named by an address file (app/src/DriveAddress.h).
add_executable(firn-cli cli.cpp)
target_link_libraries(firn-cli PRIVATE firncore)
if(WIN32)
  target_include_directories(firn-cli PRIVATE ${CMAKE_SOURCE_DIR}/app/src)
  target_link_libraries(firn-cli PRIVATE ws2_32)
endif()
install(TARGETS firn-cli RUNTIME DESTINATION bin)
