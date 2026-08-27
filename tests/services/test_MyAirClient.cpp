/**
 * SDD-020: the myAir client's pure halves.
 *
 * The network flow cannot be unit tested without standing up a fake Okta, and a
 * fake Okta would only ever prove that our fake matches our code. What CAN be
 * pinned here is everything that would be silently wrong against the real thing:
 *
 *   - PKCE, against RFC 7636's own published test vector, so a broken challenge
 *     is caught here rather than as an opaque "invalid_grant" from ResMed;
 *   - reading the authorization code out of a redirect FRAGMENT, which is the
 *     one step that looks like a failed login when you get it wrong;
 *   - lifting the country claim out of an id_token we do not verify;
 *   - the shape of ResMed's GraphQL responses, so a change on their side is a
 *     test failure with a name on it rather than a table of zeroes.
 */

#include <gtest/gtest.h>

#include "services/MyAirClient.h"

#include <string>
#include <vector>

using namespace hms_cpap;

// ─────────────────────────────────────────────────────────────────────────────
// PKCE
// ─────────────────────────────────────────────────────────────────────────────

TEST(MyAirClient, PkceMatchesTheRfcTestVector) {
    // RFC 7636 appendix B, verbatim. If this passes, our S256 is the S256 the
    // rest of the world means.
    const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    EXPECT_EQ(MyAirClient::pkceChallenge(verifier),
              "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST(MyAirClient, PkceChallengeIsUnpaddedBase64Url) {
    const std::string c = MyAirClient::pkceChallenge("anything at all");
    EXPECT_EQ(c.size(), 43u) << "SHA-256 base64url unpadded is always 43 characters";
    EXPECT_EQ(c.find('='), std::string::npos) << "padding is not allowed";
    EXPECT_EQ(c.find('+'), std::string::npos) << "'+' is base64, not base64url";
    EXPECT_EQ(c.find('/'), std::string::npos) << "'/' is base64, not base64url";
}

TEST(MyAirClient, CodeVerifiersAreLongAndNotRepeated) {
    const std::string a = MyAirClient::makeCodeVerifier();
    const std::string b = MyAirClient::makeCodeVerifier();
    EXPECT_GE(a.size(), 43u) << "RFC 7636 sets a floor of 43 characters";
    EXPECT_NE(a, b) << "two logins in a row must not share a verifier";
    for (char c : a) {
        EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)))
            << "unexpected character in a verifier: " << c;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The redirect
// ─────────────────────────────────────────────────────────────────────────────

TEST(MyAirClient, ReadsTheCodeOutOfTheFragment) {
    const std::string location =
        "https://myair.resmed.com#code=aBc123-_xyz&state=hmscpap";
    EXPECT_EQ(MyAirClient::codeFromRedirect(location), "aBc123-_xyz");
}

TEST(MyAirClient, IgnoresACodeInTheQueryString) {
    // The authorize call asks for response_mode=fragment. A code arriving as a
    // query parameter means something changed, and quietly accepting it would
    // hide that. There is nothing after the '#', so there is nothing to take.
    const std::string location = "https://myair.resmed.com?code=fromquery";
    EXPECT_EQ(MyAirClient::codeFromRedirect(location), "");
}

TEST(MyAirClient, HandlesAFragmentWithoutACode) {
    EXPECT_EQ(MyAirClient::codeFromRedirect("https://myair.resmed.com#error=login_required"), "");
    EXPECT_EQ(MyAirClient::codeFromRedirect("https://myair.resmed.com"), "");
    EXPECT_EQ(MyAirClient::codeFromRedirect(""), "");
}

TEST(MyAirClient, DoesNotMatchAParameterMerelyEndingInCode) {
    const std::string location = "https://myair.resmed.com#othercode=nope&code=yes";
    EXPECT_EQ(MyAirClient::codeFromRedirect(location), "yes");
}

// ─────────────────────────────────────────────────────────────────────────────
// The id_token claim
// ─────────────────────────────────────────────────────────────────────────────

TEST(MyAirClient, ReadsTheCountryClaimWithoutVerifying) {
    // header {"alg":"none"} . payload {"myAirCountryId":"US","sub":"x"} . sig
    const std::string jwt =
        "eyJhbGciOiJub25lIn0."
        "eyJteUFpckNvdW50cnlJZCI6IlVTIiwic3ViIjoieCJ9."
        "not-a-real-signature";
    EXPECT_EQ(MyAirClient::unverifiedJwtClaim(jwt, "myAirCountryId"), "US");
    EXPECT_EQ(MyAirClient::unverifiedJwtClaim(jwt, "sub"), "x");
    EXPECT_EQ(MyAirClient::unverifiedJwtClaim(jwt, "absent"), "");
}

TEST(MyAirClient, MalformedTokensYieldNothingRatherThanThrowing) {
    EXPECT_EQ(MyAirClient::unverifiedJwtClaim("", "myAirCountryId"), "");
    EXPECT_EQ(MyAirClient::unverifiedJwtClaim("only-one-part", "myAirCountryId"), "");
    EXPECT_EQ(MyAirClient::unverifiedJwtClaim("two.parts", "myAirCountryId"), "");
    EXPECT_EQ(MyAirClient::unverifiedJwtClaim("a.!!!not-base64!!!.c", "myAirCountryId"), "");
}

// ─────────────────────────────────────────────────────────────────────────────
// Region configuration
// ─────────────────────────────────────────────────────────────────────────────

TEST(MyAirClient, RegionUrlsAreBuiltFromTheRightPieces) {
    const auto& na = myAirRegionNA();
    EXPECT_EQ(na.authnUrl(), "https://resmed-ext-1.okta.com/api/v1/authn");
    EXPECT_EQ(na.authorizeUrl(),
              "https://resmed-ext-1.okta.com/oauth2/aus4ccsxvnidQgLmA297/v1/authorize");
    EXPECT_EQ(na.tokenUrl(),
              "https://resmed-ext-1.okta.com/oauth2/aus4ccsxvnidQgLmA297/v1/token");

    const auto& eu = myAirRegionEU();
    EXPECT_EQ(eu.authnUrl(), "https://id.resmed.eu/api/v1/authn");
    EXPECT_NE(eu.graphql_url, na.graphql_url) << "the two regions are not the same backend";
    EXPECT_NE(eu.api_key, na.api_key);
}

TEST(MyAirClient, RegionLookupIsForgivingAndDefaultsToNorthAmerica) {
    EXPECT_EQ(&myAirRegion("EU"), &myAirRegionEU());
    EXPECT_EQ(&myAirRegion("eu"), &myAirRegionEU());
    EXPECT_EQ(&myAirRegion("NA"), &myAirRegionNA());
    // Anything unrecognised falls back rather than failing to build a client at
    // all, and for AU that fallback is the RIGHT answer rather than a lucky one:
    // ResMed serves Australia from the North America endpoints. The long-running
    // Home Assistant integration labels its own picker "North America and
    // Australia", and offers no third region.
    EXPECT_EQ(&myAirRegion("AU"), &myAirRegionNA());
    EXPECT_EQ(&myAirRegion(""), &myAirRegionNA());
}

TEST(MyAirClient, TheMfaUrlAsksOktaToRememberTheDevice) {
    // Without rememberDevice=true ResMed emails a code on every single start,
    // which on a headless service means it can never sign in unattended.
    const std::string url = myAirRegionEU().mfaUrl("emfg9cmjqxEPr52cT417");
    EXPECT_NE(url.find("/api/v1/authn/factors/emfg9cmjqxEPr52cT417/verify"), std::string::npos);
    EXPECT_NE(url.find("rememberDevice=true"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Response parsing
// ─────────────────────────────────────────────────────────────────────────────

namespace {

const char* kSleepRecordsBody = R"({
  "data": {
    "getPatientWrapper": {
      "patient": {"firstName": "Albin"},
      "sleepRecords": {
        "items": [
          {"startDate":"2026-08-24","totalUsage":432,"sleepScore":91,"usageScore":70,
           "ahiScore":5,"maskScore":16,"leakScore":0,"ahi":1.4,"maskPairCount":2,
           "leakPercentile":18.5,"sleepRecordPatientId":"abc"},
          {"startDate":"2026-08-25","totalUsage":80,"sleepScore":34,"usageScore":13,
           "ahiScore":5,"maskScore":16,"leakScore":0,"ahi":0.0,"maskPairCount":13,
           "leakPercentile":1.2,"sleepRecordPatientId":"abc"}
        ]
      }
    }
  }
})";

}  // namespace

TEST(MyAirClient, ParsesSleepRecordsIncludingEverySubScore) {
    std::vector<MyAirSleepRecord> records;
    std::string err;
    ASSERT_TRUE(MyAirClient::parseSleepRecords(kSleepRecordsBody, records, err)) << err;
    ASSERT_EQ(records.size(), 2u);

    const auto& first = records[0];
    EXPECT_EQ(first.start_date, "2026-08-24");
    EXPECT_DOUBLE_EQ(first.total_usage_min, 432.0);
    EXPECT_EQ(first.sleep_score, 91);
    // The sub-scores are the whole point: without them the comparison could only
    // hold one opaque number against another.
    EXPECT_EQ(first.usage_score, 70);
    EXPECT_EQ(first.ahi_score, 5);
    EXPECT_EQ(first.mask_score, 16);
    EXPECT_EQ(first.leak_score, 0);
    EXPECT_DOUBLE_EQ(first.ahi, 1.4);
    EXPECT_EQ(first.mask_pair_count, 2);
    EXPECT_DOUBLE_EQ(first.leak_percentile, 18.5);

    EXPECT_EQ(records[1].start_date, "2026-08-25");
    EXPECT_EQ(records[1].mask_pair_count, 13);
}

TEST(MyAirClient, AcceptsNumbersSentAsStrings) {
    const char* body = R"({"data":{"getPatientWrapper":{"sleepRecords":{"items":[
        {"startDate":"2026-08-24","totalUsage":"432","sleepScore":"91","ahi":"1.4"}]}}}})";
    std::vector<MyAirSleepRecord> records;
    std::string err;
    ASSERT_TRUE(MyAirClient::parseSleepRecords(body, records, err)) << err;
    ASSERT_EQ(records.size(), 1u);
    EXPECT_DOUBLE_EQ(records[0].total_usage_min, 432.0);
    EXPECT_EQ(records[0].sleep_score, 91);
    EXPECT_DOUBLE_EQ(records[0].ahi, 1.4);
}

TEST(MyAirClient, SkipsANightWithNoDate) {
    const char* body = R"({"data":{"getPatientWrapper":{"sleepRecords":{"items":[
        {"totalUsage":432,"sleepScore":91},
        {"startDate":"2026-08-24","totalUsage":100}]}}}})";
    std::vector<MyAirSleepRecord> records;
    std::string err;
    ASSERT_TRUE(MyAirClient::parseSleepRecords(body, records, err)) << err;
    ASSERT_EQ(records.size(), 1u) << "a record with no date cannot be filed against a night";
    EXPECT_EQ(records[0].start_date, "2026-08-24");
}

TEST(MyAirClient, AnEmptyWindowIsSuccessWithNothingInIt) {
    // A brand new account, or a month with no therapy, is not an error.
    const char* body = R"({"data":{"getPatientWrapper":{"sleepRecords":{"items":[]}}}})";
    std::vector<MyAirSleepRecord> records;
    std::string err;
    EXPECT_TRUE(MyAirClient::parseSleepRecords(body, records, err)) << err;
    EXPECT_TRUE(records.empty());
}

TEST(MyAirClient, GraphQlErrorsAreReportedRatherThanSwallowed) {
    const char* body = R"({"errors":[{"message":"Unauthorized"}],"data":null})";
    std::vector<MyAirSleepRecord> records;
    std::string err;
    EXPECT_FALSE(MyAirClient::parseSleepRecords(body, records, err));
    EXPECT_NE(err.find("Unauthorized"), std::string::npos) << "err was: " << err;
}

TEST(MyAirClient, RubbishIsRejectedWithAnExplanation) {
    std::vector<MyAirSleepRecord> records;
    std::string err;
    EXPECT_FALSE(MyAirClient::parseSleepRecords("<html>maintenance</html>", records, err));
    EXPECT_FALSE(err.empty());

    err.clear();
    EXPECT_FALSE(MyAirClient::parseSleepRecords("{\"data\":{}}", records, err));
    EXPECT_FALSE(err.empty());
}

TEST(MyAirClient, ParsesTheDeviceAndItsMask) {
    const char* body = R"({"data":{"getPatientWrapper":{
        "masks":[{"maskCode":"63801"}],
        "fgDevices":[{"serialNumber":"23243570851","localizedName":"AirSense 10 AutoSet",
                      "deviceSeries":"AirSense 10","deviceFamily":"Series10",
                      "lastSleepDataReportTime":"2026-08-25T11:02:03Z",
                      "fgDeviceManufacturerName":"ResMed"}]}}})";
    MyAirDevice device;
    std::string err;
    ASSERT_TRUE(MyAirClient::parseDevice(body, device, err)) << err;
    EXPECT_EQ(device.serial_number, "23243570851");
    EXPECT_EQ(device.localized_name, "AirSense 10 AutoSet");
    EXPECT_EQ(device.device_series, "AirSense 10");
    EXPECT_EQ(device.manufacturer_name, "ResMed");
    EXPECT_EQ(device.mask_code, "63801");
}

TEST(MyAirClient, ADeviceWithNoMaskIsStillADevice) {
    const char* body = R"({"data":{"getPatientWrapper":{
        "masks":[],
        "fgDevices":[{"serialNumber":"123","localizedName":"AirCurve 10"}]}}})";
    MyAirDevice device;
    std::string err;
    ASSERT_TRUE(MyAirClient::parseDevice(body, device, err)) << err;
    EXPECT_EQ(device.serial_number, "123");
    EXPECT_TRUE(device.mask_code.empty());
}

TEST(MyAirClient, AnAccountWithNoMachineIsAnError) {
    const char* body = R"({"data":{"getPatientWrapper":{"masks":[],"fgDevices":[]}}})";
    MyAirDevice device;
    std::string err;
    EXPECT_FALSE(MyAirClient::parseDevice(body, device, err));
    EXPECT_FALSE(err.empty());
}

TEST(MyAirClient, RefusesToSignInWithoutCredentials) {
    // Cheap, but it is the difference between a clear message and a pointless
    // round trip to Okta on a misconfigured install.
    MyAirClient client("", "", "NA");
    std::string err;
    EXPECT_EQ(client.connect(err), MyAirAuthState::Failed);
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(client.isAuthenticated());
}
