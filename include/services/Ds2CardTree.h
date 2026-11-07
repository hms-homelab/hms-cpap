#pragma once

#include <cpapdash/ds2ble/ReadFile.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace hms_cpap {

/**
 * SDD-065: a DreamStation 2's card tree, as the machine would write it to its
 * own card, built from the files a Mule & Miner's miner hands back already in
 * their card form (the `.ds2r`'s SEALED items). hms-cpap does not open, change
 * or make any of those bytes: it places each one at its path, the way a card
 * holds them, so OSCAR and SleepHQ can import the tree like a card.
 *
 *   <data_dir>/ds2card/<serial>/P-SERIES/LAST.TXT            the folder's name, no line end
 *   <data_dir>/ds2card/<serial>/P-SERIES/<folder>/PROP.BIN   from stream 0
 *   <data_dir>/ds2card/<serial>/P-SERIES/<folder>/LOG.SEQ    from stream 0
 *   <data_dir>/ds2card/<serial>/P-SERIES/<folder>/Pn/<SESSION>.B01|.B02
 *   <data_dir>/ds2card/<serial>/P-SERIES/<folder>/D/<nnn>.B03, E/<nnn>.B04
 *
 * The folder is named from the serial (folderFor), the same for every read of
 * that machine. Each read numbers its D/ and E/ files from 000; across reads
 * they are numbered on here, in the order they come, one number per log
 * record (its bookmark), so the same record read again keeps its name.
 * <data_dir>/ds2card/<serial>/tree.json keeps that numbering and the oldest
 * session's start (FD), which the card's PROP.BIN carries.
 */
class Ds2CardTree {
public:
    explicit Ds2CardTree(std::string data_dir);

    /// Eight upper-case hex digits named from the serial: six from a 32-bit
    /// FNV-1a of its ASCII, then "00", the shape a card's folder has. Stable
    /// for a serial on every platform.
    static std::string folderFor(const std::string& serial);

    /// <data_dir>/ds2card/<serial> (the serial as letters and digits only).
    std::string root(const std::string& serial) const;
    /// root/P-SERIES/<folder>
    std::string machineDir(const std::string& serial) const;

    struct Placed {
        bool ok{false};
        std::string rel;     // where it went, under machineDir
        std::string error;   // why not: bad_path, storage
    };
    /// One SEALED item to its place, written whole (to a temporary name, then
    /// renamed). Stream 0: PROP.BIN or LOG.SEQ only. 1 and 2: P<n>/<8 hex>.B01
    /// or .B02, the extension the stream's. 3 and 4: the D/ or E/ number for
    /// its bookmark (a new one gets the next). Any other path is refused.
    Placed place(const std::string& serial, const cpapdash::ds2ble::ReadSealed& s);

    /// The oldest session's start (epoch s) seen for the machine, kept as the
    /// card's FD: the smaller of the one kept and `start`.
    void noteSessionStart(const std::string& serial, long long start);

    /// What the card's PROP.BIN and LOG.SEQ count: the files in the current
    /// (highest) P folder, in D/, in E/; and FD (0 when no start is known).
    struct Counts {
        int pfn{0}, dfn{0}, efn{0};
        int p_folder{0};
        long long fd{0};
    };
    Counts counts(const std::string& serial) const;

private:
    struct State {
        std::map<std::string, int> d, e;   // log record bookmark (hex) -> its number
        long long fd{0};
    };
    State load(const std::string& serial) const;   // mu_ held
    bool  save(const std::string& serial, const State& s) const;
    std::string data_dir_;
    mutable std::mutex mu_;
};

}  // namespace hms_cpap
