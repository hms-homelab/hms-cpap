# HMS-CPAP Build & Deploy Quick Reference

## 🚀 Quick Commands

### Build for x86 (Native Development)
```bash
cd build
cmake ..
make -j$(nproc)
./hms_cpap  # Test locally
```

### Build for ARM (Pi Zero 2 W)
```bash
./build_arm.sh
```

### Deploy to Pi
```bash
./deploy_to_pi.sh
```

---

## 📋 File Reference

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Native x86 build configuration |
| `CMakeLists.txt.arm` | ARM cross-compilation configuration |
| `build_arm.sh` | ARM cross-compile build script |
| `deploy_to_pi.sh` | Build + deploy + restart service |
| `sysroot/` | Pi libraries (220MB, synced from Pi) |
| `build/` | x86 build directory |
| `build-arm/` | ARM build directory |

---

## 🔧 Build Configurations

### Native x86 Build

**When to use:**
- Local development and testing
- Running unit tests
- Debugging with GDB

**Configuration:**
- Compiler: `g++` (x86_64)
- Libraries: System native (`/usr/lib/x86_64-linux-gnu/`)
- Binary: `build/hms_cpap` (698K)

**Example:**
```bash
cd build
cmake -DBUILD_TESTS=ON ..
make -j$(nproc)
./tests/run_tests
```

---

### ARM Cross-Compilation

**When to use:**
- Deploying to Pi Zero 2 W
- Fast iteration (40s vs 5min)
- Building release binaries

**Configuration:**
- Compiler: `arm-linux-gnueabihf-g++`
- Libraries: Sysroot (`sysroot/`)
- Target: Cortex-A53 (ARMv8)
- Binary: `build-arm/hms_cpap` (841K)

**Example:**
```bash
./build_arm.sh
# Binary ready at: build-arm/hms_cpap
```

---

## ⚙️ One-Time Setup

### Install ARM Toolchain
```bash
sudo apt-get install -y g++-arm-linux-gnueabihf sshpass
```

### Create Sysroot
```bash
mkdir -p sysroot/usr sysroot/lib/arm-linux-gnueabihf

# Sync from Pi (takes 2-3 minutes)
rsync -avz aamat@192.168.2.73:/usr/include ./sysroot/usr/
rsync -avz aamat@192.168.2.73:/usr/lib ./sysroot/usr/
rsync -avz aamat@192.168.2.73:/lib/arm-linux-gnueabihf ./sysroot/lib/

# Verify size (~220MB)
du -sh sysroot/
```

---

## 🐛 Troubleshooting

### "Permission denied" when deploying
```bash
# Add your SSH key to Pi
ssh-copy-id aamat@192.168.2.73
```

### "undefined reference to mqtt::"
```bash
# Re-sync MQTT libraries from Pi
rsync -avz aamat@192.168.2.73:/lib/arm-linux-gnueabihf/libpaho-mqtt* ./sysroot/lib/arm-linux-gnueabihf/
```

### x86 build uses ARM libraries
```bash
# Clean and rebuild
rm -rf build/*
cd build
cmake ..  # (no toolchain flags!)
make
```

### ARM binary won't run on Pi
```bash
# Check architecture
file build-arm/hms_cpap
# Should say: ARM, EABI5 version 1

# If wrong, rebuild
rm -rf build-arm/*
./build_arm.sh
```

---

## 📊 Performance Comparison

| Method | Build Time | CPU | Memory |
|--------|-----------|-----|--------|
| **x86 native** | 30s | 8 cores | 500MB |
| **ARM cross** | 40s | 8 cores | 600MB |
| **Pi native** | 5 min | 4 cores | 400MB |

**Recommendation:** Use ARM cross-compilation for deployment builds.

---

## 🔄 Typical Development Workflow

```bash
# 1. Edit code
vim src/mqtt/MqttClient.cpp

# 2. Test locally (x86)
cd build
make -j$(nproc)
./hms_cpap --help

# 3. Build for ARM and deploy
cd ..
./deploy_to_pi.sh

# 4. Watch logs on Pi
ssh aamat@192.168.2.73 "journalctl -u hms-cpap -f"
```

---

## 📝 Deployment Script Details

**`deploy_to_pi.sh` does:**
1. ✅ Builds ARM binary (`./build_arm.sh`)
2. ✅ Stops HMS-CPAP service on Pi
3. ✅ Backs up old binary
4. ✅ Deploys new binary via SCP
5. ✅ Restarts service
6. ✅ Shows status and logs

**Example output:**
```
╔══════════════════════════════════════════════════════════╗
║         HMS-CPAP Pi Zero 2 W Deployment                 ║
╚══════════════════════════════════════════════════════════╝

📦 Step 1: Building ARM binary...
✅ ARM binary built successfully

🛑 Step 2: Stopping HMS-CPAP service on Pi...
✅ Service stopped

💾 Step 3: Backing up old binary on Pi...
✅ Backup created

🚀 Step 4: Deploying new binary to Pi...
✅ Binary deployed

🔄 Step 5: Restarting HMS-CPAP service...
✅ Service restarted

✅ Deployment complete!
```

---

## 📚 Full Documentation

- **Detailed Guide:** [docs/CROSS_COMPILATION.md](docs/CROSS_COMPILATION.md)
- **Main README:** [README.md](README.md)
- **CHANGELOG:** [CHANGELOG.md](CHANGELOG.md)

---

## 🆘 Getting Help

```bash
# View service logs
ssh aamat@192.168.2.73 "journalctl -u hms-cpap -f"

# Check service status
ssh aamat@192.168.2.73 "systemctl status hms-cpap"

# Test binary without service
ssh aamat@192.168.2.73
cd ~/test-hms-cpap
./hms_cpap --help
```

---

**Last Updated:** 2026-02-21
**Version:** 1.1.3
