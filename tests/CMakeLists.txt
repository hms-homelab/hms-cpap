# HMS-CPAP Unit Tests
cmake_minimum_required(VERSION 3.16)

# Find Google Test
find_package(GTest REQUIRED)
include(GoogleTest)

# Include directories (same as main project)
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${GTEST_INCLUDE_DIRS}
)
if(NOT MSVC)
    include_directories(/usr/local/include)
endif()

# Collect all source files from main project (except main.cpp and web/controllers)
file(GLOB_RECURSE PROJECT_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*main\\.cpp$")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/controllers/.*")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/web/.*")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/WiFiSwitchClient\\.cpp$")

# Test sources
set(TEST_SOURCES
    parsers/test_EDFParser.cpp
    parsers/test_STRParser.cpp
    parsers/test_PLDParser.cpp
    parsers/test_SA2Parser.cpp
    parsers/test_STRParser_ASV.cpp
    database/test_DatabaseService.cpp
    services/test_BurstCollectorService.cpp
    services/test_DataPublisherService.cpp
    models/test_CPAPModels.cpp
    utils/test_ConfigManager.cpp
    utils/test_AppConfig.cpp
    clients/test_EzShareClient.cpp
    mqtt/test_MqttClient.cpp
    integration/test_STRIntegration.cpp
    integration/test_ASVIntegration.cpp
    agent/test_AgentTools.cpp
    agent/test_AgentMemory.cpp
    agent/test_AgentService.cpp
)

# Create test executable
add_executable(run_tests
    ${TEST_SOURCES}
    ${PROJECT_SOURCES}
)

# Link libraries (same as main project + GTest)
if(MSVC)
    target_link_libraries(run_tests
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
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
    target_link_libraries(run_tests
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
        jsoncpp_lib
        ${PAHO_MQTTPP3_LIB}
        ${PAHO_MQTT3AS_LIB}
        CURL::libcurl
        ${SQLITE3_LIB}
        OpenSSL::SSL
        OpenSSL::Crypto
        hms_llm
        hms_mqtt
        pthread
    )
endif()

# Optional DB backends for tests
if(BUILD_WITH_POSTGRESQL)
    if(MSVC)
        target_link_libraries(run_tests libpqxx::pqxx PostgreSQL::PostgreSQL hms_db)
    else()
        target_link_libraries(run_tests ${PQXX_LIB} ${PQ_LIB} hms_db)
    endif()
endif()

# Discover tests
gtest_discover_tests(run_tests)

# Print test configuration
message(STATUS "Unit Tests Configuration:")
message(STATUS "  Test sources: ${TEST_SOURCES}")
message(STATUS "  GTest: ${GTEST_LIBRARIES}")
