#pragma once
//
// SDD-031 (#28): a Sefam S.Box card read through an ez Share in its SD slot.
//
// The ResMed ez Share path looks for DATALOG; an S.Box card has none. This
// copies the card's Sefam tree into the archive, where the existing Sefam
// ingestion reads it as it reads a local folder:
//
//   1263R/24337476/DATA_229/DATA_229.INI   (S.Box AUTO: a folder per session)
//   <model><serial>/260913/221004.ini      (SleepBox: a folder per day)
//
// D4 (Albin): the WHOLE card while the database holds no session for the
// device; once there is history, only the session folders of the card's two
// most recent nights. A file is fetched when it is new or its listed size or
// card timestamp changed (the card files' rule), remembered in a small manifest
// beside the archive copy.
//
#include "clients/EzShareClient.h"   // EzShareFileEntry
#include "clients/IDataSource.h"

#include <string>
#include <vector>

namespace hms_cpap {

struct SefamMirrorResult {
    bool ok = false;           ///< the card answered and the pass finished
    std::string error;
    bool whole_card = false;   ///< the first-burst pass
    int dirs_listed = 0;
    int files_seen = 0;
    int fetched = 0;
    int unchanged = 0;
    int failed = 0;
};

/// `DATA_<n>` (S.Box AUTO) or a six-digit day folder `YYMMDD` (SleepBox).
bool isSefamSessionFolderName(const std::string& name);

/// The session folders of the two most recent nights, by the card's own
/// timestamps (a night is the timestamp shifted back 12 h). When the card
/// gives no timestamps, the last four by name (a DATA_<n> is one session, and
/// two nights rarely hold more).
std::vector<EzShareFileEntry> lastTwoNights(std::vector<EzShareFileEntry> session_dirs);

/// One pass: list the card from its root to the session folders and fetch
/// what changed into [archive_root].
SefamMirrorResult mirrorSefamCard(IDataSource& card, const std::string& archive_root,
                                  bool whole_card);

}  // namespace hms_cpap
