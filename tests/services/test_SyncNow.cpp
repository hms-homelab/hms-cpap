/**
 * HMS-CPAP sync-now Unit Tests (SDD-005 phase 1)
 *
 * `POST /api/sync/now` backs the tray's "Sync Now" item. The controller that
 * serves it is excluded from this test binary (tests/CMakeLists.txt filters
 * src/controllers/ and src/web/ out of PROJECT_SOURCES), so the decision it
 * makes deliberately lives in BurstCollectorService::decideSyncNow as a pure
 * static, shaped after the existing decideFysetcLifecycle. That is what these
 * tests drive.
 *
 * The state machine is small but the ordering behind it is not obvious, so
 * each case below states WHY the answer is what it is rather than just
 * asserting the enum.
 */

#include <gtest/gtest.h>

#include "services/BurstCollectorService.h"

#include <string>

using namespace hms_cpap;
using Outcome = BurstCollectorService::SyncNowOutcome;

// ── the pure decision ────────────────────────────────────────────────────

TEST(SyncNowDecision, IdleWorkerAcceptsTheRequest) {
    // Nothing running, nothing pending: the worker is asleep between cycles
    // and the request will cut that sleep short.
    EXPECT_EQ(BurstCollectorService::decideSyncNow(true, false, false), Outcome::Requested);
}

TEST(SyncNowDecision, RequestDuringARunningCycleIsNotQueued) {
    // The caller asked for a sync and a sync is happening right now. Queueing
    // would run a second, pointless cycle against an unchanged card the
    // instant the first finished, so this reports the truth instead.
    EXPECT_EQ(BurstCollectorService::decideSyncNow(true, true, false), Outcome::AlreadyRunning);
}

TEST(SyncNowDecision, RapidRepeatsCollapseIntoOnePendingRequest) {
    // Someone leaning on the tray item must not stack up cycles.
    EXPECT_EQ(BurstCollectorService::decideSyncNow(true, false, true), Outcome::AlreadyRequested);
}

TEST(SyncNowDecision, StoppedServiceHasNothingToWake) {
    EXPECT_EQ(BurstCollectorService::decideSyncNow(false, false, false), Outcome::NotRunning);
}

TEST(SyncNowDecision, NotRunningWinsOverEveryOtherState) {
    // If the worker is stopped, whatever the other two flags say is stale
    // state and must not produce a promise we cannot keep.
    EXPECT_EQ(BurstCollectorService::decideSyncNow(false, true, false),  Outcome::NotRunning);
    EXPECT_EQ(BurstCollectorService::decideSyncNow(false, false, true),  Outcome::NotRunning);
    EXPECT_EQ(BurstCollectorService::decideSyncNow(false, true, true),   Outcome::NotRunning);
}

TEST(SyncNowDecision, RunningCycleWinsOverAPendingRequest) {
    // Both flags set is reachable: a request lands, the worker wakes and
    // starts a cycle, and the flag is cleared a moment later. Reporting
    // "already running" is the more useful of the two truths.
    EXPECT_EQ(BurstCollectorService::decideSyncNow(true, true, true), Outcome::AlreadyRunning);
}

// ── the enum's wire form ─────────────────────────────────────────────────

TEST(SyncNowDecision, OutcomeStringsAreStable) {
    // These strings go out in the API body and the tray reads them, so they
    // are contract, not debug text.
    EXPECT_STREQ(BurstCollectorService::syncNowOutcomeString(Outcome::Requested),
                 "requested");
    EXPECT_STREQ(BurstCollectorService::syncNowOutcomeString(Outcome::AlreadyRunning),
                 "already_running");
    EXPECT_STREQ(BurstCollectorService::syncNowOutcomeString(Outcome::AlreadyRequested),
                 "already_requested");
    EXPECT_STREQ(BurstCollectorService::syncNowOutcomeString(Outcome::NotRunning),
                 "not_running");
}

// ── the instance wiring ──────────────────────────────────────────────────

TEST(SyncNowRequest, FreshServiceReportsNotRunning) {
    // A constructed-but-never-started collector has running_ == false, so the
    // web thread gets an honest "not_running" rather than a request that
    // nothing will ever service.
    //
    // Driving the Requested path on a real instance would mean start()ing the
    // worker, which opens a data source and touches the filesystem. That is
    // exactly what decideSyncNow being pure lets these tests avoid, and why
    // the branch coverage above lives on the static.
    BurstCollectorService svc(300);
    EXPECT_EQ(svc.requestSyncNow(), Outcome::NotRunning);
}

TEST(SyncNowRequest, RepeatedCallsOnAStoppedServiceStayNotRunning) {
    BurstCollectorService svc(300);
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(svc.requestSyncNow(), Outcome::NotRunning);
    }
    EXPECT_FALSE(svc.isRunning());
}
