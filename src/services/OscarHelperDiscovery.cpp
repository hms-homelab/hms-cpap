#include "services/OscarHelperDiscovery.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <map>
#include <set>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

// mjansson/mdns, vendored (see DeviceDiscoveryService.cpp).
#include "mdns.h"

namespace hms_cpap::oscar::helper {

namespace {

constexpr const char* kTag = "OSCAR helper: ";

#ifdef _WIN32
void netInit() {
    static bool done = false;
    if (done) return;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) done = true;
}
void closeSock(int s) { closesocket(static_cast<SOCKET>(s)); }
void nonBlocking(int s) {
    u_long on = 1;
    ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &on);
}
#else
void netInit() {}
void closeSock(int s) { ::close(s); }
void nonBlocking(int s) { ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

std::string ipText(uint32_t be) {
    in_addr a{};
    a.s_addr = be;
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

/// The IPv4 interface addresses, network order.
std::vector<uint32_t> interfaces() {
    std::vector<uint32_t> out;
#ifdef _WIN32
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) == 0) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
            for (auto* p = res; p; p = p->ai_next) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
                if ((ntohl(sin->sin_addr.s_addr) >> 24) != 127) out.push_back(sin->sin_addr.s_addr);
            }
            freeaddrinfo(res);
        }
    }
#else
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) return out;
    for (ifaddrs* it = ifa; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
        out.push_back(reinterpret_cast<const sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr);
    }
    ::freeifaddrs(ifa);
#endif
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool mentions(const uint8_t* d, size_t n, const char* label) {
    // The DNS label as encoded: its length byte, then the text.
    std::string enc(1, static_cast<char>(std::strlen(label)));
    enc += label;
    const std::string hay(reinterpret_cast<const char*>(d), n);
    std::string low = hay;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return low.find(enc) != std::string::npos;
}

}  // namespace

Json::Value toJson(const Found& f) {
    Json::Value v(Json::objectValue);
    v["address"]    = f.address;
    v["port"]       = f.port;
    v["name"]       = f.name;
    v["install_id"] = f.install_id;
    v["version"]    = f.version;
    v["helper"]     = f.helper;
    return v;
}

std::vector<std::string> localAddresses() {
    netInit();
    std::vector<std::string> out;
    for (uint32_t a : interfaces()) out.push_back(ipText(a));
    return out;
}

std::vector<std::string> subnetOf(const std::vector<std::string>& own) {
    std::set<std::string> mine(own.begin(), own.end());
    std::set<std::string> out;
    for (const auto& a : own) {
        in_addr ia{};
        if (inet_pton(AF_INET, a.c_str(), &ia) != 1) continue;
        const uint32_t base = ntohl(ia.s_addr) & 0xffffff00u;
        // A link-local address has no LAN to sweep.
        if ((base >> 16) == 0xa9fe) continue;
        for (uint32_t i = 1; i < 255; ++i) {
            const std::string s = ipText(htonl(base | i));
            if (!mine.count(s)) out.insert(s);
        }
    }
    return {out.begin(), out.end()};
}

std::vector<std::string> mdnsHosts(std::chrono::milliseconds wait) {
    netInit();
    std::vector<int> socks;
    for (uint32_t ifaddr : interfaces()) {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = ifaddr;
        sa.sin_port = 0;   // an ephemeral port: the answers come back unicast
#ifdef __APPLE__
        sa.sin_len = sizeof(sockaddr_in);
#endif
        const int s = mdns_socket_open_ipv4(&sa);
        if (s >= 0) socks.push_back(s);
    }
    std::set<std::string> hosts;
    if (socks.empty()) return {};
    std::vector<uint8_t> buf(2048);
    for (const char* svc : {kMdnsService, kMdnsHa}) {
        const std::string name = std::string(svc) + ".";
        for (int s : socks) mdns_query_send(s, MDNS_RECORDTYPE_PTR, name.c_str(), name.size(), buf.data(), buf.size(), 0);
    }
    std::vector<uint8_t> in(9000);
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        fd_set fds;
        FD_ZERO(&fds);
        int maxfd = 0;
        for (int s : socks) {
            FD_SET(s, &fds);
            maxfd = std::max(maxfd, s);
        }
        timeval tv{};
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(left.count() / 1000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>((left.count() % 1000) * 1000);
        if (::select(maxfd + 1, &fds, nullptr, nullptr, &tv) <= 0) break;
        for (int s : socks) {
            if (!FD_ISSET(s, &fds)) continue;
            for (;;) {
                sockaddr_in from{};
                socklen_t len = sizeof(from);
                const int n = ::recvfrom(s, reinterpret_cast<char*>(in.data()), static_cast<int>(in.size()), 0,
                                         reinterpret_cast<sockaddr*>(&from), &len);
                if (n <= 0) break;
                if (mentions(in.data(), static_cast<size_t>(n), "_hms-cpap") ||
                    mentions(in.data(), static_cast<size_t>(n), "_home-assistant"))
                    hosts.insert(ipText(from.sin_addr.s_addr));
                if (hosts.size() > 64) break;
            }
        }
    }
    for (int s : socks) mdns_socket_close(s);
    return {hosts.begin(), hosts.end()};
}

std::vector<std::string> acceptingOn(const std::vector<std::string>& hosts, int port, std::chrono::milliseconds wait) {
    netInit();
    std::vector<std::string> out;
    // In groups: select() holds at most 64 sockets on Windows.
    constexpr size_t kGroup = 60;
    for (size_t from = 0; from < hosts.size(); from += kGroup) {
        std::vector<std::pair<int, std::string>> pending;
        for (size_t i = from; i < std::min(hosts.size(), from + kGroup); ++i) {
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_port = htons(static_cast<uint16_t>(port));
            if (inet_pton(AF_INET, hosts[i].c_str(), &sa.sin_addr) != 1) continue;
            const int s = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
            if (s < 0) continue;
            nonBlocking(s);
            ::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
            pending.emplace_back(s, hosts[i]);
        }
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (!pending.empty()) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) break;
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            int maxfd = 0;
            for (const auto& [s, h] : pending) {
                FD_SET(s, &wfds);
                FD_SET(s, &efds);
                maxfd = std::max(maxfd, s);
            }
            timeval tv{};
            tv.tv_sec = static_cast<decltype(tv.tv_sec)>(left.count() / 1000);
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>((left.count() % 1000) * 1000);
            if (::select(maxfd + 1, nullptr, &wfds, &efds, &tv) <= 0) break;
            std::vector<std::pair<int, std::string>> still;
            for (const auto& [s, h] : pending) {
                if (!FD_ISSET(s, &wfds) && !FD_ISSET(s, &efds)) {
                    still.emplace_back(s, h);
                    continue;
                }
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                if (err == 0 && FD_ISSET(s, &wfds)) out.push_back(h);
                closeSock(s);
            }
            pending.swap(still);
        }
        for (const auto& [s, h] : pending) closeSock(s);
    }
    return out;
}

std::optional<Found> probe(const Helper::LinkFactory& factory, const std::string& host, int port,
                           const std::string& pc_name, std::chrono::milliseconds wait) {
    if (!factory) return std::nullopt;
    CpapDashEndpoint ep;
    ep.tls = false;
    ep.host = host;
    ep.port = static_cast<uint16_t>(port);
    ep.path = kRoute;
    ep.ping_every_s = 0;
    ep.headers.emplace_back(kHeaderProbe, "1");
    ep.headers.emplace_back(kHeaderName, pc_name);
    auto link = factory(ep);
    if (!link) return std::nullopt;
    const auto c = link->connect("", "", "hms-cpap-helper-probe", wait);
    if (!c.ok) return std::nullopt;
    Json::Value hello(Json::objectValue);
    hello["t"] = "hello";
    hello["probe"] = true;
    hello["proto"] = kProto;
    link->sendText(frame(hello));
    const auto deadline = std::chrono::steady_clock::now() + wait;
    std::optional<Found> out;
    while (!out) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        const auto f = link->next(left);
        if (!f || f->kind == CpapDashLink::Frame::Kind::Closed) break;
        if (f->kind != CpapDashLink::Frame::Kind::Text) continue;
        const auto v = parse(f->text);
        if (!v || (*v)["t"].asString() != "ack") continue;
        const auto& h = (*v)["hms_cpap"];
        Found x;
        x.address = host;
        x.port = port;
        x.name = h["name"].asString();
        x.install_id = h["install_id"].asString();
        x.version = h["version"].asString();
        x.helper = h["helper"].asString();
        out = x;
    }
    link->close();
    return out;
}

std::vector<Found> discover(const Helper::LinkFactory& factory, const std::string& pc_name, int port) {
    std::vector<std::string> hosts = mdnsHosts(std::chrono::milliseconds(2000));
    std::vector<Found> found;
    const auto probeAll = [&](const std::vector<std::string>& list) {
        std::set<std::string> seen;
        for (const auto& h : list) {
            if (!seen.insert(h).second) continue;
            if (auto f = probe(factory, h, port, pc_name, std::chrono::milliseconds(3000))) found.push_back(*f);
        }
    };
    probeAll(acceptingOn(hosts, port, std::chrono::milliseconds(1500)));
    if (found.empty()) {
        // Nothing by name: every address of this PC's /24 that takes 8894.
        std::cout << kTag << "nothing answered over mDNS; looking on this network" << std::endl;
        probeAll(acceptingOn(subnetOf(localAddresses()), port, std::chrono::milliseconds(1500)));
    }
    return found;
}

// ── the advertisement ───────────────────────────────────────────────────────

namespace {

struct AdvertCtx {
    std::string service;    // "_hms-cpap._tcp.local."
    std::string instance;   // "<name>._hms-cpap._tcp.local."
    std::string host;       // "<name>.local."
    uint16_t    port{0};
    uint32_t    addr{0};    // the interface's, network order
    std::string txt_id, txt_version;
    std::vector<uint8_t> buf = std::vector<uint8_t>(2048);
};

std::vector<mdns_record_t> extras(AdvertCtx& c) {
    std::vector<mdns_record_t> r;
    mdns_record_t srv{};
    srv.name = {c.instance.c_str(), c.instance.size()};
    srv.type = MDNS_RECORDTYPE_SRV;
    srv.data.srv.name = {c.host.c_str(), c.host.size()};
    srv.data.srv.port = c.port;
    srv.rclass = 0;
    srv.ttl = 0;
    r.push_back(srv);
    mdns_record_t a{};
    a.name = {c.host.c_str(), c.host.size()};
    a.type = MDNS_RECORDTYPE_A;
    a.data.a.addr.sin_family = AF_INET;
    a.data.a.addr.sin_addr.s_addr = c.addr;
#ifdef __APPLE__
    a.data.a.addr.sin_len = sizeof(sockaddr_in);
#endif
    r.push_back(a);
    for (auto [k, v] : {std::pair<const char*, const std::string*>{"install_id", &c.txt_id}, {"version", &c.txt_version}}) {
        mdns_record_t t{};
        t.name = {c.instance.c_str(), c.instance.size()};
        t.type = MDNS_RECORDTYPE_TXT;
        t.data.txt.key = {k, std::strlen(k)};
        t.data.txt.value = {v->c_str(), v->size()};
        r.push_back(t);
    }
    return r;
}

mdns_record_t ptrRecord(AdvertCtx& c) {
    mdns_record_t p{};
    p.name = {c.service.c_str(), c.service.size()};
    p.type = MDNS_RECORDTYPE_PTR;
    p.data.ptr.name = {c.instance.c_str(), c.instance.size()};
    return p;
}

int onQuestion(int sock, const struct sockaddr* from, size_t addrlen, mdns_entry_type_t entry, uint16_t query_id,
               uint16_t rtype, uint16_t rclass, uint32_t, const void* data, size_t size, size_t name_offset, size_t,
               size_t, size_t, void* user) {
    if (entry != MDNS_ENTRYTYPE_QUESTION) return 0;
    auto& c = *static_cast<AdvertCtx*>(user);
    char namebuf[256];
    size_t off = name_offset;
    const mdns_string_t name = mdns_string_extract(data, size, &off, namebuf, sizeof(namebuf));
    const std::string asked(name.str, name.length);
    const auto same = [&](const std::string& s) {
        if (s.size() != asked.size()) return false;
        for (size_t i = 0; i < s.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(asked[i])))
                return false;
        return true;
    };
    const bool unicast = (rclass & MDNS_UNICAST_RESPONSE) != 0;
    auto add = extras(c);
    mdns_record_t answer{};
    if (same("_services._dns-sd._udp.local.") && (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY)) {
        answer.name = name;
        answer.type = MDNS_RECORDTYPE_PTR;
        answer.data.ptr.name = {c.service.c_str(), c.service.size()};
        add.clear();
    } else if (same(c.service) && (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY)) {
        answer = ptrRecord(c);
    } else if (same(c.instance) && (rtype == MDNS_RECORDTYPE_SRV || rtype == MDNS_RECORDTYPE_ANY)) {
        answer = add.front();
        add.erase(add.begin());
    } else if (same(c.host) && (rtype == MDNS_RECORDTYPE_A || rtype == MDNS_RECORDTYPE_ANY)) {
        answer = add[1];
        add.clear();
    } else {
        return 0;
    }
    if (unicast)
        mdns_query_answer_unicast(sock, from, addrlen, c.buf.data(), c.buf.size(), query_id,
                                  static_cast<mdns_record_type_t>(rtype), name.str, name.length, answer, nullptr, 0,
                                  add.data(), add.size());
    else
        mdns_query_answer_multicast(sock, c.buf.data(), c.buf.size(), answer, nullptr, 0, add.data(), add.size());
    return 0;
}

std::string label(const std::string& s) {
    std::string out;
    for (unsigned char ch : s)
        if (std::isalnum(ch) || ch == '-') out.push_back(static_cast<char>(ch));
    return out.empty() ? "hms-cpap" : out.substr(0, 60);
}

}  // namespace

Advertiser::Advertiser(std::string instance, int port, std::string install_id, std::string version)
    : instance_(label(instance)), install_id_(std::move(install_id)), version_(std::move(version)), port_(port) {}

Advertiser::~Advertiser() { stop(); }

void Advertiser::start() {
    if (worker_.joinable()) return;
    netInit();
    // The wake socket: stop() sends it one byte, which ends the select below.
    wake_ = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (wake_ >= 0) {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;
        socklen_t len = sizeof(sa);
        if (::bind(wake_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0 &&
            ::getsockname(wake_, reinterpret_cast<sockaddr*>(&sa), &len) == 0)
            wake_port_ = ntohs(sa.sin_port);
    }
    stop_ = false;
    worker_ = std::thread([this] { run(); });
}

void Advertiser::stop() {
    stop_ = true;
    if (wake_ >= 0 && wake_port_) {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(static_cast<uint16_t>(wake_port_));
        const char b = 1;
        ::sendto(wake_, &b, 1, 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    }
    if (worker_.joinable()) worker_.join();
    if (wake_ >= 0) closeSock(wake_);
    wake_ = -1;
}

void Advertiser::run() {
    std::vector<std::pair<int, std::unique_ptr<AdvertCtx>>> socks;
    for (uint32_t ifaddr : interfaces()) {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = ifaddr;
        sa.sin_port = htons(MDNS_PORT);
#ifdef __APPLE__
        sa.sin_len = sizeof(sockaddr_in);
#endif
        const int s = mdns_socket_open_ipv4(&sa);
        if (s < 0) continue;
        auto c = std::make_unique<AdvertCtx>();
        c->service = std::string(kMdnsService) + ".";
        c->instance = instance_ + "." + c->service;
        c->host = instance_ + ".local.";
        c->port = static_cast<uint16_t>(port_);
        c->addr = ifaddr;
        c->txt_id = install_id_;
        c->txt_version = version_;
        // Announced once, at start (an event); after that only answers.
        auto add = extras(*c);
        mdns_announce_multicast(s, c->buf.data(), c->buf.size(), ptrRecord(*c), nullptr, 0, add.data(), add.size());
        socks.emplace_back(s, std::move(c));
    }
    if (socks.empty()) {
        std::cout << kTag << "mDNS port 5353 not available here; not advertised (the helper finds this hms-cpap "
                     "on the network, or by its address)" << std::endl;
        return;
    }
    std::cout << kTag << "advertised as " << instance_ << "." << kMdnsService << " on port " << port_ << std::endl;
    std::vector<uint8_t> buf(4096);
    while (!stop_) {
        fd_set fds;
        FD_ZERO(&fds);
        int maxfd = 0;
        for (const auto& [s, c] : socks) {
            FD_SET(s, &fds);
            maxfd = std::max(maxfd, s);
        }
        if (wake_ >= 0) {
            FD_SET(wake_, &fds);
            maxfd = std::max(maxfd, wake_);
        }
        // Blocked until a question or stop(): no timeout.
        if (::select(maxfd + 1, &fds, nullptr, nullptr, nullptr) < 0) break;
        if (stop_) break;
        for (auto& [s, c] : socks)
            if (FD_ISSET(s, &fds)) mdns_socket_listen(s, buf.data(), buf.size(), onQuestion, c.get());
        if (wake_ >= 0 && FD_ISSET(wake_, &fds)) {
            char b[16];
            ::recv(wake_, b, sizeof(b), 0);
        }
    }
    for (auto& [s, c] : socks) {
        auto add = extras(*c);
        mdns_goodbye_multicast(s, c->buf.data(), c->buf.size(), ptrRecord(*c), nullptr, 0, add.data(), add.size());
        mdns_socket_close(s);
    }
}

}  // namespace hms_cpap::oscar::helper
