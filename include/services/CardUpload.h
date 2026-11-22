#pragma once
//
// SDD-031 (#28): a card uploaded as a zip, read by what it is.
//
// The upload page used to understand one card: a ResMed DATALOG tree. A Sefam
// S.Box or a Löwenstein Prisma zip was refused as "No DATALOG date folders".
// Now the extracted zip is classified by its own files, and a Sefam or
// Löwenstein card is kept under <data_dir>/uploads/<format>/ (D2) and every
// session in it the database does not hold yet is imported (D3: the whole
// history, not only nights newer than the last).
//
// No MQTT and no AI summary here: an upload is history, and publishing
// hundreds of past nights would flood Home Assistant's history with them.
//
#include "services/OscarCardReader.h"

#include <json/json.h>

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap {

class IDatabase;

enum class UploadedCard { ResMed, Sefam, Lowenstein, Bmc, Unknown };

/// "resmed", "sefam", "lowenstein", "bmc", "unknown": the folder name under uploads/.
const char* uploadedCardName(UploadedCard kind);

/// What an extracted card is, by its files. Sefam by its session folders,
/// Löwenstein by .wmedf files or a Prisma Line therapy.pdat, ResMed by its
/// DATALOG, as the parser's detectManufacturer, plus the .pdat it cannot see
/// until the archive is opened.
UploadedCard classifyUploadedCard(const std::string& dir);

/// Copy an extracted card into the upload store, merging with what earlier
/// uploads left there; a file at the same path is replaced by the newer copy.
bool mergeCardInto(const std::string& from, const std::string& to, std::string& error);

/// Löwenstein: every .pdat (a Prisma Line's therapy archive, whatever the
/// browser renamed it to) is opened into its own folder under [to], named for
/// the file, and never kept as a .pdat there: PrismaIngestion would extract a
/// root-level therapy.pdat into one temp cache it shares with a Löwenstein
/// install's own card. Everything else is copied as for any card. Returns the
/// card roots this upload produced, for the reply's nights.
std::vector<std::string> prepareLowensteinUpload(const std::string& from, const std::string& to,
                                                 std::string& error);

/// The nights (YYYY-MM-DD) a Sefam or Löwenstein card holds, from its session
/// list alone (no channel data is read), for the upload's reply.
std::set<std::string> cardNights(UploadedCard kind, const std::string& root);

struct CardImportCounts {
    int found = 0;           ///< sessions on the card
    int imported = 0;        ///< parsed and saved
    int already_stored = 0;  ///< the database had them
    int removed = 0;         ///< on a night an operator removed (SDD-029)
    int refused = 0;         ///< the parser would not vouch for them
    std::set<std::string> nights;  ///< YYYY-MM-DD of the sessions imported
};

using CardImportProgress = std::function<void(int done, int total)>;

/// Import every session on a Sefam or Löwenstein card the database does not
/// hold yet, then re-derive the daily summary. Removed nights stay removed.
CardImportCounts importCardSessions(IDatabase& db, UploadedCard kind, const std::string& root,
                                    const std::string& device_id,
                                    const std::string& device_name,
                                    const CardImportProgress& progress = {});

// ── SDD-083: a DreamStation 2 card whose files are encrypted ───────────────
//
// hms-cpap does not decrypt it. It is kept as a card (P-SERIES/<machine>/)
// and read by the OSCAR the user paired (SDD-080 here, SDD-081 a helper),
// the same read a card from a Sleep Bridge Lite or a folder gets. With none
// paired the upload is refused, saying why and how to pair one, and nothing
// is kept. A DreamStation card whose files are plain is not one of these.

/// The folder of a DreamStation card (PROP.BIN beside P0 .. Pn) in [dir]
/// whose files are encrypted, as the Philips parser judges it; nullopt for no
/// DreamStation card or one whose files are plain.
std::optional<std::string> encryptedPhilipsCardIn(const std::string& dir);

/// The card at [card_dir] kept under [store] the way OSCAR takes a card:
/// <store>/P-SERIES/<machine>/, merged with what earlier uploads left there.
/// A card inside a P-SERIES folder brings that folder whole (LAST.TXT and
/// every machine in it); a machine folder alone keeps its name ("UPLOAD"
/// when the zip had none). False, with [error], when it could not be copied.
bool keepPhilipsUpload(const std::string& card_dir, const std::string& store, std::string& error);

/// The refusal when no OSCAR is paired. [containerised]: Docker or the Home
/// Assistant add-on, where only a helper on a home PC can read it.
Json::Value philipsUploadRefusal(bool containerised);

/// The upload's reply from OSCAR's read: on an import the nights stored
/// ("dates"), on anything else an "error" that says what to do. [helper]:
/// the read was the helper's.
Json::Value philipsUploadReply(const oscar::OscarCardReader::ReadResult& r, bool helper);

}  // namespace hms_cpap
