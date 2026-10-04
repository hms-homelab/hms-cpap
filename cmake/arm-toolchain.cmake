# Cross-compile for 32-bit Raspberry Pi OS (any Pi, incl. the Zero 2 W), from an
# x86 Debian with g++-arm-linux-gnueabihf, against a sysroot of Raspberry Pi OS.
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-toolchain.cmake [-DRASPBIAN_SYSROOT=/path] ..
#
# The sysroot defaults to ../sysroot (a copy of a Pi's /usr), or comes from
# RASPBIAN_SYSROOT (the release build makes one from the Raspberry Pi OS
# archive: packaging/pi/build-armhf.sh).
#
# Raspberry Pi OS's 32-bit userland is ARMv6 with VFPv2, and its libraries are
# built for that, libstdc++ included: no 2-byte compare-and-swap, so no atomic
# lock policy for shared_ptr. Debian's armhf compiler (ARMv7) ships C++ headers
# that assume the atomic policy. Compiled against those headers and run against
# the Pi's libstdc++, Drogon or libpqxx, every shared_ptr that crosses the
# boundary is counted two ways: heap corruption ("corrupted double-linked list"
# while parsing a night, an abort on every stop; earlier, the SEGV that took
# pqxx out of executeQuery). So the C++ headers come from the sysroot, never
# from the compiler, along with the ARMv6 flags.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT RASPBIAN_SYSROOT)
    set(RASPBIAN_SYSROOT "$ENV{RASPBIAN_SYSROOT}")
endif()
if(NOT RASPBIAN_SYSROOT)
    get_filename_component(RASPBIAN_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../sysroot" ABSOLUTE)
endif()
# Try-compiles get a fresh copy of this file; hand them the sysroot too.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RASPBIAN_SYSROOT)

set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(CMAKE_SYSROOT ${RASPBIAN_SYSROOT})
set(CMAKE_FIND_ROOT_PATH ${RASPBIAN_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Multiarch cmake config path (Debian puts cmake package configs here)
set(CMAKE_PREFIX_PATH ${RASPBIAN_SYSROOT}/usr/lib/arm-linux-gnueabihf/cmake)
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${RASPBIAN_SYSROOT})
set(ENV{PKG_CONFIG_LIBDIR}
    "${RASPBIAN_SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig:${RASPBIAN_SYSROOT}/usr/share/pkgconfig")

set(_arch "-march=armv6 -mfpu=vfp -mfloat-abi=hard -marm")
set(_cxx_headers
    "-nostdinc++"
    "-isystem ${RASPBIAN_SYSROOT}/usr/include/c++/14"
    "-isystem ${RASPBIAN_SYSROOT}/usr/include/arm-linux-gnueabihf/c++/14"
    "-isystem ${RASPBIAN_SYSROOT}/usr/include/c++/14/backward")
string(JOIN " " _cxx_headers ${_cxx_headers})
set(CMAKE_C_FLAGS_INIT   "${_arch}")
set(CMAKE_CXX_FLAGS_INIT "${_arch} ${_cxx_headers}")

set(_libdirs
    "-Wl,-rpath-link,${RASPBIAN_SYSROOT}/usr/lib/arm-linux-gnueabihf"
    "-Wl,-rpath-link,${RASPBIAN_SYSROOT}/lib/arm-linux-gnueabihf")
string(JOIN " " _libdirs ${_libdirs})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_libdirs}")

# Library search paths (ARM multiarch)
set(CMAKE_LIBRARY_PATH
    ${RASPBIAN_SYSROOT}/lib/arm-linux-gnueabihf
    ${RASPBIAN_SYSROOT}/usr/lib/arm-linux-gnueabihf
    ${RASPBIAN_SYSROOT}/usr/lib
)
set(CMAKE_INCLUDE_PATH
    ${RASPBIAN_SYSROOT}/usr/include/arm-linux-gnueabihf
    ${RASPBIAN_SYSROOT}/usr/include
)
