# SDD-013 D-701-1: the narrator model, inside the package.
#
# The pin (cmake/narrator-pin.cmake) is compiled into ModelFile.cpp, for every
# target, so the hash a running hms-cpap checks a model against is the one
# this build bundled.
#
# -DBUNDLE_NARRATOR_MODEL=ON puts the pinned model at
#   <build>/models/<version>/model.gguf
# at configure time: downloaded from the pinned URL, or taken from
# -DNARRATOR_MODEL_FILE=<path> (an offline build), and in both cases checked
# against the pinned SHA-256; a mismatch fails the configure. The packaging
# steps copy that models/ folder to where the runtime looks for it
# (model/ModelFile.h, bundledModelDirs). The file is never committed: it is a
# release asset, fetched by its hash.
#
# Off by default, so a developer's configure stays offline and a 45 MB file is
# not fetched for a test build. A build without it still downloads the model
# from Settings, as 7.0.0 did.

include(${CMAKE_CURRENT_LIST_DIR}/narrator-pin.cmake)

add_compile_definitions(
    "HMS_NARRATOR_VERSION=\"${HMS_NARRATOR_VERSION}\""
    "HMS_NARRATOR_URL=\"${HMS_NARRATOR_URL}\""
    "HMS_NARRATOR_SHA256=\"${HMS_NARRATOR_SHA256}\"")

option(BUNDLE_NARRATOR_MODEL "Put the pinned narrator model in <build>/models for the package (SDD-013)" OFF)
set(NARRATOR_MODEL_FILE "" CACHE FILEPATH
    "A local copy of the pinned narrator model to bundle instead of downloading it")

if(BUNDLE_NARRATOR_MODEL)
    set(_hms_narrator_dest "${CMAKE_BINARY_DIR}/models/${HMS_NARRATOR_VERSION}/model.gguf")
    set(_hms_narrator_have FALSE)
    if(EXISTS "${_hms_narrator_dest}")
        file(SHA256 "${_hms_narrator_dest}" _hms_narrator_got)
        if(_hms_narrator_got STREQUAL HMS_NARRATOR_SHA256)
            set(_hms_narrator_have TRUE)
        else()
            file(REMOVE "${_hms_narrator_dest}")
        endif()
    endif()
    if(NOT _hms_narrator_have)
        if(NARRATOR_MODEL_FILE)
            file(SHA256 "${NARRATOR_MODEL_FILE}" _hms_narrator_got)
            if(NOT _hms_narrator_got STREQUAL HMS_NARRATOR_SHA256)
                message(FATAL_ERROR "NARRATOR_MODEL_FILE ${NARRATOR_MODEL_FILE} is not ${HMS_NARRATOR_VERSION}: "
                                    "its SHA-256 is ${_hms_narrator_got}, the pin's is ${HMS_NARRATOR_SHA256}")
            endif()
            file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/models/${HMS_NARRATOR_VERSION}")
            file(COPY_FILE "${NARRATOR_MODEL_FILE}" "${_hms_narrator_dest}")
        else()
            message(STATUS "Narrator model: downloading ${HMS_NARRATOR_VERSION}")
            file(DOWNLOAD "${HMS_NARRATOR_URL}" "${_hms_narrator_dest}"
                 EXPECTED_HASH SHA256=${HMS_NARRATOR_SHA256}
                 TLS_VERIFY ON
                 STATUS _hms_narrator_status)
            list(GET _hms_narrator_status 0 _hms_narrator_code)
            if(NOT _hms_narrator_code EQUAL 0)
                file(REMOVE "${_hms_narrator_dest}")
                message(FATAL_ERROR "Narrator model: the download failed: ${_hms_narrator_status}")
            endif()
        endif()
    endif()
    message(STATUS "Narrator model: ${HMS_NARRATOR_VERSION} bundled at ${_hms_narrator_dest}")
endif()
