#pragma once
//
// SDD-065: what a Mule & Miner hands back for one ds2_read, a ".ds2r" file
// (stream kind 7 on the local socket), is read with cpapdash-ds2-ble's
// parseReadFile (ReadFile.h, v0.1.1: ANSWER, RECORD, STREAM_END, END and the
// SEALED item). A file that does not parse, a cut-short one included, is not
// stored at all: its bookmarks stay where they were, and the next read asks
// from there again. These are the few things this host adds around it.
//
#include <cpapdash/ds2ble/ReadFile.h>
#include <cpapdash/ds2ble/Record.h>
#include <cpapdash/ds2ble/SyncSession.h>

#include <cstdint>
#include <optional>
#include <string>

namespace hms_cpap::mule::ds2r {

/// What the identity answers say (serial, firmware, model, newest session),
/// in the shape Ds2Store keeps.
cpapdash::ds2ble::Identity identity(const cpapdash::ds2ble::ReadFile& f);

/// push-c3 SDD-043 §4's token for a result number ("" for 0); a protocol
/// failure with a machine status byte is "status_<hex>".
std::string resultToken(uint8_t result, uint8_t status = 0);

/// Ten zero bytes, or empty: no bookmark.
bool noBookmark(const cpapdash::ds2ble::Bookmark& b);

/// "0a1b..." (20 lower-case hex digits) and back; fromHex is nullopt for
/// anything but 20 hex digits.
std::string toHex(const cpapdash::ds2ble::Bookmark& b);
std::optional<cpapdash::ds2ble::Bookmark> fromHex(const std::string& s);

}  // namespace hms_cpap::mule::ds2r
