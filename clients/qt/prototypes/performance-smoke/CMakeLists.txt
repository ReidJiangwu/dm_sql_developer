add_executable(stage-zero-performance
    src/StageZeroPerformance.cpp
    ../../src/codeintel/LanguageServerClient.cpp
    ../../src/codeintel/LanguageServerClient.h
    ../../src/ui/SqlEditor.cpp
    ../../src/ui/SqlEditor.h
    ../../src/ui/Theme.cpp
    ../../src/ui/Theme.h
)

target_include_directories(stage-zero-performance PRIVATE ../../src/ui)
target_link_libraries(stage-zero-performance PRIVATE Qt6::Widgets)

if(MSVC)
    target_compile_options(stage-zero-performance PRIVATE /W4)
else()
    target_compile_options(stage-zero-performance PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_test(NAME performance.editor-30 COMMAND stage-zero-performance --editor-probe)
    add_test(NAME performance.export-stream COMMAND stage-zero-performance --export-probe)
    add_test(NAME performance.batch-model-500 COMMAND stage-zero-performance --batch-model-probe)
    add_test(NAME performance.multi-instance COMMAND stage-zero-performance --multi-instance-probe)
    set_tests_properties(
        performance.editor-30
        performance.export-stream
        performance.batch-model-500
        performance.multi-instance
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    reid_configure_test_runtime(
        performance.editor-30
        performance.export-stream
        performance.batch-model-500
        performance.multi-instance)
endif()
