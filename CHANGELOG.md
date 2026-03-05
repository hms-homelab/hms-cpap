# Changelog

All notable changes to HMS-CPAP will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.1.6] - 2026-03-05

### Fixed
- **Historical metrics based on single BRP session instead of full night**: `publishHistoricalState()`
  was called with metrics from one BRP session only. Nights with multiple therapy periods (CPAP
  turned off and back on) showed partial duration and wrong AHI, because each BRP file creates its
  own session row. Fix: new `getNightlyMetrics()` aggregates all sessions in the same sleep day
  (noon-to-noon window), sums `duration_seconds` for total usage hours, uses `MAX` for event counts
  (all sessions share the same EVE file so events are identical), and recomputes AHI as
  `total_events / total_hours`. All historical MQTT metrics now reflect the full night's therapy.

## [1.1.5] - 2026-03-04

### Fixed
- **Historical MQTT never published on session completion**: When BRP files stopped changing
  (session complete), code called `publishSessionCompleted()` which only published
  `session_status=completed` and `session_active=OFF` — AHI, event counts, and all other
  historical metrics were never sent to Home Assistant. HA showed stale zeros for every session.
- Fix: on session completion, load `SessionMetrics` from DB via new `getSessionMetrics()` and
  call `publishHistoricalState(const SessionMetrics&)` before the status publish.

### Added
- `DatabaseService::getSessionMetrics()` — loads aggregated session metrics from
  `cpap_session_metrics` + `cpap_calculated_metrics` for MQTT republishing
- `DataPublisherService::publishHistoricalState(const SessionMetrics&)` — public overload
  that publishes historical MQTT topics directly from a metrics struct (no full CPAPSession needed)
- Unit tests: `SessionMetrics_DefaultValues_AreZero`, `SessionMetrics_PopulatedFromSession`,
  `PublishHistoricalState_PublishesAHIAndEvents`, `PublishHistoricalState_ZeroEvents_PublishesZeros`

## [1.1.4] - 2026-02-25

### Fixed
- **Docker build**: Added missing build dependencies (libjsoncpp-dev, libpqxx-dev, Paho MQTT C/C++ from source)
- **libpqxx 6.x compatibility**: Replaced `conn_->close()` with `conn_.reset()` in DatabaseService (close() is protected in libpqxx 6.x)

### Added
- `.dockerignore` to exclude sysroot (220 MB), build dirs, and unnecessary files from Docker context
- `curl` in runtime image for health check endpoint

### Changed
- Restored native CMakeLists.txt (was accidentally overwritten with ARM cross-compilation config)
- Docker image reduced to 99 MB with proper dependency management

## [1.1.3] - 2026-02-21

### Fixed
- **Event Metrics Publishing**: Fixed obstructive apneas, central apneas, and hypopneas not being published to Home Assistant
- **Database Schema**: Added missing event count columns to `cpap_session_metrics` table (obstructive_apneas, central_apneas, hypopneas, reras, clear_airway_apneas)
- **Data Integrity**: Event counts now correctly stored and retrieved from PostgreSQL database

### Added
- Unit tests for event counting in calculateMetrics() function
- Database migration script for backfilling event counts from cpap_events table
- Tests for mixed event types, high AHI scenarios, and zero-event sessions

### Changed
- Updated DatabaseService.insertSessionMetrics() to include all event type columns
- Improved SessionMetrics structure documentation

## [1.1.2] - 2026-02-15

### Fixed
- **MQTT auto-reconnection**: Fixed `connected_` flag not updating after successful auto-reconnect, causing publish failures even when connection was restored
- **Home Assistant integration**: Added subscription to `homeassistant/status` topic to automatically republish discovery messages when Home Assistant restarts
- **Connection reliability**: Improved resilience to network interruptions and broker restarts

### Added
- Unit tests for MQTT reconnection behavior (`tests/mqtt/test_MqttClient.cpp`)
- Unit tests for Home Assistant status subscription (`tests/services/test_DataPublisherService.cpp`)

## [1.1.1] - 2026-02-13

### Added
- Pi Zero 2 W support and optimization
- Improved session grouping logic
- Health check HTTP endpoint
- 52 unit tests with 95% coverage

### Changed
- Migrated from hardcoded config to environment variables
- Optimized memory footprint (6.5 MB)
- Improved EDF parsing performance

### Fixed
- Session boundary detection accuracy
- File locking race conditions
- MQTT reconnection stability

## [1.1.0] - 2026-02-10

### Added
- Session discovery service
- PostgreSQL archival with file path tracking
- 34 comprehensive metrics (real-time + summary)
- Active file writing detection
- Burst collection mode

### Changed
- Refactored service architecture
- Improved error handling
- Enhanced logging

## [1.0.0] - 2026-02-03

### Added
- Initial C++ implementation
- ez Share WiFi SD client
- EDF file parser (OSCAR algorithms)
- MQTT publishing
- Home Assistant auto-discovery
- PostgreSQL storage
- Basic session grouping

### Features
- ResMed AirSense 10/11 support
- Bridge and direct WiFi modes
- Configurable collection intervals
- Systemd service integration

---

## Version History Summary

- **1.1.4** - Docker build fixes, libpqxx 6.x compatibility
- **1.1.3** - Event metrics publishing fixes
- **1.1.2** - MQTT reconnection fixes, Home Assistant status subscription
- **1.1.1** - Pi Zero 2 W optimization, comprehensive testing
- **1.1.0** - Session discovery, archival, 34 metrics
- **1.0.0** - Initial release with core functionality

[Unreleased]: https://github.com/hms-homelab/hms-cpap/compare/v1.1.4...HEAD
[1.1.4]: https://github.com/hms-homelab/hms-cpap/compare/v1.1.3...v1.1.4
[1.1.3]: https://github.com/hms-homelab/hms-cpap/compare/v1.1.2...v1.1.3
[1.1.2]: https://github.com/hms-homelab/hms-cpap/compare/v1.1.1...v1.1.2
[1.1.1]: https://github.com/aamat09/hms-cpap/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/aamat09/hms-cpap/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/aamat09/hms-cpap/releases/tag/v1.0.0
