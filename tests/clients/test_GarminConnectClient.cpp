// SDD-078 section 4: the Garmin Connect client against a local fake of
// Garmin's sign-in, token and data endpoints. No real account is used.
#include "clients/GarminConnectClient.h"

#include "../services/FakeGarminServer.h"

#include <gtest/gtest.h>

using namespace hms_cpap;

namespace {

GarminEndpoints at(const garminfake::FakeGarmin& g) {
    GarminEndpoints e;
    e.sso        = g.base();
    e.diauth     = g.base() + "/di-oauth2-service/oauth/token";
    e.connectapi = g.base();
    return e;
}

}  // namespace

TEST(GarminConnectClient, SignsInWithThePasswordAndGetsTokens) {
    garminfake::FakeGarmin g;
    GarminConnectClient c(at(g));
    const auto a = c.signIn("a@example.com", "right-password");
    ASSERT_EQ(a.state, GarminAuth::Ok) << a.message;
    EXPECT_FALSE(c.tokens().access_token.empty());
    EXPECT_EQ(c.tokens().refresh_token, "refresh-1");
    EXPECT_EQ(c.tokens().client_id, "GARMIN_CONNECT_MOBILE_ANDROID_DI") << "read from the token's client_id claim";
    // The page first (its cookie went back with the credentials), then the
    // login, then the exchange: the two newer client ids are refused first.
    EXPECT_EQ(g.server.count("/mobile/sso/en_US/sign-in"), 1u);
    EXPECT_EQ(g.server.count("/mobile/api/login"), 1u);
    EXPECT_EQ(g.server.count("/di-oauth2-service/oauth/token"), 3u);
    for (const auto& r : g.server.requests()) {
        if (r.path != "/di-oauth2-service/oauth/token") continue;
        EXPECT_EQ(r.headers.at("authorization").rfind("Basic ", 0), 0u);
        EXPECT_NE(r.body.find("service_ticket=ST-1"), std::string::npos) << r.body;
    }
}

TEST(GarminConnectClient, ARefusedPasswordIsSaidAsSuch) {
    garminfake::FakeGarmin g;
    GarminConnectClient c(at(g));
    const auto a = c.signIn("a@example.com", "wrong");
    EXPECT_EQ(a.state, GarminAuth::BadPassword);
    EXPECT_TRUE(c.tokens().empty());
    EXPECT_FALSE(c.mfaPending());
}

TEST(GarminConnectClient, MfaIsAskedAndAnswered) {
    garminfake::FakeGarmin g;
    g.mfa = true;
    GarminConnectClient c(at(g));
    auto a = c.signIn("a@example.com", "right-password");
    ASSERT_EQ(a.state, GarminAuth::MfaRequired);
    EXPECT_TRUE(c.mfaPending());
    EXPECT_TRUE(c.tokens().empty());

    a = c.verifyMfa("000000");
    EXPECT_EQ(a.state, GarminAuth::BadMfaCode);
    EXPECT_TRUE(c.mfaPending()) << "a wrong code leaves the sign-in open for another";

    a = c.verifyMfa("123456");
    ASSERT_EQ(a.state, GarminAuth::Ok) << a.message;
    EXPECT_FALSE(c.mfaPending());
    EXPECT_FALSE(c.tokens().access_token.empty());
    const auto reqs = g.server.requests();
    bool sent_method = false;
    for (const auto& r : reqs)
        if (r.path == "/mobile/api/mfa/verifyCode")
            sent_method |= r.body.find("\"mfaMethod\":\"email\"") != std::string::npos &&
                           r.headers.count("cookie") && r.headers.at("cookie").find("SESSION=s1") != std::string::npos;
    EXPECT_TRUE(sent_method) << "the code goes back on the same SSO session, with Garmin's method";
}

TEST(GarminConnectClient, AChangedSignInPageIsTheClearError) {
    garminfake::FakeGarmin g;
    g.changed_sign_in = true;
    GarminConnectClient c(at(g));
    const auto a = c.signIn("a@example.com", "right-password");
    EXPECT_EQ(a.state, GarminAuth::Changed);
    EXPECT_EQ(a.message, "Garmin changed its sign-in; update hms-cpap");
}

TEST(GarminConnectClient, TokensAreReusedWithoutASignIn) {
    garminfake::FakeGarmin g;
    GarminConnectClient first(at(g));
    ASSERT_TRUE(first.signIn("a@example.com", "right-password").ok());
    const auto saved = first.tokens();

    // A new start: the saved tokens, no password.
    GarminConnectClient c(at(g));
    c.setTokens(saved);
    const auto before = g.server.count("/mobile/api/login");
    const auto p = c.socialProfile();
    ASSERT_TRUE(p.ok) << p.message;
    EXPECT_EQ(p.body["displayName"], "runner-42");
    EXPECT_EQ(g.server.count("/mobile/api/login"), before) << "no new sign-in";
    EXPECT_EQ(g.refreshes, 0);
}

TEST(GarminConnectClient, AnExpiringTokenIsRefreshedFirst) {
    garminfake::FakeGarmin g;
    g.token_exp = 1000;   // the token says it ends at t=1000
    GarminConnectClient c(at(g));
    ASSERT_TRUE(c.signIn("a@example.com", "right-password").ok());
    c.setNowForTest(500);   // within 15 minutes of exp
    const auto old = c.tokens().access_token;
    const auto p = c.socialProfile();
    EXPECT_EQ(g.refreshes, 1);
    EXPECT_NE(c.tokens().access_token, old);
    EXPECT_EQ(c.tokens().refresh_token, "refresh-2") << "the rotated refresh token is kept";
    EXPECT_TRUE(p.ok) << p.message;
}

TEST(GarminConnectClient, A401RefreshesOnceAndRetries) {
    garminfake::FakeGarmin g;
    GarminConnectClient c(at(g));
    ASSERT_TRUE(c.signIn("a@example.com", "right-password").ok());
    auto stale = c.tokens();
    stale.access_token = garminfake::jwt(R"({"exp":4102444800,"client_id":"GARMIN_CONNECT_MOBILE_ANDROID_DI"})");
    c.setTokens(stale);   // Garmin no longer takes this one
    const auto p = c.lastUsedDevice();
    EXPECT_TRUE(p.ok) << p.message;
    EXPECT_EQ(g.refreshes, 1);
    EXPECT_EQ(p.body["lastUsedDeviceName"], "fenix 7X");
}

TEST(GarminConnectClient, ARefusedRefreshSaysANewSignInIsNeeded) {
    garminfake::FakeGarmin g;
    g.refuse_refresh = true;
    GarminConnectClient c(at(g));
    ASSERT_TRUE(c.signIn("a@example.com", "right-password").ok());
    auto stale = c.tokens();
    stale.access_token = "not-garmins";
    c.setTokens(stale);
    const auto p = c.socialProfile();
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.auth, GarminAuth::Expired);
}

TEST(GarminConnectClient, TheSleepCallAsksForTheDayWithTheBuffer) {
    garminfake::FakeGarmin g;
    g.sleep_by_date["2026-11-19"] = R"({"dailySleepDTO":{"id":1}})";
    GarminConnectClient c(at(g));
    ASSERT_TRUE(c.signIn("a@example.com", "right-password").ok());
    const auto s = c.sleepData("runner-42", "2026-11-19");
    ASSERT_TRUE(s.ok) << s.message;
    EXPECT_EQ(s.body["dailySleepDTO"]["id"], 1);
    bool asked = false;
    for (const auto& r : g.server.requests())
        if (r.path == "/wellness-service/wellness/dailySleepData/runner-42")
            asked = r.query == "date=2026-11-19&nonSleepBufferMinutes=60";
    EXPECT_TRUE(asked);
}

TEST(GarminConnectClient, NothingAnsweringIsUnreachable) {
    GarminEndpoints e;
    e.sso = "http://127.0.0.1:9";   // discard port: nothing listens
    GarminConnectClient c(e);
    EXPECT_EQ(c.signIn("a@example.com", "x").state, GarminAuth::Unreachable);
}

TEST(GarminConnectClient, ExpiryIsReadFromTheJwt) {
    const auto t = garminfake::jwt(R"({"exp":10000})");
    EXPECT_FALSE(GarminConnectClient::expiresSoon(t, 9000));
    EXPECT_TRUE(GarminConnectClient::expiresSoon(t, 9200)) << "within 15 minutes";
    EXPECT_FALSE(GarminConnectClient::expiresSoon("not-a-jwt", 1)) << "unreadable is taken as good";
    EXPECT_EQ(GarminConnectClient::basicAuth("ABC"), "Basic QUJDOg==");
}
