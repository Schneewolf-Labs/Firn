add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE firncore)
target_compile_definitions(test_core PRIVATE FIRN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME core COMMAND test_core)

add_executable(test_psp_corpus test_psp_corpus.cpp)
target_link_libraries(test_psp_corpus PRIVATE firncore)
target_compile_definitions(test_psp_corpus PRIVATE FIRN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME psp_corpus COMMAND test_psp_corpus)
