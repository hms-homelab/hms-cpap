#pragma once
//
// DeviceDiscoveryService (SDD-005, phase 1) — one-shot mDNS/DNS-SD browse for
// CpapDash Mule and Miner units on the local network.
//
// The mule already advertises itself (cpapdash-push-c3,
// mule/main/wifi_manager.c): hostname "cpapdash" plus a per-unit service
// `_cpapdash._tcp` on its control port, carrying TXT records `serial`, `fw`
// and `mode` ("proxy" or "cloud"). A unit in proxy mode serves ezShare-shaped
// `/dir` and `/download` (mule/main/ezshare_proxy.c), which is byte-for-byte
// what EzShareClient already requests. So discovery's ONLY job is to turn a
// multicast response into a host string that can be written to
// `config.ezshare_url`. There is no new ingest path behind this.
//
// WHY THE PARSER IS HAND-ROLLED. The obvious dependency (mjansson/mdns,
// Unlicense) does its record parsing inside `mdns_query_recv()`, which calls
// recvfrom() itself. That makes it impossible to drive with captured packets,
// and SDD-005 requires the parsing tests to run offline against fixture byte
// buffers with no multicast in CI. `parseResponse` below is therefore a pure
// function over a buffer, and the only injected seam is the datagram
// transport. This also means zero new entries in vcpkg.json.
//
// THREADING. `browse()` is synchronous and blocks for at most the timeout it
// is given. Callers on the web thread must keep that timeout short; the
// controller uses 2.5s.
//
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hms_cpap {

class DeviceDiscoveryService {
public:
    /// One advertised unit. `host` is preferred as a dotted-quad taken from an
    /// A record when the responder supplied one, because resolving a `.local`
    /// name afterwards would put us right back into mDNS on a platform that
    /// may not have a resolver wired up. It falls back to the SRV target.
    struct Device {
        std::string instance;        ///< mDNS instance label (the unit serial)
        std::string host;            ///< dotted-quad, else SRV target hostname
        std::string srv_target;      ///< raw SRV target, kept for diagnostics
        uint16_t    port = 0;        ///< SRV port (the mule's control port)
        std::string serial;          ///< TXT "serial"
        std::string fw;              ///< TXT "fw"
        std::string mode;            ///< TXT "mode": "proxy" or "cloud"

        /// Only a unit in proxy mode serves /dir and /download, so only a
        /// proxy-mode unit can feed a local install. A unit that advertised no
        /// `mode` at all is treated as not usable rather than assumed good.
        bool isLocalCapable() const { return mode == "proxy"; }

        /// What gets written into config.ezshare_url.
        std::string baseUrl() const;
    };

    /// Sends one query and returns whatever datagrams came back before the
    /// timeout. Injected so the parser can be tested against captured packets.
    /// An implementation returning an empty vector is a valid "nobody
    /// answered" and must not be reported as an error.
    using Transport =
        std::function<std::vector<std::vector<uint8_t>>(std::chrono::milliseconds)>;

    explicit DeviceDiscoveryService(Transport transport);

    /// Real transport: IPv4 multicast query to 224.0.0.251:5353.
    static DeviceDiscoveryService withMulticast();

    /// Browse for `_cpapdash._tcp.local`. Returns an empty vector when nothing
    /// answers; that is a success, not a failure.
    std::vector<Device> browse(std::chrono::milliseconds timeout);

    // ---- exposed for tests -------------------------------------------------

    /// The DNS-SD service this browses for.
    static const char* serviceName();

    /// Build the PTR query datagram. Deterministic apart from `query_id`.
    /// `unicast_response` sets the mDNS QU bit (RFC 6762 5.4), which is only
    /// correct when the caller could not bind 5353 and is listening on an
    /// ephemeral port instead. Default is a normal multicast question,
    /// because responders are free to ignore QU and several do.
    static std::vector<uint8_t> buildQuery(uint16_t query_id,
                                           bool unicast_response = false);

    /// Parse ONE response datagram, merging what it learns into `devices`
    /// (keyed by instance label) and `addresses` (hostname -> dotted-quad).
    /// Never throws and never reads outside [data, data+len). Returns false if
    /// the datagram was malformed enough to abandon, but a false return may
    /// still have merged the records it managed to read before the damage.
    static bool parseResponse(const uint8_t* data,
                              size_t len,
                              std::map<std::string, Device>& devices,
                              std::map<std::string, std::string>& addresses);

    /// Resolve `host` on each device from the collected A records. Applied by
    /// browse() after every datagram has been parsed, because the A record
    /// often arrives in a different packet than the SRV that needs it.
    static void applyAddresses(std::map<std::string, Device>& devices,
                               const std::map<std::string, std::string>& addresses);

private:
    Transport transport_;
};

} // namespace hms_cpap
