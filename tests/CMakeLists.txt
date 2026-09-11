add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE firncore)
target_compile_definitions(test_core PRIVATE FIRN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME core COMMAND test_core)

add_executable(test_psp_corpus test_psp_corpus.cpp)
target_link_libraries(test_psp_corpus PRIVATE firncore)
target_compile_definitions(test_psp_corpus PRIVATE FIRN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME psp_corpus COMMAND test_psp_corpus)

if(FIRN_BUILD_APP)
  add_executable(test_app_state test_app_state.cpp)
  target_compile_definitions(test_app_state PRIVATE SDL_MAIN_HANDLED)
  target_link_libraries(test_app_state PRIVATE firn_app)
  add_test(NAME app_state COMMAND test_app_state)
  if(WIN32)
    # SDL2.dll beside the test, as for the app (app/CMakeLists.txt).
    add_custom_command(TARGET test_app_state POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:test_app_state> $<TARGET_FILE_DIR:test_app_state>
      COMMAND_EXPAND_LISTS)
  endif()
endif()
