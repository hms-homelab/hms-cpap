#pragma once

#include <set>
#include <string>

namespace hms_cpap {

/// What a card mirror actually did, so the endpoint can report it instead of
/// answering "queued" over a payload it silently dropped.
struct CardImportResult {
    std::set<std::string> dates;   ///< YYYYMMDD folders seen
    int  copied  = 0;
    int  skipped = 0;              ///< junk / oversize, per residualSkip
    bool saw_str = false;          ///< an STR.edf reached the card root
};

/**
 * Mirror an extracted SD card into the archive, keeping the card's own layout.
 *
 * The upload path used to keep ONLY `YYYYMMDD` directories, so a zip of the card
 * ROOT -- the natural thing to upload, and the layout SDD-010 pins -- lost
 * STR.edf, Identification.* and SETTINGS/ on the way in, while the endpoint
 * still answered "queued". Backfill then derived the daily summary from sessions
 * and the dashboard reported AHI 0.0 (issue #23).
 *
 * Same shape as the cloud's Tier 4 residual sweep (hms-cpapdash-api SDD-016):
 * eight-digit date directories land under the archive's DATALOG, everything else
 * keeps its path relative to the card root. `residualSkip` is the only filter,
 * so real card files nothing would think to allowlist (Identification.tgt,
 * *.crc, Journal.dat, therapy.pdat) survive.
 *
 * @param extracted_root  where the zip was unpacked; the card root is found
 *                        inside it, so a zip that wraps its contents in a folder
 *                        resolves the same as one that does not
 * @param card_dir        the card ROOT under the archive (config.local_dir)
 * @param archive_dir     the archive's DATALOG directory
 */
CardImportResult mirrorCardInto(const std::string& extracted_root,
                                const std::string& card_dir,
                                const std::string& archive_dir);

}  // namespace hms_cpap
