#!/bin/bash
# HMS-CPAP ARM Cross-Compilation Build Script
# This script builds HMS-CPAP for Raspberry Pi Zero 2 W

set -e

echo "=== HMS-CPAP ARM Cross-Compilation ==="
echo ""

# Create build directory
BUILD_DIR="build-arm"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Temporarily use ARM CMakeLists
cp ../CMakeLists.txt.arm ../CMakeLists.txt.backup
cp ../CMakeLists.txt.arm ../CMakeLists.txt

# Configure
echo "Configuring ARM build..."
cmake -DBUILD_TESTS=OFF ..

# Restore original CMakeLists
mv ../CMakeLists.txt.backup ../CMakeLists.txt

# Build
echo ""
echo "Building for ARM..."
make -j$(nproc)

# Check result
if [ -f "hms_cpap" ]; then
    echo ""
    echo "✅ ARM build successful!"
    ls -lh hms_cpap
    file hms_cpap
else
    echo ""
    echo "❌ ARM build failed"
    exit 1
fi
