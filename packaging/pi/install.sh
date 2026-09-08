#!/bin/bash
# CpapDash (hms-cpap) on a Raspberry Pi, 32-bit Raspberry Pi OS (trixie).
#
# SDD-025. Installs the runtime libraries, the service binary, the web UI and
# a systemd unit that runs the service as the user who invoked sudo. Running it
# again on a newer zip is the upgrade: it replaces the binary and the UI and
# restarts. It never touches ~/.hms-cpap, where the config, the database and
# the card archive live.
#
#   sudo ./install.sh            install or upgrade for the invoking user
#   sudo ./install.sh --user bob install or upgrade for bob
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN_DST=/usr/local/bin/hms_cpap
UNIT_DST=/etc/systemd/system/hms-cpap.service

# The runtime list is the Dockerfile's final stage, package for package. A
# test in the suite compares the two, so a library added there has to be added
# here or the build fails.
# runtime-packages-begin
RUNTIME_PACKAGES="
ca-certificates
curl
libcurl4t64
libpq5
libpqxx-7.10
libssl3t64
libjsoncpp26
libpaho-mqtt1.3
libpaho-mqttpp3-1
libspdlog1.15
libfmt10
libsqlite3-0
libmariadb3
libdrogon1t64
libtrantor1
libhpdf-2.3.0
"
# runtime-packages-end

die() { echo "install.sh: $*" >&2; exit 1; }

# ── Who this is for ──────────────────────────────────────────────────────────
SERVICE_USER="${SUDO_USER:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --user) SERVICE_USER="${2:-}"; shift 2 ;;
        *) die "unknown argument: $1" ;;
    esac
done
[ "$(id -u)" -eq 0 ] || die "run it with sudo; /usr/local/bin and /etc/systemd need root"
[ -n "$SERVICE_USER" ] || die "no user to install for: run it with sudo from that user, or pass --user NAME"
[ "$SERVICE_USER" != root ] || die "the service does not run as root; pass --user NAME"
SERVICE_HOME="$(getent passwd "$SERVICE_USER" | cut -d: -f6)"
[ -n "$SERVICE_HOME" ] && [ -d "$SERVICE_HOME" ] || die "user $SERVICE_USER has no home directory"

# ── What this is built for ───────────────────────────────────────────────────
# The binary is linked against trixie's libraries (Drogon is not packaged on
# bookworm), and it is a 32-bit ARM executable. Say so before touching anything.
ARCH="$(dpkg --print-architecture 2>/dev/null || true)"
[ "$ARCH" = armhf ] || die "this zip is for armhf (32-bit Raspberry Pi OS); this system is '${ARCH:-unknown}'. A 64-bit OS runs the Docker image instead."
CODENAME="$(. /etc/os-release 2>/dev/null && echo "${VERSION_CODENAME:-}")"
[ "$CODENAME" = trixie ] || die "this zip is built on Debian trixie; this system is '${CODENAME:-unknown}'"
[ -f "$HERE/hms_cpap" ] || die "hms_cpap is not beside this script; unzip the whole release first"
[ -f "$HERE/static/browser/index.html" ] || die "static/browser is not beside this script; unzip the whole release first"
[ -f "$HERE/hms-cpap.service" ] || die "hms-cpap.service is not beside this script; unzip the whole release first"

echo "Installing CpapDash for $SERVICE_USER ($SERVICE_HOME)"

# ── 1. Runtime libraries ─────────────────────────────────────────────────────
echo "Installing runtime libraries..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# shellcheck disable=SC2086
apt-get install -y -qq --no-install-recommends $RUNTIME_PACKAGES

# ── 2. The service binary ────────────────────────────────────────────────────
if [ -f "$BIN_DST" ]; then
    cp -f "$BIN_DST" "$BIN_DST.previous"
    echo "Kept the previous binary as $BIN_DST.previous"
fi
install -m 755 -o root -g root "$HERE/hms_cpap" "$BIN_DST"

# ── 3. The web UI ────────────────────────────────────────────────────────────
STATIC_DST="$SERVICE_HOME/static/browser"
rm -rf "$STATIC_DST"
mkdir -p "$STATIC_DST"
cp -R "$HERE/static/browser/." "$STATIC_DST/"
chown -R "$SERVICE_USER:" "$SERVICE_HOME/static"

# ── 4. The unit ──────────────────────────────────────────────────────────────
sed -e "s|__USER__|$SERVICE_USER|g" -e "s|__HOME__|$SERVICE_HOME|g" \
    "$HERE/hms-cpap.service" > "$UNIT_DST"
chmod 644 "$UNIT_DST"
systemctl daemon-reload

# ── 5. Start, or restart if it was already running ───────────────────────────
if systemctl is-enabled --quiet hms-cpap 2>/dev/null; then
    systemctl restart hms-cpap
    echo "Restarted hms-cpap"
else
    systemctl enable --now hms-cpap
    echo "Enabled and started hms-cpap"
fi

# ── 6. Prove it answers ──────────────────────────────────────────────────────
PORT=8893
CONFIG="$SERVICE_HOME/.hms-cpap/config.json"
if [ -f "$CONFIG" ] && command -v python3 >/dev/null; then
    PORT="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('web_port', 8893))" "$CONFIG" 2>/dev/null || echo 8893)"
fi
HEALTH=""
for _ in $(seq 1 30); do
    HEALTH="$(curl -s -m 2 "http://127.0.0.1:$PORT/health" 2>/dev/null || true)"
    [ -n "$HEALTH" ] && break
    sleep 1
done
if [ -z "$HEALTH" ]; then
    echo "The service was started but /health did not answer within 30 seconds." >&2
    echo "Look at: journalctl -u hms-cpap -n 50" >&2
    exit 1
fi

IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
echo
echo "CpapDash is running: http://${IP:-$(hostname)}:$PORT"
case "$HEALTH" in
    *'"setup_complete":true'*)  echo "Setup is complete." ;;
    *)                          echo "Open that address to finish setup." ;;
esac
