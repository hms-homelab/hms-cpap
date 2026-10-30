#pragma once
//
// Where a Sleep Bridge's batches land, and the answer each one gets.
//
//   <root>/<sender>/                  the card root the rebuild writes into
//   <root>/<sender>/batches/          everything the receiver keeps:
//       <first>-<last>.cys            a batch as it came (CYS2 expanded)
//       cards/<next>-<ms>-<n>.json    a card statement, named by the first
//                                     sequence it comes before (mark + 1)
//       cursor.json                   the high-water mark
//       gaps.jsonl                    one line per hole in the sequence
//
// The order is the whole point: the batch's bytes are written to a temporary
// name, fsynced and renamed; the cursor is saved the same way after them; the
// answer is built last. A crash anywhere before the cursor save leaves the
// mark where it was, so the bridge sends the batch again and it lands on the
// same name: stored once. A batch is never acked that is not on disk.
//
// One batch at a time across every bridge (one mutex): a cursor is read,
// decided on and written back, and two batches interleaving there would both
// see the old mark.
//
#include "sleepbridge/Wire.h"

#include <json/json.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::sleepbridge {

/// The answer to one batch: an HTTP-style status and the body the bridge
/// reads. `ack` is in every answer that knows the mark, including refusals,
/// so a bridge can skip a batch it will never get stored.
struct Answer {
    int         status{200};
    Json::Value body{Json::objectValue};
};

/// What a stored batch was, for whoever rebuilds or reports on it.
struct Stored {
    bool    with_records{false};   // a batch (not a heartbeat or statement)
    bool    card{false};
    bool    gap{false};
};

/// Write `len` bytes to `path` so the path holds all of them or what it held
/// before: a temporary name in the same folder, flushed to disk, renamed over.
bool durableWrite(const std::string& path, const void* data, size_t len, std::string* error);

class BatchStore {
public:
    explicit BatchStore(std::string root);

    /// One batch body (CYS1, CYS2, CYR1, heartbeat or card statement) from
    /// `sender`. `stored`, when given, says what it was on a 200.
    Answer handle(const std::string& sender, const std::string& machine,
                  const uint8_t* body, size_t len, Stored* stored = nullptr);

    const std::string& root() const { return root_; }
    std::string cardRoot(const std::string& sender) const;
    std::string batchesDir(const std::string& sender) const;

    /// The saved mark, read from disk on first use. nullopt when the file is
    /// there but unreadable: no number is better than a wrong one.
    std::optional<CursorState> cursorOf(const std::string& sender);

    /// Bridges that have a folder here.
    std::vector<std::string> senders() const;

    /// Test seam: the next cursor save fails, as a crash between the batch's
    /// fsync and the ack would leave things.
    void failNextCursorSave() { fail_next_cursor_save_ = true; }

private:
    bool saveCursor(const std::string& sender, const CursorState& s, std::string* error);

    std::string root_;
    std::mutex  mu_;
    std::map<std::string, CursorState> cursors_;
    uint64_t    card_counter_{0};
    bool        fail_next_cursor_save_{false};
};

}  // namespace hms_cpap::sleepbridge
