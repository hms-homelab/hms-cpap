# ARM Cross-compilation toolchain for Pi Zero 2 W (Cortex-A53)
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=../arm-toolchain.cmake ..

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER /usr/bin/arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/arm-linux-gnueabihf-g++)

# Sysroot (contains ARM libraries from Pi)
set(CMAKE_SYSROOT ${CMAKE_CURRENT_LIST_DIR}/../sysroot)
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Multiarch cmake config path (Debian puts cmake package configs here)
set(CMAKE_PREFIX_PATH ${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf/cmake)

# Compiler flags for Pi Zero 2 W (Cortex-A53)
set(CMAKE_C_FLAGS_INIT "-march=armv8-a+crc -mtune=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv8-a+crc -mtune=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard")

# Linker flags to find ARM libraries
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,${CMAKE_SYSROOT}/lib/arm-linux-gnueabihf")

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
