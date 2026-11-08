#pragma once
//
// A Philips DreamStation card read through an ez Share in its SD slot, or
// through a Mule & Miner in proxy mode (the same /dir and /download).
//
// The card keeps its data under P-SERIES/<folder>/: PROP.BIN (or PROP.TXT),
// LOG.SEQ, and the session files in P0 .. Pn (<session>.B01 summary, .B02
// events, .B05 flow). This keeps a copy of them in a folder, laid out as on
// the card, where PhilipsCardIngestion reads it as it reads a local card.
//
//   - Only the identity files and the P folders are copied, never D/ or E/
//     (the machine's own logs, which nothing here reads).
//   - A file is fetched when it is new or its listed size or card timestamp
//     changed (the card files' rule), remembered in a manifest beside the
//     copy. The open session's .B05 grows by one block every four minutes, so
//     its listed size moves with it.
//   - PROP.BIN comes first. When it is not plain text the card is encrypted:
//     hms-cpap does not decrypt it, so nothing more is copied.
//
#include "clients/EzShareClient.h"   // EzShareFileEntry
#include "clients/IDataSource.h"

#include <string>

namespace hms_cpap {

struct PhilipsMirrorResult {
    bool ok = false;          ///< the card answered and the pass finished
    bool encrypted = false;   ///< PROP.BIN is not plain: nothing else was copied
    std::string error;
    int dirs_listed = 0;
    int fetched = 0;
    int unchanged = 0;
    int failed = 0;
};

/// One pass: list the card and bring [local_dir] up to date.
PhilipsMirrorResult mirrorPhilipsCard(IDataSource& card, const std::string& local_dir);

}  // namespace hms_cpap
