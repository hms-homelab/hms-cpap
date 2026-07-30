#include "services/DeviceDiscoveryService.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #pragma comment(lib, "ws2_32.lib")
  #pragma comment(lib, "iphlpapi.lib")
#else
  #include <arpa/inet.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
#endif

// mjansson/mdns, vendored under third_party/mdns (Unlicense, public domain,
// verified 2026-07-23 and compatible with this repo's MIT terms).
//
// It owns the socket layer and the query encoding, which is the part that is
// genuinely fiddly and platform-specific: it binds 5353 with SO_REUSEPORT,
// joins 224.0.0.251, sets IP_MULTICAST_IF, and picks a multicast question
// (QM) over a unicast one (QU) based on the bound port. Critically it is
// designed to be opened ONCE PER INTERFACE, which a hand-rolled single
// INADDR_ANY socket is not: on a machine with Wi-Fi plus bridges plus VPN
// adapters the kernel's default multicast interface is frequently not the one
// the CPAP bridge is on, and a scan then finds nothing while the unit is
// plainly up.
//
// Record parsing stays in parseResponse() below: the library parses inside
// mdns_query_recv(), which calls recvfrom() itself and so cannot be driven by
// captured packets, and the fixture tests are worth keeping.
#include "mdns.h"

namespace hms_cpap {

namespace {

// DNS record types we care about. Anything else is skipped by rdlength.
constexpr uint16_t kTypeA    = 1;
constexpr uint16_t kTypePTR  = 12;
constexpr uint16_t kTypeTXT  = 16;
constexpr uint16_t kTypeSRV  = 33;

constexpr const char* kMdnsGroup = "224.0.0.251";
constexpr uint16_t    kMdnsPort  = 5353;

bool readU16(const uint8_t* d, size_t len, size_t& off, uint16_t& v) {
    if (off + 2 > len) return false;
    v = static_cast<uint16_t>((static_cast<uint16_t>(d[off]) << 8) | d[off + 1]);
    off += 2;
    return true;
}

// Decode a DNS name at `off`. On return `off` points just past the name AS
// ENCODED AT THAT POSITION: a compression pointer costs 2 bytes no matter how
// much it expands to, which is why the "jumped" flag freezes `off` on the
// first jump.
//
// The jump cap is the whole reason this is safe to run on the web thread. A
// pointer that targets itself, or a pair that point at each other, is
// perfectly expressible in a DNS message and a naive decoder loops forever on
// it. 128 is far above anything legitimate.
bool readName(const uint8_t* d, size_t len, size_t& off, std::string& out) {
    out.clear();
    size_t cur = off;
    bool   jumped = false;
    int    jumps = 0;
    constexpr int kMaxJumps = 128;

    while (true) {
        if (cur >= len) return false;
        const uint8_t l = d[cur];

        if ((l & 0xC0) == 0xC0) {                    // compression pointer
            if (cur + 1 >= len) return false;
            const size_t ptr =
                (static_cast<size_t>(l & 0x3F) << 8) | static_cast<size_t>(d[cur + 1]);
            if (!jumped) {
                off = cur + 2;
                jumped = true;
            }
            if (++jumps > kMaxJumps) return false;
            if (ptr >= len) return false;
            cur = ptr;
            continue;
        }
        if ((l & 0xC0) != 0) return false;           // reserved label type

        if (l == 0) {                                // end of name
            if (!jumped) off = cur + 1;
            return true;
        }

        cur++;
        if (cur + l > len) return false;
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(d + cur), l);
        cur += l;
    }
}

// DNS names are case-insensitive (RFC 1035 2.3.3), and responders do vary the
// case, so every name comparison here goes through this.
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

// "CPD-0007._cpapdash._tcp.local" under "_cpapdash._tcp.local" -> "CPD-0007".
// Returns false for anything that is not an instance of `svc`, which is how a
// response advertising some unrelated service gets ignored.
bool instanceOf(const std::string& owner, const std::string& svc, std::string& instance) {
    if (owner.size() <= svc.size() + 1) return false;
    const size_t cut = owner.size() - svc.size();
    if (owner[cut - 1] != '.') return false;
    if (!iequals(owner.substr(cut), svc)) return false;
    instance = owner.substr(0, cut - 1);
    return !instance.empty();
}

// TXT rdata is a run of length-prefixed strings, each conventionally "k=v".
// A malformed length simply stops the walk; the pairs already read are kept,
// because a responder that garbles its third TXT string has still told us the
// truth in the first two.
void parseTxt(const uint8_t* d, size_t rd, size_t rdlen, DeviceDiscoveryService::Device& dev) {
    size_t p = rd;
    const size_t end = rd + rdlen;
    while (p < end) {
        const uint8_t sl = d[p];
        p++;
        if (sl == 0) continue;
        if (p + sl > end) return;
        const std::string kv(reinterpret_cast<const char*>(d + p), sl);
        p += sl;

        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq);
        const std::string v = kv.substr(eq + 1);
        if (k == "serial")    dev.serial = v;
        else if (k == "fw")   dev.fw = v;
        else if (k == "mode") dev.mode = v;
    }
}

} // namespace

const char* DeviceDiscoveryService::serviceName() {
    return "_cpapdash._tcp.local";
}

std::string DeviceDiscoveryService::Device::baseUrl() const {
    if (host.empty()) return "";
    std::string url = "http://" + host;
    // The mule shares one httpd across control, proxy, OTA and o2ring on port
    // 80 (CONTROL_HTTP_PORT is defined as PROXY_HTTP_PORT), so the common case
    // produces a bare host with no port suffix, which is also what a user
    // expects to see in the settings field.
    if (port != 0 && port != 80) url += ":" + std::to_string(port);
    return url;
}

DeviceDiscoveryService::DeviceDiscoveryService(Transport transport)
    : transport_(std::move(transport)) {}

std::vector<uint8_t> DeviceDiscoveryService::buildQuery(uint16_t query_id,
                                                        bool unicast_response) {
    std::vector<uint8_t> q;
    auto push16 = [&q](uint16_t v) {
        q.push_back(static_cast<uint8_t>(v >> 8));
        q.push_back(static_cast<uint8_t>(v & 0xFF));
    };

    push16(query_id);
    push16(0x0000);   // standard query, no flags set
    push16(1);        // QDCOUNT
    push16(0);        // ANCOUNT
    push16(0);        // NSCOUNT
    push16(0);        // ARCOUNT

    // Encode "_cpapdash._tcp.local" as length-prefixed labels.
    const std::string svc = serviceName();
    size_t start = 0;
    while (start <= svc.size()) {
        const size_t dot = svc.find('.', start);
        const std::string label =
            (dot == std::string::npos) ? svc.substr(start) : svc.substr(start, dot - start);
        if (label.empty()) break;
        q.push_back(static_cast<uint8_t>(label.size()));
        q.insert(q.end(), label.begin(), label.end());
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    q.push_back(0);   // root label

    push16(kTypePTR);
    // QCLASS = IN. The top bit is the mDNS QU "unicast response requested"
    // flag (RFC 6762 5.4).
    //
    // It is OFF by default, and that is a bug fix, not a preference. Asking
    // for a unicast reply from an ephemeral port looks tidier, but a responder
    // is entitled to ignore QU and answer by multicast to 224.0.0.251:5353.
    // The ESP-IDF responder on the mule does exactly that, so the first cut of
    // this code found nothing on a network that demonstrably had a unit
    // advertising on it. The transport now shares port 5353 and joins the
    // group, so a plain multicast question is the right thing to send.
    push16(unicast_response ? 0x8001 : 0x0001);

    return q;
}

bool DeviceDiscoveryService::parseResponse(const uint8_t* data,
                                           size_t len,
                                           std::map<std::string, Device>& devices,
                                           std::map<std::string, std::string>& addresses) {
    if (!data || len < 12) return false;

    size_t off = 0;
    uint16_t id = 0, flags = 0, qd = 0, an = 0, ns = 0, ar = 0;
    if (!readU16(data, len, off, id))    return false;
    if (!readU16(data, len, off, flags)) return false;
    if (!readU16(data, len, off, qd))    return false;
    if (!readU16(data, len, off, an))    return false;
    if (!readU16(data, len, off, ns))    return false;
    if (!readU16(data, len, off, ar))    return false;

    // A response that echoes the question is normal; skip the question section.
    for (uint16_t i = 0; i < qd; i++) {
        std::string qname;
        if (!readName(data, len, off, qname)) return false;
        if (off + 4 > len) return false;
        off += 4;   // QTYPE + QCLASS
    }

    const std::string svc = serviceName();
    const size_t total = static_cast<size_t>(an) + static_cast<size_t>(ns) + static_cast<size_t>(ar);

    for (size_t i = 0; i < total; i++) {
        std::string owner;
        if (!readName(data, len, off, owner)) return false;

        uint16_t type = 0, cls = 0, rdlen = 0;
        if (!readU16(data, len, off, type)) return false;
        if (!readU16(data, len, off, cls))  return false;
        if (off + 4 > len) return false;
        off += 4;                                  // TTL, unused
        if (!readU16(data, len, off, rdlen)) return false;
        if (off + rdlen > len) return false;

        const size_t rd = off;
        off += rdlen;                              // advance now; handlers read from rd

        switch (type) {
        case kTypePTR: {
            // Only a PTR whose owner IS the service enumerates instances.
            if (!iequals(owner, svc)) break;
            size_t p = rd;
            std::string target;
            if (!readName(data, len, p, target)) break;
            std::string instance;
            if (!instanceOf(target, svc, instance)) break;
            auto& dev = devices[instance];
            dev.instance = instance;
            break;
        }
        case kTypeSRV: {
            std::string instance;
            if (!instanceOf(owner, svc, instance)) break;
            if (rdlen < 7) break;                  // prio + weight + port + >=1 name byte
            size_t p = rd + 6;                     // skip priority and weight
            uint16_t port = static_cast<uint16_t>(
                (static_cast<uint16_t>(data[rd + 4]) << 8) | data[rd + 5]);
            std::string target;
            if (!readName(data, len, p, target)) break;
            auto& dev = devices[instance];
            dev.instance   = instance;
            dev.port       = port;
            dev.srv_target = target;
            break;
        }
        case kTypeTXT: {
            std::string instance;
            if (!instanceOf(owner, svc, instance)) break;
            auto& dev = devices[instance];
            dev.instance = instance;
            parseTxt(data, rd, rdlen, dev);
            break;
        }
        case kTypeA: {
            if (rdlen != 4) break;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                          data[rd], data[rd + 1], data[rd + 2], data[rd + 3]);
            addresses[owner] = buf;
            break;
        }
        default:
            break;                                 // AAAA and everything else
        }
    }

    return true;
}

void DeviceDiscoveryService::applyAddresses(std::map<std::string, Device>& devices,
                                            const std::map<std::string, std::string>& addresses) {
    for (auto& [instance, dev] : devices) {
        if (dev.srv_target.empty()) continue;

        // Exact key first, then a case-insensitive sweep, since the A record's
        // owner and the SRV target are independently cased by the responder.
        auto it = addresses.find(dev.srv_target);
        if (it != addresses.end()) {
            dev.host = it->second;
            continue;
        }
        bool found = false;
        for (const auto& [name, ip] : addresses) {
            if (iequals(name, dev.srv_target)) {
                dev.host = ip;
                found = true;
                break;
            }
        }
        // No A record: fall back to the advertised hostname and let the OS
        // resolver deal with ".local". Worse than a literal address, but far
        // better than reporting no device at all.
        if (!found) dev.host = dev.srv_target;
    }
}

std::vector<DeviceDiscoveryService::Device>
DeviceDiscoveryService::browse(std::chrono::milliseconds timeout) {
    std::vector<Device> result;
    if (!transport_) return result;

    std::vector<std::vector<uint8_t>> datagrams;
    try {
        datagrams = transport_(timeout);
    } catch (...) {
        return result;   // a transport that throws is "nobody answered"
    }

    std::map<std::string, Device>      devices;
    std::map<std::string, std::string> addresses;
    for (const auto& dg : datagrams) {
        if (dg.empty()) continue;
        // A malformed datagram is skipped, not fatal: one bad responder on the
        // LAN must not hide a good unit that answered in another packet.
        parseResponse(dg.data(), dg.size(), devices, addresses);
    }

    applyAddresses(devices, addresses);

    result.reserve(devices.size());
    for (auto& [instance, dev] : devices) result.push_back(dev);
    return result;
}

namespace {

/// Every usable IPv4 interface address, so a socket can be opened on each.
/// Loopback is skipped; a CPAP bridge is never there and mDNSResponder
/// chatters on it.
std::vector<uint32_t> localIPv4Interfaces() {
    std::vector<uint32_t> addrs;
#ifdef _WIN32
    // Windows enumeration is deliberately minimal for now: Phase 3 of SDD-005
    // is the Windows phase and will exercise this properly. INADDR_ANY still
    // works on a single-adapter machine, which is the common case.
    addrs.push_back(INADDR_ANY);
#else
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) {
        addrs.push_back(INADDR_ANY);
        return addrs;
    }
    for (ifaddrs* it = ifa; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (!(it->ifa_flags & IFF_UP)) continue;
        if (it->ifa_flags & IFF_LOOPBACK) continue;
        if (!(it->ifa_flags & IFF_MULTICAST)) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        addrs.push_back(sin->sin_addr.s_addr);
    }
    ::freeifaddrs(ifa);
    if (addrs.empty()) addrs.push_back(INADDR_ANY);
#endif
    return addrs;
}

} // namespace

DeviceDiscoveryService DeviceDiscoveryService::withMulticast() {
    return DeviceDiscoveryService([](std::chrono::milliseconds timeout) {
        std::vector<std::vector<uint8_t>> out;

#ifdef _WIN32
        {
            static bool wsa_inited = false;
            if (!wsa_inited) {
                WSADATA wsa;
                if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return out;
                wsa_inited = true;
            }
        }
#endif
        // One socket per interface. This is the whole reason the library is
        // here: a single INADDR_ANY socket sends the query out whichever
        // interface the kernel happens to prefer, which on a machine with
        // Wi-Fi plus bridges plus VPN adapters is regularly the wrong one, and
        // the scan then comes back empty while the unit is up and serving.
        std::vector<int> socks;
        for (uint32_t ifaddr : localIPv4Interfaces()) {
            sockaddr_in saddr{};
            saddr.sin_family      = AF_INET;
            saddr.sin_addr.s_addr = ifaddr;
            saddr.sin_port        = htons(kMdnsPort);
#ifdef __APPLE__
            saddr.sin_len = sizeof(sockaddr_in);
#endif
            const int s = mdns_socket_open_ipv4(&saddr);
            if (s >= 0) socks.push_back(s);
        }
        if (socks.empty()) return out;

        // mdns_query_send builds the PTR question and picks QM over QU based
        // on the bound port (5353 here, so a normal multicast question).
        std::vector<uint8_t> sendbuf(2048);
        const std::string svc = std::string(DeviceDiscoveryService::serviceName()) + ".";
        for (int s : socks) {
            mdns_query_send(s, MDNS_RECORDTYPE_PTR, svc.c_str(), svc.size(),
                            sendbuf.data(), sendbuf.size(), 0);
        }

        // Read raw datagrams off the same sockets rather than calling
        // mdns_query_recv, so parseResponse stays the single record-parsing
        // path and keeps its offline fixture tests.
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::vector<uint8_t> buf(9000);

        while (std::chrono::steady_clock::now() < deadline) {
            const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remain.count() <= 0) break;

            fd_set fds;
            FD_ZERO(&fds);
            int maxfd = 0;
            for (int s : socks) {
                FD_SET(s, &fds);
                if (s > maxfd) maxfd = s;
            }
            // decltype, not time_t/suseconds_t: those are the POSIX field types
            // and MSVC has neither (its timeval carries two longs), so naming
            // them directly fails to compile on Windows. Deriving the cast from
            // the field itself is correct on every platform.
            timeval tv{};
            tv.tv_sec  = static_cast<decltype(tv.tv_sec)>(remain.count() / 1000);
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>((remain.count() % 1000) * 1000);

            const int ready = ::select(maxfd + 1, &fds, nullptr, nullptr, &tv);
            if (ready <= 0) break;   // timeout or error: what we have is the answer

            for (int s : socks) {
                if (!FD_ISSET(s, &fds)) continue;
                // Sockets are non-blocking (the library sets O_NONBLOCK), so
                // drain each one until it would block.
                for (;;) {
                    const int n = ::recvfrom(s, reinterpret_cast<char*>(buf.data()),
                                             static_cast<int>(buf.size()), 0, nullptr, nullptr);
                    if (n <= 0) break;
                    out.emplace_back(buf.begin(), buf.begin() + n);
                    // Sharing 5353 means every AirPlay, Cast and printer
                    // announcement lands here too. parseResponse filters them
                    // out by service name, but the buffer still has to be
                    // bounded: a noisy LAN must not grow this without limit
                    // inside a web request.
                    if (out.size() >= 256) break;
                }
                if (out.size() >= 256) break;
            }
            if (out.size() >= 256) break;
        }

        for (int s : socks) mdns_socket_close(s);
        return out;
    });
}

} // namespace hms_cpap
