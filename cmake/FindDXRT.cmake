# FindDXRT.cmake — locate the DEEPX runtime installed by the dx-runtime installer.
#
# DXRT lands in /usr/local by default (headers in /usr/local/include/dxrt,
# libdxrt.so in /usr/local/lib). Inside the Docker image the same layout is
# reproduced, so no per-environment branching is needed here.
#
# Defines: DXRT_FOUND, DXRT_INCLUDE_DIR, DXRT_LIBRARY and the imported
# target DXRT::dxrt.

find_path(DXRT_INCLUDE_DIR
    NAMES dxrt/dxrt_cxx_api.h
    HINTS ${DXRT_ROOT} /usr/local/include /usr/include)

find_library(DXRT_LIBRARY
    NAMES dxrt
    HINTS ${DXRT_ROOT} /usr/local/lib /usr/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DXRT
    REQUIRED_VARS DXRT_LIBRARY DXRT_INCLUDE_DIR)

if(DXRT_FOUND AND NOT TARGET DXRT::dxrt)
    add_library(DXRT::dxrt UNKNOWN IMPORTED)
    set_target_properties(DXRT::dxrt PROPERTIES
        IMPORTED_LOCATION "${DXRT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DXRT_INCLUDE_DIR}")
endif()

mark_as_advanced(DXRT_INCLUDE_DIR DXRT_LIBRARY)
