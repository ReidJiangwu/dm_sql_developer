set(DM_CLIENT_DIR "" CACHE PATH "Path to the local DM8 client installation")

find_path(DmDpi_INCLUDE_DIR
    NAMES DPI.h
    HINTS "${DM_CLIENT_DIR}/drivers/dpi/include"
)

find_library(DmDpi_LIBRARY
    NAMES dmdpi
    HINTS
        "${DM_CLIENT_DIR}/drivers/dpi"
        "${DM_CLIENT_DIR}/bin"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DmDpi
    REQUIRED_VARS DmDpi_INCLUDE_DIR DmDpi_LIBRARY
)

if(DmDpi_FOUND AND NOT TARGET DmDpi::DmDpi)
    add_library(DmDpi::DmDpi INTERFACE IMPORTED)
    set_target_properties(DmDpi::DmDpi PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${DmDpi_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${DmDpi_LIBRARY}"
    )
endif()

mark_as_advanced(DmDpi_INCLUDE_DIR DmDpi_LIBRARY)
