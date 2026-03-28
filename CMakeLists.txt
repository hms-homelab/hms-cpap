cmake_minimum_required(VERSION 3.16)
project(hms_cpap CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")

# Find required packages
option(BUILD_WITH_WEB "Build with Drogon web UI server" ON)
option(BUILD_WITH_POSTGRESQL "Enable PostgreSQL support" ON)
option(BUILD_WITH_MYSQL "Enable MySQL/MariaDB support" OFF)

if(BUILD_WITH_WEB)
    find_package(Drogon CONFIG REQUIRED)
    add_definitions(-DBUILD_WITH_WEB)
endif()

# SQLite3 — always available (embedded default DB)
find_library(SQLITE3_LIB sqlite3 REQUIRED)

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

find_package(jsoncpp REQUIRED)
find_package(CURL REQUIRED)

# nlohmann_json: header-only, use host headers when cross-compiling
# (sysroot cmake config has broken INTERFACE_INCLUDE_DIRECTORIES)
if(CMAKE_CROSSCOMPILING)
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
    set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "/usr/include"
    )
else()
    find_package(nlohmann_json CONFIG REQUIRED)
endif()
find_library(PAHO_MQTTPP3_LIB paho-mqttpp3)
find_library(PAHO_MQTT3AS_LIB paho-mqtt3as)
if(BUILD_WITH_POSTGRESQL)
    find_library(PQXX_LIB pqxx)
    find_library(PQ_LIB pq)
endif()

# ── hms-shared modules (via FetchContent) ───────────────────────────────────
include(FetchContent)
FetchContent_Declare(hms_shared
    GIT_REPOSITORY https://github.com/hms-homelab/hms-shared.git
    GIT_TAG        v1.6.5
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
target_link_libraries(hms_mqtt PUBLIC
    spdlog::spdlog
    ${PAHO_MQTTPP3_LIB}
    ${PAHO_MQTT3AS_LIB}
)

# DB module (DbPool connection pool) — only with PostgreSQL
if(BUILD_WITH_POSTGRESQL)
    add_library(hms_db STATIC
        ${hms_shared_SOURCE_DIR}/db/src/db_pool.cpp
    )
    target_include_directories(hms_db PUBLIC
        ${hms_shared_SOURCE_DIR}/db/include
    )
    target_link_libraries(hms_db PUBLIC
        ${PQXX_LIB}
        ${PQ_LIB}
    )
endif()

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    include
)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    jsoncpp_lib
    ${PAHO_MQTTPP3_LIB}
    ${PAHO_MQTT3AS_LIB}
    CURL::libcurl
    ${SQLITE3_LIB}
    hms_llm
    hms_mqtt
    pthread
    ssl
    crypto
)

# PostgreSQL libraries (optional)
if(BUILD_WITH_POSTGRESQL)
    target_link_libraries(${PROJECT_NAME} ${PQXX_LIB} ${PQ_LIB} hms_db)
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
install(TARGETS ${PROJECT_NAME} DESTINATION /usr/local/bin)

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
message(STATUS "  EDF: raw parser (ResMed compatible)")
message(STATUS "  Paho MQTT: ${PAHO_MQTTPP3_LIB}")
message(STATUS "  PostgreSQL: ${PQXX_LIB}")
message(STATUS "  LLM: hms_llm (from hms-shared)")
message(STATUS "  DB Pool: hms_db (from hms-shared)")
message(STATUS "  Build Tests: ${BUILD_TESTS}")
