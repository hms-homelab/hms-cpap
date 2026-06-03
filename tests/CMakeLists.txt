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
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/cli/.*\\.cpp$")
if(MSVC)
    list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/FysetcTcpServer\\.cpp$")
    list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/FysetcDataSource\\.cpp$")
    list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/FysetcSectorCollectorService\\.cpp$")
endif()
# EDFParser + CPAPModels now provided by cpapdash-parser library
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/parsers/EDFParser\\.cpp$")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/models/CPAPModels\\.cpp$")
# PDF/report services depend on QueryService (web/) and libhpdf — exclude from test build
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/PdfRenderer\\.cpp$")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/GnuplotService\\.cpp$")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/ReportGeneratorService\\.cpp$")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/services/reports/.*\\.cpp$")

# Test sources
set(TEST_SOURCES
    parsers/test_EDFParser.cpp
    parsers/test_Fat32Parser.cpp
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
    clients/test_FysetcTcpServer.cpp
    mqtt/test_MqttClient.cpp
    integration/test_STRIntegration.cpp
    integration/test_ASVIntegration.cpp
    agent/test_AgentTools.cpp
    agent/test_AgentMemory.cpp
    agent/test_AgentService.cpp
    ml/test_StandardScaler.cpp
    ml/test_DecisionTree.cpp
    ml/test_RandomForest.cpp
    ml/test_FeatureEngine.cpp
    ml/test_MLTrainingService.cpp
    ml/test_sleep_stage_feature_extractor.cpp
    ml/test_hmm_smoother.cpp
    ml/test_sleep_stage_trainer.cpp
    services/test_BackfillService.cpp
    services/test_SessionDiscoveryService.cpp
    services/test_OximetryService.cpp
    services/test_FysetcSectorCollectorService.cpp
    clients/test_O2RingClient.cpp
    clients/test_O2RingBleProtocol.cpp
    services/test_sleep_stage_classifier.cpp
    services/test_PrismaIngestion.cpp
    integration/test_PrismaE2E.cpp
    services/test_InsightsEngine.cpp
    database/test_SQLiteDatabase.cpp
    mqtt/test_DiscoveryPublisher.cpp
    database/test_DatabaseService_pg.cpp
)
if(MSVC)
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*Fysetc.*")
endif()

# Create test executable
add_executable(run_tests
    ${TEST_SOURCES}
    ${PROJECT_SOURCES}
)

# Opt-in code coverage (gcov/lcov). OFF by default; enable with
# -DENABLE_COVERAGE=ON (see scripts/coverage.sh). Not supported on MSVC.
option(ENABLE_COVERAGE "Instrument run_tests with gcov coverage" OFF)
if(ENABLE_COVERAGE AND NOT MSVC)
    target_compile_options(run_tests PRIVATE --coverage -O0 -g -fno-inline)
    target_link_options(run_tests PRIVATE --coverage)
endif()

# Link libraries (same as main project + GTest)
if(MSVC)
    target_link_libraries(run_tests
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
        cpapdash_parser
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

if(BUILD_WITH_MYSQL)
    target_link_libraries(run_tests ${MYSQLCLIENT_LIBRARIES})
    target_include_directories(run_tests PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
endif()

# Discover tests (PRE_TEST defers discovery until ctest runs, fixing MSVC multi-config)
gtest_discover_tests(run_tests DISCOVERY_MODE PRE_TEST)

# Print test configuration
message(STATUS "Unit Tests Configuration:")
message(STATUS "  Test sources: ${TEST_SOURCES}")
message(STATUS "  GTest: ${GTEST_LIBRARIES}")
