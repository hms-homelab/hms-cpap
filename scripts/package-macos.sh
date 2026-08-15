#!/usr/bin/env bash
#
# Build CpapDash.app: one bundle holding the desktop supervisor, the service,
# the dashboard, and every library either of them needs.
#
# WHY THIS EXISTS
#
# The macOS release was a zip containing a bare hms_cpap binary, and that binary
# links twelve HOMEBREW dylibs -- drogon, trantor, libpqxx, libpq,
# mysql-client, libharu, spdlog, jsoncpp, openssl@3, fmt, sqlite and brotli.
# On the build machine those resolve. On a user's Mac they do not exist, so the
# download could not start at all. It was not that macOS shipped without a
# desktop shell; macOS shipped something nobody could run.
#
# So both binaries get their dependencies copied inside and their load paths
# rewritten to @executable_path/../Frameworks. After this the bundle needs
# nothing but macOS itself.
#
# Usage:  scripts/package-macos.sh <build-dir> [output-dir]
#   build-dir   a configured build tree containing hms_cpap and the supervisor
#   output-dir  where CpapDash.app is assembled (default: <build-dir>/package)
set -euo pipefail

BUILD_DIR="${1:?usage: package-macos.sh <build-dir> [output-dir]}"
OUT_DIR="${2:-$BUILD_DIR/package}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

APP="$OUT_DIR/CpapDash.app"
MACOS_DIR="$APP/Contents/MacOS"
FRAMEWORKS="$APP/Contents/Frameworks"
RESOURCES="$APP/Contents/Resources"

SUPERVISOR_SRC="$BUILD_DIR/desktop/qt/CpapDashDesktop.app"
SERVICE_SRC="$BUILD_DIR/hms_cpap"
FRONTEND_SRC="$REPO_ROOT/frontend/dist/frontend/browser"

for required in "$SUPERVISOR_SRC" "$SERVICE_SRC"; do
    [ -e "$required" ] || { echo "missing: $required" >&2; exit 1; }
done

echo "==> assembling $APP"
rm -rf "$APP"
mkdir -p "$OUT_DIR"

# Start from the supervisor's own bundle: it already has the Info.plist with
# LSUIElement, the icon, and Qt's layout. The service moves in beside it.
cp -R "$SUPERVISOR_SRC" "$APP"
mkdir -p "$FRAMEWORKS" "$RESOURCES"

cp "$SERVICE_SRC" "$MACOS_DIR/hms_cpap"
chmod +x "$MACOS_DIR/hms_cpap"

# The service resolves its web assets relative to its own executable
# (SetupService::resolveStaticDir), so static/browser sits next to the binary
# rather than in Resources where a Mac app would normally put it.
if [ -d "$FRONTEND_SRC" ]; then
    rm -rf "$MACOS_DIR/static"
    mkdir -p "$MACOS_DIR/static"
    cp -R "$FRONTEND_SRC" "$MACOS_DIR/static/browser"
    echo "==> dashboard bundled"
else
    echo "!! frontend not built at $FRONTEND_SRC -- the dashboard will 404" >&2
fi

# ---------------------------------------------------------------------------
# Qt, for the supervisor.
#
# macdeployqt understands frameworks, plugins and the platform plugin, which a
# generic dylib tool does not. Run it FIRST: it rewrites paths of its own that
# the service pass below must not disturb.
echo "==> bundling Qt"
QT_BIN="${QT_ROOT_DIR:-/opt/homebrew/opt/qt}/bin"
"$QT_BIN/macdeployqt" "$APP" -always-overwrite >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# Everything the SERVICE needs.
#
# Written out rather than delegated to dylibbundler, which ran for twenty-one
# minutes on this bundle without adding a single file: pointed at a tree that
# already contains Qt frameworks, it walks back into them and goes in circles.
#
# The job is small and bounded when stated properly: start from the service's
# own Homebrew dependencies, follow theirs, and stop when nothing new appears.
# The visited set is what makes it terminate -- a shared library graph has
# cycles, and any walk without one runs forever.
echo "==> bundling the service's libraries"

# macOS still ships bash 3.2, which has no mapfile and no associative arrays,
# and `set -u` treats an empty array as unbound. So the work list lives in
# files: portable, and it survives whatever bash the build machine has.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
: > "$WORK/seen"
: > "$WORK/queue"

# Where an @rpath reference actually resolves to.
#
# Not every dependency is an absolute path. Anything built from source rather
# than poured from a bottle -- paho MQTT here -- gets an install name like
# @rpath/libpaho-mqttpp3.1.dylib, which a scan for /opt/homebrew misses
# entirely. It is then absent from the bundle, and the hardened runtime refuses
# to fall back to the Homebrew copy because the Team IDs differ, so the service
# dies at launch with a signature error rather than a missing-file error.
resolve_rpath() {
    local ref="${1#@rpath/}" dir
    for dir in $(otool -l "$2" 2>/dev/null | awk '/LC_RPATH/{f=1} f&&/path /{print $2; f=0}') \
               /opt/homebrew/lib /usr/local/lib; do
        case "$dir" in @*) continue;; esac
        [ -f "$dir/$ref" ] && { echo "$dir/$ref"; return; }
    done
}

nonsystem_deps() {
    # The `|| true` is load-bearing. grep exits 1 when it matches nothing, and
    # under `set -o pipefail` that fails the pipeline -- so the script died the
    # first time it reached a library with no outside dependencies left, which
    # is the SUCCESS case. Finding nothing is the goal here, not an error.
    local raw
    raw=$(otool -L "$1" 2>/dev/null | tail -n +2 | awk '{print $1}' || true)

    { echo "$raw" | grep -E '^(/opt/homebrew|/usr/local/(opt|Cellar))' | grep '\.dylib'; } || true

    # @rpath entries, resolved to the file they actually load.
    # Every one of these greps needs `|| true` for the same reason as above:
    # under pipefail, "matched nothing" is an error status, and matching
    # nothing is the normal case here.
    echo "$raw" | grep '^@rpath/' 2>/dev/null | while IFS= read -r ref; do
        [ -n "$ref" ] || continue
        resolve_rpath "$ref" "$1"
    done || true
}

# Breadth-first over the dependency graph. `seen` is what makes it terminate:
# library graphs contain cycles, and a walk without a visited set runs forever
# -- which is exactly how dylibbundler spent twenty-one minutes here achieving
# nothing.
nonsystem_deps "$MACOS_DIR/hms_cpap" > "$WORK/queue"
while [ -s "$WORK/queue" ]; do
    lib=$(head -1 "$WORK/queue")
    sed -i '' '1d' "$WORK/queue" 2>/dev/null || sed -i '1d' "$WORK/queue"
    grep -Fxq "$lib" "$WORK/seen" && continue
    echo "$lib" >> "$WORK/seen"
    [ -f "$lib" ] || continue
    nonsystem_deps "$lib" | while IFS= read -r d; do
        grep -Fxq "$d" "$WORK/seen" || echo "$d" >> "$WORK/queue"
    done
done

COUNT=$(wc -l < "$WORK/seen" | tr -d ' ')
echo "   $COUNT libraries to bring in"

while IFS= read -r dep; do
    [ -f "$dep" ] || continue
    cp -f "$dep" "$FRAMEWORKS/$(basename "$dep")"
    chmod u+w "$FRAMEWORKS/$(basename "$dep")"
done < "$WORK/seen"

# Rewrite load commands so every reference points inside the bundle.
retarget() {
    # Absolute references.
    nonsystem_deps "$1" | while IFS= read -r dep; do
        install_name_tool -change "$dep" \
            "@executable_path/../Frameworks/$(basename "$dep")" "$1" 2>/dev/null || true
    done
    # ...and @rpath ones, which have to be changed by the exact string the load
    # command carries, not by the path they happen to resolve to today.
    otool -L "$1" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '^@rpath/' 2>/dev/null | \
    while IFS= read -r ref; do
        [ -f "$FRAMEWORKS/$(basename "$ref")" ] || continue
        install_name_tool -change "$ref" \
            "@executable_path/../Frameworks/$(basename "$ref")" "$1" 2>/dev/null || true
    done || true
    true
}

retarget "$MACOS_DIR/hms_cpap"
while IFS= read -r dep; do
    base=$(basename "$dep")
    [ -f "$FRAMEWORKS/$base" ] || continue
    install_name_tool -id "@executable_path/../Frameworks/$base" "$FRAMEWORKS/$base" 2>/dev/null || true
    retarget "$FRAMEWORKS/$base"
done < "$WORK/seen"

# ---------------------------------------------------------------------------
# Prove it. A bundle that still points at /opt/homebrew is the bug, not a
# warning, so this fails the build rather than printing advice nobody reads.
echo "==> checking nothing points outside the bundle"
LEAKS=0
while IFS= read -r bin; do
    refs=$(otool -L "$bin" 2>/dev/null | tail -n +2 | awk '{print $1}' \
           | grep -E '^(/opt/homebrew|/usr/local/(opt|Cellar))' || true)
    # An @rpath reference with nothing in Frameworks to satisfy it is just as
    # broken as an absolute one, and fails at launch rather than at build time.
    for r in $(otool -L "$bin" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '^@rpath/' 2>/dev/null || true); do
        [ -f "$FRAMEWORKS/$(basename "$r")" ] || refs="$refs
$r (unbundled @rpath)"
    done
    if [ -n "$refs" ]; then
        echo "   $bin"
        echo "$refs" | sed 's/^/      /'
        LEAKS=1
    fi
done < <(find "$APP" -type f \( -perm +111 -o -name '*.dylib' \) 2>/dev/null)

if [ "$LEAKS" -ne 0 ]; then
    echo "!! the bundle depends on libraries a user will not have" >&2
    exit 1
fi
echo "   ok self-contained"

SIZE=$(du -sh "$APP" | cut -f1)
echo "==> $APP ($SIZE)"
