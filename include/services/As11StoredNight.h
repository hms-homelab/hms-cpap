#pragma once

#include <cpapdash/parser/AirSense11Pages.h>
#include <cpapdash/parser/AirSense11Summary.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * SDD-070: an AirSense 11 night the stream did not see, filled from the
 * pages the machine stores.
 *
 * After a night, the machine's stored pages are pulled (every night with
 * usage, D2) and kept as they came under
 *
 *   <root>/stored/<sleep day>/<spool>.<n>.page     n from 0, fsync'd
 *
 * and the minutes no streamed segment covers (the night's mask-on spans minus
 * the streamed ones, D2/D3) are written into the night's folder as card files
 * (D4): a BRP at 6.25 Hz for flow and mask pressure, a PLD with leak and
 * inspiratory pressure, and the stored events merged into the night's EVE.
 *
 * This file is the part with no radio and no EDF: which spools, where the
 * pages are kept, which minutes are gaps, and which samples fall in them.
 */

/// A span of time, UTC epoch ms, [start, end).
struct StoredSpan {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    int64_t length() const { return end_ms - start_ms; }
};

/// The spools pulled for a night: the events, then the four signals.
const std::vector<std::string>& storedSpools();
inline constexpr const char* kStoredEventsSpool = "TherapyEvents-RespiratoryEvents";

/// <root>/stored/<night>
std::string storedNightDir(const std::string& root, const std::string& night);

/// Keeps page n of a spool, atomically and fsync'd. True when it is on disk
/// (written now, or the same bytes were there already).
bool keepStoredPage(const std::string& root, const std::string& night, const std::string& spool,
                    int n, const std::vector<uint8_t>& bytes);

/// The pages kept for a spool, page 0 first, stopping at the first missing.
std::vector<std::vector<uint8_t>> keptStoredPages(const std::string& root, const std::string& night,
                                                  const std::string& spool);

/// <root>/stored/<night>/done: every spool of the night was pulled to its end.
bool storedNightDone(const std::string& root, const std::string& night);
bool markStoredNightDone(const std::string& root, const std::string& night);

/// A Summary day's mask-on spans.
std::vector<StoredSpan> maskOnSpans(const cpapdash::parser::AirSense11Day& day);

/// The mask-on spans minus the covered ones, keeping only stretches of at
/// least min_ms (a stream segment is cut at a 60 s pause, so a shorter hole
/// is a pause, not lost data).
std::vector<StoredSpan> gapsToFill(std::vector<StoredSpan> mask_on, std::vector<StoredSpan> covered,
                                   int64_t min_ms = 60 * 1000);

/// Samples of two signals that share a grid, cut into contiguous runs: a run
/// holds the instants where both have a sample, on a step of the interval,
/// inside one gap. Values are in the signals' units (L/s, cmH2O).
struct StoredRun {
    int64_t start_ms = 0;      // the first sample
    int64_t interval_ms = 0;
    std::vector<double> a, b;
};
std::vector<StoredRun> storedRuns(const std::vector<cpapdash::parser::AirSense11PageChunk>& a,
                                  const std::vector<cpapdash::parser::AirSense11PageChunk>& b,
                                  const std::vector<StoredSpan>& gaps);

/// Every chunk of one signal across a spool's pages, in time order, a chunk
/// held twice (two pages overlapping) kept once.
std::vector<cpapdash::parser::AirSense11PageChunk>
storedChunks(const std::vector<std::vector<uint8_t>>& pages, int* skipped = nullptr);

/// Every event across the events pages, in time order, each kept once.
std::vector<cpapdash::parser::AirSense11PageEvent>
storedEvents(const std::vector<std::vector<uint8_t>>& pages);

/// The live stream's name for a stored event type ("CentralApneaEnd",
/// "ObstructiveApneaEnd"), or "" for a type not yet seen.
std::string storedEventName(int type);

}  // namespace hms_cpap
