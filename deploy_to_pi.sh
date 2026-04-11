#!/bin/bash
# HMS-CPAP Pi Deployment Script
# Cross-compiles for ARM, deploys to Pi, and restarts the service.
#
# Usage:
#   PI_HOST=user@192.168.1.50 PI_PASSWORD=mypass ./deploy_to_pi.sh
#
# Environment variables:
#   PI_HOST       - SSH target (default: from .env or PI_HOST env)
#   PI_PASSWORD   - SSH/sudo password (default: from .env or PI_PASSWORD env)
#   PI_BINARY_PATH - Remote binary path (default: /usr/local/bin/hms_cpap)
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
SERVICE_NAME="${SERVICE_NAME:-hms-cpap}"

echo "╔══════════════════════════════════════════════════════════╗"
echo "║         HMS-CPAP Pi Zero 2 W Deployment                 ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

# Step 1a: Build frontend
echo "📦 Step 1a: Building Angular frontend..."
(cd frontend && npm run build --quiet 2>&1 | tail -3)
echo "✅ Frontend built"
echo ""

# Step 1b: Build ARM binary
echo "📦 Step 1b: Building ARM binary..."
./build_arm.sh
if [ ! -f "build-arm/hms_cpap" ]; then
    echo "❌ ARM build failed!"
    exit 1
fi
echo "✅ ARM binary built successfully"
echo ""

# Step 2: Stop service on Pi
echo "🛑 Step 2: Stopping HMS-CPAP service on Pi..."
sshpass -p "$PI_PASSWORD" ssh -o StrictHostKeyChecking=no "$PI_HOST" \
    "echo '$PI_PASSWORD' | sudo -S systemctl stop $SERVICE_NAME"
echo "✅ Service stopped"
echo ""

# Step 3: Backup old binary
echo "💾 Step 3: Backing up old binary on Pi..."
sshpass -p "$PI_PASSWORD" ssh -o StrictHostKeyChecking=no "$PI_HOST" \
    "echo '$PI_PASSWORD' | sudo -S cp $PI_BINARY_PATH ${PI_BINARY_PATH}.backup 2>/dev/null || true"
echo "✅ Backup created"
echo ""

# Step 4: Deploy new binary
echo "🚀 Step 4: Deploying new binary to Pi..."
sshpass -p "$PI_PASSWORD" scp build-arm/hms_cpap "$PI_HOST:~/hms_cpap.new"
sshpass -p "$PI_PASSWORD" ssh -o StrictHostKeyChecking=no "$PI_HOST" \
    "echo '$PI_PASSWORD' | sudo -S mv ~/hms_cpap.new $PI_BINARY_PATH && \
     echo '$PI_PASSWORD' | sudo -S chmod +x $PI_BINARY_PATH"
echo "✅ Binary deployed"
echo ""

# Step 5: Deploy Web UI static files
UI_DIR="frontend/dist/frontend/browser"
if [ -d "$UI_DIR" ]; then
    echo "🌐 Step 5: Deploying Web UI..."
    sshpass -p "$PI_PASSWORD" ssh -o StrictHostKeyChecking=no "$PI_HOST" "mkdir -p ~/static/browser"
    sshpass -p "$PI_PASSWORD" rsync -az "$UI_DIR/" "$PI_HOST:~/static/browser/"
    echo "✅ Web UI deployed"
else
    echo "⏭️  Step 5: Web UI not found at $UI_DIR, skipping"
fi
echo ""

# Step 6: Restart service
echo "🔄 Step 6: Restarting HMS-CPAP service..."
sshpass -p "$PI_PASSWORD" ssh -o StrictHostKeyChecking=no "$PI_HOST" \
    "echo '$PI_PASSWORD' | sudo -S systemctl start $SERVICE_NAME"
sleep 3
echo "✅ Service restarted"
echo ""

# Step 7: Check service status
echo "📊 Step 7: Checking service status..."
echo ""
sshpass -p "$PI_PASSWORD" ssh -o StrictHostKeyChecking=no "$PI_HOST" \
    "systemctl status $SERVICE_NAME --no-pager | head -20"
echo ""

# Step 8: Show recent logs
echo "📋 Recent logs:"
echo "───────────────────────────────────────────────────────────"
sshpass -p "$PI_PASSWORD" ssh -o StrictHostKeyChecking=no "$PI_HOST" \
    "journalctl -u $SERVICE_NAME --since '10 seconds ago' --no-pager -n 15"
echo "───────────────────────────────────────────────────────────"
echo ""

echo "✅ Deployment complete!"
echo ""
echo "📝 To view logs: ssh $PI_HOST 'journalctl -u $SERVICE_NAME -f'"
echo "📝 To check status: ssh $PI_HOST 'systemctl status $SERVICE_NAME'"
