if(Sodium_ROOT)
    find_path(Sodium_INCLUDE_DIR
        NAMES sodium.h
        PATHS "${Sodium_ROOT}/include"
        NO_DEFAULT_PATH
    )
    if(WIN32 AND EXISTS "${Sodium_ROOT}/lib/libsodium.a")
        set(Sodium_LIBRARY "${Sodium_ROOT}/lib/libsodium.a")
    else()
        find_library(Sodium_LIBRARY
            NAMES sodium libsodium
            PATHS "${Sodium_ROOT}/lib"
            NO_DEFAULT_PATH
        )
    endif()
else()
    find_path(Sodium_INCLUDE_DIR NAMES sodium.h)
    find_library(Sodium_LIBRARY
        NAMES sodium libsodium
    )
endif()

if(Sodium_INCLUDE_DIR
    AND EXISTS "${Sodium_INCLUDE_DIR}/sodium/version.h")
    file(STRINGS "${Sodium_INCLUDE_DIR}/sodium/version.h"
        _sodium_version_line
        REGEX "^#define SODIUM_VERSION_STRING \"[^\"]+\"")
    string(REGEX REPLACE
        "^#define SODIUM_VERSION_STRING \"([^\"]+)\".*" "\\1"
        Sodium_VERSION "${_sodium_version_line}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Sodium
    REQUIRED_VARS Sodium_INCLUDE_DIR Sodium_LIBRARY
    VERSION_VAR Sodium_VERSION
)

if(Sodium_FOUND AND NOT TARGET Sodium::Sodium)
    add_library(Sodium::Sodium UNKNOWN IMPORTED)
    set_target_properties(Sodium::Sodium PROPERTIES
        IMPORTED_LOCATION "${Sodium_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Sodium_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS SODIUM_STATIC
    )
endif()

mark_as_advanced(Sodium_INCLUDE_DIR Sodium_LIBRARY)
