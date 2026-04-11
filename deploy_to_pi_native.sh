#!/bin/bash
# HMS-CPAP Pi Deployment Script (Native Build)
# Pushes code to Pi, builds natively, and restarts the service.
# Use this instead of deploy_to_pi.sh when cross-compilation produces
# bad binaries (e.g. the getMetricsForDateRange SEGV).
#
# Usage:
#   PI_HOST=user@192.168.1.50 PI_PASSWORD=mypass ./deploy_to_pi_native.sh
#
# Environment variables:
#   PI_HOST       - SSH target (default: from .env or PI_HOST env)
#   PI_PASSWORD   - SSH/sudo password (default: from .env or PI_PASSWORD env)
#   PI_BINARY_PATH - Remote binary path (default: /usr/local/bin/hms_cpap)
#   PI_REPO_DIR   - Repo path on Pi (default: $HOME/hms-cpap)
#   SERVICE_NAME  - Systemd service name (default: hms-cpap)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Load .env if present (for PI_HOST / PI_PASSWORD)
if [ -f "$SCRIPT_DIR/.env" ]; then
    # shellcheck disable=SC1091
    source <(grep -E '^PI_(HOST|PASSWORD)=' "$SCRIPT_DIR/.env")
fi

PI_HOST="${PI_HOST:?Set PI_HOST (e.g. PI_HOST=user@192.168.1.50)}"
PI_PASSWORD="${PI_PASSWORD:?Set PI_PASSWORD}"
PI_BINARY_PATH="${PI_BINARY_PATH:-/usr/local/bin/hms_cpap}"
PI_REPO_DIR="${PI_REPO_DIR:-\$HOME/hms-cpap}"
SERVICE_NAME="${SERVICE_NAME:-hms-cpap}"

SSH="sshpass -p $PI_PASSWORD ssh -o StrictHostKeyChecking=no $PI_HOST"

echo "╔══════════════════════════════════════════════════════════╗"
echo "║     HMS-CPAP Pi Zero 2 W Deployment (Native Build)      ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

# Step 1: Push local changes
echo "Step 1: Pushing to remote..."
git push
echo ""

# Step 2: Pull on Pi and build natively
echo "Step 2: Building natively on Pi (this takes ~2 min)..."
$SSH "cd $PI_REPO_DIR && git pull && \
      mkdir -p build-native && cd build-native && \
      cmake .. -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3 && \
      make -j2 2>&1 | tail -5"
echo ""

# Step 3: Stop service
echo "Step 3: Stopping service..."
$SSH "echo '$PI_PASSWORD' | sudo -S systemctl stop $SERVICE_NAME"
echo ""

# Step 4: Deploy
echo "Step 4: Deploying native binary..."
$SSH "echo '$PI_PASSWORD' | sudo -S cp $PI_BINARY_PATH ${PI_BINARY_PATH}.backup 2>/dev/null || true; \
      echo '$PI_PASSWORD' | sudo -S cp $PI_REPO_DIR/build-native/hms_cpap $PI_BINARY_PATH && \
      echo '$PI_PASSWORD' | sudo -S chmod +x $PI_BINARY_PATH"
echo ""

# Step 5: Deploy Web UI
UI_DIR="frontend/dist/frontend/browser"
if [ -d "$UI_DIR" ]; then
    echo "Step 5: Deploying Web UI..."
    $SSH "mkdir -p ~/static/browser"
    sshpass -p "$PI_PASSWORD" rsync -az "$UI_DIR/" "$PI_HOST:~/static/browser/"
else
    echo "Step 5: Web UI not found, skipping"
fi
echo ""

# Step 6: Restart
echo "Step 6: Restarting service..."
$SSH "echo '$PI_PASSWORD' | sudo -S systemctl start $SERVICE_NAME"
sleep 3
echo ""

# Step 7: Show logs
echo "Recent logs:"
echo "---"
$SSH "journalctl -u $SERVICE_NAME --since '10 seconds ago' --no-pager -n 15"
echo "---"
echo ""
echo "Deployment complete!"
echo ""
echo "To view logs: ssh $PI_HOST 'journalctl -u $SERVICE_NAME -f'"
