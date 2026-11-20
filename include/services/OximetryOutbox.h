#pragma once
//
// SDD-079 D4: oximetry is files too. The oximetry outbox.
//
// The platform SDD-059 method ("write the files first, then send the chunks")
// applied to oximetry, so the uploader's index, resume and ack work for a
// recording exactly as they do for a card file. One folder,
// <data_dir>/cpapdash/oximetry/, its own root (source "oximetry"), never a
// card root:
//
//   <name>               a ring's own recording, byte for byte (vld, o2s)
//   <name>.json          a recording with no ring file of its own (Garmin, a
//                        CSV upload) in hms-json v1 (platform SDD-061 D4)
//   <file>.meta          beside each of those: the frame's `oxi` object
//                        (name, format, source, device_name); never sent
//   live-<YYYYMMDD>.jsonl one line per live sample stored (SDD-061 D5),
//                        append-only, sent as ranges
//
// Every recording store site calls stored() right after its save succeeds,
// and the burst loop calls liveSample() right after a live sample is stored:
// an event each, never a timer. Then the uploader is told through card_feed.
// With no root set (a test, a build without the stream) nothing is written.
//
#include "database/IDatabase.h"

#include <cpapdash/parser/VLDParser.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::oximetry_outbox {

/// <data_dir>/cpapdash/oximetry
std::string rootFor(const std::string& data_dir);

/// Where the outbox is ("" = nowhere: nothing is written).
void setRoot(const std::string& root);
std::string root();

/// The recording's key on the wire (platform SDD-061 D3): [filename] when it
/// is 1 to 64 bytes of [A-Za-z0-9._-], does not start with '.' and is not
/// shaped as one of the outbox's own files (ending .meta or .tmp, starting
/// live-); otherwise
/// "hms-" and the first 16 hex of its SHA-256, the same every time.
std::string wireName(const std::string& filename);

/// "o2s" for an O2Ring-S file by its bytes, else "vld".
std::string ringFormat(const std::string& bytes);

/// The frame's `oxi` object, as kept in a .meta.
struct Meta {
    std::string name;
    std::string format;        ///< vld, o2s, hms-json, live-jsonl
    std::string source;        ///< o2ring, garmin, upload; "" = left out
    std::string device_name;   ///< "" = left out
};
std::string metaJson(const Meta& m);
std::optional<Meta> parseMeta(const std::string& text);

/// What the uploader sends for an outbox file name, or nullopt when it is
/// not one to send (a .meta, a write in progress, a recording whose .meta is
/// not there yet).
std::optional<Meta> metaFor(const std::string& outbox_root, const std::string& file);

/// Is [file] the live spool (live-<YYYYMMDD>.jsonl)?
bool isLiveSpool(const std::string& file);

/// The hms-json v1 body of [s], keys in the contract's order:
/// {"format":"hms-oximetry","v":1,"clock":..,"interval_s":..,"samples":[..]}.
/// `t` is [utc_s][i] with clock "utc" when [utc_s] has one instant per
/// sample, else the sample's own (wall-clock-shaped) time with clock "wall".
/// SpO2 is 0 when it is not a reading; a pulse that is not a reading is
/// null (validity is per field, decided 2026-11-19).
std::string hmsJson(const cpapdash::parser::OximetrySession& s,
                    const std::vector<long long>* utc_s = nullptr);

/// One live line: {"t":..,"spo2":..,"hr":..,"motion":..}\n
std::string liveLine(long long t, int spo2, int hr, int motion);

/// live-<YYYYMMDD>.jsonl for a sample dated [wall_clock] (the day
/// saveLiveOximetrySample files it under).
std::string liveSpoolName(std::chrono::system_clock::time_point wall_clock);

/// A recording [row] was stored with [origin]. [raw] is the ring's own file
/// when the store came from one (written byte for byte), else empty (written
/// as hms-json from the row's samples, clock "utc" when [utc_s] is given).
/// Returns the outbox file written, "" when nothing was.
std::string stored(const cpapdash::parser::OximetrySession& row,
                   const IDatabase::OximetryOrigin& origin, const std::string& raw,
                   const std::vector<long long>* utc_s = nullptr);

/// A live sample read at [read_at] (a true instant) was stored under the
/// day of [wall_clock]. Appends one line. Returns the spool's file name.
std::string liveSample(std::chrono::system_clock::time_point read_at,
                       std::chrono::system_clock::time_point wall_clock, int spo2, int hr,
                       int motion);

/// SDD-079 D7: every recording in oximetry_sessions (never a live row) with
/// no outbox file gets one: the retained raw file <archive>/DATALOG/<name>
/// when it is there and parses to the same start, else hms-json from its
/// stored samples (clock "wall": the stored times are wall clock). Returns
/// how many were written.
int backfill(IDatabase& db, const std::string& archive_dir);

}  // namespace hms_cpap::oximetry_outbox
