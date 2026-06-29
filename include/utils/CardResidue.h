#pragma once

#include <cstdint>
#include <string>

namespace hms_cpap {

// SDD-002: full-card OSCAR residue capture. These two predicates split a card
// listing into ANALYTICAL files (parsed + grouped into sessions) and backup-only
// RESIDUE (written to the OSCAR layout on disk, never parsed). They are kept in
// LOCKSTEP with the CpapDash cloud's SDD-016 sweep (FirmwarePushController.cc
// residualSkip / EzShareListing isCpapEdf) so both archives stay byte-identical.

// True for the *_BRP/EVE/SAD/SA2/PLD/CSL.edf analytical session files — the single
// gate that routes a folder's NON-EDF residue (the per-night .crc) to backup-only.
// NOTE: this lists 6 suffixes (incl _SA2 oximetry) to match the local download set
// in EzShareClient::downloadSession; the cloud's isCpapEdf omits _SA2. STR.edf is
// intentionally NOT matched here (no _xxx.edf suffix) — it is handled separately.
bool isCpapEdf(const std::string& name);

// True if a file should be DROPPED from the residue capture: anything > 20 MB
// (not a card metadata file), OS/sidecar junk, or a multimedia/office/archive
// extension. Ported verbatim from the cloud's residualSkip().
bool residualSkip(const std::string& name, uint64_t size_bytes);

}  // namespace hms_cpap
