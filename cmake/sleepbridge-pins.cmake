# The Sleep Bridge asset this source builds against (see SleepBridge.cmake).
# One version, one SHA-256 per platform. Assets live at
#   ${SLEEPBRIDGE_BASE_URL}/sleepbridge-v<version>/hms-cpap-sleepbridge-<version>-<platform>.tar.gz
# and each holds lib/libsleepbridge.a (lib/sleepbridge.lib on Windows),
# include/sleepbridge/rebuild.h and LICENSE-sleepbridge.
#
# The assets sit on this repository's GitHub release tagged
# sleepbridge-v<version>, which holds only the tarballs and exists before any
# hms-cpap tag that builds against it. A platform with an empty pin makes
# BUILD_WITH_SLEEPBRIDGE=ON stop at configure with a clear message; OFF builds
# as always.

set(SLEEPBRIDGE_BASE_URL "https://github.com/hms-homelab/hms-cpap/releases/download")
set(SLEEPBRIDGE_VERSION "0.3.0")

set(SLEEPBRIDGE_SHA256_linux_amd64   "a413bffafbbcb499c0a0949287f952697dbb0bed9ad3cbae2704d92b01c1ea71")
set(SLEEPBRIDGE_SHA256_linux_arm64   "8a405e87c4a0b5f007966c90e4abb65b9763ef1d4df3c9623a7280ac0c1ffd03")
set(SLEEPBRIDGE_SHA256_linux_armhf   "e69a9cb7a5f43cd441f8b50175bdeb3fc13d4731957835938b28f1a9e6836e22")
set(SLEEPBRIDGE_SHA256_macos_arm64   "")
set(SLEEPBRIDGE_SHA256_macos_amd64   "")
set(SLEEPBRIDGE_SHA256_windows_amd64 "")
