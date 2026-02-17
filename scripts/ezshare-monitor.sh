#!/bin/bash
# ezshare-monitor.sh - Monitor ez Share WiFi connection and notify on disconnect
# Uses nmcli monitor (event-driven, no polling)

INTERFACE="wlan1"
CONNECTION_NAME="ezshare"

# Home Assistant notification config
HA_HOST="${HA_HOST:-192.168.2.7}"
HA_PORT="${HA_PORT:-8123}"
HA_TOKEN="${HA_TOKEN:-}"
NOTIFY_SERVICE="${NOTIFY_SERVICE:-notify.mobile_app_albins_iphone}"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

send_notification() {
    local title="$1"
    local message="$2"

    if [[ -z "$HA_TOKEN" ]]; then
        log "ERROR: HA_TOKEN not set, cannot send notification"
        return 1
    fi

    curl -s -X POST \
        -H "Authorization: Bearer $HA_TOKEN" \
        -H "Content-Type: application/json" \
        -d "{\"title\": \"$title\", \"message\": \"$message\"}" \
        "http://${HA_HOST}:${HA_PORT}/api/services/${NOTIFY_SERVICE//.//}" \
        > /dev/null 2>&1

    if [[ $? -eq 0 ]]; then
        log "Notification sent: $title"
    else
        log "ERROR: Failed to send notification"
    fi
}

# Check if interface exists
if ! ip link show "$INTERFACE" > /dev/null 2>&1; then
    log "ERROR: Interface $INTERFACE does not exist"
    exit 1
fi

log "Starting ez Share WiFi monitor on $INTERFACE"
log "Watching for connection: $CONNECTION_NAME"

# Track connection state
was_connected=false
if nmcli -t -f DEVICE,STATE device | grep -q "^${INTERFACE}:connected"; then
    was_connected=true
    log "Initial state: connected"
else
    log "Initial state: disconnected"
fi

# Monitor NetworkManager events (event-driven, blocks until event)
nmcli monitor | while read -r line; do
    # Check if this event is about our interface/connection
    if echo "$line" | grep -qiE "(${INTERFACE}|${CONNECTION_NAME})"; then
        log "Event: $line"

        # Check current state
        is_connected=false
        if nmcli -t -f DEVICE,STATE device | grep -q "^${INTERFACE}:connected"; then
            is_connected=true
        fi

        # Detect state change
        if $was_connected && ! $is_connected; then
            log "DISCONNECT detected on $INTERFACE"
            send_notification "ez Share Disconnected" "CPAP WiFi SD card connection lost at $(date '+%H:%M')"
        elif ! $was_connected && $is_connected; then
            log "CONNECT detected on $INTERFACE"
            send_notification "ez Share Connected" "CPAP WiFi SD card connected at $(date '+%H:%M')"
        fi

        was_connected=$is_connected
    fi
done
