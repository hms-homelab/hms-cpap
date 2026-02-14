# Changelog

All notable changes to HMS-CPAP will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial public release
- Docker support with multi-stage builds
- GitHub Actions for automated builds
- Comprehensive documentation
- MIT License

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

- **1.1.1** - Pi Zero 2 W optimization, comprehensive testing
- **1.1.0** - Session discovery, archival, 34 metrics
- **1.0.0** - Initial release with core functionality

[Unreleased]: https://github.com/aamat09/hms-cpap/compare/v1.1.1...HEAD
[1.1.1]: https://github.com/aamat09/hms-cpap/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/aamat09/hms-cpap/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/aamat09/hms-cpap/releases/tag/v1.0.0
