cmake_minimum_required(VERSION 3.16)
project(hms_cpap CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")

# ARM Cross-compilation specific
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER /usr/bin/arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/arm-linux-gnueabihf-g++)

# Sysroot (contains ARM libraries from Pi)
set(CMAKE_SYSROOT ${CMAKE_CURRENT_LIST_DIR}/sysroot)
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Compiler flags for Pi Zero 2 W (Cortex-A53)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a+crc -mtune=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+crc -mtune=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard")

# Linker flags to find ARM libraries
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,${CMAKE_SYSROOT}/lib/arm-linux-gnueabihf")

# Library search paths (ARM multiarch)
set(CMAKE_LIBRARY_PATH
    ${CMAKE_SYSROOT}/lib/arm-linux-gnueabihf
    ${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf
    ${CMAKE_SYSROOT}/usr/lib
)
set(CMAKE_INCLUDE_PATH
    ${CMAKE_SYSROOT}/usr/include/arm-linux-gnueabihf
    ${CMAKE_SYSROOT}/usr/include
)

# Find required packages
option(BUILD_WITH_HEALTHCHECK "Build with Drogon health check endpoint" OFF)

if(BUILD_WITH_HEALTHCHECK)
    find_package(Drogon CONFIG REQUIRED)
    add_definitions(-DBUILD_WITH_HEALTHCHECK)
endif()

find_package(jsoncpp REQUIRED)

# Find MQTT libraries (use shared .so files from Pi)
find_library(PAHO_MQTTPP3_LIB
    NAMES paho-mqttpp3
    PATHS ${CMAKE_SYSROOT}/lib/arm-linux-gnueabihf ${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf
    NO_DEFAULT_PATH
)
find_library(PAHO_MQTT3AS_LIB
    NAMES paho-mqtt3as
    PATHS ${CMAKE_SYSROOT}/lib/arm-linux-gnueabihf ${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf
    NO_DEFAULT_PATH
)

find_library(PQXX_LIB pqxx)
find_library(PQ_LIB pq)
find_library(CURL_LIB curl)

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/jsoncpp
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

# Testing support - disabled for cross-compilation
option(BUILD_TESTS "Build unit tests" OFF)

# Print configuration
message(STATUS "HMS-CPAP ARM Cross-Compilation Build:")
message(STATUS "  Target: ARM (Pi Zero 2 W - Cortex-A53)")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  Sysroot: ${CMAKE_SYSROOT}")
message(STATUS "  EDF: raw parser (ResMed compatible)")
message(STATUS "  CURL: ${CURL_LIB}")
message(STATUS "  Paho MQTT C++: ${PAHO_MQTTPP3_LIB}")
message(STATUS "  Paho MQTT C: ${PAHO_MQTT3AS_LIB}")
message(STATUS "  PostgreSQL: ${PQXX_LIB}")
message(STATUS "  Build Tests: ${BUILD_TESTS}")
