add_executable(dmdbg-smoke src/DmdbgSmoke.cpp)
target_link_libraries(dmdbg-smoke PRIVATE Qt6::Core)

if(MSVC)
    target_compile_options(dmdbg-smoke PRIVATE /W4)
else()
    target_compile_options(dmdbg-smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_test(NAME dmdbg-adapter.transcript-parser COMMAND dmdbg-smoke --transcript-self-test)
    add_test(NAME dmdbg-adapter.process-exit COMMAND dmdbg-smoke --process-exit-self-test)
    add_test(NAME dmdbg-adapter.rejects-missing-arguments COMMAND dmdbg-smoke)
    set_tests_properties(dmdbg-adapter.rejects-missing-arguments PROPERTIES WILL_FAIL TRUE)
    reid_configure_test_runtime(
        dmdbg-adapter.transcript-parser
        dmdbg-adapter.process-exit
        dmdbg-adapter.rejects-missing-arguments)
endif()
