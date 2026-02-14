cmake_minimum_required(VERSION 3.16)
project(hms_cpap CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")

# Find required packages
option(BUILD_WITH_HEALTHCHECK "Build with Drogon health check endpoint" OFF)

if(BUILD_WITH_HEALTHCHECK)
    find_package(Drogon CONFIG REQUIRED)
    add_definitions(-DBUILD_WITH_HEALTHCHECK)
endif()

find_package(jsoncpp REQUIRED)
find_library(PAHO_MQTTPP3_LIB paho-mqttpp3)
find_library(PAHO_MQTT3AS_LIB paho-mqtt3as)
find_library(PQXX_LIB pqxx)
find_library(PQ_LIB pq)
find_library(CURL_LIB curl)
# EDF parsing: raw implementation (edflib 1.27 rejects ResMed non-standard files)

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    /usr/local/include
)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    jsoncpp_lib
    ${PAHO_MQTTPP3_LIB}
    ${PAHO_MQTT3AS_LIB}
    ${PQXX_LIB}
    ${PQ_LIB}
    ${CURL_LIB}
    pthread
    ssl
    crypto
)

if(BUILD_WITH_HEALTHCHECK)
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
message(STATUS "  CURL: ${CURL_LIB}")
message(STATUS "  Paho MQTT: ${PAHO_MQTTPP3_LIB}")
message(STATUS "  PostgreSQL: ${PQXX_LIB}")
message(STATUS "  Build Tests: ${BUILD_TESTS}")
