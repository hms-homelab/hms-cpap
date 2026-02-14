# Contributing to HMS-CPAP

Thank you for your interest in contributing to HMS-CPAP! This document provides guidelines and instructions for contributing.

## Code of Conduct

Be respectful, inclusive, and constructive. We're all here to improve sleep health and open health data.

## How to Contribute

### Reporting Bugs

1. Check if the bug is already reported in [Issues](https://github.com/aamat09/hms-cpap/issues)
2. If not, create a new issue with:
   - Clear, descriptive title
   - Steps to reproduce
   - Expected vs actual behavior
   - Environment details (OS, Docker version, etc.)
   - Logs (sanitize sensitive data!)

### Suggesting Features

1. Check [Discussions](https://github.com/aamat09/hms-cpap/discussions) first
2. Open a new discussion describing:
   - Use case and motivation
   - Proposed solution
   - Alternative approaches considered

### Pull Requests

#### Before Starting

1. Fork the repository
2. Create a feature branch from `main`
3. Discuss significant changes in an issue first

#### Development Process

1. **Set up development environment:**
   ```bash
   git clone https://github.com/YOUR_USERNAME/hms-cpap.git
   cd hms-cpap
   mkdir build && cd build
   cmake -DCMAKE_BUILD_TYPE=Debug ..
   make -j$(nproc)
   ```

2. **Make changes:**
   - Follow existing code style
   - Add unit tests for new functionality
   - Update documentation if needed

3. **Test your changes:**
   ```bash
   cd build
   make test
   ```

4. **Commit:**
   - Use clear, descriptive commit messages
   - Reference issues (e.g., "Fixes #123")
   - Keep commits focused and atomic

5. **Push and create PR:**
   ```bash
   git push origin feature/your-feature-name
   ```
   - Open PR with description of changes
   - Link related issues
   - Wait for CI checks to pass

#### Code Style

- **Language:** C++17 standard
- **Formatting:** Google C++ Style Guide
- **Naming:**
  - Classes: `PascalCase`
  - Functions: `camelCase`
  - Variables: `snake_case`
  - Constants: `UPPER_SNAKE_CASE`
- **Comments:** Document public APIs with Doxygen-style comments

#### Testing Requirements

- New features must include unit tests
- Aim for >90% code coverage
- Tests must pass on all platforms (x86_64, ARM64)

#### Documentation

Update documentation for:
- New configuration variables → `.env.example`
- New features → `README.md`
- API changes → Header comments
- Breaking changes → `CHANGELOG.md`

### Areas Needing Help

Current priorities:

1. **Hardware Support:**
   - Philips/Respironics parser
   - Other CPAP brands
   - Alternative WiFi SD cards

2. **Features:**
   - Machine learning models for event prediction
   - Advanced data visualization
   - Mobile app integration
   - HACS integration

3. **Documentation:**
   - Video tutorials
   - Hardware setup photos
   - Dashboard examples
   - Non-English translations

4. **Testing:**
   - Integration tests
   - Real hardware testing
   - Performance benchmarks

## Development Guidelines

### File Organization

```
src/
├── clients/        # External service clients
├── parsers/        # Data parsing logic
├── services/       # Business logic
├── database/       # Database interface
├── mqtt/           # MQTT communication
└── utils/          # Utility functions
```

### Adding New Features

1. **Parser for new CPAP brand:**
   - Add parser in `src/parsers/`
   - Implement `IParser` interface
   - Add unit tests
   - Document data format

2. **New MQTT sensor:**
   - Update `DiscoveryPublisher.cpp`
   - Add sensor config in `publishDiscovery()`
   - Update `README.md` metrics table

3. **New database table:**
   - Add migration in `database/migrations/`
   - Update `DatabaseService.cpp`
   - Document schema

### Security Considerations

- Never commit credentials or secrets
- Sanitize user input
- Validate external data (EDF files)
- Use prepared statements for SQL
- Document security implications

## Release Process

(For maintainers)

1. Update `VERSION` file
2. Update `CHANGELOG.md`
3. Tag release: `git tag -a v1.2.0 -m "Release v1.2.0"`
4. Push: `git push origin v1.2.0`
5. GitHub Actions builds and publishes Docker image
6. Create GitHub Release with changelog

## Questions?

- **General questions:** [GitHub Discussions](https://github.com/aamat09/hms-cpap/discussions)
- **Bug reports:** [GitHub Issues](https://github.com/aamat09/hms-cpap/issues)
- **Security issues:** Email directly (see README)

## License

By contributing, you agree that your contributions will be licensed under the MIT License.

---

Thank you for helping improve HMS-CPAP! 🫁
