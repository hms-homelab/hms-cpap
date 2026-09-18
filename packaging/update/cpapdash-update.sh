#!/bin/bash
# SDD-041 D8/D9: swap the whole CpapDash.app for a verified new one, and put
# the old one back if the new one does not come up.
#
# Started by the supervisor (desktop/qt TrayShell::onUpdateRequested) from a
# COPY outside the bundle, because the bundle is what this replaces. By then the
# service has downloaded the DMG and checked it against the release manifest,
# and the supervisor has hashed it again. This script checks it a third time,
# because it is the one that acts.
#
#   cpapdash-update.sh --pending <pending.json> --app <CpapDash.app>
#                      --data-dir <dir> --pid <supervisor pid> --port <port>
#
# Every outcome is written to <data-dir>/update/result.json, which the service
# reads on its next start and Settings shows. The step names match the SDD:
# verify, install, preflight, start, health.
set -u

PENDING="" APP="" DATA_DIR="" SUP_PID="" PORT="8893"
while [ $# -gt 0 ]; do
    case "$1" in
        --pending)  PENDING="$2"; shift 2 ;;
        --app)      APP="$2"; shift 2 ;;
        --data-dir) DATA_DIR="$2"; shift 2 ;;
        --pid)      SUP_PID="$2"; shift 2 ;;
        --port)     PORT="$2"; shift 2 ;;
        *) shift ;;
    esac
done

# The Developer ID every release is signed with (Amat Solutions LLC). A bundle
# signed by anyone else is not ours, however it got here.
TEAM_ID="${CPAPDASH_UPDATE_TEAM_ID:-9JYJU98VQ3}"
UPDATE_DIR="$DATA_DIR/update"
RESULT="$UPDATE_DIR/result.json"
mkdir -p "$UPDATE_DIR"
exec >>"$UPDATE_DIR/update.log" 2>&1
echo "=== $(date -u +%Y-%m-%dT%H:%M:%SZ) cpapdash-update.sh"

# Not "*.previous.app": that would sit in /Applications as a second launchable
# CpapDash. Without the .app suffix Finder shows it as a plain folder.
VERSION="" MNT="" PREV="${APP}.previous"

# `open` starts the app through LaunchServices, which does NOT pass this
# script's environment on. A data dir other than the default has to be handed
# over explicitly, or the new version would start against ~/.hms-cpap.
launch_app() {
    if [ "$DATA_DIR" != "$HOME/.hms-cpap" ]; then
        open -n --env "HMS_CPAP_DATA_DIR=$DATA_DIR" "$APP"
    else
        open "$APP"
    fi
}

json_escape() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | tr '\n' ' '; }

result() {   # ok step message
    printf '{"ok":%s,"version":"%s","step":"%s","message":"%s","at":"%s"}\n' \
        "$1" "$(json_escape "$VERSION")" "$2" "$(json_escape "$3")" \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$RESULT"
    echo "result: ok=$1 step=$2 $3"
}

detach() { [ -n "$MNT" ] && hdiutil detach -quiet -force "$MNT" >/dev/null 2>&1; MNT=""; }

# Nothing has been touched yet: say why and leave the install as it is.
refuse() {   # step message
    detach
    result false "$1" "$2"
    launch_app 2>/dev/null
    rm -f "$PENDING"
    exit 1
}

# The new bundle is in place and failed: put the old one back and start it.
roll_back() {   # step message
    echo "rolling back: $2"
    pkill -f "$APP/Contents/MacOS/" 2>/dev/null
    sleep 2
    pkill -9 -f "$APP/Contents/MacOS/" 2>/dev/null
    if [ -d "$PREV" ]; then
        rm -rf "$APP"
        mv "$PREV" "$APP"
    fi
    result false "$1" "$2"
    launch_app 2>/dev/null
    rm -f "$PENDING"
    exit 1
}

[ -f "$PENDING" ] || { result false verify "no pending.json at $PENDING"; exit 1; }
[ -d "$APP" ]     || { result false verify "no application at $APP"; exit 1; }

field() { plutil -extract "$1" raw -o - "$PENDING" 2>/dev/null; }
VERSION="$(field version)"
FILE="$(field file)"
SHA="$(field sha256)"

# 1. Wait for the supervisor, and the service it ran, to be gone.
for _ in $(seq 1 120); do
    kill -0 "$SUP_PID" 2>/dev/null || break
    sleep 0.5
done
kill -0 "$SUP_PID" 2>/dev/null && refuse verify "the supervisor did not exit"
for _ in $(seq 1 60); do
    pgrep -f "$APP/Contents/MacOS/hms_cpap" >/dev/null || break
    sleep 0.5
done
pkill -9 -f "$APP/Contents/MacOS/hms_cpap" 2>/dev/null

# 2. Verify, again.
[ -f "$FILE" ] || refuse verify "the download is missing"
[ "$(shasum -a 256 "$FILE" | awk '{print $1}')" = "$SHA" ] \
    || refuse verify "the download does not match its checksum"

MNT="$(mktemp -d /tmp/cpapdash-update.XXXXXX)"
mounted=0
for _ in 1 2 3; do   # hdiutil answers "Resource busy" now and then; it passes
    if hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "$MNT" "$FILE" >/dev/null; then
        mounted=1; break
    fi
    sleep 3
done
[ "$mounted" = 1 ] || { MNT=""; refuse verify "the disk image would not open"; }

NEW="$MNT/CpapDash.app"
[ -d "$NEW" ] || refuse verify "the disk image holds no CpapDash.app"
codesign --verify --deep --strict "$NEW" || refuse verify "the new application's signature is not valid"
codesign -dv "$NEW" 2>&1 | grep -q "^TeamIdentifier=$TEAM_ID\$" \
    || refuse verify "the new application is not signed by CpapDash's developer"

# 3. Install: the old bundle becomes .previous, in one rename on one volume.
rm -rf "$PREV"
mv "$APP" "$PREV" || refuse install "the current application could not be moved aside"
if ! ditto "$NEW" "$APP"; then
    detach
    roll_back install "the new application could not be copied into place"
fi
detach

# 4. Preflight the new service against this machine's configuration.
export HMS_CPAP_DATA_DIR="$DATA_DIR"
if ! "$APP/Contents/MacOS/hms_cpap" --preflight; then
    roll_back preflight "the new version's configuration check failed"
fi

# 5. Start it the way a user would.
launch_app || roll_back start "the new application would not open"

# 6. It has to answer, and answer as the NEW version.
for _ in $(seq 1 120); do
    if curl -s -m 2 "http://127.0.0.1:$PORT/health" | grep -q "\"version\":\"$VERSION\""; then
        result true done "updated to $VERSION"
        rm -rf "$PREV" "$FILE" "$PENDING"
        rm -f "$0"
        exit 0
    fi
    sleep 1
done
roll_back health "the new version did not answer as $VERSION within two minutes"
