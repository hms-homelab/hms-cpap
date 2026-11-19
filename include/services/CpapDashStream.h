#pragma once
//
// platform SDD-059: hms-cpap streams the card files it holds to CpapDash.
//
// ONE uploader (D3). It knows one thing: a file under a card root, and the
// machine serial it belongs to. The collectors tell it, through card_feed
// (services/CardFileFeed.h), once a piece of a card is on disk; it never makes
// them wait, and CpapDash being unreachable, the switch being off, or a 401
// changes nothing about what they collect.
//
// What it sends (D4): a file's new bytes as binary chunks (12-byte header,
// kind 3) under a req_id it picks, then a text `file_ready` naming the file in
// the mule's terms plus `chunks`, `size`, `md5` and `machine_serial`. Ingest
// answers each with `file_ack {req_id, ok, whole}` (D6), and only that moves
// the index (D5).
//
// What "new bytes" means (D4): a Bluetooth BRP or PLD is rewritten every 10 s
// with more records, and an EDF's header changes with its record count while
// the records before the end do not. So a growing signal file goes from the
// acked offset when the bytes after its header, up to that offset, are the
// ones acked; otherwise the whole file from 0, which ingest takes as a
// replacement. EVE, CSL, STR and everything else go whole whenever they
// change.
//
// The index (D5) is the cpapdash_sent table: nothing is held in memory that a
// restart could lose. On every connection the uploader walks every card root
// it holds and sends whatever is past acked_to, or the whole file when
// acked_md5 no longer matches (D8, D9). The switch going off, or on, clears it.
//
#include "database/IDatabase.h"
#include "services/CpapDashLink.h"

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hms_cpap {

namespace cdstream {

/// The binary frame kind for a file's bytes (cpapdash-socket Protocol.h, Sector).
inline constexpr uint16_t kKindFileBytes = 3;

/// md5 of bytes [from, to) of [bytes], lowercase hex.
std::string md5Hex(const std::string& bytes, std::size_t from, std::size_t to);

/// Is [name] an EDF by its name (.edf, any case)?
bool isEdfName(const std::string& name);

/// A signal file the machine grows by appending records: _BRP, _PLD, _SAD and
/// _SA2 .edf. These go as ranges; everything else goes whole when it changes.
bool isGrowingName(const std::string& name);

/// The header length an EDF's own header states (bytes 184-191), or 0 when
/// [bytes] is not an EDF.
std::size_t edfHeaderBytes(const std::string& bytes);

/// The md5 the index keeps for the first [upto] bytes of [bytes]: the bytes
/// after the EDF header for an EDF, all of [0, upto) for any other file.
std::string ackedMd5(const std::string& name, const std::string& bytes, std::size_t upto);

/// What to send of a file whose current contents are [bytes], given what was
/// acked of it ([row], nullopt when nothing was).
struct Plan {
    bool        send   = false;   ///< anything to send at all
    uint64_t    offset = 0;       ///< from this byte to the end
    bool        whole  = false;   ///< offset 0 because the file is new or changed before its end
    std::string why;              ///< for the log
};
Plan planSend(const std::string& name, const std::string& bytes,
              const std::optional<IDatabase::CpapDashSent>& row);

/// Is [rel] (card-relative, forward slashes) a card file to send? The root's
/// own files, SETTINGS/ and DATALOG/<YYYYMMDD>/; never hms-cpap's own folders
/// beside them (a bridge's batches/, an AirSense 11's stream/ and AS11/), a
/// hidden file, or a write still in progress (.tmp, .part).
bool isCardPath(const std::string& rel);

/// The upload fields the mule would send for a card path (ingest's
/// coyoteHeadersFor forms): STR.edf -> "STR"; another root file -> "STR" with
/// rel_dir "\"; DATALOG/<date>/<name> -> <date>; anything deeper -> its
/// parent folder with the full rel_dir.
struct CardHeaders {
    std::string date_folder;
    std::string rel_dir;
    std::string filename;
};
std::optional<CardHeaders> cardHeadersFor(const std::string& rel);

/// The 12-byte little-endian header of a binary frame: req_id u32, seq u32,
/// flags u16 (bit 0 = last), kind u16.
std::string binaryHeader(uint32_t req_id, uint32_t seq, bool last, uint16_t kind);

/// {"t":"hello","mode":"hms","fw":<version>,"state":{"source":<source>}}
std::string helloFrame(const std::string& version, const std::string& source);

/// The one source behind this install, for the hello (ingest closes a night
/// by it): "ble" when a Bluetooth reader is switched on (AirSense 11 or
/// DreamStation 2, directly or through the bridge client); otherwise
/// "bridge" when the Sleep Bridge receiver is on and a bridge has a card
/// folder under <data_dir>/sleepbridge; otherwise "ezshare" (ez Share,
/// FlashAir, a local card folder). The config names no Sleep Bridge choice
/// (the receiver is on by default and a bridge install writes no source), so
/// a bridge is known by its folder: a local card folder that IS a bridge's
/// card folder (under <data_dir>/sleepbridge) is "bridge" from the first
/// hello, before the bridge has rebuilt anything into it. Saying "ezshare"
/// there closed that install's first nights by the card rule, which a bridge
/// never satisfies, so they closed partial (2026-11-18).
std::string sourceKind(bool bluetooth_on, bool bridge_receiver_on, const std::string& data_dir,
                       const std::string& local_dir = "");

/// The text frame that closes a file's chunks (D4).
std::string fileReadyFrame(uint32_t req_id, const CardHeaders& h, uint64_t offset,
                           uint64_t total_size, uint32_t chunks, uint64_t size,
                           const std::string& md5, const std::string& machine_serial);

/// An ez Share burst's end, as ingest reads it (its mule card-round rule):
/// {"t":"round","done":true}. No count, no listing.
std::string roundFrame();

/// Now, as ISO-8601 UTC ("2026-11-16T03:04:05Z").
std::string isoNowUtc();

}  // namespace cdstream

class CpapDashStreamer {
public:
    /// From config.json's "cpapdash" block: the switch, and the SDD-057 token
    /// it shares with the equipment sync.
    struct Settings {
        bool        stream = false;
        std::string token;
    };

    /// A card root and the machine whose card it is ("" = read it from the
    /// root's Identification file).
    struct Root {
        std::string path;
        std::string machine;
    };

    struct Timing {
        std::chrono::milliseconds connect_timeout{15000};
        std::chrono::milliseconds hello_timeout{15000};
        std::chrono::milliseconds ack_timeout{120000};
        std::chrono::milliseconds backoff_min{2000};
        std::chrono::milliseconds backoff_max{300000};
        /// Nothing at all from the server for this long: the socket is dead.
        /// The socket pings every 30 s and gives up after 90 s of silence.
        std::chrono::milliseconds silence{120000};
        std::size_t               chunk_bytes = 32 * 1024;
    };

    struct Status {
        bool        stream = false;
        /// off | no_token | connecting | connected | unreachable | refused (a
        /// 401) | revoked (the gateway's close on a revoked token) | closed |
        /// unavailable
        std::string state = "off";
        int         http_status = 0;
        std::string reason;          ///< the server's or the network's words
        std::string since;           ///< when this state began
        std::string last_ack_at;
        long long   files_acked = 0; ///< since this process started
        long long   bytes_acked = 0;
        std::size_t queued = 0;      ///< roots and files waiting to be looked at
        std::string install_id;
        Json::Value toJson() const;
    };

    using LinkFactory   = std::function<std::unique_ptr<CpapDashLink>()>;
    using RootsProvider = std::function<std::vector<Root>()>;

    CpapDashStreamer(std::shared_ptr<IDatabase> db, LinkFactory factory, std::string install_id,
                     std::string version);
    ~CpapDashStreamer();

    CpapDashStreamer(const CpapDashStreamer&) = delete;
    CpapDashStreamer& operator=(const CpapDashStreamer&) = delete;

    void setTiming(const Timing& t);
    /// Every card root hms-cpap holds, asked on each connection (D8).
    void setRootsProvider(RootsProvider p);
    /// The install's source ("ezshare" | "bridge" | "ble"), asked on every
    /// hello so a change of source shows on the next connect.
    void setSourceProvider(std::function<std::string()> p);

    /// The settings now. The first call only sets them; after that, the switch
    /// going off or on clears the index (D5), and any change reconnects.
    void apply(const Settings& s);

    void start();
    void stop();

    /// The collectors' notes (card_feed). Never block, never fail.
    void fileWritten(const std::string& root, const std::string& rel, const std::string& machine);
    void rootWritten(const std::string& root, const std::string& machine);
    /// An ez Share burst over [root] ended, having reached the card. Queued
    /// behind what that burst wrote, so its frame goes only once those files
    /// are sent and acked, in order. Dropped unless the install's source is
    /// "ezshare": a bridge or Bluetooth install sends no round.
    void burstEnded(const std::string& root);

    Status status() const;

    /// The install id, made once and kept in <data_dir>/cpapdash_install_id.
    static std::string loadOrCreateInstallId(const std::string& data_dir);

    /// The card roots under [data_dir] (as11/<serial>/, sleepbridge/<sender>/)
    /// plus [card_roots] that exist (the ez Share archive, a local folder).
    static std::vector<Root> discoverRoots(const std::string& data_dir,
                                           const std::vector<std::string>& card_roots);

private:
    struct Work {
        std::string root;
        std::string rel;       // "" = look at the whole root
        std::string machine;
        bool        round = false;   // a burst's end: say so once what is ahead of it is acked
    };
    struct Stamp {
        uint64_t  size  = 0;
        long long mtime = 0;
        bool operator==(const Stamp& o) const { return size == o.size && mtime == o.mtime; }
    };
    enum class End { Stop, SettingsChanged, Bye, Closed, Silence, AckTimeout, HelloFailed };
    struct SessionEnd {
        End         end = End::Closed;
        int         retry_in_s = 0;
        std::string reason;
        bool        connected = false;
    };
    enum class SendResult { Done, Lost };

    void run();
    SessionEnd session(CpapDashLink& link, uint64_t gen);
    /// A frame that is not the one being waited for. Returns an end when the
    /// session is over.
    std::optional<SessionEnd> handle(CpapDashLink& link, const CpapDashLink::Frame& f);
    SendResult sendFile(CpapDashLink& link, const Work& w, uint64_t gen, SessionEnd& end);
    void scanRoot(const Work& w);
    std::string machineFor(const std::string& root, const std::string& given);

    void enqueue(Work w, bool front);
    std::optional<Work> popWork();
    void markAllRootsDirty();
    void doPendingClear();
    bool stopping() const { return stop_.load(); }
    bool changed(uint64_t gen) const;
    void waitForChange(uint64_t gen, std::optional<std::chrono::milliseconds> at_most);
    void setState(const std::string& state, int http_status = 0, const std::string& reason = "");

    std::shared_ptr<IDatabase> db_;
    LinkFactory                factory_;
    std::string                install_id_;
    std::string                version_;
    Timing                     timing_;
    RootsProvider              roots_provider_;
    std::function<std::string()> source_provider_;

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    Settings                settings_;
    bool                    applied_ = false;
    uint64_t                gen_ = 0;
    uint64_t                refused_gen_ = UINT64_MAX;   // a 401 stops it until the settings change
    bool                    clear_pending_ = false;
    std::deque<Work>        queue_;
    std::set<std::string>   queued_keys_;
    std::map<std::string, std::string> roots_;          // root -> machine, from the collectors
    Status                  status_;

    // Worker thread only.
    std::map<std::string, Stamp>       settled_;   // root\npath -> what was on disk when it matched the index
    std::map<std::string, std::string> machines_;  // root -> serial read from its card
    std::set<std::string>              told_no_serial_;
    uint32_t                           next_req_ = 1;

    std::atomic<bool> stop_{false};
    std::thread       worker_;
};

}  // namespace hms_cpap
