#pragma once

/**
 * When to ask an O2 Ring for its files.
 *
 * A ring does not always list the recording the instant it goes off the
 * finger: the file can take several seconds to appear, and an O2Ring-S file
 * may be listed before it is whole. So when a session ends the ring's file is
 * marked as owed, and the ring is asked on each poll where it answers and is
 * off the finger, until an ask saves a new file or kMaxTries asks have been
 * made. Nothing waits and nothing sleeps: the next poll is the retry.
 */
class O2RingFileWatch {
public:
    /// How many asks one session's end buys before the file is given up on
    /// until the next session ends.
    static constexpr int kMaxTries = 3;

    /// Called once per poll with what the poll saw. True means the ring should
    /// be asked for its files now: a session has ended, its file is still
    /// owed, the ring answered, and it is off the finger.
    bool shouldCollect(bool active, bool reachable) {
        if (active) {                 // on the finger: a session is under way
            was_active_ = true;
            return false;
        }
        if (!reachable) return false;  // no answer says nothing; keep waiting
        if (was_active_) {            // the session has just ended
            was_active_ = false;
            owed_ = true;
            tries_ = 0;
        }
        if (!owed_) return false;
        if (tries_ >= kMaxTries) {    // asked enough; the next session starts over
            owed_ = false;
            return false;
        }
        ++tries_;
        return true;
    }

    /// What the ask found: a new file saved clears the debt.
    void collected(bool saved) {
        if (saved) owed_ = false;
    }

    /// The ask just made is the first since a session ended.
    bool firstTry() const { return tries_ == 1; }
    int tries() const { return tries_; }
    bool owed() const { return owed_; }

private:
    bool was_active_ = false;
    bool owed_ = false;
    int tries_ = 0;
};
