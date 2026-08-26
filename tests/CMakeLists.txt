# HMS-CPAP Unit Tests
cmake_minimum_required(VERSION 3.16)

# Find Google Test
find_package(GTest REQUIRED)
include(GoogleTest)

# Include directories (same as main project)
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/mdns   # SDD-005: vendored mjansson/mdns
    ${CMAKE_CURRENT_SOURCE_DIR}   # shared test headers, e.g. EquipmentStubs.h
    ${GTEST_INCLUDE_DIRS}
)
if(NOT MSVC)
    include_directories(/usr/local/include)
endif()

# Collect all source files from main project (except main.cpp and web/controllers)
file(GLOB_RECURSE PROJECT_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*main\\.cpp$")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/controllers/.*")
# ...but EquipmentController is testable in-process: unlike CpapController it pulls in
# no QueryService/web machinery, only IDatabase + SupplyStatus. Its handlers are driven
# directly by tests/controllers/test_EquipmentController.cpp (testing it over a socket
# is useless here — the SPA fallback answers 404s with index.html).
list(APPEND PROJECT_SOURCES "${CMAKE_SOURCE_DIR}/src/controllers/EquipmentController.cpp")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX ".*/web/.*")
# ...but QueryService is testable in-process for the same reason: it pulls in no
# Drogon, only IDatabase + SqlDialect + jsoncpp, so its read queries can run
# against a temp SQLite file. It builds SQL for three dialects by hand, which is
# exactly the kind of code that needs a test — an unnarrowed OR in the oximetry
# query silently served every recording on two consecutive nights.
list(APPEND PROJECT_SOURCES "${CMAKE_SOURCE_DIR}/src/web/IngressBase.cpp")
list(APPEND PROJECT_SOURCES "${CMAKE_SOURCE_DIR}/src/web/QueryService.cpp")
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
    database/test_EquipmentBackends.cpp
    database/test_OximetryBackends.cpp
    database/test_MySQLMigration.cpp
    database/test_CleaningBackends.cpp
    database/test_SyncFolderBackends.cpp
    database/test_SessionFilesBackends.cpp
    database/test_ReportBackends.cpp
    services/test_BurstCollectorService.cpp
    services/test_DataPublisherService.cpp
    models/test_CPAPModels.cpp
    utils/test_ConfigManager.cpp
    utils/test_AppConfig.cpp
    utils/test_FileUtils.cpp
    utils/test_CardResidue.cpp
    utils/test_CardImport.cpp
    utils/test_CardLayout.cpp
    utils/test_DbProbeCli.cpp
    utils/test_FileLogger.cpp
    utils/test_ParentWatch.cpp

    # SDD-016: the supervisor's Qt-free core. Listed by explicit path, and the
    # sources it tests are added the same way below -- desktop/qt/ is outside
    # src/ precisely so neither GLOB_RECURSE sweeps a GUI into the service or
    # the test binary.
    desktop/test_PreflightReport.cpp
    desktop/test_ConfigGate.cpp

    utils/test_FailureLogThrottle.cpp
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
    services/test_O2RingCsvParser.cpp
    services/test_O2RingCsvWriter.cpp
    services/test_FysetcSectorCollectorService.cpp
    clients/test_O2RingClient.cpp
    clients/test_O2RingBleProtocol.cpp
    services/test_sleep_stage_classifier.cpp
    services/test_PrismaIngestion.cpp
    integration/test_PrismaE2E.cpp
    services/test_InsightsEngine.cpp
    database/test_SQLiteDatabase.cpp
    database/test_DatabaseFactory.cpp
    mqtt/test_DiscoveryPublisher.cpp
    database/test_DatabaseService_pg.cpp
    database/test_PostgresEdges.cpp
    services/test_DataPublisherDeeper.cpp
    clients/test_FysetcDataSource.cpp
    services/test_LiveSleepStageRunner.cpp
    services/test_SleepHqExportService.cpp
    services/test_CpapDashSyncService.cpp
    services/test_DeviceDiscoveryService.cpp
    services/test_SetupService.cpp
    services/test_PreflightService.cpp
    services/test_CleaningStatus.cpp
    services/test_CleaningPublisher.cpp
    services/test_SyncFolderState.cpp
    services/test_SyncNow.cpp
    services/test_SupplyStatus.cpp
    services/test_SupplyPublisher.cpp
    controllers/test_EquipmentController.cpp
    web/test_IngressBase.cpp
    web/test_QueryService_index.cpp
    web/test_QueryService_oximetry.cpp
    web/test_QueryService_reads.cpp
)
if(MSVC)
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*Fysetc.*")
endif()

# The supervisor's Qt-free core, compiled straight into run_tests. These files
# depend on nothing but the standard library, which is the point: the parsing
# and validation the desktop app relies on is testable on every platform with
# no display, no offscreen plugin and no Qt in the test build at all.
set(SUPERVISOR_PURE_SOURCES
    ${CMAKE_SOURCE_DIR}/desktop/qt/core/pure/PreflightReport.cpp
    ${CMAKE_SOURCE_DIR}/desktop/qt/core/pure/ConfigModel.cpp
    ${CMAKE_SOURCE_DIR}/desktop/qt/core/pure/FieldSpec.cpp
    ${CMAKE_SOURCE_DIR}/desktop/qt/core/pure/ConfigGate.cpp
)

# Create test executable
add_executable(run_tests
    ${TEST_SOURCES}
    ${PROJECT_SOURCES}
    ${SUPERVISOR_PURE_SOURCES}
)
target_include_directories(run_tests PRIVATE ${CMAKE_SOURCE_DIR}/desktop/qt/core/pure)

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

# EquipmentController is exercised in-process by the controller tests, so the test
# binary needs Drogon's request/response types. find_package(Drogon) already ran in
# the parent scope when BUILD_WITH_WEB is on.
if(BUILD_WITH_WEB)
    target_link_libraries(run_tests Drogon::Drogon)
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
    # Must resolve to the SAME client library the main target links. Bare -l names
    # let the linker pick whichever client happens to sit in a default path, and a
    # run_tests on MariaDB Connector/C while hms_cpap is on libmysqlclient tests
    # different TLS defaults than the binary that ships. Mirrors the root
    # CMakeLists so both agree.
    if(MYSQLCLIENT_LINK_LIBRARIES)
        target_link_libraries(run_tests ${MYSQLCLIENT_LINK_LIBRARIES})
    else()
        target_link_directories(run_tests PRIVATE ${MYSQLCLIENT_LIBRARY_DIRS})
        target_link_libraries(run_tests ${MYSQLCLIENT_LIBRARIES})
    endif()
    target_include_directories(run_tests PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
endif()

# Discover tests (PRE_TEST defers discovery until ctest runs, fixing MSVC multi-config)
gtest_discover_tests(run_tests DISCOVERY_MODE PRE_TEST)

# Print test configuration
message(STATUS "Unit Tests Configuration:")
message(STATUS "  Test sources: ${TEST_SOURCES}")
message(STATUS "  GTest: ${GTEST_LIBRARIES}")
