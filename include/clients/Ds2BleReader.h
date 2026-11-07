#pragma once

#include "clients/ble/BleTransport.h"
#include "services/Ds2Store.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hms_cpap {

/// How long Ds2BleReader waits for each step.
struct Ds2Timeouts {
    int scan_ms = 20000;
    int answer_ms = 10000;     // one request's answer
    int pair_ms = 90000;       // the whole pairing, scan excluded
    int confirm_ms = 60000;    // the user's answer to the code
};

/**
 * SDD-064: a Philips DreamStation 2 read over its own Bluetooth, after the
 * night. The protocol is cpapdash-ds2-ble's SyncSession; this class only
 * finds the machine, bonds with it, carries bytes both ways over a
 * ble::ITransport, and stores what comes back (Ds2Store) record by record.
 *
 * Pairing is the OS's, by numeric comparison (SDD-064 D3): the machine shows
 * six digits and so does hms-cpap's page, which asks the user whether they
 * match (Linux, Windows). On macOS the OS shows its own window and the page
 * says so. The machine keeps ONE bond: pairing hms-cpap replaces any other
 * reader (D4).
 *
 * Nothing here decides WHEN to read (D5): the caller does, after the night.
 */
class Ds2BleReader {
public:
    using Timeouts = Ds2Timeouts;

    /// `address`: the machine's address (MAC; on macOS the system's
    /// identifier), or empty for the first DreamStation 2 the scan finds.
    /// `transport`: the host's when null; a test passes a fake.
    explicit Ds2BleReader(std::string data_dir, std::string address = "",
                          std::shared_ptr<ble::ITransport> transport = nullptr, Timeouts t = {});
    ~Ds2BleReader();
    Ds2BleReader(const Ds2BleReader&) = delete;
    Ds2BleReader& operator=(const Ds2BleReader&) = delete;

    // ── pairing ─────────────────────────────────────────────────────────

    enum class PairState {
        Idle,
        Scanning,     // looking for the machine
        Pairing,      // asked the OS to bond, waiting for the code
        Confirm,      // the code is on show: the user must say whether it matches
        OsDialog,     // macOS shows its own window with the code
        Paired,
        Rejected,     // the codes did not match, or the machine refused
        TimedOut,
        NotFound,     // no DreamStation 2 advertising
        RadioOff,     // this computer's Bluetooth is off or not allowed
        Unsupported,  // this build cannot pair
        Failed,
        Cancelled,
    };
    static const char* stateCode(PairState s);

    struct PairStatus {
        PairState state = PairState::Idle;
        std::string code;          // six digits while Confirm
        int seconds_left = 0;      // while Confirm or OsDialog
        std::string error;         // English detail, for the log
        std::string machine_name;  // "PR BT ...", once found
    };

    /// Starts pairing on its own thread. False when a pairing or a read runs.
    bool pairStart();
    /// The user's answer to the code. False when no code is waiting.
    bool pairAnswer(bool match);
    void pairCancel();
    PairStatus pairStatus() const;
    /// The OS shows the code itself (macOS).
    bool osShowsCode() const;

    // ── reading ─────────────────────────────────────────────────────────

    struct ReadResult {
        bool ok = false;
        std::string error;                 // English detail
        std::string code;                  // stable: ok, not_paired, not_found, radio_off, connect,
                                           // bond_lost, protocol, storage, busy, not_compiled
        cpapdash::ds2ble::Identity identity;
        int records = 0;                   // records stored by this read
        std::vector<uint32_t> sessions;    // session numbers with a new summary or events record
    };
    /// One read of every stream from its stored bookmark. Blocks; one at a time.
    ReadResult read();

    bool busy() const { return busy_.load(); }
    bool paired() const;
    /// The paired machine, if any.
    std::optional<Ds2Store::Paired> pairedMachine() const;
    /// Removes the OS bond where the OS allows it and forgets the pairing.
    /// The bookmarks stay: they belong to the machine, not to the bond.
    /// Returns whether the OS bond was removed (false where the OS has no way).
    bool forget();

    Ds2Store& store() { return store_; }
    const Ds2Store& store() const { return store_; }

private:
    std::shared_ptr<ble::ITransport> transport();
    bool radioReady(std::string* why);
    std::optional<ble::Device> find(const std::optional<Ds2Store::Paired>& paired, int timeout_ms);
    void pairThread();
    void setPair(PairState s, const std::string& error = "");

    std::string data_dir_;
    std::string address_;
    std::shared_ptr<ble::ITransport> transport_;
    Timeouts t_;
    Ds2Store store_;

    std::atomic<bool> busy_{false};
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    PairStatus pair_;
    std::chrono::steady_clock::time_point deadline_{};
    bool answered_ = false;
    bool answer_ = false;
    bool cancel_ = false;
    std::thread pair_thread_;
};

}  // namespace hms_cpap
