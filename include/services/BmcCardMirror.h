#pragma once
//
// SDD-060: a BMC / React Health Luna card read through an ez Share in its SD
// slot, or through a Mule and Miner in local mode (the same /dir and /download).
//
// A Luna card is a handful of files at the card root, every session in the same
// serial-named files: <stem>.000 (.001 ...), .EVT, .IDX, .LOG, .USR. This keeps
// a copy of them in a folder, where BmcIngestion reads it as it reads a local
// card. A file's signature is its listed size AND its card stamp, as for any
// card: the size is KB-rounded and misses a rewrite under a KB. (One bench
// Luna left its entries dated 1980-0-0, which compares equal each pass; a
// machine that dates its files is followed by its dates.)
//
//   D2  only the files of the card's one stem are copied
//   D3  a raw file is fetched when its signature changed; .EVT, .IDX and .LOG
//       whenever a raw file changed, and when their own signature did; .USR
//       only when its own signature changed
//   D4  a raw file that grew is appended: the last 4 KB we hold are asked for
//       again and compared, and only when they match is the rest kept.
//       Anything else (a mismatch, a shorter file, a source without Range)
//       takes the whole file. Every write goes to .part and is renamed.
//
#include "clients/EzShareClient.h"   // EzShareFileEntry
#include "clients/IDataSource.h"

#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

struct BmcMirrorResult {
    bool ok = false;          ///< the card answered and the pass finished
    std::string error;
    std::string stem;         ///< the card's serial-named stem, e.g. 22B39570
    int fetched = 0;          ///< files taken whole
    int appended = 0;         ///< raw files extended in place (D4)
    int unchanged = 0;
    int failed = 0;
    bool raw_changed = false; ///< any raw file moved this pass
};

/// The stem of a Luna card among these root entries: one with a ".usr" and a
/// ".000".."999" or ".evt" beside it. Several: the one whose raw file is
/// listed largest (the card in use); nullopt when there is none.
std::optional<std::string> bmcStemOf(const std::vector<EzShareFileEntry>& root);

/// True for "<stem>.000".."<stem>.999" (any case).
bool isBmcRawName(const std::string& name, const std::string& stem);

/// One pass: list the card root and bring [local_dir] up to date.
BmcMirrorResult mirrorBmcCard(IDataSource& card, const std::string& local_dir);

}  // namespace hms_cpap
