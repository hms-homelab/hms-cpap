/**
 * HMS-CPAP DeviceDiscoveryService Unit Tests (SDD-005 phase 1)
 *
 * Every test here runs against a byte buffer built in-process. Nothing opens
 * a socket and nothing joins a multicast group, which is the whole reason the
 * DNS-SD parser was hand-rolled instead of pulled in as a dependency: the
 * obvious library parses inside its own recvfrom() and cannot be driven with
 * captured packets.
 *
 * The builder below emits real DNS wire format rather than hex blobs so the
 * fixtures stay readable and a failing test points at a record, not an offset.
 */

#include <gtest/gtest.h>

#include "services/DeviceDiscoveryService.h"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hms_cpap;

namespace {

constexpr const char* kSvc      = "_cpapdash._tcp.local";
constexpr const char* kInstance = "CPD-0007";
constexpr const char* kFqdn     = "CPD-0007._cpapdash._tcp.local";
constexpr const char* kHostname = "cpapdash.local";

void u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

void u32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 24));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

void writeName(std::vector<uint8_t>& b, const std::string& dotted) {
    size_t start = 0;
    while (start <= dotted.size()) {
        const size_t dot = dotted.find('.', start);
        const std::string label =
            (dot == std::string::npos) ? dotted.substr(start) : dotted.substr(start, dot - start);
        if (label.empty()) break;
        b.push_back(static_cast<uint8_t>(label.size()));
        b.insert(b.end(), label.begin(), label.end());
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    b.push_back(0);
}

std::vector<uint8_t> nameRdata(const std::string& dotted) {
    std::vector<uint8_t> rd;
    writeName(rd, dotted);
    return rd;
}

std::vector<uint8_t> srvRdata(uint16_t port, const std::string& target) {
    std::vector<uint8_t> rd;
    u16(rd, 0);       // priority
    u16(rd, 0);       // weight
    u16(rd, port);
    writeName(rd, target);
    return rd;
}

std::vector<uint8_t> txtRdata(const std::vector<std::string>& pairs) {
    std::vector<uint8_t> rd;
    for (const auto& p : pairs) {
        rd.push_back(static_cast<uint8_t>(p.size()));
        rd.insert(rd.end(), p.begin(), p.end());
    }
    return rd;
}

std::vector<uint8_t> aRdata(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return {a, b, c, d};
}

void addRecord(std::vector<uint8_t>& b,
               const std::string& owner,
               uint16_t type,
               const std::vector<uint8_t>& rdata) {
    writeName(b, owner);
    u16(b, type);
    u16(b, 1);          // class IN
    u32(b, 120);        // TTL
    u16(b, static_cast<uint16_t>(rdata.size()));
    b.insert(b.end(), rdata.begin(), rdata.end());
}

/// Header with everything counted in the ANSWER section, which is how a real
/// responder that has all the records handy tends to reply anyway.
std::vector<uint8_t> header(uint16_t answers) {
    std::vector<uint8_t> b;
    u16(b, 0);          // ID (mDNS responses use 0)
    u16(b, 0x8400);     // response + authoritative
    u16(b, 0);          // QDCOUNT
    u16(b, answers);    // ANCOUNT
    u16(b, 0);          // NSCOUNT
    u16(b, 0);          // ARCOUNT
    return b;
}

/// The happy path: a unit answering with the full PTR + SRV + TXT + A set.
std::vector<uint8_t> goodResponse() {
    auto b = header(4);
    addRecord(b, kSvc,      12, nameRdata(kFqdn));                    // PTR
    addRecord(b, kFqdn,     33, srvRdata(80, kHostname));             // SRV
    addRecord(b, kFqdn,     16, txtRdata({"serial=CPD-0007",
                                          "fw=4.1.3",
                                          "mode=proxy"}));            // TXT
    addRecord(b, kHostname,  1, aRdata(192, 168, 2, 90));             // A
    return b;
}

using DevMap  = std::map<std::string, DeviceDiscoveryService::Device>;
using AddrMap = std::map<std::string, std::string>;

} // namespace

// ── happy path ───────────────────────────────────────────────────────────

TEST(DeviceDiscoveryParse, ParsesFullAdvertisement) {
    const auto pkt = goodResponse();
    DevMap devices;
    AddrMap addresses;

    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(pkt.data(), pkt.size(), devices, addresses));
    DeviceDiscoveryService::applyAddresses(devices, addresses);

    ASSERT_EQ(devices.size(), 1u);
    const auto& d = devices.at(kInstance);
    EXPECT_EQ(d.instance, kInstance);
    EXPECT_EQ(d.serial, "CPD-0007");
    EXPECT_EQ(d.fw, "4.1.3");
    EXPECT_EQ(d.mode, "proxy");
    EXPECT_EQ(d.port, 80);
    EXPECT_EQ(d.srv_target, kHostname);
    EXPECT_EQ(d.host, "192.168.2.90");
    EXPECT_TRUE(d.isLocalCapable());
}

TEST(DeviceDiscoveryParse, BaseUrlOmitsPort80AndKeepsOthers) {
    // Port 80 is the normal case: the mule shares one httpd across control,
    // proxy, OTA and o2ring, so the URL a user sees should stay clean.
    DeviceDiscoveryService::Device d;
    d.host = "192.168.2.90";
    d.port = 80;
    EXPECT_EQ(d.baseUrl(), "http://192.168.2.90");

    d.port = 8080;
    EXPECT_EQ(d.baseUrl(), "http://192.168.2.90:8080");

    d.host.clear();
    EXPECT_EQ(d.baseUrl(), "");
}

// ── malformed input ──────────────────────────────────────────────────────

TEST(DeviceDiscoveryParse, TruncatedPacketIsRejectedNotCrashed) {
    const auto full = goodResponse();
    // Cut mid-record at several depths; none may read out of bounds.
    for (size_t cut : {13u, 20u, 30u, 45u}) {
        ASSERT_LT(cut, full.size());
        const std::vector<uint8_t> pkt(full.begin(), full.begin() + cut);
        DevMap devices;
        AddrMap addresses;
        EXPECT_FALSE(
            DeviceDiscoveryService::parseResponse(pkt.data(), pkt.size(), devices, addresses))
            << "cut at " << cut;
    }
}

TEST(DeviceDiscoveryParse, ShorterThanHeaderIsRejected) {
    const std::vector<uint8_t> pkt(8, 0);
    DevMap devices;
    AddrMap addresses;
    EXPECT_FALSE(DeviceDiscoveryService::parseResponse(pkt.data(), pkt.size(), devices, addresses));
    EXPECT_FALSE(DeviceDiscoveryService::parseResponse(nullptr, 0, devices, addresses));
}

TEST(DeviceDiscoveryParse, RdlengthPastEndOfBufferIsRejected) {
    auto b = header(1);
    writeName(b, kFqdn);
    u16(b, 16);        // TXT
    u16(b, 1);         // IN
    u32(b, 120);
    u16(b, 500);       // rdlength far past what follows
    b.push_back('x');

    DevMap devices;
    AddrMap addresses;
    EXPECT_FALSE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
}

TEST(DeviceDiscoveryParse, SelfReferentialNamePointerTerminates) {
    // A name that is nothing but a pointer to itself. A naive decoder spins
    // here forever; this must come back promptly and simply say no. The
    // browse path runs on a Drogon worker thread, so a hang would wedge a
    // request slot, not just fail a scan.
    auto b = header(1);
    const size_t name_at = b.size();
    b.push_back(0xC0);
    b.push_back(static_cast<uint8_t>(name_at));   // points at itself
    u16(b, 16);
    u16(b, 1);
    u32(b, 120);
    u16(b, 0);

    DevMap devices;
    AddrMap addresses;
    EXPECT_FALSE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
}

TEST(DeviceDiscoveryParse, MutuallyReferentialNamePointersTerminate) {
    auto b = header(1);
    const size_t first = b.size();
    b.push_back(0xC0);
    b.push_back(static_cast<uint8_t>(first + 2));  // -> second
    b.push_back(0xC0);
    b.push_back(static_cast<uint8_t>(first));      // -> first
    u16(b, 16);
    u16(b, 1);
    u32(b, 120);
    u16(b, 0);

    DevMap devices;
    AddrMap addresses;
    EXPECT_FALSE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
}

TEST(DeviceDiscoveryParse, PointerBeyondBufferIsRejected) {
    auto b = header(1);
    b.push_back(0xC0);
    b.push_back(0xFF);   // offset 255, well past this packet
    u16(b, 16);
    u16(b, 1);
    u32(b, 120);
    u16(b, 0);

    DevMap devices;
    AddrMap addresses;
    EXPECT_FALSE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
}

// ── selectivity ──────────────────────────────────────────────────────────

TEST(DeviceDiscoveryParse, OtherServiceTypesAreIgnored) {
    // A busy home LAN answers with printers, Chromecasts and HomeKit. None of
    // it may turn into a device.
    auto b = header(3);
    addRecord(b, "_http._tcp.local", 12, nameRdata("printer._http._tcp.local"));
    addRecord(b, "printer._http._tcp.local", 33, srvRdata(631, "printer.local"));
    addRecord(b, "printer._http._tcp.local", 16, txtRdata({"mode=proxy"}));

    DevMap devices;
    AddrMap addresses;
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
    EXPECT_TRUE(devices.empty());
}

TEST(DeviceDiscoveryParse, ServiceNameMatchIsCaseInsensitive) {
    // DNS names are case-insensitive and responders really do vary the case.
    auto b = header(2);
    addRecord(b, "_CpapDash._TCP.local", 12, nameRdata("CPD-0007._CpapDash._TCP.local"));
    addRecord(b, "CPD-0007._cpapdash._tcp.LOCAL", 16, txtRdata({"mode=proxy"}));

    DevMap devices;
    AddrMap addresses;
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices.begin()->second.mode, "proxy");
}

// ── TXT handling ─────────────────────────────────────────────────────────

TEST(DeviceDiscoveryParse, MissingModeTxtIsNotLocalCapable) {
    // A unit that never advertised a mode is treated as unusable rather than
    // optimistically assumed to be a proxy. Guessing wrong here would write a
    // dead ezshare_url into the user's config during setup.
    auto b = header(2);
    addRecord(b, kFqdn, 33, srvRdata(80, kHostname));
    addRecord(b, kFqdn, 16, txtRdata({"serial=CPD-0007", "fw=4.1.3"}));

    DevMap devices;
    AddrMap addresses;
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
    ASSERT_EQ(devices.size(), 1u);
    const auto& d = devices.at(kInstance);
    EXPECT_EQ(d.serial, "CPD-0007");
    EXPECT_TRUE(d.mode.empty());
    EXPECT_FALSE(d.isLocalCapable());
}

TEST(DeviceDiscoveryParse, CloudModeUnitIsFoundButNotLocalCapable) {
    auto b = header(2);
    addRecord(b, kFqdn, 33, srvRdata(80, kHostname));
    addRecord(b, kFqdn, 16, txtRdata({"serial=CPD-0009", "fw=4.1.3", "mode=cloud"}));

    DevMap devices;
    AddrMap addresses;
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
    ASSERT_EQ(devices.size(), 1u);
    const auto& d = devices.at(kInstance);
    EXPECT_EQ(d.mode, "cloud");
    EXPECT_FALSE(d.isLocalCapable());
}

TEST(DeviceDiscoveryParse, MalformedTxtKeepsPairsReadBeforeTheDamage) {
    // Truncated third string: the first two already told us the truth.
    std::vector<uint8_t> rd;
    const std::string ok1 = "serial=CPD-0007";
    const std::string ok2 = "mode=proxy";
    rd.push_back(static_cast<uint8_t>(ok1.size()));
    rd.insert(rd.end(), ok1.begin(), ok1.end());
    rd.push_back(static_cast<uint8_t>(ok2.size()));
    rd.insert(rd.end(), ok2.begin(), ok2.end());
    rd.push_back(200);            // claims 200 bytes that are not there
    rd.push_back('f');

    auto b = header(1);
    addRecord(b, kFqdn, 16, rd);

    DevMap devices;
    AddrMap addresses;
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices.at(kInstance).serial, "CPD-0007");
    EXPECT_EQ(devices.at(kInstance).mode, "proxy");
}

TEST(DeviceDiscoveryParse, TxtEntriesWithoutEqualsAreSkipped) {
    auto b = header(1);
    addRecord(b, kFqdn, 16, txtRdata({"justaflag", "mode=proxy"}));

    DevMap devices;
    AddrMap addresses;
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
    EXPECT_EQ(devices.at(kInstance).mode, "proxy");
}

// ── address resolution ───────────────────────────────────────────────────

TEST(DeviceDiscoveryParse, FallsBackToSrvTargetWhenNoARecord) {
    auto b = header(2);
    addRecord(b, kFqdn, 33, srvRdata(80, kHostname));
    addRecord(b, kFqdn, 16, txtRdata({"mode=proxy"}));

    DevMap devices;
    AddrMap addresses;
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
    DeviceDiscoveryService::applyAddresses(devices, addresses);

    // Worse than a literal address, but a resolvable .local name beats
    // reporting that no unit was found.
    EXPECT_EQ(devices.at(kInstance).host, kHostname);
}

TEST(DeviceDiscoveryParse, ARecordMatchesSrvTargetCaseInsensitively) {
    auto b = header(3);
    addRecord(b, kFqdn, 33, srvRdata(80, "CpapDash.local"));
    addRecord(b, kFqdn, 16, txtRdata({"mode=proxy"}));
    addRecord(b, "cpapdash.LOCAL", 1, aRdata(10, 0, 0, 5));

    DevMap devices;
    AddrMap addresses;
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
    DeviceDiscoveryService::applyAddresses(devices, addresses);

    EXPECT_EQ(devices.at(kInstance).host, "10.0.0.5");
}

TEST(DeviceDiscoveryParse, ARecordOfWrongLengthIsIgnored) {
    auto b = header(2);
    addRecord(b, kFqdn, 33, srvRdata(80, kHostname));
    addRecord(b, kHostname, 1, {192, 168});   // 2 bytes, not 4

    DevMap devices;
    AddrMap addresses;
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(b.data(), b.size(), devices, addresses));
    EXPECT_TRUE(addresses.empty());
}

// ── accumulation across datagrams ────────────────────────────────────────

TEST(DeviceDiscoveryParse, RepeatedResponsesDeduplicateToOneDevice) {
    const auto pkt = goodResponse();
    DevMap devices;
    AddrMap addresses;

    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(
            DeviceDiscoveryService::parseResponse(pkt.data(), pkt.size(), devices, addresses));
    }
    DeviceDiscoveryService::applyAddresses(devices, addresses);

    EXPECT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices.at(kInstance).host, "192.168.2.90");
}

TEST(DeviceDiscoveryParse, SrvAndAArriveInSeparateDatagrams) {
    // Real responders routinely split these. The A record for the SRV target
    // showing up in a later packet must still resolve.
    auto p1 = header(2);
    addRecord(p1, kFqdn, 33, srvRdata(80, kHostname));
    addRecord(p1, kFqdn, 16, txtRdata({"serial=CPD-0007", "mode=proxy"}));

    auto p2 = header(1);
    addRecord(p2, kHostname, 1, aRdata(192, 168, 2, 91));

    DevMap devices;
    AddrMap addresses;
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(p1.data(), p1.size(), devices, addresses));
    ASSERT_TRUE(DeviceDiscoveryService::parseResponse(p2.data(), p2.size(), devices, addresses));
    DeviceDiscoveryService::applyAddresses(devices, addresses);

    EXPECT_EQ(devices.at(kInstance).host, "192.168.2.91");
}

// ── query construction ───────────────────────────────────────────────────

TEST(DeviceDiscoveryQuery, AsksForThePtrRecordAsAMulticastQuestionByDefault) {
    const auto q = DeviceDiscoveryService::buildQuery(0x1234);
    ASSERT_GT(q.size(), 12u);

    EXPECT_EQ(q[0], 0x12);
    EXPECT_EQ(q[1], 0x34);
    EXPECT_EQ((q[4] << 8) | q[5], 1);      // QDCOUNT
    EXPECT_EQ((q[6] << 8) | q[7], 0);      // ANCOUNT

    // The trailing 4 bytes are QTYPE then QCLASS.
    const size_t n = q.size();
    EXPECT_EQ((q[n - 4] << 8) | q[n - 3], 12);      // PTR

    // REGRESSION GUARD. This asserted 0x8001 (the QU "unicast response
    // requested" bit) in the first cut, and that is why a live scan found
    // nothing on a LAN that had a mule advertising on it: the mule's ESP-IDF
    // responder ignores QU and answers by multicast to 224.0.0.251:5353. The
    // default question must stay QM.
    EXPECT_EQ((q[n - 2] << 8) | q[n - 1], 0x0001);
}

TEST(DeviceDiscoveryQuery, UnicastBitIsAvailableForTheEphemeralPortFallback) {
    // Only correct when 5353 could not be bound and we are listening on an
    // ephemeral port, where a multicast answer would never reach us.
    const auto q = DeviceDiscoveryService::buildQuery(1, /*unicast_response=*/true);
    const size_t n = q.size();
    EXPECT_EQ((q[n - 2] << 8) | q[n - 1], 0x8001);
}

TEST(DeviceDiscoveryQuery, IsParseableAndNamesTheService) {
    // Round-trip through our own reader: the question section must decode.
    const auto q = DeviceDiscoveryService::buildQuery(1);
    std::string decoded;
    for (size_t i = 12; i < q.size() - 4;) {
        const uint8_t l = q[i];
        if (l == 0) break;
        if (!decoded.empty()) decoded += '.';
        decoded.append(reinterpret_cast<const char*>(q.data() + i + 1), l);
        i += 1 + l;
    }
    EXPECT_EQ(decoded, kSvc);
    EXPECT_STREQ(DeviceDiscoveryService::serviceName(), kSvc);
}

// ── browse() over the injected transport ─────────────────────────────────

TEST(DeviceDiscoveryBrowse, NoRespondersYieldsEmptyListNotAnError) {
    DeviceDiscoveryService svc([](std::chrono::milliseconds) {
        return std::vector<std::vector<uint8_t>>{};
    });
    EXPECT_TRUE(svc.browse(std::chrono::milliseconds(10)).empty());
}

TEST(DeviceDiscoveryBrowse, ReturnsParsedDevices) {
    DeviceDiscoveryService svc([](std::chrono::milliseconds) {
        return std::vector<std::vector<uint8_t>>{goodResponse()};
    });
    const auto found = svc.browse(std::chrono::milliseconds(10));

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].serial, "CPD-0007");
    EXPECT_EQ(found[0].baseUrl(), "http://192.168.2.90");
}

TEST(DeviceDiscoveryBrowse, OneBadDatagramDoesNotHideAGoodOne) {
    // A single broken responder on the LAN must not cost the user their unit.
    DeviceDiscoveryService svc([](std::chrono::milliseconds) {
        std::vector<std::vector<uint8_t>> out;
        out.push_back({0x00, 0x01, 0x02});      // garbage
        out.push_back({});                       // empty
        out.push_back(goodResponse());
        return out;
    });
    const auto found = svc.browse(std::chrono::milliseconds(10));

    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].instance, kInstance);
}

TEST(DeviceDiscoveryBrowse, ThrowingTransportIsTreatedAsNobodyAnswered) {
    DeviceDiscoveryService svc([](std::chrono::milliseconds) -> std::vector<std::vector<uint8_t>> {
        throw std::runtime_error("socket exploded");
    });
    EXPECT_TRUE(svc.browse(std::chrono::milliseconds(10)).empty());
}

TEST(DeviceDiscoveryBrowse, EmptyTransportIsSafe) {
    DeviceDiscoveryService svc(nullptr);
    EXPECT_TRUE(svc.browse(std::chrono::milliseconds(10)).empty());
}
