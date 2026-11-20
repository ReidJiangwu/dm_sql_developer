cmake_minimum_required(VERSION 3.25)

set(REID_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../version.json")
set(REID_RELEASE_NOTES_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../release-notes.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${REID_VERSION_FILE}" "${REID_RELEASE_NOTES_FILE}")
file(READ "${REID_VERSION_FILE}" REID_VERSION_JSON)
string(JSON REID_APP_VERSION GET "${REID_VERSION_JSON}" version)
if(NOT REID_APP_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "version.json version must use major.minor.patch")
endif()
file(READ "${REID_RELEASE_NOTES_FILE}" REID_RELEASE_NOTES_JSON)
string(JSON REID_RELEASE_NOTES_CURRENT_VERSION GET
    "${REID_RELEASE_NOTES_JSON}" releases 0 version)
if(NOT REID_RELEASE_NOTES_CURRENT_VERSION STREQUAL REID_APP_VERSION)
    message(FATAL_ERROR
        "release-notes.json first version must match version.json (${REID_APP_VERSION})")
endif()
string(REGEX REPLACE "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" "\\1,\\2,\\3,0"
    REID_APP_VERSION_RC "${REID_APP_VERSION}")

project(GzydDmToolsQt VERSION ${REID_APP_VERSION} LANGUAGES CXX)

if(REID_CROSS_AR AND REID_CROSS_RANLIB)
    set(CMAKE_AR "${REID_CROSS_AR}")
    set(CMAKE_RANLIB "${REID_CROSS_RANLIB}")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "The Qt client targets Windows. Package the Windows runtime with Wine for macOS; native Linux/UOS and macOS clients are retired.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(cmake/Edition.cmake)

option(REID_REQUIRE_EMBEDDED_DOWNLOAD_KEY
    "Fail configuration when REID_DMTOOLS_DOWNLOAD_KEY is not set" OFF)
set(REID_DOWNLOAD_KEY "$ENV{REID_DMTOOLS_DOWNLOAD_KEY}")
if(NOT GZYD_FEATURE_internalHub)
    set(REID_DOWNLOAD_KEY "")
    if(REID_REQUIRE_EMBEDDED_DOWNLOAD_KEY)
        message(FATAL_ERROR "Community builds must not require an internal download key")
    endif()
endif()
if(REID_REQUIRE_EMBEDDED_DOWNLOAD_KEY AND REID_DOWNLOAD_KEY STREQUAL "")
    message(FATAL_ERROR
        "REID_DMTOOLS_DOWNLOAD_KEY must be set for a distributable build")
endif()
string(HEX "${REID_DOWNLOAD_KEY}" REID_DOWNLOAD_KEY_HEX)
string(LENGTH "${REID_DOWNLOAD_KEY_HEX}" REID_DOWNLOAD_KEY_HEX_LENGTH)
math(EXPR REID_DOWNLOAD_KEY_SIZE "${REID_DOWNLOAD_KEY_HEX_LENGTH} / 2")
set(REID_DOWNLOAD_KEY_BYTES "")
if(REID_DOWNLOAD_KEY_SIZE GREATER 0)
    math(EXPR REID_DOWNLOAD_KEY_LAST_INDEX "${REID_DOWNLOAD_KEY_SIZE} - 1")
    foreach(REID_DOWNLOAD_KEY_INDEX RANGE 0 ${REID_DOWNLOAD_KEY_LAST_INDEX})
        math(EXPR REID_DOWNLOAD_KEY_HEX_INDEX "${REID_DOWNLOAD_KEY_INDEX} * 2")
        string(SUBSTRING "${REID_DOWNLOAD_KEY_HEX}"
            ${REID_DOWNLOAD_KEY_HEX_INDEX} 2 REID_DOWNLOAD_KEY_BYTE_HEX)
        math(EXPR REID_DOWNLOAD_KEY_MASK
            "(0xA7 + (${REID_DOWNLOAD_KEY_INDEX} * 73)) & 0xFF")
        math(EXPR REID_DOWNLOAD_KEY_BYTE
            "0x${REID_DOWNLOAD_KEY_BYTE_HEX} ^ ${REID_DOWNLOAD_KEY_MASK}")
        string(APPEND REID_DOWNLOAD_KEY_BYTES
            "${REID_DOWNLOAD_KEY_BYTE}, ")
    endforeach()
endif()
set(REID_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${REID_GENERATED_INCLUDE_DIR}")
configure_file(cmake/EmbeddedDownloadKeyData.h.in
    "${REID_GENERATED_INCLUDE_DIR}/EmbeddedDownloadKeyData.h" @ONLY)
unset(REID_DOWNLOAD_KEY)
unset(REID_DOWNLOAD_KEY_HEX)
unset(REID_DOWNLOAD_KEY_BYTES)

find_package(Qt6 6.4 REQUIRED COMPONENTS Concurrent Network Sql Widgets)
set(ZLIB_USE_STATIC_LIBS ON)
if(NOT ZLIB_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../build/windows/toolchains/mingw1310/x86_64-w64-mingw32/include/zlib.h")
    set(ZLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../build/windows/toolchains/mingw1310/x86_64-w64-mingw32")
endif()
find_package(ZLIB 1.2.13 REQUIRED)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(DmDpi REQUIRED)
if(WIN32 AND NOT Sodium_ROOT)
    set(Sodium_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build/windows/vendor/libsodium-1.0.22/libsodium-win64"
        CACHE PATH "Project-local libsodium root")
endif()
find_package(Sodium 1.0.22 EXACT REQUIRED)
qt_standard_project_setup()

include(FetchContent)
set(BUILD_TRANSLATIONS OFF CACHE BOOL "Do not build QtKeychain translations" FORCE)
set(BUILD_TEST_APPLICATION OFF CACHE BOOL "Do not build QtKeychain test application" FORCE)
set(BUILD_QTQUICK_DEMO OFF CACHE BOOL "Do not build QtKeychain demo" FORCE)
set(LIBSECRET_SUPPORT OFF CACHE BOOL "Use QtKeychain DBus backends without libsecret build dependency" FORCE)
FetchContent_Declare(qtkeychain
    GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
    GIT_TAG 875f77d9f61bd97fd84cca47ce3bc71186dfbd09
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
set(BUILD_TESTING OFF)
FetchContent_MakeAvailable(qtkeychain)
set(BUILD_TESTING ON)

add_library(reid-publication-engine STATIC
    src/core/PublicationTask.cpp src/core/PublicationTask.h
    src/core/ContextBootstrapTask.cpp src/core/ContextBootstrapTask.h
    src/core/PublicationSchedule.cpp src/core/PublicationSchedule.h src/core/CommandParallelPlan.cpp
    src/core/CommandDiagnostics.h
    src/data/PublicationEngine.cpp src/data/PublicationEngine.h
    src/data/PublicationBatch.cpp
    src/data/PublicationAutomation.cpp src/data/NativeProgramDiagnostics.h src/data/ModuleOwnership.h
    src/core/ConnectionProfile.cpp src/core/ConnectionStore.cpp
    src/core/CommandScript.cpp src/core/SqlStatementParser.cpp
    src/core/SqlSafetyAnalyzer.cpp src/core/EditableQuery.cpp src/core/SqlTools.cpp
    src/core/TestCallBuilder.cpp src/core/RuntimeErrorParser.cpp
    src/data/DpiSession.cpp src/data/DpiSession.h
    src/data/DpiEndpointRouting.cpp src/data/ConnectionFailure.cpp
    src/data/TestTypes.cpp src/data/XlsxWriter.cpp src/data/PackageBodyBackupStore.cpp
    src/security/CredentialStore.cpp)
target_link_libraries(reid-publication-engine PUBLIC
    DmDpi::DmDpi Qt6::Concurrent Qt6::Core Qt6::Network Qt6::Sql qt6keychain)
target_compile_definitions(reid-publication-engine PUBLIC DM64)
if(WIN32)
    target_link_libraries(reid-publication-engine PRIVATE advapi32)
endif()
# Plain add_executable deliberately selects the Windows console subsystem.
add_executable(GzydDmToolsCli src/cli/main.cpp)
set_target_properties(GzydDmToolsCli PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(GzydDmToolsCli PRIVATE reid-publication-engine)
target_compile_definitions(GzydDmToolsCli PRIVATE REID_APP_VERSION=\"${REID_APP_VERSION}\")

qt_add_executable(GzydDmTools
    src/main.cpp
    src/core/AppPaths.h
    src/core/LegacyDataMigration.h
    src/core/LegacyDataMigration.cpp
    src/ui/LegacyMigrationProgressDialog.h
    src/core/HistoryCleanup.h
    src/ui/HistoryCleanupDialog.h
    src/core/ConnectionProfile.cpp
    src/core/ConnectionProfile.h
    src/core/ConnectionStore.cpp
    src/core/ConnectionStore.h
    src/core/ParallelExecutionSettings.h
    src/core/ObjectChangePlan.cpp
    src/core/ObjectChangePlan.h
    src/core/CommandScript.cpp
    src/core/CommandScript.h
    src/core/CommandReconnectSettings.h
    src/core/CommandParallelPlan.h
    src/core/CommandParallelPlan.cpp
    src/core/SqlStatementParser.cpp
    src/core/SqlStatementParser.h
    src/core/SqlSafetyAnalyzer.cpp
    src/core/SqlSafetyAnalyzer.h
    src/core/SqlTools.cpp
    src/core/SqlTools.h
    src/core/TestCallBuilder.cpp
    src/core/TestCallBuilder.h
    src/core/RuntimeErrorParser.cpp
    src/core/RuntimeErrorParser.h
    src/core/DbmsDebugProtocol.cpp
    src/core/DbmsDebugProtocol.h
    src/core/DmdbgProtocol.cpp
    src/core/DmdbgProtocol.h
    src/core/DmdbgCallBuilder.cpp
    src/core/DmdbgCallBuilder.h
    src/core/EditableQuery.cpp
    src/core/EditableQuery.h
    src/core/EmbeddedUpdateCredential.cpp
    src/core/EmbeddedUpdateCredential.h
    src/core/DbaDiagnostics.cpp
    src/core/DbaDiagnostics.h
    src/core/DbaManagement.cpp
    src/core/DbaManagement.h
    src/core/DatabaseEnvironment.cpp
    src/core/DatabaseEnvironment.h
    src/core/RoleEditorModel.cpp
    src/core/RoleEditorModel.h
    src/core/TestDataGenerator.cpp
    src/core/TestDataGenerator.h
    src/core/UpdateManifest.cpp
    src/core/UpdateManifest.h
    src/core/ResourceHubUpdate.cpp
    src/core/ResourceHubUpdate.h
    src/core/UpdateVerifier.cpp
    src/core/UpdateVerifier.h
    src/core/UpdateService.cpp
    src/core/UpdateService.h
    src/core/UpdateInstallerLauncher.cpp
    src/core/UpdateInstallerLauncher.h
    src/core/ReleaseNotes.cpp
    src/core/ReleaseNotes.h
    src/codeintel/LanguageServerClient.cpp
    src/codeintel/LanguageServerClient.h
    src/data/DpiSession.cpp
    src/data/DpiSession.h
    src/data/DpiEndpointRouting.cpp
    src/data/DpiEndpointRouting.h
    src/data/ConnectionFailure.cpp
    src/data/ConnectionFailure.h
    src/data/SessionTypes.h
    src/data/LobFileTransfer.h
    src/data/TestTypes.cpp
    src/data/TestTypes.h
    src/data/DebuggerTypes.cpp
    src/data/DebuggerTypes.h
    src/data/DbmsDebugCoordinator.cpp
    src/data/DbmsDebugCoordinator.h
    src/data/DmdbgProcessAdapter.cpp
    src/data/DmdbgProcessAdapter.h
    src/data/DmdbgTerminalDecoder.h
    src/data/EditTypes.h
    src/data/XlsxWriter.cpp
    src/data/XlsxWriter.h
    src/data/MetadataService.cpp
    src/data/PowerCompileTask.cpp
    src/data/PowerCompileTask.h
    src/data/PowerCompilePlan.h
    src/data/PowerCompileWorkers.h
    src/data/MetadataService.h
    src/data/MetadataTypes.h
    src/data/ObjectAttributeLoading.h
    src/ui/ObjectAttributeCells.h
    src/data/OperationLogStore.cpp
    src/data/PackageBodyBackupStore.cpp
    src/data/PackageBodyBackupStore.h
    src/data/PackageBodySourceBatch.h
    src/data/ProgramCompileSupport.h
    src/data/MetadataSnapshotStore.cpp
    src/data/MetadataSnapshotStore.h
    src/security/CredentialStore.cpp
    src/security/CredentialStore.h
    src/security/KeychainTaskRunner.h
    src/ui/ConnectionDialog.cpp
    src/ui/ConnectionDialog.h
    src/ui/ConnectionExport.h
    src/ui/ConnectionProfileTree.h
    src/ui/CommandDocumentWidget.cpp
    src/ui/CommandDocumentWidget.h
    src/ui/SelectableTableText.h
    src/ui/JsonDocumentEditor.cpp
    src/ui/JsonDocumentEditor.h
    src/ui/DbaDocumentWidget.cpp
    src/ui/DbaDocumentWidget.h
    src/ui/FindReplaceDialog.cpp
    src/ui/FindReplaceDialog.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/ObjectDetailsWidget.cpp
    src/ui/ObjectDetailsWidget.h
    src/ui/ObjectDdlWidget.cpp
    src/ui/ObjectDdlWidget.h
    src/ui/ObjectListWidget.cpp
    src/ui/ObjectListWidget.h
    src/ui/InvalidObjectsWidget.cpp
    src/ui/InvalidObjectsWidget.h
    src/ui/MissingPackageBodiesWidget.cpp
    src/ui/MissingPackageBodiesWidget.h
    src/ui/ProgramDocumentWidget.cpp
    src/ui/ProgramDocumentWidget.h
    src/ui/ViewDocumentWidget.cpp
    src/ui/ViewDocumentWidget.h
    src/ui/SqlEditor.cpp
    src/ui/SqlEditor.h
    src/ui/SqlDocumentWidget.cpp
    src/ui/SqlDocumentWidget.h
    src/ui/OutputSplitSettings.h
    src/ui/SnippetPanel.cpp
    src/ui/SnippetPanel.h
    src/core/BuiltInSnippets.h
    src/core/FileContentType.h
    src/core/ZipCompression.h
    src/ui/ResultFileCell.h
    src/ui/ResultLobFileDialog.h
    src/ui/OperationLogPanel.cpp
    src/ui/OperationLogPanel.h
    src/ui/QueryResultModel.cpp
    src/ui/QueryResultModel.h
    src/ui/QueryResultPane.cpp
    src/ui/QueryResultPane.h
    src/ui/ResultMetadataRowsModel.h
    src/ui/ResultExportDialog.cpp
    src/ui/ResultExportDialog.h
    src/ui/ResultValueDelegate.cpp
    src/ui/ResultValueDelegate.h
    src/ui/RoleEditorDocumentWidget.cpp
    src/ui/RoleEditorDocumentWidget.h
    src/ui/UserEditorDocumentWidget.cpp
    src/ui/UserEditorDocumentWidget.h
    src/ui/Theme.cpp
    src/ui/Theme.h
    src/ui/SearchField.h
    src/ui/CommandButton.h
    src/ui/LogView.h
    src/ui/StartupSplash.h
    src/ui/TextTransformMenu.h
    src/core/TextTransform.h
    src/ui/RuntimeUiScale.h
    src/ui/TestDocumentWidget.cpp
    src/ui/TestDocumentWidget.h
    src/ui/TestDataDialog.cpp
    src/ui/TestDataDialog.h
    src/ui/TableCopySupport.cpp
    src/ui/TableCopySupport.h
    src/ui/DatabaseVersionPanel.h
    src/ui/UpdateDialog.cpp
    src/ui/UpdateDialog.h
    src/ui/ObjectDropConfirmationDialog.cpp
    src/ui/ObjectDropConfirmationDialog.h
    src/ui/PackageBodyBackupsDialog.cpp
    src/ui/PackageBodyBackupsDialog.h
    src/ui/HelpDocumentDialog.cpp
    src/ui/HelpDocumentDialog.h
    src/ui/VersionNotesDialog.cpp
    src/ui/VersionNotesDialog.h
)


qt_add_resources(GzydDmTools "user_help"
    PREFIX "/help"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/help"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/help/user-guide.md"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/help/images/showcase.json")
    qt_add_resources(GzydDmTools "help_images"
        PREFIX "/help"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/help"
        FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/help/images/startup.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/help/images/home.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/help/images/connections.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/help/images/sql-results.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/help/images/operation-log.png"
    )
endif()

qt_add_resources(GzydDmTools "release_notes"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    FILES "${REID_RELEASE_NOTES_FILE}"
)

qt_add_resources(GzydDmTools "database_environment"
    PREFIX "/database-environment"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/database-environment"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/database-environment/parameters.json"
)



qt_add_resources(GzydDmTools "reid_brand_icon"
    PREFIX "/icons" BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons"
    FILES resources/icons/app.svg
)

target_link_libraries(GzydDmTools PRIVATE
    DmDpi::DmDpi Qt6::Concurrent Qt6::Network Qt6::Sql Qt6::Widgets
    Sodium::Sodium qt6keychain)

# The console client must have the Windows console subsystem, not Qt's GUI one.
add_executable(GzydDmToolsDebugHost
    src/data/DmdbgConsoleMain.cpp
    src/data/DmdbgConsoleHost.cpp
    src/data/DmdbgConsoleHost.h)
set_target_properties(GzydDmToolsDebugHost PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(MINGW)
    target_link_options(GzydDmToolsDebugHost PRIVATE -municode -static)
    target_link_libraries(GzydDmToolsDebugHost PRIVATE winpthread)
endif()
add_dependencies(GzydDmTools GzydDmToolsDebugHost)
add_dependencies(GzydDmTools GzydDmToolsCli)
target_include_directories(GzydDmTools PRIVATE "${REID_GENERATED_INCLUDE_DIR}")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(GzydDmTools PRIVATE DM64)
endif()
get_filename_component(DmDpi_LIBRARY_DIR "${DmDpi_LIBRARY}" DIRECTORY)
get_filename_component(REID_COMPILER_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)

if(WIN32)
    target_link_libraries(GzydDmTools PRIVATE ole32 oleaut32 uuid)
endif()

function(reid_configure_test_runtime)
    if(WIN32)
        set(runtime_modifications
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:qt6keychain>"
            "PATH=path_list_prepend:${DmDpi_LIBRARY_DIR}"
            "PATH=path_list_prepend:${REID_COMPILER_RUNTIME_DIR}"
        )
        set_tests_properties(${ARGN} PROPERTIES ENVIRONMENT_MODIFICATION
            "${runtime_modifications}")
    endif()
endfunction()
set_target_properties(GzydDmTools PROPERTIES OUTPUT_NAME GzydDmTools)
set_target_properties(GzydDmTools PROPERTIES BUILD_RPATH "${DmDpi_LIBRARY_DIR}")
target_compile_definitions(GzydDmTools PRIVATE REID_APP_VERSION=\"${REID_APP_VERSION}\")
set(REID_UPDATE_KEY_ID "release-2026" CACHE STRING "Trusted update signing key id")
set(REID_UPDATE_PUBLIC_KEY_BASE64 "" CACHE STRING
    "Base64 encoded 32-byte Ed25519 update public key")
target_compile_definitions(GzydDmTools PRIVATE
    REID_UPDATE_KEY_ID=\"${REID_UPDATE_KEY_ID}\"
    REID_UPDATE_PUBLIC_KEY_BASE64=\"${REID_UPDATE_PUBLIC_KEY_BASE64}\")
if(WIN32)
    configure_file(cmake/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/GzydDmTools-version.rc" @ONLY)
    target_sources(GzydDmTools PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/GzydDmTools-version.rc")
    target_sources(GzydDmToolsCli PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/GzydDmTools-version.rc")
    target_sources(GzydDmToolsDebugHost PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/GzydDmTools-version.rc")
    set_target_properties(GzydDmTools PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(MSVC)
    target_compile_options(GzydDmTools PRIVATE /W4)
else()
    target_compile_options(GzydDmTools PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    set(reid_version_check_runner powershell.exe -NoProfile -ExecutionPolicy Bypass -File)
    if(REID_CROSS_RUNTIME_ROOT)
        # CTest on the build host can discover the script itself. The cross
        # inventory adapter supplies PowerShell when executing it on Windows.
        set(reid_version_check_runner)
    endif()
    add_test(NAME windows.version-resources COMMAND ${reid_version_check_runner}
        "${CMAKE_CURRENT_SOURCE_DIR}/../../packaging/windows/test-version-resources.ps1"
        -BinaryDirectory "$<TARGET_FILE_DIR:GzydDmTools>"
        -ExpectedVersion "${REID_APP_VERSION}")
    set_tests_properties(windows.version-resources PROPERTIES TIMEOUT 30)
    add_executable(publication-automation-test tests/PublicationAutomationTest.cpp
        src/core/ContextBootstrapTask.cpp src/core/PublicationTask.cpp src/core/CommandScript.cpp
        src/core/SqlStatementParser.cpp src/core/SqlSafetyAnalyzer.cpp)
    target_link_libraries(publication-automation-test PRIVATE Qt6::Core)
    foreach(scenario native-diagnostics module-ownership context-sql context-input)
        add_test(NAME cli.automation.${scenario} COMMAND publication-automation-test ${scenario})
        reid_configure_test_runtime(cli.automation.${scenario})
    endforeach()
    add_executable(publication-task-test tests/PublicationTaskTest.cpp
        src/core/PublicationTask.cpp src/core/PublicationSchedule.cpp src/core/CommandParallelPlan.cpp src/core/CommandScript.cpp
        src/core/SqlStatementParser.cpp src/core/SqlSafetyAnalyzer.cpp)
    target_link_libraries(publication-task-test PRIVATE Qt6::Core)
    foreach(scenario many-files single-file hash scope snapshot variables define-quotes nested-declarations prompt-apostrophe alternative-quotes noninteractive policy shared-compile
            defaults batch-plan schedule-stop session-barrier exclusive-barrier output-context worker-affinity worker-limit)
        add_test(NAME cli.task.${scenario} COMMAND publication-task-test ${scenario})
        reid_configure_test_runtime(cli.task.${scenario})
    endforeach()
    add_test(NAME cli.help COMMAND GzydDmToolsCli --help)
    add_test(NAME cli.version COMMAND GzydDmToolsCli --version)
    reid_configure_test_runtime(cli.help)
    reid_configure_test_runtime(cli.version)
        add_executable(community-edition-test tests/CommunityEditionTest.cpp
            src/core/EmbeddedUpdateCredential.cpp src/core/ResourceHubUpdate.cpp
            src/core/UpdateManifest.cpp)
        target_include_directories(community-edition-test PRIVATE "${REID_GENERATED_INCLUDE_DIR}")
        target_link_libraries(community-edition-test PRIVATE Qt6::Core)
        add_test(NAME community.feature-boundary COMMAND community-edition-test)
        reid_configure_test_runtime(community.feature-boundary)
        add_test(NAME community.window-boundary COMMAND GzydDmTools --self-test-community-edition)
        set_tests_properties(community.window-boundary PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        reid_configure_test_runtime(community.window-boundary)

endif()

if(BUILD_TESTING)
    add_test(NAME stage-one.qt-license-presentation COMMAND GzydDmTools
        --self-test-license-presentation)
    set_tests_properties(stage-one.qt-license-presentation PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    reid_configure_test_runtime(stage-one.qt-license-presentation)
    add_test(NAME stage-one.qt-application-role-switch COMMAND GzydDmTools
        --self-test-application-role-switch)
    set_tests_properties(stage-one.qt-application-role-switch PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    reid_configure_test_runtime(stage-one.qt-application-role-switch)
    foreach(editor_drop_case package package-body view table other guards)
        add_test(NAME stage-two.qt-object-editor-drop-${editor_drop_case} COMMAND GzydDmTools
            --self-test-object-editor-drop=${editor_drop_case})
        set_tests_properties(stage-two.qt-object-editor-drop-${editor_drop_case} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        reid_configure_test_runtime(stage-two.qt-object-editor-drop-${editor_drop_case})
    endforeach()
endif()

# Share embedded completion sounds with the client and its widget tests.
qt_add_resources(REID_COMPLETION_SOUND_RESOURCES resources/completion_sounds.qrc)
add_library(reid-completion-sounds STATIC src/ui/CompletionSounds.cpp
    src/ui/CompletionSounds.h ${REID_COMPLETION_SOUND_RESOURCES})
target_link_libraries(reid-completion-sounds PUBLIC Qt6::Core PRIVATE winmm)
if(BUILD_TESTING)
    add_executable(completion-sounds-test tests/CompletionSoundsTest.cpp)
    target_link_libraries(completion-sounds-test PRIVATE reid-completion-sounds)
    add_test(NAME completion-sounds.core COMMAND completion-sounds-test)
    reid_configure_test_runtime(completion-sounds.core)
    add_executable(save-file-dialog-test tests/SaveFileDialogTest.cpp src/ui/SaveFileDialog.h)
    target_link_libraries(save-file-dialog-test PRIVATE Qt6::Widgets)
    add_test(NAME file-dialog.save-protocol COMMAND save-file-dialog-test)
    reid_configure_test_runtime(file-dialog.save-protocol)
    add_executable(mac-display-scale-test tests/MacDisplayScaleTest.cpp src/ui/MacDisplayScale.h)
    target_link_libraries(mac-display-scale-test PRIVATE Qt6::Core)
    add_test(NAME macos.display-scale-policy COMMAND mac-display-scale-test)
    add_test(NAME macos.display-preferences COMMAND mac-display-scale-test --preferences-only)
    reid_configure_test_runtime(macos.display-scale-policy)
    reid_configure_test_runtime(macos.display-preferences)
endif()
foreach(sound_target GzydDmTools stage-one-core-test stage-two-core-test)
    if(TARGET ${sound_target})
        target_link_libraries(${sound_target} PRIVATE reid-completion-sounds)
    endif()
endforeach()

# Share the stylesheet subcontrol assets with every executable using Theme.
qt_add_resources(REID_THEME_RESOURCES resources/theme-controls.qrc resources/interface-icons.qrc)
foreach(theme_target GzydDmTools stage-one-core-test result-io-core-test
        stage-ten-code-intelligence-test stage-ten-definition-test stage-two-core-test
        code-intelligence-live-probe stage-three-core-test
        lobby-element-panel-test lobby-element-interactions-test)
    if(TARGET ${theme_target})
        target_sources(${theme_target} PRIVATE ${REID_THEME_RESOURCES})
    endif()
endforeach()

# CredentialStore enumerates and removes only this product's Windows credentials.
if(WIN32)
    foreach(credential_target GzydDmTools stage-one-core-test
            database-environment-live-probe command-publish-live-probe
            program-editor-live-probe transaction-state-live-probe code-intelligence-live-probe)
        if(TARGET ${credential_target})
            target_link_libraries(${credential_target} PRIVATE advapi32)
        endif()
    endforeach()
endif()

option(REID_BUILD_DPI_SMOKE "Build the stage-zero DM DPI smoke probe" ON)
# File content probes and Lobby archives share bounded raw-Deflate support.
get_property(reid_local_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(reid_target IN LISTS reid_local_targets)
    get_target_property(reid_target_type ${reid_target} TYPE)
    if(reid_target_type STREQUAL "EXECUTABLE"
            AND reid_target MATCHES "^(GzydDmTools$|stage-(one|two|three)-core-test$|lobby-)")
        target_link_libraries(${reid_target} PRIVATE ZLIB::ZLIB)
    endif()
endforeach()
if(REID_BUILD_DPI_SMOKE)
    add_subdirectory(prototypes/dpi-smoke)
endif()

option(REID_BUILD_STAGE_ZERO_PERFORMANCE "Build stage-zero performance probes" ON)
if(REID_BUILD_STAGE_ZERO_PERFORMANCE)
    add_subdirectory(prototypes/performance-smoke)
endif()

option(REID_BUILD_DMDBG_SMOKE "Build the stage-zero dmdbg process adapter probe" ON)
if(REID_BUILD_DMDBG_SMOKE)
    add_subdirectory(prototypes/dmdbg-smoke)
endif()
