#pragma once
//
// platform SDD-059 D3: the one thing a collector tells the CpapDash uploader.
//
// Every path hms-cpap reads ends in card files under a card root (the ez Share
// archive, a Sleep Bridge's rebuilt card, an AirSense 11's Bluetooth night).
// Once a piece of one is on disk, the collector says so here and carries on.
// The call returns at once and never fails: collection and assembly never
// depend on CpapDash, the switch, or the network (SDD-059 D3). With no sink
// installed (the switch was never on, a test) it does nothing at all.
//
// `root` is the card root; `rel_path` is card-relative with forward slashes
// (DATALOG/20261116/20261116_221502_BRP.edf, STR.edf); `machine` is the
// machine's serial, or "" when the collector does not know it (the uploader
// reads it from the card's Identification file).
//
#include <functional>
#include <string>

namespace hms_cpap::card_feed {

using FileSink = std::function<void(const std::string& root, const std::string& rel_path,
                                    const std::string& machine)>;
using RootSink = std::function<void(const std::string& root, const std::string& machine)>;

/// Installs (or, with empty functions, removes) where the notes go.
void setSinks(FileSink file_sink, RootSink root_sink);

/// One file under [root] was written.
void fileWritten(const std::string& root, const std::string& rel_path, const std::string& machine);

/// Something under [root] was written; the uploader looks at the whole root.
void rootWritten(const std::string& root, const std::string& machine);

}  // namespace hms_cpap::card_feed
