#pragma once
//
// SDD-078: one Garmin get_sleep_data answer turned into an oximetry recording,
// by the reference importer's rules (section 1):
//
//   - wellnessEpochSPO2DataDTOList: epochTimestamp (a GMT string),
//     spo2Reading kept from 50 to 100;
//   - sleepHeartRate: startGMT (epoch ms), value kept from 30 to 220, paired
//     to the nearest SpO2 epoch within 2 minutes, else no pulse: blank
//     (kGarminNoHeartRate), and the SpO2 reading still counts (validity is
//     per field, decided 2026-11-19);
//   - sleepMovement: startGMT and endGMT (GMT strings), activityLevel, given
//     to an epoch that falls in [start, end);
//   - a record that is not well formed is skipped, never filled in;
//   - 60 valid SpO2 samples at least, or the night is not stored yet;
//   - dailySleepDTO's sleepStartTimestampGMT and sleepEndTimestampGMT say
//     whether Garmin's sleep period is complete: both there, end after start,
//     end not in the future, an hour at least. An incomplete one is left for
//     the next event (D2).
//
// D5: Garmin's times are UTC instants. Each is stored on the CPAP machine's
// wall clock (SDD-074), the frame every oximetry and CPAP row is in, through
// [wall_offset] (machineOffset at that instant), never a fixed zone.
//
// D6: the recording is keyed by its sleep period, "garmin-<start ms>", so
// the same night fetched again replaces its row.
//
#include <cpapdash/parser/VLDParser.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

struct GarminNight {
    enum class Verdict {
        Ready,          ///< session is the night, to store
        Incomplete,     ///< Garmin's sleep period is not over (or not there): next event
        TooFewSamples,  ///< fewer than kGarminMinSpo2 valid SpO2 samples: next event
    };
    Verdict     verdict = Verdict::Incomplete;
    std::string reason;
    long long   sleep_start_ms = 0;   ///< dailySleepDTO, UTC
    long long   sleep_end_ms   = 0;
    int         valid_spo2 = 0;
    int         hr_paired  = 0;
    int         motion_paired = 0;
    cpapdash::parser::OximetrySession session;   ///< set when Ready
    /// SDD-079 D4: each sample's UTC instant (unix seconds), in
    /// session.samples order. The samples themselves are on the machine's
    /// wall clock (D5); these are what goes to CpapDash as clock "utc".
    std::vector<long long> sample_utc_s;
};

inline constexpr int kGarminMinSpo2 = 60;
/// A sample with no pulse within kGarminHrPairSeconds: the pulse is blank and
/// the SpO2 still counts (validity per field, decided 2026-11-19).
inline constexpr unsigned char kGarminNoHeartRate = 0;
inline constexpr int kGarminHrPairSeconds = 120;

/// The recording's name, from its sleep period's start (D6).
std::string garminSessionFilename(long long sleep_start_ms);

/// A Garmin GMT timestamp ("2026-11-18T13:00:00.0", with a 'Z' or an offset
/// or neither; neither is UTC), as UTC epoch milliseconds; nullopt when it is
/// not one.
std::optional<long long> parseGarminGmt(const nlohmann::json& v);

/// Epoch milliseconds given as a number or a numeric string; nullopt else.
std::optional<long long> parseGarminEpochMs(const nlohmann::json& v);

/// The rules above. [now_utc] is the current UTC second (a sleep period that
/// ends after it is not over); [wall_offset] gives, for a UTC second, the
/// machine wall clock's offset from it in seconds.
GarminNight convertGarminSleep(const nlohmann::json& sleep, long long now_utc,
                               const std::function<long long(long long)>& wall_offset);

const char* garminVerdictName(GarminNight::Verdict v);

}  // namespace hms_cpap
