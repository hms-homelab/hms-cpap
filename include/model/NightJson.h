#pragma once
//
// SDD-013: a night written as JSON, read into the structs the runtime renders.
//
// The narrator's corpus is built offline from synthetic nights, and its inputs
// must be the runtime's inputs byte for byte. So a synthetic night is not
// rendered by a second renderer: it is read into the same SessionMetrics,
// STRDailyRecord and NightOximetryFacts the database gives, and goes through
// NightFacts::from() and model::factBlock() like a stored night.
//
// The shape (every figure optional unless said; an absent key is an absent
// figure, never 0):
//   {"id": "...",
//    "metrics": {"sleep_day": "YYYY-MM-DD", "index_kind": "ahi"|"ungraded",
//                "total_events", "ahi", "obstructive_apneas", ... (the
//                SessionMetrics names NightFacts::from reads)},
//    "str": {"family": "unknown"|"cpap"|"autoset"|"bilevel"|"asv", "mode",
//            "ahi", "mask_events", "leak_95", "mask_press_95", "asv_epap", ...},
//    "oximetry": {"avg_spo2", "min_spo2", "spo2_baseline", "odi_3pct",
//                 "time_below_90", "time_below_88", "avg_hr", "min_hr", "max_hr"}}
// A key the reader does not know is an error, so a typo in a generator is
// caught instead of silently becoming a missing figure.
//
#include "parsers/CpapdashBridge.h"
#include "services/NightFacts.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace hms_cpap::model {

/// A night read from JSON: what NightFacts::from() takes.
struct NightInput {
    SessionMetrics metrics;
    std::optional<STRDailyRecord> str;
    std::optional<NightOximetryFacts> oximetry;

    NightFacts facts() const;
};

/// [j] read as a night. Throws std::invalid_argument naming the bad key.
NightInput nightFromJson(const nlohmann::json& j);

}  // namespace hms_cpap::model
