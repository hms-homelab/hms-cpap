#!/bin/bash
# HMS-CPAP Pi Deployment Script
# Builds for ARM, deploys to Pi, and restarts the service

set -e

# Configuration
PI_HOST="aamat@192.168.2.73"
PI_PASSWORD="exploracion"
PI_BINARY_PATH="/usr/local/bin/hms_cpap"
SERVICE_NAME="hms-cpap"

echo "╔══════════════════════════════════════════════════════════╗"
echo "║         HMS-CPAP Pi Zero 2 W Deployment                 ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

# Step 1: Build ARM binary
echo "📦 Step 1: Building ARM binary..."
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

# Step 5: Restart service
echo "🔄 Step 5: Restarting HMS-CPAP service..."
sshpass -p "$PI_PASSWORD" ssh -o StrictHostKeyChecking=no "$PI_HOST" \
    "echo '$PI_PASSWORD' | sudo -S systemctl start $SERVICE_NAME"
sleep 3
echo "✅ Service restarted"
echo ""

# Step 6: Check service status
echo "📊 Step 6: Checking service status..."
echo ""
sshpass -p "$PI_PASSWORD" ssh -o StrictHostKeyChecking=no "$PI_HOST" \
    "systemctl status $SERVICE_NAME --no-pager | head -20"
echo ""

# Step 7: Show recent logs
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
