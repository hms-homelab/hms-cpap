cmake_minimum_required(VERSION 3.16)
project(hms_cpap CXX)

# C++20 on MSVC (needed for designated initializers in hms-shared/db),
# C++17 on Linux (system pqxx is built with C++17 — ABI mismatch if we use C++20)
if(MSVC)
    set(CMAKE_CXX_STANDARD 20)
else()
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform-aware compiler flags
if(MSVC)
    add_compile_options(/W4 /O2 /utf-8)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DNOMINMAX)
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")
endif()

# Find required packages
option(BUILD_WITH_WEB "Build with Drogon web UI server" ON)
option(BUILD_WITH_POSTGRESQL "Enable PostgreSQL support" ON)
option(BUILD_WITH_MYSQL "Enable MySQL/MariaDB support" OFF)

if(BUILD_WITH_WEB)
    find_package(Drogon CONFIG REQUIRED)
    add_definitions(-DBUILD_WITH_WEB)
endif()

# SQLite3 — always available (embedded default DB)
if(MSVC)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
else()
    find_library(SQLITE3_LIB sqlite3 REQUIRED)
endif()

# PostgreSQL — optional (power users)
if(BUILD_WITH_POSTGRESQL)
    add_definitions(-DWITH_POSTGRESQL)
endif()

# MySQL — optional
if(BUILD_WITH_MYSQL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MYSQLCLIENT mysqlclient)
    if(MYSQLCLIENT_FOUND)
        add_definitions(-DWITH_MYSQL)
    else()
        message(WARNING "MySQL client not found — MySQL support disabled")
        set(BUILD_WITH_MYSQL OFF)
    endif()
endif()

if(MSVC)
    find_package(jsoncpp CONFIG REQUIRED)
else()
    find_package(jsoncpp REQUIRED)
endif()
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

# nlohmann_json: header-only, use host headers when cross-compiling (Linux)
if(CMAKE_CROSSCOMPILING AND NOT WIN32)
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
    set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "/usr/include"
    )
else()
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

# Paho MQTT
if(MSVC)
    find_package(eclipse-paho-mqtt-c CONFIG REQUIRED)
    find_package(PahoMqttCpp CONFIG REQUIRED)
else()
    find_library(PAHO_MQTTPP3_LIB paho-mqttpp3)
    find_library(PAHO_MQTT3AS_LIB paho-mqtt3as)
endif()

if(BUILD_WITH_POSTGRESQL)
    if(MSVC)
        find_package(libpqxx CONFIG REQUIRED)
        find_package(PostgreSQL REQUIRED)
    else()
        find_library(PQXX_LIB pqxx)
        find_library(PQ_LIB pq)
        # libpq-fe.h lives in /usr/include/postgresql on Debian
        find_path(PQ_INCLUDE_DIR libpq-fe.h PATHS /usr/include/postgresql)
        if(PQ_INCLUDE_DIR)
            include_directories(${PQ_INCLUDE_DIR})
        endif()
    endif()
endif()

# ── External libraries (via FetchContent) ────────────────────────────────────
include(FetchContent)

# sleeplink-parser: shared CPAP parsing library (ResMed EDF + Philips DS2)
set(SLEEPLINK_PARSER_WITH_PHILIPS ON CACHE BOOL "Enable Philips DreamStation parser" FORCE)
set(SLEEPLINK_PARSER_LOCAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../hms-sleeplink/hms-sleeplink-parser)
if(EXISTS ${SLEEPLINK_PARSER_LOCAL_DIR}/CMakeLists.txt)
    FetchContent_Declare(sleeplink_parser
        SOURCE_DIR ${SLEEPLINK_PARSER_LOCAL_DIR}
    )
else()
    FetchContent_Declare(sleeplink_parser
        GIT_REPOSITORY https://github.com/hms-homelab/hms-cpapdash-parser.git
        GIT_TAG        master
    )
endif()
FetchContent_MakeAvailable(sleeplink_parser)

# hms-shared modules
FetchContent_Declare(hms_shared
    GIT_REPOSITORY https://github.com/hms-homelab/hms-shared.git
    GIT_TAG        v1.6.6
)
FetchContent_Populate(hms_shared)

# LLM module (nlohmann_json + curl only)
add_library(hms_llm STATIC
    ${hms_shared_SOURCE_DIR}/llm/src/llm_client.cpp
)
target_include_directories(hms_llm PUBLIC
    ${hms_shared_SOURCE_DIR}/llm/include
)
target_link_libraries(hms_llm PUBLIC
    nlohmann_json::nlohmann_json
    CURL::libcurl
)

# MQTT module (spdlog + paho)
find_package(spdlog REQUIRED)
add_library(hms_mqtt STATIC
    ${hms_shared_SOURCE_DIR}/mqtt/src/mqtt_client.cpp
)
target_include_directories(hms_mqtt PUBLIC
    ${hms_shared_SOURCE_DIR}/mqtt/include
)
if(MSVC)
    target_compile_definitions(hms_mqtt PUBLIC PAHO_MQTTPP_IMPORTS)
    target_link_libraries(hms_mqtt PUBLIC
        spdlog::spdlog
        PahoMqttCpp::paho-mqttpp3
        eclipse-paho-mqtt-c::paho-mqtt3as
    )
else()
    target_link_libraries(hms_mqtt PUBLIC
        spdlog::spdlog
        ${PAHO_MQTTPP3_LIB}
        ${PAHO_MQTT3AS_LIB}
    )
endif()

# DB module (DbPool connection pool) — only with PostgreSQL
if(BUILD_WITH_POSTGRESQL)
    add_library(hms_db STATIC
        ${hms_shared_SOURCE_DIR}/db/src/db_pool.cpp
    )
    target_include_directories(hms_db PUBLIC
        ${hms_shared_SOURCE_DIR}/db/include
    )
    if(MSVC)
        target_link_libraries(hms_db PUBLIC libpqxx::pqxx PostgreSQL::PostgreSQL)
    else()
        target_link_libraries(hms_db PUBLIC ${PQXX_LIB} ${PQ_LIB})
    endif()
endif()

# Source files — exclude platform-specific code and duplicates now in sleeplink-parser
file(GLOB_RECURSE SOURCES "src/*.cpp")
# WiFiSwitchClient is Linux-only (wpa_supplicant, dhcpcd); exclude on all platforms (dead code)
list(FILTER SOURCES EXCLUDE REGEX ".*/WiFiSwitchClient\\.cpp$")
# EDFParser + CPAPModels are now provided by sleeplink-parser library
list(FILTER SOURCES EXCLUDE REGEX ".*/parsers/EDFParser\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*/models/CPAPModels\\.cpp$")

# Executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    include
)

# Link libraries
if(MSVC)
    target_link_libraries(${PROJECT_NAME}
        sleeplink_parser
        JsonCpp::JsonCpp
        PahoMqttCpp::paho-mqttpp3
        eclipse-paho-mqtt-c::paho-mqtt3as
        CURL::libcurl
        unofficial::sqlite3::sqlite3
        OpenSSL::SSL
        OpenSSL::Crypto
        hms_llm
        hms_mqtt
    )
else()
    target_link_libraries(${PROJECT_NAME}
        sleeplink_parser
        jsoncpp_lib
        ${PAHO_MQTTPP3_LIB}
        ${PAHO_MQTT3AS_LIB}
        CURL::libcurl
        ${SQLITE3_LIB}
        OpenSSL::SSL
        OpenSSL::Crypto
        hms_llm
        hms_mqtt
        spdlog::spdlog
        stdc++fs
        pthread
    )
endif()

# PostgreSQL libraries (optional)
if(BUILD_WITH_POSTGRESQL)
    if(MSVC)
        target_link_libraries(${PROJECT_NAME} libpqxx::pqxx PostgreSQL::PostgreSQL hms_db)
    else()
        target_link_libraries(${PROJECT_NAME} ${PQXX_LIB} ${PQ_LIB} hms_db)
    endif()
endif()

# MySQL libraries (optional)
if(BUILD_WITH_MYSQL)
    target_link_libraries(${PROJECT_NAME} ${MYSQLCLIENT_LIBRARIES})
    target_include_directories(${PROJECT_NAME} PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
endif()

if(BUILD_WITH_WEB)
    target_link_libraries(${PROJECT_NAME} Drogon::Drogon)
endif()

# Install target
if(WIN32)
    install(TARGETS ${PROJECT_NAME} DESTINATION bin)
else()
    install(TARGETS ${PROJECT_NAME} DESTINATION /usr/local/bin)
endif()

# Testing support
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Print configuration
message(STATUS "HMS-CPAP Build Configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  EDF: raw parser (ResMed compatible)")
if(MSVC)
    message(STATUS "  Paho MQTT: vcpkg (static)")
    message(STATUS "  PostgreSQL: vcpkg")
else()
    message(STATUS "  Paho MQTT: ${PAHO_MQTTPP3_LIB}")
    message(STATUS "  PostgreSQL: ${PQXX_LIB}")
endif()
message(STATUS "  LLM: hms_llm (from hms-shared)")
message(STATUS "  DB Pool: hms_db (from hms-shared)")
message(STATUS "  Build Tests: ${BUILD_TESTS}")
