#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hms_cpap {

/**
 * The settings an AirSense 11 is set to, for the STR.edf of a night read over
 * Bluetooth, where the Summary never carries them (so Mode and every S.* field
 * were -1, and a reader that opens the folder as a card does not know the
 * therapy mode).
 *
 * The source is <root>/SETTINGS/CurrentSettings.json, the file a card carries:
 * the machine's "SettingProfiles" object under "FlowGenerator". It gets there
 * from a card copy, or from Get ["SettingProfiles"] over Bluetooth, whichever
 * was applied on the machine last (AppliedDateTime): a newer one is never
 * replaced by an older.
 *
 * Only a value whose STR code was matched against a real card is written, and
 * the rest stay -1 rather than guessed:
 *
 *   - numbers as they are, in the STR signal's unit: the three profiles'
 *     pressures (S.A.*, S.AFH.*, S.C.*), RampTime, EprPressure, HumidifierLevel,
 *     HeatedTubeTemperature;
 *   - Off/No as 1 and On/Yes as 2 for the switches (SmartStart, AutoSetComfort,
 *     RampEnable, EprEnable, EprEnablePatientAccess, AntiBacterialFilter,
 *     HumidifierSettingEnable, HeatedTubeSettingEnable);
 *   - the one value of each of these seen on a card: TherapyMode AutoSet (Mode
 *     1), MaskType Pillows (S.Mask 2), TubeType 15mmNonHeated (S.Tube 3),
 *     EprType FullTime (S.EPR.EPRType 2), ClimateControl Auto (S.ClimateControl
 *     1). Another value leaves its field -1 until someone shows what it is.
 *
 * Settings are a snapshot, not a history: they apply from the sleep day the
 * machine says it applied them, and no day before it is written with them.
 */
struct As11Settings {
    int64_t applied_ms = 0;      ///< AppliedDateTime, ms since the epoch; 0 when absent
    int offset_min = 0;          ///< TimeZoneOffset, minutes east of UTC
    /// STR label and value, in the unit of that STR signal.
    std::vector<std::pair<std::string, double>> values;

    /// The STR day (days since 1970-01-01 of the day's noon start on the
    /// machine's calendar) these settings first apply to.
    int64_t firstDay() const;
};

/// <root>/SETTINGS/CurrentSettings.json
std::string settingsPath(const std::string& root);

/// A card's CurrentSettings.json ({"FlowGenerator":{"SettingProfiles":...}}) or
/// the object Get ["SettingProfiles"] returns ({"SettingProfiles":...}).
/// nullopt when it is neither, or names no profile.
std::optional<As11Settings> parseSettings(const std::string& json_text);

/// What <root>/SETTINGS/CurrentSettings.json holds, if it parses.
std::optional<As11Settings> readSettings(const std::string& root);

/// Keeps the Get ["SettingProfiles"] result as <root>/SETTINGS/CurrentSettings.json,
/// laid out as a card's file, unless the one there was applied on the machine
/// at the same time or later. True when the file now holds this result.
bool writeSettings(const std::string& root, const std::string& result_json);

}  // namespace hms_cpap
