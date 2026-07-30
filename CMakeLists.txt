cmake_minimum_required(VERSION 3.16)
project(hms_cpap CXX)

# Read version from VERSION file
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" HMS_CPAP_VERSION LIMIT_COUNT 1)
string(STRIP "${HMS_CPAP_VERSION}" HMS_CPAP_VERSION)
add_compile_definitions(HMS_CPAP_VERSION="${HMS_CPAP_VERSION}")

# C++20 on MSVC (needed for designated initializers in hms-shared/db),
# C++17 on Linux (system pqxx is built with C++17 — ABI mismatch if we use C++20).
# The default stays 17, but an explicit -DCMAKE_CXX_STANDARD=20 is honored so a
# local build can match a C++20-built libpqxx (e.g. Homebrew on macOS).
if(MSVC)
    set(CMAKE_CXX_STANDARD 20)
elseif(NOT DEFINED CMAKE_CXX_STANDARD)
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
    set(_hms_mysql_found FALSE)

    # Route 1: pkg-config (Linux, macOS). Module name varies by vendor: Oracle
    # ships mysqlclient, Debian's libmariadb-dev ships mariadb, Homebrew's
    # mariadb-connector-c ships libmariadb.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MYSQLCLIENT mysqlclient)
        if(NOT MYSQLCLIENT_FOUND)
            pkg_check_modules(MYSQLCLIENT mariadb)
        endif()
        if(NOT MYSQLCLIENT_FOUND)
            pkg_check_modules(MYSQLCLIENT libmariadb)
        endif()
        if(MYSQLCLIENT_FOUND)
            set(_hms_mysql_found TRUE)
        endif()
    endif()

    # Route 2: direct search, for MSVC/vcpkg where pkg-config is normally absent
    # and for any install that ships no .pc file.
    #
    # Searches into HMS_MYSQL_* rather than straight into MYSQLCLIENT_*: a failed
    # pkg_check_modules still DEFINES MYSQLCLIENT_INCLUDE_DIRS as an empty cache
    # entry, and find_path treats an already-set variable as done and skips the
    # search entirely. The results are copied across once found so the link step
    # below sees one pair of variables whichever route won.
    if(NOT _hms_mysql_found)
        find_path(HMS_MYSQL_INCLUDE_DIR mysql.h PATH_SUFFIXES mysql mariadb)
        find_library(HMS_MYSQL_LIBRARY
                     NAMES libmysql mysqlclient libmariadb mariadbclient)
        if(HMS_MYSQL_INCLUDE_DIR AND HMS_MYSQL_LIBRARY)
            set(MYSQLCLIENT_INCLUDE_DIRS   "${HMS_MYSQL_INCLUDE_DIR}")
            set(MYSQLCLIENT_LINK_LIBRARIES "${HMS_MYSQL_LIBRARY}")
            set(_hms_mysql_found TRUE)
            message(STATUS "Found MySQL client: ${HMS_MYSQL_LIBRARY}")
        endif()
    endif()

    if(_hms_mysql_found)
        add_definitions(-DWITH_MYSQL)
    else()
        # Downgrading to OFF here used to leave BUILD_WITH_MYSQL=ON in the cache
        # while WITH_MYSQL went undefined, producing a binary that accepts
        # database.type="mysql" in config.json and then refuses to start. If the
        # backend was asked for explicitly, fail now instead.
        message(FATAL_ERROR
            "BUILD_WITH_MYSQL=ON but no MySQL/MariaDB client was found. Install one, "
            "or point the build at it. macOS: brew install mysql-client and "
            "PKG_CONFIG_PATH=/opt/homebrew/opt/mysql-client/lib/pkgconfig. "
            "Debian: apt install libmariadb-dev. MSVC: vcpkg install libmysql.")
    endif()
endif()

if(MSVC)
    find_package(jsoncpp CONFIG REQUIRED)
elseif(APPLE)
    # CONFIG mode so Homebrew's jsoncppConfig.cmake defines the jsoncpp_lib
    # IMPORTED target we link below. In plain module mode, Drogon's bundled
    # Findjsoncpp.cmake (on the module path after find_package(Drogon)) answers
    # first and, depending on the Drogon version, does not create jsoncpp_lib --
    # so CMake emits a bare -ljsoncpp_lib and the link fails on CI.
    find_package(jsoncpp CONFIG REQUIRED)
else()
    find_package(jsoncpp REQUIRED)
endif()
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

# libharu for PDF generation (Linux/macOS only — not available via vcpkg for Windows)
if(NOT MSVC)
    find_library(HPDF_LIB hpdf)
    find_path(HPDF_INCLUDE_DIR hpdf.h)
    if(NOT HPDF_LIB OR NOT HPDF_INCLUDE_DIR)
        message(FATAL_ERROR "libhpdf not found — run: sudo apt install libhpdf-dev")
    endif()
    include_directories(${HPDF_INCLUDE_DIR})
endif()

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
    # Static triplets (x64-windows-static-md) use -static suffixed targets
    if(VCPKG_TARGET_TRIPLET MATCHES "static")
        set(PAHO_C_TARGET   eclipse-paho-mqtt-c::paho-mqtt3as-static)
        set(PAHO_CPP_TARGET PahoMqttCpp::paho-mqttpp3-static)
    else()
        set(PAHO_C_TARGET   eclipse-paho-mqtt-c::paho-mqtt3as)
        set(PAHO_CPP_TARGET PahoMqttCpp::paho-mqttpp3)
    endif()
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

# cpapdash-parser: shared CPAP parsing library (ResMed EDF + Lowenstein Prisma)
set(CPAPDASH_PARSER_WITH_LOWENSTEIN ON CACHE BOOL "Enable Lowenstein Prisma parser" FORCE)
set(CPAPDASH_PARSER_LOCAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../hms-cpapdash-parser)
set(CPAPDASH_PARSER_LOCAL_DIR2 ${CMAKE_CURRENT_SOURCE_DIR}/../hms-cpapdash/hms-cpapdash-parser)
if(EXISTS ${CPAPDASH_PARSER_LOCAL_DIR}/CMakeLists.txt)
    FetchContent_Declare(cpapdash_parser
        SOURCE_DIR ${CPAPDASH_PARSER_LOCAL_DIR}
    )
elseif(EXISTS ${CPAPDASH_PARSER_LOCAL_DIR2}/CMakeLists.txt)
    FetchContent_Declare(cpapdash_parser
        SOURCE_DIR ${CPAPDASH_PARSER_LOCAL_DIR2}
    )
else()
    FetchContent_Declare(cpapdash_parser
        GIT_REPOSITORY https://github.com/hms-homelab/hms-cpapdash-parser.git
        GIT_TAG        v2026.1.3
    )
endif()
FetchContent_MakeAvailable(cpapdash_parser)

# miniz: ZIP library (for Lowenstein Prisma Line therapy.pdat extraction)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.0.2
)
FetchContent_MakeAvailable(miniz)

# hms-shared modules
FetchContent_Declare(hms_shared
    GIT_REPOSITORY https://github.com/hms-homelab/hms-shared.git
    GIT_TAG        v1.6.7
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
    # PAHO_MQTTPP_IMPORTS is only for dynamic (DLL) builds — skip for static triplets
    if(NOT VCPKG_TARGET_TRIPLET MATCHES "static")
        target_compile_definitions(hms_mqtt PUBLIC PAHO_MQTTPP_IMPORTS)
    endif()
    target_link_libraries(hms_mqtt PUBLIC
        spdlog::spdlog
        ${PAHO_CPP_TARGET}
        ${PAHO_C_TARGET}
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

# Source files — exclude platform-specific code and duplicates now in cpapdash-parser
file(GLOB_RECURSE SOURCES "src/*.cpp")
# WiFiSwitchClient is Linux-only (wpa_supplicant, dhcpcd); exclude on all platforms (dead code)
list(FILTER SOURCES EXCLUDE REGEX ".*/WiFiSwitchClient\\.cpp$")
# EDFParser + CPAPModels are now provided by cpapdash-parser library
list(FILTER SOURCES EXCLUDE REGEX ".*/parsers/EDFParser\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*/models/CPAPModels\\.cpp$")
# CLI binaries have their own main() — exclude from main executable
list(FILTER SOURCES EXCLUDE REGEX ".*/cli/.*\\.cpp$")
if(MSVC)
    list(FILTER SOURCES EXCLUDE REGEX ".*/FysetcTcpServer\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*/FysetcDataSource\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*/FysetcSectorCollectorService\\.cpp$")
    # PDF/report/gnuplot not available on Windows
    list(FILTER SOURCES EXCLUDE REGEX ".*/PdfRenderer\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*/GnuplotService\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*/ReportGeneratorService\\.cpp$")
    list(FILTER SOURCES EXCLUDE REGEX ".*/services/reports/.*\\.cpp$")
endif()

# Executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    # SDD-005: vendored mjansson/mdns (Unlicense / public domain). Single
    # self-contained header, used by DeviceDiscoveryService for the mDNS
    # socket layer and query encoding.
    third_party/mdns
)

if(WIN32)
    # mdns.h needs these for GetAdaptersAddresses and the Winsock calls.
    target_link_libraries(${PROJECT_NAME} ws2_32 iphlpapi)
endif()

# Link libraries
if(MSVC)
    target_link_libraries(${PROJECT_NAME}
        cpapdash_parser
        miniz
        JsonCpp::JsonCpp
        ${PAHO_CPP_TARGET}
        ${PAHO_C_TARGET}
        CURL::libcurl
        unofficial::sqlite3::sqlite3
        OpenSSL::SSL
        OpenSSL::Crypto
        hms_llm
        hms_mqtt
    )
else()
    target_link_libraries(${PROJECT_NAME}
        cpapdash_parser
        miniz
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
    )
    if(NOT APPLE)
        target_link_libraries(${PROJECT_NAME} stdc++fs pthread)
    endif()
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
    # LINK_LIBRARIES carries absolute paths; LIBRARIES is bare -l names that need
    # LIBRARY_DIRS to resolve for keg-only installs (Homebrew mysql-client).
    if(MYSQLCLIENT_LINK_LIBRARIES)
        target_link_libraries(${PROJECT_NAME} ${MYSQLCLIENT_LINK_LIBRARIES})
    else()
        target_link_directories(${PROJECT_NAME} PRIVATE ${MYSQLCLIENT_LIBRARY_DIRS})
        target_link_libraries(${PROJECT_NAME} ${MYSQLCLIENT_LIBRARIES})
    endif()
    target_include_directories(${PROJECT_NAME} PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
endif()

if(BUILD_WITH_WEB)
    target_link_libraries(${PROJECT_NAME} Drogon::Drogon)
endif()

# libharu PDF generation (Linux/macOS)
if(NOT MSVC)
    if(NOT APPLE)
        target_link_options(${PROJECT_NAME} PRIVATE -Wl,--no-as-needed)
    endif()
    target_link_libraries(${PROJECT_NAME} ${HPDF_LIB})
endif()

# BLE support (optional, requires libsdbus-c++)
option(BUILD_WITH_BLE "Build with BLE support for direct O2 Ring connectivity" OFF)
if(BUILD_WITH_BLE)
    find_package(sdbus-c++ REQUIRED)
    target_link_libraries(${PROJECT_NAME} SDBusCpp::sdbus-c++)
    target_compile_definitions(${PROJECT_NAME} PUBLIC WITH_BLE=1)
    target_sources(${PROJECT_NAME} PRIVATE src/clients/O2RingBleClient.cpp)
    message(STATUS "BLE: Enabled (sdbus-c++ ${sdbus-c++_VERSION})")
endif()

# ── Sleep Stage Training CLI ─────────────────────────────────────────────────
# Only needs ML source files (RandomForest, DecisionTree, Scaler, Trainer, etc.)
file(GLOB_RECURSE TRAINER_LIB_SOURCES "src/ml/*.cpp")

add_executable(hms_cpap_train_sleep_stage
    src/cli/train_sleep_stage_main.cpp
    ${TRAINER_LIB_SOURCES}
)

target_include_directories(hms_cpap_train_sleep_stage PRIVATE include)

target_link_libraries(hms_cpap_train_sleep_stage
    cpapdash_parser
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)
if(NOT MSVC AND NOT APPLE)
    target_link_libraries(hms_cpap_train_sleep_stage stdc++fs pthread)
endif()

# Install target
if(WIN32)
    install(TARGETS ${PROJECT_NAME} hms_cpap_train_sleep_stage DESTINATION bin)
else()
    install(TARGETS ${PROJECT_NAME} hms_cpap_train_sleep_stage DESTINATION /usr/local/bin)
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
