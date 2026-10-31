# The Sleep Bridge component: a prebuilt static library and one public header,
# <sleepbridge/rebuild.h>, that rebuild card files from a Sleep Bridge's
# sector writes. It is not part of this repository's MIT source: it ships as a
# binary under its own licence (LICENSE-sleepbridge), as versioned assets on
# this repository's GitHub releases.
#
#   -DBUILD_WITH_SLEEPBRIDGE=ON    fetch the asset for this platform, check its
#                                  SHA-256 against the pin below, link it
#   -DSLEEPBRIDGE_ASSET_FILE=...   use a local copy of the asset instead of the
#                                  download; the SHA-256 pin still applies
#
# OFF (the default for a from-source build), hms-cpap builds and runs without
# it, and the Sleep Bridge port refuses bridges with "not in this build".
#
# A pin is a version and one SHA-256 per platform. CMake refuses an asset whose
# bytes differ from the pin, and refuses a platform with no pin, so a build
# never links anything it was not told the exact bytes of.

option(BUILD_WITH_SLEEPBRIDGE "Link the prebuilt Sleep Bridge component (LICENSE-sleepbridge)" OFF)

include(${CMAKE_CURRENT_LIST_DIR}/sleepbridge-pins.cmake)

set(SLEEPBRIDGE_ASSET_FILE "" CACHE FILEPATH "A local copy of the Sleep Bridge asset (optional)")

function(hms_sleepbridge_platform out)
    if(WIN32)
        set(_os "windows")
    elseif(APPLE)
        set(_os "macos")
    else()
        set(_os "linux")
    endif()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _cpu)
    if(_cpu MATCHES "^(x86_64|amd64)$")
        set(_cpu "amd64")
    elseif(_cpu MATCHES "^(aarch64|arm64)$")
        set(_cpu "arm64")
    endif()
    set(${out} "${_os}-${_cpu}" PARENT_SCOPE)
endfunction()

if(BUILD_WITH_SLEEPBRIDGE)
    hms_sleepbridge_platform(_sb_platform)
    string(REPLACE "-" "_" _sb_key "${_sb_platform}")
    set(_sb_sha "${SLEEPBRIDGE_SHA256_${_sb_key}}")

    if(NOT SLEEPBRIDGE_VERSION OR NOT _sb_sha)
        message(FATAL_ERROR
            "BUILD_WITH_SLEEPBRIDGE=ON but cmake/sleepbridge-pins.cmake pins no Sleep Bridge "
            "asset for ${_sb_platform} (version '${SLEEPBRIDGE_VERSION}'). Build with "
            "-DBUILD_WITH_SLEEPBRIDGE=OFF, or pin the asset's version and SHA-256.")
    endif()

    set(_sb_name "hms-cpap-sleepbridge-${SLEEPBRIDGE_VERSION}-${_sb_platform}.tar.gz")
    set(_sb_dir  "${CMAKE_BINARY_DIR}/_deps/sleepbridge-${SLEEPBRIDGE_VERSION}-${_sb_platform}")
    set(_sb_tar  "${_sb_dir}/${_sb_name}")

    if(SLEEPBRIDGE_ASSET_FILE)
        file(SHA256 "${SLEEPBRIDGE_ASSET_FILE}" _sb_got)
        if(NOT _sb_got STREQUAL _sb_sha)
            message(FATAL_ERROR
                "Sleep Bridge asset ${SLEEPBRIDGE_ASSET_FILE} has SHA-256 ${_sb_got}; "
                "the pin for ${_sb_platform} ${SLEEPBRIDGE_VERSION} is ${_sb_sha}. Refused.")
        endif()
        file(MAKE_DIRECTORY "${_sb_dir}")
        configure_file("${SLEEPBRIDGE_ASSET_FILE}" "${_sb_tar}" COPYONLY)
    elseif(NOT EXISTS "${_sb_tar}")
        set(_sb_url "${SLEEPBRIDGE_BASE_URL}/sleepbridge-v${SLEEPBRIDGE_VERSION}/${_sb_name}")
        message(STATUS "Sleep Bridge: fetching ${_sb_url}")
        # EXPECTED_HASH makes the download fail on any other bytes.
        file(DOWNLOAD "${_sb_url}" "${_sb_tar}"
             EXPECTED_HASH SHA256=${_sb_sha}
             TLS_VERIFY ON
             STATUS _sb_status)
        list(GET _sb_status 0 _sb_code)
        if(NOT _sb_code EQUAL 0)
            file(REMOVE "${_sb_tar}")
            list(GET _sb_status 1 _sb_msg)
            message(FATAL_ERROR "Sleep Bridge: download of ${_sb_url} failed: ${_sb_msg}")
        endif()
    endif()

    # Checked again on every configure: a cached archive is no exception.
    file(SHA256 "${_sb_tar}" _sb_got)
    if(NOT _sb_got STREQUAL _sb_sha)
        file(REMOVE "${_sb_tar}")
        message(FATAL_ERROR "Sleep Bridge: ${_sb_tar} does not match its SHA-256 pin. Refused.")
    endif()

    set(_sb_root "${_sb_dir}/pkg")
    if(NOT EXISTS "${_sb_root}/include/sleepbridge/rebuild.h")
        file(REMOVE_RECURSE "${_sb_root}")
        file(MAKE_DIRECTORY "${_sb_root}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${_sb_tar}"
                        WORKING_DIRECTORY "${_sb_root}"
                        RESULT_VARIABLE _sb_rc)
        if(NOT _sb_rc EQUAL 0)
            message(FATAL_ERROR "Sleep Bridge: could not unpack ${_sb_tar}")
        endif()
    endif()

    if(WIN32)
        set(_sb_lib "${_sb_root}/lib/sleepbridge.lib")
    else()
        set(_sb_lib "${_sb_root}/lib/libsleepbridge.a")
    endif()
    foreach(_f "${_sb_lib}" "${_sb_root}/include/sleepbridge/rebuild.h"
               "${_sb_root}/LICENSE-sleepbridge")
        if(NOT EXISTS "${_f}")
            message(FATAL_ERROR "Sleep Bridge: the asset has no ${_f}")
        endif()
    endforeach()

    add_library(sleepbridge_component STATIC IMPORTED GLOBAL)
    set_target_properties(sleepbridge_component PROPERTIES
        IMPORTED_LOCATION "${_sb_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_sb_root}/include"
        INTERFACE_COMPILE_DEFINITIONS "HMS_WITH_SLEEPBRIDGE=1")
    set(SLEEPBRIDGE_LICENSE_FILE "${_sb_root}/LICENSE-sleepbridge")
    message(STATUS "Sleep Bridge: ${SLEEPBRIDGE_VERSION} ${_sb_platform} (SHA-256 ${_sb_sha})")
else()
    message(STATUS "Sleep Bridge: not in this build (BUILD_WITH_SLEEPBRIDGE=OFF)")
endif()

# The adapter to <sleepbridge/rebuild.h> compiles only with the component.
function(hms_sleepbridge_filter_sources var)
    if(NOT BUILD_WITH_SLEEPBRIDGE)
        set(_s ${${var}})
        list(FILTER _s EXCLUDE REGEX ".*/sleepbridge/library/.*")
        set(${var} ${_s} PARENT_SCOPE)
    endif()
endfunction()
