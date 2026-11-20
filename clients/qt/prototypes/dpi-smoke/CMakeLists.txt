add_executable(dpi-smoke
    src/DpiSmokeTest.cpp
)

target_link_libraries(dpi-smoke PRIVATE DmDpi::DmDpi)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(dpi-smoke PRIVATE DM64)
endif()

if(MSVC)
    target_compile_options(dpi-smoke PRIVATE /W4)
else()
    target_compile_options(dpi-smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

get_filename_component(DmDpi_LIBRARY_DIR "${DmDpi_LIBRARY}" DIRECTORY)
set_target_properties(dpi-smoke PROPERTIES
    BUILD_RPATH "${DmDpi_LIBRARY_DIR}"
)

if(BUILD_TESTING)
    add_test(NAME dpi-smoke.rejects-missing-arguments COMMAND dpi-smoke)
    set_tests_properties(dpi-smoke.rejects-missing-arguments PROPERTIES
        WILL_FAIL TRUE
    )
    reid_configure_test_runtime(dpi-smoke.rejects-missing-arguments)
endif()
