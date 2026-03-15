#!/bin/bash
# HMS-CPAP Quick Start Script
# Builds and runs hms-cpap natively (no Docker)

set -e

echo "HMS-CPAP Quick Start"
echo "===================="
echo ""

# Check build dependencies
MISSING=""
for cmd in cmake g++ pkg-config; do
    if ! command -v "$cmd" &> /dev/null; then
        MISSING="$MISSING $cmd"
    fi
done
if [ -n "$MISSING" ]; then
    echo "Missing build tools:$MISSING"
    echo "Install: sudo apt-get install build-essential cmake pkg-config"
    exit 1
fi

# Check library dependencies
for lib in libcurl4-openssl-dev libpqxx-dev libpq-dev libssl-dev libspdlog-dev; do
    if ! dpkg -s "$lib" &>/dev/null 2>&1; then
        MISSING="$MISSING $lib"
    fi
done
if [ -n "$MISSING" ]; then
    echo "Missing libraries:$MISSING"
    echo "Install: sudo apt-get install$MISSING"
    exit 1
fi

echo "Dependencies OK"

# Create .env if needed
if [ ! -f .env ]; then
    echo ""
    echo "Creating .env from .env.example..."
    cp .env.example .env
    echo "Edit .env with your settings before running."
    echo ""
fi

# Build
echo ""
echo "Building..."
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. 2>&1 | tail -3
make -j$(nproc) 2>&1 | tail -5

# Run tests
echo ""
echo "Running tests..."
./tests/run_tests --gtest_brief=1 2>&1 | tail -3

echo ""
echo "Build complete: build/hms_cpap"
echo ""
echo "Run modes:"
echo "  CPAP_SOURCE=ezshare ./hms_cpap   # Poll ezShare WiFi SD (default)"
echo "  CPAP_SOURCE=local   ./hms_cpap   # Read from local directory"
echo "  CPAP_SOURCE=fysetc  ./hms_cpap   # Receive from FYSETC via MQTT"
echo ""
echo "See docs/FYSETC_RECEIVER.md for FYSETC setup."
