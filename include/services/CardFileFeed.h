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
// SDD-079 D1: `source` is where the root's files came from, said by the
// collector that knows it, never read back from a path: kSourceCard for a
// file the machine wrote to its card (the burst collector, a Sleep Bridge's
// rebuild), kSourceBleAs11 for a file hms-cpap built from an AirSense 11's
// Bluetooth stream, kSourceOximetry for the oximetry outbox (SDD-079 D4).
//
#include <functional>
#include <string>

namespace hms_cpap::card_feed {

/// platform SDD-061 D1's values, and the outbox's own (SDD-079 D4).
inline constexpr const char* kSourceCard     = "card";
inline constexpr const char* kSourceBleAs11  = "ble-as11";
inline constexpr const char* kSourceBleDs2   = "ble-ds2";
inline constexpr const char* kSourceOximetry = "oximetry";

using FileSink = std::function<void(const std::string& root, const std::string& rel_path,
                                    const std::string& machine, const std::string& source)>;
using RootSink = std::function<void(const std::string& root, const std::string& machine,
                                    const std::string& source)>;
using BurstSink = std::function<void(const std::string& root)>;

/// Installs (or, with empty functions, removes) where the notes go.
void setSinks(FileSink file_sink, RootSink root_sink, BurstSink burst_sink = {});

/// A burst collection pass over [root] ended, whether or not it found
/// anything new. The uploader says so to CpapDash after every file that
/// burst wrote has been sent and acked: the burst is the only clock in the
/// process, and ingest closes an ez Share night from it.
void burstEnded(const std::string& root);

/// One file under [root] was written.
void fileWritten(const std::string& root, const std::string& rel_path, const std::string& machine,
                 const std::string& source);

/// Something under [root] was written; the uploader looks at the whole root.
void rootWritten(const std::string& root, const std::string& machine, const std::string& source);

}  // namespace hms_cpap::card_feed
