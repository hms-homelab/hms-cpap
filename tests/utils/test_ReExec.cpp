// The in-place restart's hand-over (utils/ReExec.h). The Windows wait itself is
// proven on CpapDash-Win (a restart that leaves one process serving); these
// pin what can be checked everywhere.
#include <gtest/gtest.h>

#include "utils/ReExec.h"

#include <cstdlib>

using hms_cpap::reexec::kParentEnv;
using hms_cpap::reexec::parentPidFrom;
using hms_cpap::reexec::waitForReplacedProcess;

TEST(ReExec, APidIsReadOnlyWhenItIsAllDigits) {
    EXPECT_EQ(parentPidFrom("7104"), 7104ul);
    EXPECT_EQ(parentPidFrom(nullptr), std::nullopt);
    EXPECT_EQ(parentPidFrom(""), std::nullopt);
    EXPECT_EQ(parentPidFrom("0"), std::nullopt) << "pid 0 is the idle process, never ours";
    EXPECT_EQ(parentPidFrom("71a4"), std::nullopt);
    EXPECT_EQ(parentPidFrom("-5"), std::nullopt);
    EXPECT_EQ(parentPidFrom(" 12"), std::nullopt);
    EXPECT_EQ(parentPidFrom("99999999999"), std::nullopt) << "larger than any pid";
}

TEST(ReExec, StartingWithNoParentReturnsAtOnce) {
    // A normal start: nothing to wait for. Must not block.
#ifdef _WIN32
    _putenv_s(kParentEnv, "");
#else
    ::unsetenv(kParentEnv);
#endif
    waitForReplacedProcess();
    SUCCEED();
}

TEST(ReExec, AParentThatIsAlreadyGoneIsNotWaitedFor) {
    // The old process may be gone before the new one looks: no wait then.
    // On POSIX the call does nothing at all (execv kept the process).
#ifdef _WIN32
    _putenv_s(kParentEnv, "4294967290");
    waitForReplacedProcess();
    EXPECT_EQ(std::getenv(kParentEnv) == nullptr || *std::getenv(kParentEnv) == '\0', true)
        << "cleared, so nothing started later inherits a stale pid";
#else
    ::setenv(kParentEnv, "4294967290", 1);
    waitForReplacedProcess();
    ::unsetenv(kParentEnv);
#endif
    SUCCEED();
}
