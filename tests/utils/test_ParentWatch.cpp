// SDD-016: the die-with-parent watcher.
//
// This is the macOS half of a guarantee Windows gets from a Job Object, and it
// is the piece with no kernel enforcement behind it -- so it is worth testing
// properly rather than trusting. The watcher takes an fd precisely so these can
// drive it with a real pipe instead of hijacking the test process's stdin.

#include <gtest/gtest.h>

#include "utils/ParentWatch.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

/// Blocks until `flag` is set or the timeout expires. Returns whether it fired.
bool waitFor(std::atomic<bool>& flag, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return flag.load();
}

}  // namespace

#ifndef _WIN32

TEST(ParentWatch, ClosingTheWriteEndFiresTheCallback) {
    // The whole contract: the supervisor dies, its descriptors close, we notice.
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    std::atomic<bool> fired{false};
    hms_cpap::watchParentPipe(fds[0], [&fired] { fired = true; });

    EXPECT_FALSE(fired.load()) << "fired before the parent went anywhere";

    ::close(fds[1]);   // the supervisor exits
    EXPECT_TRUE(waitFor(fired, std::chrono::seconds(2)))
        << "the child never noticed the pipe close, so it would outlive its parent "
           "and hold the port";

    ::close(fds[0]);
}

TEST(ParentWatch, DataOnThePipeIsNotMistakenForDeath) {
    // Only closure counts. If a write killed the service, anything that ever
    // sent a byte down the channel -- a future handshake, a stray newline --
    // would shut the therapy collector down mid-session.
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    std::atomic<bool> fired{false};
    hms_cpap::watchParentPipe(fds[0], [&fired] { fired = true; });

    ASSERT_EQ(::write(fds[1], "hello", 5), 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_FALSE(fired.load()) << "a write was read as the parent dying";

    // ...and closing after the write still fires.
    ::close(fds[1]);
    EXPECT_TRUE(waitFor(fired, std::chrono::seconds(2)));

    ::close(fds[0]);
}

TEST(ParentWatch, TheCallbackFiresExactlyOnce) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    std::atomic<int> count{0};
    hms_cpap::watchParentPipe(fds[0], [&count] { ++count; });

    ::close(fds[1]);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(count.load(), 1)
        << "a repeated callback would run the shutdown path more than once";

    ::close(fds[0]);
}

TEST(ParentWatch, AnAlreadyClosedPipeFiresImmediately) {
    // The parent could die between spawn and the watcher starting. That race
    // must resolve as "gone", not as a watcher blocked forever on a dead pipe.
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    ::close(fds[1]);

    std::atomic<bool> fired{false};
    hms_cpap::watchParentPipe(fds[0], [&fired] { fired = true; });
    EXPECT_TRUE(waitFor(fired, std::chrono::seconds(2)))
        << "the parent died before we started watching and we never noticed";

    ::close(fds[0]);
}

#endif  // !_WIN32
