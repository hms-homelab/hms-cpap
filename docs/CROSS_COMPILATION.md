# HMS-CPAP Cross-Compilation Guide

**Building HMS-CPAP for Raspberry Pi Zero 2 W on x86_64 Linux**

This guide explains how to cross-compile HMS-CPAP from an x86_64 development machine for the ARM-based Raspberry Pi Zero 2 W, enabling fast iteration cycles.

## Table of Contents

- [Overview](#overview)
- [Build Time Comparison](#build-time-comparison)
- [Prerequisites](#prerequisites)
- [Quick Start](#quick-start)
- [Architecture Details](#architecture-details)
- [Troubleshooting](#troubleshooting)
- [Maintenance](#maintenance)

---

## Overview

HMS-CPAP supports two build configurations:

1. **Native x86_64 Build** (`CMakeLists.txt`) - For development/testing on x86 machines
2. **ARM Cross-Compilation** (`CMakeLists.txt.arm`) - For building Pi binaries on x86

### Why Cross-Compile?

**Native Pi compilation:**
- ❌ Takes ~5 minutes (Pi Zero 2 W has limited CPU)
- ❌ Can cause out-of-memory issues with parallel builds
- ❌ Requires SSHing into Pi for every build

**Cross-compilation:**
- ✅ Takes ~40 seconds on x86 (12x faster!)
- ✅ Uses all x86 CPU cores efficiently
- ✅ Deploy in one command: `./deploy_to_pi.sh`
- ✅ No disruption to Pi's resources

---

## Build Time Comparison

| Method | Build Time | CPU Cores | Memory Usage |
|--------|-----------|-----------|--------------|
| **Native x86** | ~30s | 8+ cores | ~500MB |
| **ARM Cross-compile** | ~40s | 8+ cores | ~600MB |
| **Native Pi** | ~5 min | 4 cores | ~400MB (can OOM) |

---

## Prerequisites

### 1. Install ARM Cross-Compiler

```bash
sudo apt-get update
sudo apt-get install -y g++-arm-linux-gnueabihf
```

### 2. Create Sysroot (One-Time Setup)

The **sysroot** contains all ARM libraries from the Pi, isolated from your native x86 libraries.

```bash
cd /home/aamat/maestro_hub/projects/hms-cpap

# Create sysroot directory
mkdir -p sysroot/usr sysroot/lib/arm-linux-gnueabihf

# Sync libraries from Pi
rsync -avz aamat@192.168.2.73:/usr/include ./sysroot/usr/
rsync -avz aamat@192.168.2.73:/usr/lib ./sysroot/usr/
rsync -avz aamat@192.168.2.73:/lib/arm-linux-gnueabihf ./sysroot/lib/

# Verify sysroot size (~220MB)
du -sh sysroot/
```

**Important:** The sysroot is a **snapshot** of Pi's libraries. If you update libraries on the Pi (e.g., `apt-get upgrade libpaho-mqtt`), re-sync the sysroot.

### 3. Set Pi SSH Credentials

The deploy script uses `sshpass` for automation:

```bash
sudo apt-get install -y sshpass
```

Edit `deploy_to_pi.sh` if your Pi credentials differ:
```bash
PI_HOST="aamat@192.168.2.73"
PI_PASSWORD="exploracion"
```

---

## Quick Start

### Build for ARM

```bash
cd /home/aamat/maestro_hub/projects/hms-cpap
./build_arm.sh
```

Output:
```
✅ ARM build successful!
-rwxrwxr-x 1 aamat aamat 841K Feb 21 13:05 build-arm/hms_cpap
hms_cpap: ELF 32-bit LSB pie executable, ARM, EABI5
```

### Build for x86 (Native)

```bash
cd /home/aamat/maestro_hub/projects/hms-cpap/build
cmake ..
make -j$(nproc)
```

Output:
```
-rwxrwxr-x 1 aamat aamat 698K Feb 21 13:02 hms_cpap
hms_cpap: ELF 64-bit LSB pie executable, x86-64
```

### Deploy to Pi

```bash
./deploy_to_pi.sh
```

This script:
1. Builds ARM binary
2. Stops HMS-CPAP service on Pi
3. Backs up old binary
4. Deploys new binary
5. Restarts service
6. Shows logs

---

## Architecture Details

### File Structure

```
hms-cpap/
├── CMakeLists.txt           # Native x86 build configuration
├── CMakeLists.txt.arm       # ARM cross-compilation configuration
├── build_arm.sh             # ARM build script
├── deploy_to_pi.sh          # Automated deployment script
├── build/                   # x86 build directory
├── build-arm/               # ARM build directory
├── sysroot/                 # Pi libraries (220MB, gitignored)
│   ├── usr/
│   │   ├── include/         # ARM headers
│   │   └── lib/             # ARM libraries
│   └── lib/
│       └── arm-linux-gnueabihf/  # ARM system libraries
└── cmake/
    └── arm-toolchain.cmake  # (deprecated, merged into CMakeLists.txt.arm)
```

### How Cross-Compilation Works

```
┌─────────────────────────────────────────────────────────────┐
│  x86_64 Development Machine                                 │
│                                                              │
│  ┌────────────────────┐                                     │
│  │  CMakeLists.txt.arm│                                     │
│  │  (ARM config)      │                                     │
│  └────────┬───────────┘                                     │
│           │                                                  │
│           ↓                                                  │
│  ┌────────────────────┐      ┌──────────────────┐          │
│  │ arm-linux-gnueabihf│ ───→ │    Sysroot       │          │
│  │ Cross-Compiler     │      │  (Pi libraries)  │          │
│  └────────┬───────────┘      └──────────────────┘          │
│           │                                                  │
│           ↓                                                  │
│  ┌────────────────────┐                                     │
│  │  hms_cpap (ARM)    │  ─────────────────────────┐        │
│  │  841K ELF 32-bit   │                            │        │
│  └────────────────────┘                            │        │
└─────────────────────────────────────────────────────┼───────┘
                                                      │
                          SCP/Deploy                  │
                                                      ↓
                          ┌────────────────────────────────────┐
                          │  Raspberry Pi Zero 2 W (ARM)       │
                          │                                    │
                          │  /usr/local/bin/hms_cpap           │
                          │  systemctl restart hms-cpap        │
                          └────────────────────────────────────┘
```

### CMakeLists.txt.arm Key Settings

**Target Configuration:**
```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER /usr/bin/arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/arm-linux-gnueabihf-g++)
```

**Sysroot:**
```cmake
set(CMAKE_SYSROOT ${CMAKE_CURRENT_LIST_DIR}/sysroot)
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

**Pi Zero 2 W Optimizations (Cortex-A53):**
```cmake
set(CMAKE_C_FLAGS "-march=armv8-a+crc -mtune=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard")
```

**Library Search Paths:**
```cmake
set(CMAKE_LIBRARY_PATH
    ${CMAKE_SYSROOT}/lib/arm-linux-gnueabihf
    ${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf
)
```

**MQTT Libraries (Critical!):**

The MQTT C++ wrapper (`libpaho-mqttpp3`) depends on the MQTT C library (`libpaho-mqtt3as`). We use **shared libraries from `/lib`** on the Pi:

```cmake
find_library(PAHO_MQTTPP3_LIB
    NAMES paho-mqttpp3
    PATHS ${CMAKE_SYSROOT}/lib/arm-linux-gnueabihf
    NO_DEFAULT_PATH
)
find_library(PAHO_MQTT3AS_LIB
    NAMES paho-mqtt3as
    PATHS ${CMAKE_SYSROOT}/lib/arm-linux-gnueabihf
    NO_DEFAULT_PATH
)
```

**Why `/lib` instead of `/usr/lib`?**
- Pi's MQTT runtime libraries live in `/lib/arm-linux-gnueabihf/`
- Static libraries (`.a`) in `/usr/lib` had unresolved symbols
- Using shared libraries (`.so`) from `/lib` matches Pi's runtime exactly

---

## Troubleshooting

### Issue: "undefined reference to mqtt::async_client"

**Cause:** Sysroot missing MQTT libraries from `/lib`

**Fix:**
```bash
rsync -avz aamat@192.168.2.73:/lib/arm-linux-gnueabihf/libpaho-mqtt* ./sysroot/lib/arm-linux-gnueabihf/
./build_arm.sh
```

### Issue: "No such file or directory: libcurl.so"

**Cause:** Sysroot incomplete

**Fix:**
```bash
# Re-sync entire sysroot
rsync -avz aamat@192.168.2.73:/usr/lib ./sysroot/usr/
```

### Issue: Binary runs on x86 but not on Pi

**Cause:** Wrong architecture

**Check:**
```bash
file build-arm/hms_cpap
# Should say: ARM, EABI5 version 1
```

**Fix:** Ensure you built with `build_arm.sh`, not `cmake .. && make`

### Issue: "Bus error" or "Illegal instruction" on Pi

**Cause:** Wrong ARM architecture flags

**Check:** Pi Zero 2 W is ARMv8 (Cortex-A53), not ARMv7

**Fix:** Verify in `CMakeLists.txt.arm`:
```cmake
-march=armv8-a+crc -mtune=cortex-a53
```

### Issue: Native x86 build uses ARM libraries

**Cause:** Sysroot leaked into native build

**Fix:**
```bash
# Always use separate build directories
rm -rf build/*
cmake ..  # (no -C or toolchain flags)
make
```

**Verify:**
```bash
ldd build/hms_cpap | grep -i arm
# Should be empty for x86 build
```

---

## Maintenance

### Updating Sysroot After Pi Library Upgrades

When you update libraries on the Pi (e.g., `apt-get upgrade`), sync the sysroot:

```bash
cd /home/aamat/maestro_hub/projects/hms-cpap

# Update specific library
rsync -avz aamat@192.168.2.73:/usr/lib/arm-linux-gnueabihf/libpaho-mqtt* ./sysroot/usr/lib/arm-linux-gnueabihf/

# Or update everything
rsync -avz --delete aamat@192.168.2.73:/usr/lib ./sysroot/usr/
```

### Verifying Binary Compatibility

Before deploying, verify the binary on Pi:

```bash
# Copy to test directory
scp build-arm/hms_cpap aamat@192.168.2.73:~/test/

# Test without disrupting service
ssh aamat@192.168.2.73 "cd ~/test && ./hms_cpap --help"
```

### Clean Builds

```bash
# Clean ARM build
rm -rf build-arm/*

# Clean x86 build
rm -rf build/*

# Clean both
git clean -fdx build/ build-arm/
```

---

## Development Workflow

### Typical Edit-Build-Deploy Cycle

```bash
# 1. Edit code on x86
vim src/mqtt/MqttClient.cpp

# 2. Build and deploy in one command (40 seconds)
./deploy_to_pi.sh

# 3. Watch logs
ssh aamat@192.168.2.73 "journalctl -u hms-cpap -f"
```

### Testing Both Architectures

```bash
# Build x86 for unit tests
cd build
cmake -DBUILD_TESTS=ON ..
make -j$(nproc)
./tests/run_tests

# Build ARM for deployment
cd ..
./build_arm.sh
./deploy_to_pi.sh
```

---

## Performance Notes

### Binary Size Comparison

| Build Type | Size | Notes |
|------------|------|-------|
| **x86 (native)** | 698K | Stripped, shared libs |
| **ARM (cross)** | 841K | Stripped, shared libs |
| **Pi (native)** | 14M | Unstripped, debug symbols |

The Pi native build is larger because:
- Built with `-g` debug flags
- Includes full symbol table
- Not stripped

To match cross-compiled size on Pi:
```bash
strip --strip-all /usr/local/bin/hms_cpap
```

### Memory Footprint

Both binaries have identical runtime memory (~6.5MB RSS):
```bash
# On Pi
ps aux | grep hms_cpap
# VSZ: 14268 KB, RSS: 6536 KB
```

---

## References

- [CMake Cross-Compiling](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html#cross-compiling)
- [Raspberry Pi Cross-Compilation](https://www.raspberrypi.com/documentation/computers/processors.html)
- [Paho MQTT C++ Documentation](https://github.com/eclipse/paho.mqtt.cpp)

---

**Last Updated:** 2026-02-21
**Version:** 1.1.3
