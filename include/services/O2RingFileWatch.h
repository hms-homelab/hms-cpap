#pragma once

/**
 * When to ask an O2 Ring for its files.
 *
 * A ring does not always list the recording the instant it goes off the
 * finger: the file can take about ten seconds to appear. So when a session
 * ends the ring is asked once, after a short settling delay, rather than at
 * the moment the change is seen and again on every poll after it.
 */
class O2RingFileWatch {
public:
    /// How long after a session's end is seen before the ring is asked for
    /// its files, in seconds. The ring can take about ten to list a new file.
    static constexpr int kSettleSeconds = 15;

    /// Called once per poll with what the poll saw; true means a session has
    /// just ended and, once kSettleSeconds have passed, the ring's files
    /// should be looked at.
    bool shouldCollect(bool active, bool reachable) {
        if (active) {                 // on the finger: a session is under way
            was_active_ = true;
            return false;
        }
        if (!reachable) return false;  // no answer says nothing; keep waiting
        if (!was_active_) return false;
        was_active_ = false;          // the session has just ended
        return true;
    }

private:
    bool was_active_ = false;
};
