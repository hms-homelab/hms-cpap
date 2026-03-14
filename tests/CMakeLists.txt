# HMS-CPAP Unit Tests
cmake_minimum_required(VERSION 3.16)

# Find Google Test
find_package(GTest REQUIRED)
include(GoogleTest)

# Include directories (same as main project)
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${GTEST_INCLUDE_DIRS}
    /usr/local/include
)

# Collect all source files from main project (except main.cpp)
file(GLOB_RECURSE PROJECT_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*main\\.cpp$")

# Test sources
set(TEST_SOURCES
    parsers/test_EDFParser.cpp
    parsers/test_STRParser.cpp
    database/test_DatabaseService.cpp
    services/test_BurstCollectorService.cpp
    services/test_DataPublisherService.cpp
    models/test_CPAPModels.cpp
    utils/test_ConfigManager.cpp
    clients/test_EzShareClient.cpp
    mqtt/test_MqttClient.cpp
    integration/test_STRIntegration.cpp
)

# Create test executable
add_executable(run_tests
    ${TEST_SOURCES}
    ${PROJECT_SOURCES}
)

# Link libraries (same as main project + GTest)
target_link_libraries(run_tests
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
    jsoncpp_lib
    ${PAHO_MQTTPP3_LIB}
    ${PAHO_MQTT3AS_LIB}
    ${PQXX_LIB}
    ${PQ_LIB}
    CURL::libcurl
    hms_llm
    pthread
    ssl
    crypto
)

# Link Drogon if health check is enabled
if(BUILD_WITH_HEALTHCHECK)
    target_link_libraries(run_tests Drogon::Drogon)
endif()

# Discover tests
gtest_discover_tests(run_tests)

# Print test configuration
message(STATUS "Unit Tests Configuration:")
message(STATUS "  Test sources: ${TEST_SOURCES}")
message(STATUS "  GTest: ${GTEST_LIBRARIES}")
