# HMS-CPAP 🫁

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![GHCR](https://img.shields.io/badge/ghcr.io-hms--cpap-blue?logo=docker)](https://github.com/hms-homelab/hms-cpap/pkgs/container/hms-cpap)
[![Build](https://github.com/hms-homelab/hms-cpap/actions/workflows/docker-build.yml/badge.svg)](https://github.com/hms-homelab/hms-cpap/actions)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-%23FFDD00.svg?logo=buy-me-a-coffee)](https://www.buymeacoffee.com/aamat09)

**Lightweight C++ microservice for ResMed CPAP data collection with Home Assistant integration.**

Automatically extracts sleep therapy data from your ResMed AirSense 10/11 CPAP machine, parses EDF files using OSCAR algorithms, and publishes 47+ metrics to Home Assistant via MQTT discovery. Supports three data sources: ez Share WiFi SD (HTTP polling), FYSETC SD WiFi Pro (MQTT push with realtime therapy data), or local filesystem.

![Home Assistant Dashboard](docs/images/ha-dashboard-preview.png)

## ✨ Features

- **3 Data Sources** - ezShare WiFi SD (polling), FYSETC SD WiFi Pro (MQTT push), or local files
- **47+ Metrics** - AHI, leak rate, pressure, usage hours, events, STR daily summary, LLM AI summary
- **Home Assistant Auto-Discovery** - Instant MQTT integration with 47 sensor entities
- **PostgreSQL Storage** - Historical data for ML analysis and long-term trends
- **Session Grouping** - Intelligently combines BRP/PLD/SAD/EVE/CSL files into therapy sessions
- **Realtime Therapy Data** - FYSETC mode streams BRP data every 60s during therapy
- **LLM Session Summary** - Optional AI-generated therapy analysis via Ollama
- **Ultra-Lightweight** - 6.5 MB native binary
- **155 Unit Tests** - Comprehensive coverage across all services

## 📋 Table of Contents

- [Quick Start](#-quick-start)
- [Data Sources](#-data-sources)
- [Configuration](#-configuration)
- [Deployment](#-deployment)
- [Home Assistant Integration](#-home-assistant-integration)
- [Architecture](#-architecture)
- [Development](#-development)
- [FAQ](#-faq)
- [Contributing](#-contributing)

## Quick Start

```bash
# 1. Clone and build
git clone https://github.com/hms-homelab/hms-cpap.git
cd hms-cpap
mkdir build && cd build
cmake .. && make -j$(nproc)

# 2. Configure
cp ../.env.example ../.env
nano ../.env  # Set MQTT, DB, and source settings

# 3. Run (choose your data source)
CPAP_SOURCE=ezshare ./hms_cpap   # ezShare WiFi SD polling
CPAP_SOURCE=fysetc  ./hms_cpap   # FYSETC SD WiFi Pro MQTT push
CPAP_SOURCE=local   ./hms_cpap   # Local filesystem
```

## Data Sources

### Option A: ezShare WiFi SD (Original)

**How it works:** HTTP polling every 65s via WiFi bridge on Raspberry Pi.

**Hardware:** ezShare WiFi SD card + Raspberry Pi with dual WiFi (wlan0=home, wlan1=ezShare AP).

**Pros:** Simple, no hardware mods. **Cons:** 2-4 min session detection latency, requires dedicated Pi WiFi interface.

### Option B: FYSETC SD WiFi Pro (Recommended)

**How it works:** ESP32-based PCB sits between CPAP and SD card on the SPI bus. Passively monitors CS line via hardware pulse counter, performs surgical bus steals (<10ms) during therapy, pushes EDF data via MQTT.

**Hardware:** FYSETC SD WiFi Pro board installed inside CPAP machine.

**Pros:** 65s end-to-end latency, realtime BRP streaming during therapy, zero network disruption, BRP-validated therapy detection. **Cons:** Requires hardware installation inside CPAP.

**Firmware:** [hms-cpap-fysetc](https://github.com/hms-homelab/hms-cpap-fysetc) -- ESP-IDF project, builds with `idf.py build`.

See [docs/FYSETC_RECEIVER.md](docs/FYSETC_RECEIVER.md) for the full protocol, MQTT topics, and FSM state diagram.

### Option C: Local Filesystem

**How it works:** Reads EDF files from a local directory (e.g. mounted SD card or NAS).

**Use case:** Offline analysis, backfill, or reparse of archived data.

## ⚙️ Configuration

All configuration via environment variables (12-factor app). See [`.env.example`](.env.example) for complete reference.

### Required Variables

```bash
# ez Share WiFi SD card base URL
EZSHARE_BASE_URL=http://192.168.4.1

# MQTT broker (required for Home Assistant)
MQTT_BROKER=localhost
MQTT_PORT=1883
MQTT_USER=mqtt_user
MQTT_PASSWORD=your_mqtt_password
```

### Optional Variables

```bash
# Device identification
CPAP_DEVICE_ID=resmed_airsense10
CPAP_DEVICE_NAME="ResMed AirSense 10"

# Collection interval (seconds)
BURST_INTERVAL=120  # Default: 2 minutes

# PostgreSQL (for historical storage)
DB_HOST=localhost
DB_NAME=cpap_data
DB_USER=cpap_user
DB_PASSWORD=your_db_password

# Health check port
HEALTH_CHECK_PORT=8893
```

## Deployment

### Native Systemd (Recommended)

```bash
# Build
mkdir build && cd build && cmake .. && make -j$(nproc)

# Install
sudo cp hms_cpap /usr/local/bin/
sudo cp ../.env /etc/hms-cpap/.env  # Edit with your settings

# Service file: /etc/systemd/system/hms-cpap.service
```

```ini
[Unit]
Description=HMS-CPAP Data Collection Service
After=network.target postgresql.service emqx.service

[Service]
Type=simple
EnvironmentFile=/etc/hms-cpap/.env
ExecStart=/usr/local/bin/hms_cpap
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable hms-cpap
sudo systemctl start hms-cpap
```

### ARM Cross-Compilation (Raspberry Pi)

```bash
./build_arm.sh          # Cross-compile for ARM
./deploy_to_pi.sh       # Build + deploy + restart in one step
```

See [docs/CROSS_COMPILATION.md](docs/CROSS_COMPILATION.md) for sysroot setup.

### Docker (CI/GHCR)

Docker image available for CI and containerized deployments:

```bash
docker run -d --env-file .env -p 8893:8893 ghcr.io/hms-homelab/hms-cpap:latest
```

## 🏠 Home Assistant Integration

HMS-CPAP uses **MQTT Discovery** for automatic Home Assistant integration.

### 1. Configure MQTT in Home Assistant

`configuration.yaml`:
```yaml
mqtt:
  broker: localhost  # Your MQTT broker
  username: mqtt_user
  password: your_mqtt_password
  discovery: true  # Enable discovery
```

### 2. Restart Home Assistant

Sensors will auto-appear as a device:

**Device:** `ResMed AirSense 10`

**Sensors (34 total):**
- `sensor.cpap_ahi` - Apnea-Hypopnea Index
- `sensor.cpap_leak_rate` - Leak rate (L/min)
- `sensor.cpap_pressure_current` - Current pressure (cmH2O)
- `sensor.cpap_usage_hours` - Total usage hours
- `sensor.cpap_session_start` - Last session start time
- `sensor.cpap_central_apneas` - Central apnea events
- `sensor.cpap_obstructive_apneas` - Obstructive apnea events
- ... and 27 more metrics

### 3. Create Dashboard

Example Lovelace card:

```yaml
type: entities
title: CPAP Therapy
entities:
  - entity: sensor.cpap_ahi
    name: AHI (events/hour)
  - entity: sensor.cpap_leak_rate
    name: Leak Rate
  - entity: sensor.cpap_usage_hours
    name: Usage Tonight
  - entity: sensor.cpap_pressure_current
    name: Current Pressure
```

> 📖 **Full Guide:** See [docs/HOME_ASSISTANT.md](docs/HOME_ASSISTANT.md) for dashboard examples and automations

## Architecture

```
┌─────────────────┐
│  ResMed CPAP    │
│  AirSense 10    │
└────────┬────────┘
         │ SD Card (SPI bus)
         │
    ┌────┴────────────────┐
    │                     │
    ▼                     ▼
┌──────────┐    ┌──────────────────┐
│ ezShare  │    │ FYSETC SD WiFi   │
│ WiFi SD  │    │ Pro (ESP32)      │
│          │    │ - CS line PCNT   │
│ HTTP     │    │ - <10ms steals   │
│ polling  │    │ - MQTT push      │
└────┬─────┘    └────────┬─────────┘
     │ HTTP               │ MQTT
     ▼                    ▼
┌──────────────────────────────────┐
│          HMS-CPAP Service        │
│  BurstCollector | FysetcReceiver │
│  EDFParser + SessionDiscovery    │
│  DataPublisher + LLM Summary     │
└──────────┬──────────┬────────────┘
           │          │
    ┌──────┘          └──────┐
    ▼                        ▼
┌──────────┐       ┌──────────────┐
│PostgreSQL│       │ MQTT (EMQX)  │
│(history) │       │ 47 sensors   │
└──────────┘       └──────┬───────┘
                          │
                          ▼
                  ┌───────────────┐
                  │Home Assistant │
                  └───────────────┘
```

### Data Flow

**ezShare mode:** Poll HTTP every 65s -> download EDF deltas -> detect session stop (files unchanged across 2 cycles) -> parse -> publish

**FYSETC mode:** Receive BRP chunks in realtime during therapy -> post-therapy: manifest + STR from FYSETC -> diff & fetch missing files -> parse -> publish + LLM summary

### EDF File Types

| File | Content | During Therapy | After Mask-Off |
|------|---------|----------------|----------------|
| BRP.edf | Flow/pressure (25 Hz) | Grows every 60s | Final flush |
| PLD.edf | Pressure/leak (0.5 Hz) | Grows every 60s | Final flush |
| SAD.edf | SpO2/HR (1 Hz) | Grows every 60s | Final flush |
| EVE.edf | Apnea/hypopnea events | Updated live | Final flush |
| CSL.edf | Clinical summary | Created at start | Final flush |
| STR.edf | Daily therapy summary | N/A | Written ~50s after mask-off |

Multiple checkpoint files per session are grouped by `SESSION_GAP_MINUTES` (default 60).

## 🛠️ Development

### Build Requirements

- C++17 compiler (GCC 9+, Clang 10+)
- CMake 3.16+
- libcurl, libpq, libssl

### Build & Test

**Native x86 Build:**
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run tests
make test

# Run with verbose output
./hms_cpap
```

**ARM Cross-Compilation (for Pi Zero 2 W):**
```bash
# One-time setup: Install ARM toolchain
sudo apt-get install -y g++-arm-linux-gnueabihf sshpass

# Sync Pi libraries to sysroot (one-time, ~220MB)
mkdir -p sysroot/usr sysroot/lib/arm-linux-gnueabihf
rsync -avz aamat@192.168.2.73:/usr/include ./sysroot/usr/
rsync -avz aamat@192.168.2.73:/usr/lib ./sysroot/usr/
rsync -avz aamat@192.168.2.73:/lib/arm-linux-gnueabihf ./sysroot/lib/

# Build for ARM (40 seconds vs 5 minutes on Pi!)
./build_arm.sh

# Deploy to Pi and restart service
./deploy_to_pi.sh
```

**📖 Full documentation:** See [docs/CROSS_COMPILATION.md](docs/CROSS_COMPILATION.md) for detailed setup, troubleshooting, and architecture details.

### Project Structure

```
hms-cpap/
├── src/
│   ├── main.cpp                    # Entry point (ezshare/fysetc/local modes)
│   ├── clients/
│   │   └── EzShareClient.cpp       # WiFi SD HTTP client
│   ├── parsers/
│   │   └── EDFParser.cpp           # EDF file parsing (OSCAR algorithms)
│   ├── services/
│   │   ├── BurstCollectorService.cpp     # ezShare/local polling mode
│   │   ├── FysetcReceiverService.cpp     # FYSETC MQTT push mode
│   │   ├── SessionDiscoveryService.cpp   # Session grouping
│   │   └── DataPublisherService.cpp      # MQTT + DB publishing
│   ├── database/
│   │   └── DatabaseService.cpp     # PostgreSQL interface
│   └── mqtt/
│       └── DiscoveryPublisher.cpp  # HA MQTT auto-discovery
├── include/                        # Header files (mirrors src/)
├── tests/                          # 155 unit tests (GTest + GMock)
├── docs/                           # FYSETC_RECEIVER, RESMED_WRITE_TIMING, etc.
├── cmake/                          # ARM cross-compilation toolchain
├── scripts/                        # Utility scripts
└── CMakeLists.txt
```

### Running Tests

```bash
cd build && make -j$(nproc)
./tests/run_tests
```

**155 tests** across 14 test suites:
- EDF parsing (BRP, EVE, SAD, STR)
- Session discovery and grouping
- MQTT publishing and discovery
- Database operations
- FysetcReceiver (sync, chunk, manifest, base64)
- Configuration, models, integration

## ❓ FAQ

### Why not use existing CPAP data solutions?

Most solutions require cloud services, proprietary apps, or manual SD card removal. HMS-CPAP provides:
- 100% local, no cloud
- Automatic collection via WiFi
- Open-source algorithms (OSCAR)
- Home Assistant integration
- ML-ready PostgreSQL storage

### Does this work with other CPAP brands?

Currently optimized for **ResMed AirSense 10/11** (EDF format). Other ResMed models may work. Philips/Respironics use different formats and would need parser modifications.

### What about data privacy?

All data stays local:
- No cloud services
- No external API calls
- Your network only
- PostgreSQL encryption optional
- MQTT can use TLS

### Can I use this with Oscar?

Yes! HMS-CPAP uses OSCAR algorithms for parsing. You can:
- Import HMS-CPAP archives into OSCAR
- Run both simultaneously
- Cross-validate metrics

### Bridge setup seems complex. Is it worth it?

**Yes, if you value:**
- Stable WiFi (no network switching)
- Reliable collection (always connected)
- WAF (Wife Acceptance Factor - no disruptions)

**Direct connection works fine if:**
- Single user, dedicated device
- Manual collection acceptable
- Don't mind WiFi switching

## 📊 Metrics Reference

### Real-Time Metrics (updated every 2 minutes)

| Metric | Description | Unit |
|--------|-------------|------|
| AHI | Apnea-Hypopnea Index | events/hour |
| Leak Rate | Current leak rate | L/min |
| Pressure | Current therapy pressure | cmH2O |
| Tidal Volume | Breath volume | mL |
| Minute Ventilation | Respiratory rate × tidal volume | L/min |
| Respiratory Rate | Breaths per minute | breaths/min |

### Session Summary Metrics (after session ends)

| Metric | Description |
|--------|-------------|
| Session Duration | Total therapy time |
| Usage Hours | Hours with mask on |
| Total Apneas | Central + Obstructive + Hypopneas |
| 95th Percentile Leak | Peak leak rate |
| Median Pressure | Middle pressure value |
| Efficacy | Overall therapy effectiveness |

> 📖 **Complete List:** See [docs/METRICS.md](docs/METRICS.md)

## 🤝 Contributing

Contributions welcome! Please:

1. Fork repository
2. Create feature branch (`git checkout -b feature/amazing-feature`)
3. Add tests for new functionality
4. Ensure tests pass (`make test`)
5. Commit changes (`git commit -m 'Add amazing feature'`)
6. Push to branch (`git push origin feature/amazing-feature`)
7. Open Pull Request

### Code Style

- C++17 standard
- Google C++ Style Guide
- Include unit tests
- Document public APIs

## 📄 License

This project is licensed under the **MIT License** - see [LICENSE](LICENSE) file.

### Third-Party Components

- **OSCAR algorithms** - GPL-3.0 (EDF parsing logic)
- **libcurl** - MIT-style license
- **PostgreSQL libpq** - PostgreSQL License
- **Paho MQTT** - EPL 2.0

## 🙏 Acknowledgments

- [OSCAR Project](https://www.sleepfiles.com/OSCAR/) - EDF parsing algorithms
- [ResMed](https://www.resmed.com/) - CPAP hardware
- [ez Share](http://www.ezshare.com/) - WiFi SD card
- [Home Assistant](https://www.home-assistant.io/) - Smart home platform
- CPAP community on Reddit

## 📞 Support

- **Issues:** [GitHub Issues](https://github.com/hms-homelab/hms-cpap/issues)
- **Discussions:** [GitHub Discussions](https://github.com/hms-homelab/hms-cpap/discussions)
- **Reddit:** r/CPAP, r/homeassistant

---

**Made with ❤️ for better sleep and open health data**

*If this project helps you sleep better, consider starring ⭐ the repository!*
---

## ☕ Support

If this project is useful to you, consider buying me a coffee!

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/aamat09)
