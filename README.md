# HMS-CPAP 🫁

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Docker](https://img.shields.io/docker/v/aamat09/hms-cpap?label=docker)](https://ghcr.io/aamat09/hms-cpap)
[![Build Status](https://github.com/aamat09/hms-cpap/workflows/Build/badge.svg)](https://github.com/aamat09/hms-cpap/actions)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-%23FFDD00.svg?logo=buy-me-a-coffee)](https://www.buymeacoffee.com/aamat09)

**Lightweight C++ microservice for ResMed CPAP data collection via ez Share WiFi SD card with Home Assistant integration.**

Automatically extracts sleep therapy data from your ResMed AirSense 10/11 CPAP machine using an ez Share WiFi SD card, parses the data using OSCAR algorithms, and publishes 34+ metrics to Home Assistant via MQTT discovery.

![Home Assistant Dashboard](docs/images/ha-dashboard-preview.png)

## ✨ Features

- 🔌 **Zero Network Disruption** - Bridge architecture eliminates WiFi switching
- 📊 **34+ Metrics** - AHI, leak rate, pressure, usage hours, events (real-time + session summary)
- 🏠 **Home Assistant Auto-Discovery** - Instant MQTT integration with sensor entities
- 🗄️ **PostgreSQL Storage** - Historical data for ML analysis and long-term trends
- 📁 **Session Grouping** - Intelligently combines BRP/PLD/SAD files into therapy sessions
- 🔍 **Active File Detection** - Waits for machine to finish writing before downloading
- 🪶 **Ultra-Lightweight** - 6.5 MB native, ~150 MB Docker image
- 🧪 **Well-Tested** - 52 unit tests with 95%+ coverage

## 📋 Table of Contents

- [Quick Start](#-quick-start)
- [Hardware Setup](#-hardware-setup)
- [Configuration](#-configuration)
- [Deployment Options](#-deployment-options)
- [Home Assistant Integration](#-home-assistant-integration)
- [Architecture](#-architecture)
- [Development](#-development)
- [FAQ](#-faq)
- [Contributing](#-contributing)

## 🚀 Quick Start

### Docker (Recommended)

```bash
# 1. Clone repository
git clone https://github.com/aamat09/hms-cpap.git
cd hms-cpap

# 2. Create configuration
cp .env.example .env
nano .env  # Edit with your settings

# 3. Start services
docker-compose up -d

# 4. Check health
curl http://localhost:8893/health
```

### Native Installation

```bash
# 1. Install dependencies (Debian/Ubuntu)
sudo apt-get install build-essential cmake libcurl4-openssl-dev libpq-dev

# 2. Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# 3. Configure environment
export EZSHARE_BASE_URL="http://192.168.4.1"
export MQTT_BROKER="localhost"
export DB_HOST="localhost"
# ... (see .env.example for all variables)

# 4. Run
./hms_cpap
```

## 🔧 Hardware Setup

### Option A: Direct Connection (Simple)

**Components:**
- ResMed AirSense 10/11 CPAP machine
- ez Share WiFi SD card (Class 10, 8GB+)

**Setup:**
1. Insert ez Share SD card into CPAP machine
2. Connect your device to ez Share WiFi AP (SSID: `ezShare_XXXXX`)
3. Set `EZSHARE_BASE_URL=http://192.168.4.1`

**Pros:** Simple, no extra hardware
**Cons:** Switches WiFi network, may lose internet connection

### Option B: Bridge Setup (Recommended)

**Components:**
- ResMed AirSense 10/11 CPAP machine
- ez Share WiFi SD card
- Raspberry Pi Zero 2 W
- USB WiFi dongle (dual WiFi)
- SD card extension ribbon cable (optional, for external placement)

**Setup:**
1. **Hardware:**
   - Connect Pi Zero 2 W with dual WiFi (built-in + USB dongle)
   - One WiFi interface → ez Share AP (192.168.4.1)
   - Other WiFi interface → Home network

2. **Bridge Configuration:**
   - Configure Pi as WiFi bridge/repeater
   - Access ez Share at `http://BRIDGE_IP:81`
   - Set `EZSHARE_BASE_URL=http://BRIDGE_IP:81`

3. **Optional:** Use SD extension ribbon to place ez Share adapter outside machine for better signal

**Pros:** Zero network disruption, stable connection, WAF-friendly
**Cons:** Requires additional hardware (~$30)

> 📖 **Detailed Guide:** See [docs/HARDWARE_SETUP.md](docs/HARDWARE_SETUP.md) for step-by-step instructions with images

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

## 🐳 Deployment Options

### Docker Compose (Full Stack)

Includes HMS-CPAP + PostgreSQL + Mosquitto MQTT:

```bash
docker-compose up -d
```

### Docker Only (Use Existing Services)

If you already have PostgreSQL and MQTT:

```bash
docker run -d \
  --name hms-cpap \
  --restart unless-stopped \
  -e EZSHARE_BASE_URL=http://192.168.1.100:81 \
  -e MQTT_BROKER=192.168.1.5 \
  -e DB_HOST=192.168.1.5 \
  -e DB_PASSWORD=your_password \
  -e MQTT_PASSWORD=your_password \
  -p 8893:8893 \
  -v cpap_archive:/data/cpap_archive \
  ghcr.io/aamat09/hms-cpap:latest
```

### Native Systemd Service

```bash
# Install binary
sudo cp build/hms_cpap /usr/local/bin/

# Create service file
sudo nano /etc/systemd/system/hms-cpap.service
```

**Service file:**
```ini
[Unit]
Description=HMS-CPAP Data Collection Service
After=network.target postgresql.service mosquitto.service

[Service]
Type=simple
User=cpap
Environment="EZSHARE_BASE_URL=http://192.168.1.100:81"
Environment="MQTT_BROKER=localhost"
Environment="DB_HOST=localhost"
EnvironmentFile=/etc/hms-cpap/.env
ExecStart=/usr/local/bin/hms_cpap
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
# Enable and start
sudo systemctl enable hms-cpap
sudo systemctl start hms-cpap
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

## 🏗️ Architecture

```
┌─────────────────┐
│  ResMed CPAP    │
│  AirSense 10    │
└────────┬────────┘
         │ SD Card
         ↓
┌─────────────────┐      ┌──────────────┐
│  ez Share WiFi  │◄────►│  Pi Zero 2W  │ (Optional Bridge)
│  SD Adapter     │      │  Dual WiFi   │
└────────┬────────┘      └──────┬───────┘
         │                      │
         ↓                      ↓
    ┌────────────────────────────────┐
    │        HMS-CPAP Service        │
    │   (EDF Parser + Session Mgmt)  │
    └─────────┬─────────┬────────────┘
              │         │
      ┌───────┘         └────────┐
      ↓                          ↓
┌──────────────┐       ┌─────────────────┐
│  PostgreSQL  │       │  MQTT Broker    │
│  (History)   │       │  (Real-time)    │
└──────────────┘       └────────┬────────┘
                                │
                                ↓
                       ┌─────────────────┐
                       │ Home Assistant  │
                       │  (Dashboard)    │
                       └─────────────────┘
```

### Data Flow

1. **Collection:** HMS-CPAP polls ez Share every 2 minutes
2. **Detection:** Checks if CPAP is actively writing files
3. **Download:** Fetches new/updated EDF files (BRP, PLD, SAD)
4. **Parsing:** Extracts therapy metrics using OSCAR algorithms
5. **Grouping:** Combines checkpoint files into therapy sessions
6. **Storage:** Saves to PostgreSQL + archives files
7. **Publishing:** Sends MQTT messages with sensor data
8. **Display:** Home Assistant shows real-time dashboard

### Session Grouping Logic

Multiple EDF files = ONE therapy session:
- **BRP files** - Breathing waveform data (every 5-15 min)
- **PLD files** - Pressure/leak data (every 5-15 min)
- **SAD files** - Session statistics (end of session)

HMS-CPAP groups these by timestamp proximity (±2 minutes) to create complete session records.

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
│   ├── main.cpp
│   ├── clients/
│   │   ├── EzShareClient.cpp      # WiFi SD HTTP client
│   │   └── WiFiSwitchClient.cpp   # Network management
│   ├── parsers/
│   │   └── EDFParser.cpp          # EDF file parsing (OSCAR)
│   ├── services/
│   │   ├── BurstCollectorService.cpp
│   │   ├── SessionDiscoveryService.cpp
│   │   └── DataPublisherService.cpp
│   ├── database/
│   │   └── DatabaseService.cpp    # PostgreSQL interface
│   └── mqtt/
│       ├── MqttClient.cpp         # MQTT communication
│       └── DiscoveryPublisher.cpp # HA auto-discovery
├── include/                       # Header files
├── tests/                         # Unit tests (52 tests)
├── docs/                          # Documentation
├── Dockerfile                     # Multi-stage build
├── docker-compose.yml             # Full stack
└── CMakeLists.txt
```

### Running Tests

```bash
cd build
./tests/run_tests
```

**Test Coverage:** 52 tests, 95%+ coverage across:
- EDF parsing
- Session grouping
- MQTT publishing
- Database operations
- Configuration management

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

- **Issues:** [GitHub Issues](https://github.com/aamat09/hms-cpap/issues)
- **Discussions:** [GitHub Discussions](https://github.com/aamat09/hms-cpap/discussions)
- **Reddit:** r/CPAP, r/homeassistant

---

**Made with ❤️ for better sleep and open health data**

*If this project helps you sleep better, consider starring ⭐ the repository!*
---

## ☕ Support

If this project is useful to you, consider buying me a coffee!

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/aamat09)
