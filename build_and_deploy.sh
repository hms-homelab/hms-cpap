#!/bin/bash
# HMS-CPAP Build & Deploy
# Builds frontend (Angular) + backend (C++), runs tests, optionally deploys.
#
# Usage:
#   ./build_and_deploy.sh              # Build + test only
#   ./build_and_deploy.sh --deploy     # Build + test + deploy to systemd
#   ./build_and_deploy.sh --skip-fe    # Skip frontend build (backend only)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
FE_DIR="$SCRIPT_DIR/frontend"
FE_DIST="$FE_DIR/dist/frontend/browser"

DEPLOY=false
SKIP_FE=false

for arg in "$@"; do
    case "$arg" in
        --deploy)  DEPLOY=true ;;
        --skip-fe) SKIP_FE=true ;;
        --help|-h)
            echo "Usage: $0 [--deploy] [--skip-fe]"
            echo "  --deploy   Install binary and restart hms-cpap systemd service"
            echo "  --skip-fe  Skip Angular frontend build (backend only)"
            exit 0
            ;;
    esac
done

echo "HMS-CPAP Build"
echo "=============="

# ── Frontend ─────────────────────────────────────────────────────────────────
if [ "$SKIP_FE" = false ]; then
    echo ""
    echo "[1/4] Building frontend..."

    if ! command -v node &>/dev/null; then
        echo "ERROR: Node.js not found. Install Node.js 22+ or use --skip-fe."
        exit 1
    fi

    cd "$FE_DIR"

    if [ ! -d node_modules ]; then
        echo "  Installing dependencies..."
        npm ci --silent
    fi

    npx ng build --configuration production 2>&1 | tail -3

    if [ ! -f "$FE_DIST/index.html" ]; then
        echo "ERROR: Frontend build failed (no index.html in $FE_DIST)"
        exit 1
    fi

    echo "  Frontend built -> $FE_DIST"
else
    echo ""
    echo "[1/4] Skipping frontend (--skip-fe)"
fi

# ── Backend ──────────────────────────────────────────────────────────────────
echo ""
echo "[2/4] Building backend..."

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_WITH_WEB=ON .. 2>&1 | tail -3
make -j$(nproc) 2>&1 | tail -5

if [ ! -f "$BUILD_DIR/hms_cpap" ]; then
    echo "ERROR: Backend build failed (no hms_cpap binary)"
    exit 1
fi

echo "  Backend built -> $BUILD_DIR/hms_cpap"

# ── Tests ────────────────────────────────────────────────────────────────────
echo ""
echo "[3/4] Running tests..."

RESULT=$("$BUILD_DIR/tests/run_tests" --gtest_brief=1 2>&1 | tail -3)
echo "$RESULT"

if echo "$RESULT" | grep -q "FAILED"; then
    echo ""
    echo "ERROR: Tests failed. Not deploying."
    exit 1
fi

# ── Deploy ───────────────────────────────────────────────────────────────────
if [ "$DEPLOY" = true ]; then
    echo ""
    echo "[4/4] Deploying..."

    if ! systemctl is-active --quiet hms-cpap 2>/dev/null; then
        echo "  hms-cpap service not running, installing binary only."
        sudo cp "$BUILD_DIR/hms_cpap" /usr/local/bin/
        echo "  Installed to /usr/local/bin/hms_cpap"
    else
        sudo systemctl stop hms-cpap
        sudo cp "$BUILD_DIR/hms_cpap" /usr/local/bin/
        sudo systemctl start hms-cpap
        echo "  Deployed and restarted hms-cpap service."
    fi
else
    echo ""
    echo "[4/4] Skipping deploy (use --deploy to install)"
fi

echo ""
echo "Done."
