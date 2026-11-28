#pragma once
//
// SDD-074: each machine is read on its own clock.
//
// A CPAP names its files and stamps its records on its own wall clock, with no
// zone. Inside hms-cpap that wall clock is held as a LABEL (D4): the six
// numbers counted as if they were UTC (timegm / gmtime), so no host daylight
// saving can move or fold it. Only a MachineZone turns a label into a real
// instant, and only where a real instant is needed: "now" questions, an end
// written from the clock, an instant the machine itself sends (AirSense 11
// over Bluetooth), Home Assistant.
//
// A machine's "Machine clock" setting is one of
//   ""                    Same as this computer (the default): the host's zone,
//                         through the very mktime / localtime calls used before;
//   "+10:00", "-05:00"    a fixed offset, 15 minute steps, UTC-12:00..UTC+14:00;
//   "Australia/Sydney"    a named zone with its rules, from the OS zone database
//                         (std::chrono::tzdb where the standard library has it,
//                         the system zoneinfo files elsewhere; D9).
//
#include <chrono>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

// ── labels (D4) ──────────────────────────────────────────────────────────────

namespace wall {

/// A wall clock's six fields as a label (timegm). tm_isdst is not read.
long long labelOf(const std::tm& t);

/// A label's fields back (gmtime).
std::tm fieldsOf(long long label);

/// "YYYY-MM-DD HH:MM:SS" of a label.
std::string text(long long label);
std::string text(std::chrono::system_clock::time_point label);

/// A stored "YYYY-MM-DD HH:MM:SS" (a 'T' or a trailing fraction / zone is
/// tolerated and ignored) as a label; nullopt when it is not one.
std::optional<long long> parseText(const std::string& s);

/// The label as a time point.
inline std::chrono::system_clock::time_point point(long long label) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(label));
}
inline long long seconds(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

}  // namespace wall

// ── the zone database (D9) ───────────────────────────────────────────────────

/// One named zone's rules, read once from the OS zone database.
class ZoneRules {
public:
    virtual ~ZoneRules() = default;
    /// Seconds east of UTC at a UTC instant.
    virtual long long offsetAt(long long utc) const = 0;
};

namespace zonedb {

/// The rules of a named zone, or null when the OS database has no such zone.
std::shared_ptr<const ZoneRules> load(const std::string& name);

/// Every zone name the OS database offers, sorted (for the picker).
std::vector<std::string> names();

/// Which reader this build uses: "std::chrono::tzdb" or "zoneinfo:<dir>",
/// or "none" when neither is there.
std::string source();

}  // namespace zonedb

// ── MachineZone (D2) ─────────────────────────────────────────────────────────

class MachineZone {
public:
    enum class Kind { Host, Fixed, Named };

    /// How toInstant resolved a wall time.
    enum class Resolution { Exact, Fold, Gap };

    MachineZone() = default;   // Same as this computer
    static MachineZone host() { return MachineZone(); }
    static MachineZone fixed(int offset_s);

    /// The setting as stored. "" is the host. nullopt (and why) when it is not
    /// a valid offset or a zone the OS database knows.
    static std::optional<MachineZone> parse(const std::string& setting, std::string* why = nullptr);

    Kind kind() const { return kind_; }
    bool isHost() const { return kind_ == Kind::Host; }
    /// A named zone, which follows its daylight saving.
    bool isNamed() const { return kind_ == Kind::Named; }

    /// The canonical setting: "", "+10:00", or the zone name.
    std::string setting() const;

    /// Seconds east of UTC at a UTC instant.
    long long offsetAt(long long utc) const;

    /// A UTC instant to the machine's wall clock (a label).
    long long toWall(long long utc) const { return utc + offsetAt(utc); }

    /// The machine's wall clock (a label) to a UTC instant. A fold (a wall
    /// time that happens twice) takes the earlier offset, so the first
    /// occurrence; a gap (one that never happens) moves forward by the jump,
    /// as the machine itself does. Host: std::mktime, exactly as before.
    long long toInstant(long long wall_label, Resolution* how = nullptr) const;

    /// "+10:00" style text of an offset in seconds.
    static std::string offsetText(long long offset_s);

    /// A UTC instant as ISO 8601 on this clock with its offset at that
    /// instant, "2026-11-19T22:00:00+10:00" (D8: Home Assistant places it).
    std::string isoText(long long utc) const;

    bool operator==(const MachineZone& o) const { return setting() == o.setting(); }
    bool operator!=(const MachineZone& o) const { return !(*this == o); }

private:
    Kind kind_{Kind::Host};
    int fixed_s_{0};
    std::string name_;
    std::shared_ptr<const ZoneRules> rules_;
};

/// The host's own zone name when the OS says it ("America/New_York"), else "".
std::string hostZoneName();

}  // namespace hms_cpap
