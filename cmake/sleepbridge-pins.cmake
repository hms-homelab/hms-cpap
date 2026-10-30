# The Sleep Bridge asset this source builds against (see SleepBridge.cmake).
# One version, one SHA-256 per platform. Assets live at
#   ${SLEEPBRIDGE_BASE_URL}/sleepbridge-v<version>/hms-cpap-sleepbridge-<version>-<platform>.tar.gz
# and each holds lib/libsleepbridge.a (lib/sleepbridge.lib on Windows),
# include/sleepbridge/rebuild.h and LICENSE-sleepbridge.
#
# Empty until the first asset is published: BUILD_WITH_SLEEPBRIDGE=ON then
# stops at configure with a clear message, and OFF builds as always.

set(SLEEPBRIDGE_BASE_URL "https://github.com/hms-homelab/hms-cpap/releases/download")
set(SLEEPBRIDGE_VERSION "")

set(SLEEPBRIDGE_SHA256_linux_amd64   "")
set(SLEEPBRIDGE_SHA256_linux_arm64   "")
set(SLEEPBRIDGE_SHA256_macos_arm64   "")
set(SLEEPBRIDGE_SHA256_macos_amd64   "")
set(SLEEPBRIDGE_SHA256_windows_amd64 "")
