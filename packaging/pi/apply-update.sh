#!/bin/bash
# SDD-041 D10: install a CpapDash update on a Pi, as root, safely.
#
# Run by hms-cpap-update.service when the service drops
# ~/.hms-cpap/update/request. That folder belongs to the service user, so
# NOTHING in it is trusted:
#   - the only thing read from the request is a version, reduced to digits and dots;
#   - the release's manifest.json and zip are downloaded HERE, from GitHub over
#     HTTPS, and the zip must match the manifest's size and SHA-256;
#   - nothing from the zip is ever executed as root. Its install.sh is not run;
#     the binary and the UI are copied, and the new binary is only ever run as
#     the service user;
#   - every file operation inside the user's home runs AS that user, so a
#     symlink planted there cannot turn a root write or delete against the
#     system.
#
# The steps and their names match the desktop helpers: verify, install,
# preflight, start, health. The result goes to ~/.hms-cpap/update/result.json,
# which the service shows in Settings.
#
#   apply-update.sh <user> <home>
set -u

USER_NAME="${1:?user}"
HOME_DIR="${2:?home}"
REPO="${HMS_CPAP_UPDATE_REPO:-hms-homelab/hms-cpap}"
BASE_URL="${HMS_CPAP_UPDATE_BASE:-https://github.com/$REPO/releases/download}"
UPD="$HOME_DIR/.hms-cpap/update"
BIN=/usr/local/bin/hms_cpap
STATIC="$HOME_DIR/static/browser"
LOG=/var/log/hms-cpap-update.log
VERSION=""
WORK=""

exec >>"$LOG" 2>&1
echo "=== $(date -u +%Y-%m-%dT%H:%M:%SZ) apply-update.sh for $USER_NAME"

as_user() { runuser -u "$USER_NAME" -- "$@"; }

result() {   # ok step message
    local json
    json="$(printf '{"ok":%s,"version":"%s","step":"%s","message":"%s","at":"%s"}' \
        "$1" "$VERSION" "$2" "$(printf '%s' "$3" | tr -d '"\\')" "$(date -u +%Y-%m-%dT%H:%M:%SZ)")"
    echo "$json" | as_user tee "$UPD/result.json" >/dev/null
    echo "result: ok=$1 step=$2 $3"
}

cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; }
trap cleanup EXIT

refuse() {   # step message: nothing has been touched
    result false "$1" "$2"
    exit 1
}

roll_back() {   # step message: the new version is in place and failed
    echo "rolling back: $2"
    systemctl stop hms-cpap
    [ -f "$BIN.previous" ] && install -m 755 -o root -g root "$BIN.previous" "$BIN"
    if as_user test -d "$STATIC.previous"; then
        as_user rm -rf "$STATIC"
        as_user mv "$STATIC.previous" "$STATIC"
    fi
    systemctl start hms-cpap
    result false "$1" "$2"
    exit 1
}

# ── The request: a version, nothing else ────────────────────────────────────
# Read and removed as the user. Removed FIRST, so a failure below cannot leave
# the path unit firing again and again.
REQUEST="$(as_user head -c 32 "$UPD/request" 2>/dev/null | tr -dc '0-9.')"
as_user rm -f "$UPD/request"
VERSION="$REQUEST"
[[ "$VERSION" =~ ^[0-9]+(\.[0-9]+){1,3}$ ]] || refuse verify "the request names no version"

command -v python3 >/dev/null || refuse verify "python3 is needed to read the release manifest"
command -v unzip   >/dev/null || refuse verify "unzip is needed to open the release"

# ── Verify: fetched here, checked here ─────────────────────────────────────
WORK="$(mktemp -d /var/tmp/hms-cpap-update.XXXXXX)"
curl -fsSL -m 60 -o "$WORK/manifest.json" "$BASE_URL/v$VERSION/manifest.json" \
    || refuse verify "release v$VERSION has no manifest.json"
read -r NAME SIZE SHA < <(python3 - "$WORK/manifest.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
for a in m.get("assets", []):
    if a.get("platform") == "linux-armhf" and a.get("kind") == "zip":
        print(a["name"], a["size"], a["sha256"])
        break
PY
)
[ -n "${NAME:-}" ] || refuse verify "release v$VERSION has no Pi zip"
case "$NAME" in */*|..*) refuse verify "the manifest names an unexpected file" ;; esac

curl -fsSL --speed-limit 1024 --speed-time 60 -o "$WORK/$NAME" "$BASE_URL/v$VERSION/$NAME" \
    || refuse verify "the download of $NAME failed"
[ "$(stat -c %s "$WORK/$NAME")" = "$SIZE" ] || refuse verify "$NAME is the wrong size"
[ "$(sha256sum "$WORK/$NAME" | awk '{print $1}')" = "$SHA" ] \
    || refuse verify "$NAME does not match the manifest's SHA-256"

unzip -q "$WORK/$NAME" -d "$WORK/x" || refuse verify "$NAME would not unpack"
NEW_DIR="$(dirname "$(find "$WORK/x" -maxdepth 2 -name hms_cpap -type f | head -1)")"
[ -f "$NEW_DIR/hms_cpap" ] && [ -f "$NEW_DIR/static/browser/index.html" ] \
    || refuse verify "$NAME does not hold hms_cpap and the web UI"
# The service user copies the UI out of here, so it must be able to read it.
chmod -R a+rX "$WORK"

# ── Install: keep the previous binary and UI ────────────────────────────────
systemctl stop hms-cpap
cp -f "$BIN" "$BIN.previous"
install -m 755 -o root -g root "$NEW_DIR/hms_cpap" "$BIN" || roll_back install "the new binary could not be installed"
as_user rm -rf "$STATIC.previous"
as_user test -d "$STATIC" && as_user mv "$STATIC" "$STATIC.previous"
as_user mkdir -p "$STATIC" && as_user cp -R "$NEW_DIR/static/browser/." "$STATIC/" \
    || roll_back install "the new web UI could not be copied"

# ── Preflight, as the user, never as root ───────────────────────────────────
as_user env HOME="$HOME_DIR" "$BIN" --preflight || roll_back preflight "the new version's configuration check failed"

# ── Start, and it has to answer as the NEW version ──────────────────────────
systemctl start hms-cpap || roll_back start "systemd could not start the new version"
PORT="$(as_user python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("web_port", 8893))' \
        "$HOME_DIR/.hms-cpap/config.json" 2>/dev/null || echo 8893)"
for _ in $(seq 1 120); do
    if curl -s -m 2 "http://127.0.0.1:$PORT/health" | grep -q "\"version\":\"$VERSION\""; then
        result true done "updated to $VERSION"
        exit 0
    fi
    sleep 1
done
roll_back health "the new version did not answer as $VERSION within two minutes"
