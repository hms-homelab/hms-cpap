#!/bin/bash
# SDD-052: build the Pi zip's hms_cpap for 32-bit Raspberry Pi OS, the way the
# Pi's own libraries were built. Cross-compiled on x86 (no emulation for the
# build), against a sysroot of Raspberry Pi OS packages, with that sysroot's
# C++ headers (cmake/arm-toolchain.cmake says why).
#
# Run from the source root, as root, in debian:trixie on amd64:
#
#   packaging/pi/build-armhf.sh <out-dir>
#
# Writes <out-dir>/hms_cpap. WORK (default /tmp/armhf) holds the sysroot and the
# build; a sysroot with a .complete marker is reused, so CI can cache WORK/sysroot
# keyed on sysroot-packages.txt.
#
# Steps, each one failing the build:
#   1. the Raspberry Pi OS archive key, pinned by fingerprint
#   2. the sysroot: sysroot-packages.txt plus libc and libstdc++, extracted
#   3. the guard: the C++ config the build will compile with must have no
#      atomic shared_ptr lock policy, as the Pi's libstdc++ has none
#   4. the build, stripped
#   5. --preflight under qemu-arm against the sysroot's libraries
#   6. --reparse of a one-night card under qemu-arm with malloc checking: the
#      folder walk that crashed with mismatched headers must end without a
#      signal
set -euo pipefail

OUT="${1:?usage: build-armhf.sh <out-dir>}"
SRC="$(pwd)"
WORK="${WORK:-/tmp/armhf}"
SYSROOT="$WORK/sysroot"
ARCHIVE="http://raspbian.raspberrypi.com/raspbian/"
# The keyring as Raspberry Pi OS itself installs it (the package, not
# archive.raspbian.org's 2012 export, whose SHA-1 self-signature current apt
# rejects). Pinned three ways: the .deb, the keyring in it, the key.
KEY_DEB="pool/main/r/raspbian-archive-keyring/raspbian-archive-keyring_20120528.4_all.deb"
KEY_DEB_SHA256="eb2bc175ecfad128ece8222b42eefabd0a2846afd14f3af04364f4a047cbc88f"
KEY_GPG_SHA256="28830bdb4ccfc475ea764d731cfa31e06fe01cdc2448d4cf401f45a5709425c4"
KEY_FPR="A0DA38D0D76E8B5D638872819165938D90FDDD2E"
ARCH_FLAGS=(-march=armv6 -mfpu=vfp -mfloat-abi=hard -marm)

say() { echo "== $*"; }
die() { echo "build-armhf: $*" >&2; exit 1; }

[ -f "$SRC/packaging/pi/sysroot-packages.txt" ] || die "run from the source root"
mkdir -p "$WORK" "$OUT"

say "tools"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    g++-arm-linux-gnueabihf binutils-arm-linux-gnueabihf cmake make git \
    ca-certificates curl gnupg mmdebstrap qemu-user file pkg-config >/dev/null

say "1. the Raspberry Pi OS archive key"
curl -fsSL -o "$WORK/keyring.deb" "$ARCHIVE$KEY_DEB"
echo "$KEY_DEB_SHA256  $WORK/keyring.deb" | sha256sum -c --quiet - || die "keyring .deb checksum"
rm -rf "$WORK/keyring" && dpkg-deb -x "$WORK/keyring.deb" "$WORK/keyring"
cp "$WORK/keyring/usr/share/keyrings/raspbian-archive-keyring.gpg" "$WORK/raspbian.gpg"
echo "$KEY_GPG_SHA256  $WORK/raspbian.gpg" | sha256sum -c --quiet - || die "keyring checksum"
got="$(gpg --show-keys --with-colons "$WORK/raspbian.gpg" | awk -F: '$1=="fpr"{print $10; exit}')"
[ "$got" = "$KEY_FPR" ] || die "archive key fingerprint is $got, expected $KEY_FPR"

say "2. the sysroot"
if [ ! -f "$SYSROOT/.complete" ]; then
    rm -rf "$SYSROOT"
    pkgs="$(grep -vE '^\s*(#|$)' packaging/pi/sysroot-packages.txt | paste -sd, -)"
    mmdebstrap --mode=root --variant=extract --architectures=armhf \
        --include="libc6-dev,libstdc++-14-dev,$pkgs" \
        trixie "$SYSROOT" "deb [signed-by=$WORK/raspbian.gpg] $ARCHIVE trixie main"
    # Absolute symlinks point at the build machine's / once outside a chroot;
    # point them into the sysroot instead.
    find "$SYSROOT" -type l | while read -r link; do
        target="$(readlink "$link")"
        case "$target" in /*) ln -sfn "$SYSROOT$target" "$link" ;; esac
    done
    touch "$SYSROOT/.complete"
fi
# Merged /usr, which an extract leaves to base-files: the loader's path baked
# into every binary is /lib/ld-linux-armhf.so.3.
for d in lib bin sbin; do
    [ -e "$SYSROOT/$d" ] || [ -L "$SYSROOT/$d" ] || ln -s "usr/$d" "$SYSROOT/$d"
done

# The same flags cmake/arm-toolchain.cmake gives the build; the guard checks
# what they produce.
CXX_HEADERS=(-nostdinc++
    -isystem "$SYSROOT/usr/include/c++/14"
    -isystem "$SYSROOT/usr/include/arm-linux-gnueabihf/c++/14"
    -isystem "$SYSROOT/usr/include/c++/14/backward")

say "3. the guard"
defs="$(echo '#include <bits/c++config.h>' | arm-linux-gnueabihf-g++ --sysroot="$SYSROOT" \
    "${ARCH_FLAGS[@]}" "${CXX_HEADERS[@]}" -dM -E -x c++ -)"
grep -q '_GLIBCXX_RELEASE 14' <<<"$defs" || die "the sysroot's libstdc++ headers were not used"
if grep -q '_GLIBCXX_HAVE_ATOMIC_LOCK_POLICY' <<<"$defs"; then
    die "the C++ headers define an atomic shared_ptr lock policy; Raspberry Pi OS's libstdc++ has none"
fi

say "4. the build"
cmake -S "$SRC" -B "$WORK/build" \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/arm-toolchain.cmake" \
    -DRASPBIAN_SYSROOT="$SYSROOT" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_WITH_WEB=ON -DBUILD_WITH_MYSQL=ON
cmake --build "$WORK/build" --target hms_cpap -j"$(nproc)"
install -m 755 "$WORK/build/hms_cpap" "$OUT/hms_cpap"
arm-linux-gnueabihf-strip "$OUT/hms_cpap"
file "$OUT/hms_cpap" | grep -q 'ELF 32-bit LSB.*ARM, EABI5' || die "not a 32-bit ARM executable"

# The binary under qemu-arm, the sysroot as its /, with glibc's malloc
# checking preloaded: a shared_ptr counted two ways frees its block twice, and
# a plain run may never touch the freed memory again, so it would pass. With
# the checking it aborts at the second free (the 5.4.13 release binary does,
# on the one-night card below; this build must not). Prints the exit code.
run_arm() {
    local home; home="$(mktemp -d)"
    set +e
    HOME="$home" qemu-arm -L "$SYSROOT" \
        -E LD_PRELOAD=/usr/lib/arm-linux-gnueabihf/libc_malloc_debug.so.0 -E MALLOC_CHECK_=3 \
        "$OUT/hms_cpap" "$@" >"$WORK/run.log" 2>&1
    local rc=$?
    set -e
    rm -rf "$home"
    echo "$rc"
}

say "5. --preflight"
rc="$(run_arm --preflight)"
echo "preflight exit code: $rc"
if [ "$rc" -ne 0 ] && [ "$rc" -ne 1 ]; then
    tail -20 "$WORK/run.log"
    die "--preflight did not run to completion"
fi

say "6. --reparse of a one-night card"
card="$WORK/card"
rm -rf "$card"
mkdir -p "$card/DATALOG/20260101"
for kind in BRP PLD SAD EVE CSL; do
    printf 'not an EDF' > "$card/DATALOG/20260101/20260101_230000_$kind.edf"
done
rc="$(run_arm --reparse "$card" 2026-01-01)"
echo "reparse exit code: $rc"
if [ "$rc" -ge 128 ]; then
    tail -20 "$WORK/run.log"
    die "--reparse ended on signal $((rc - 128)): the build does not match the Pi's libraries"
fi

say "built $OUT/hms_cpap"
