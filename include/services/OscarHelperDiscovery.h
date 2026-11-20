#pragma once
//
// SDD-081 D3: finding hms-cpap from the helper's PC, and hms-cpap's own mDNS
// advertisement.
//
// The helper's installer looks once, when its page opens (no timer):
//
//   1. mDNS: _home-assistant._tcp (the add-on's host) and _hms-cpap._tcp (a
//      native or host-network hms-cpap, Advertiser below);
//   2. a probe hello on 8894 to each host found, or else to each address of
//      this PC's own /24 that accepts a connection on 8894; an hms-cpap
//      answers {"t":"ack","hms_cpap":{name,install_id,version,helper}};
//   3. one found: used; several: a list by name; none: the address is typed.
//
// The waits here are each one look's own limits (an mDNS answer, a TCP
// connect, a probe's answer), never a schedule.
//
#include "services/OscarHelper.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hms_cpap::oscar::helper {

struct Found {
    std::string address;
    int         port{kDefaultPort};
    std::string name;
    std::string install_id;
    std::string version;
    std::string helper;   ///< the PC it already reads OSCAR on, "" for none
};
Json::Value toJson(const Found& f);

/// The source addresses of every mDNS answer naming _hms-cpap._tcp or
/// _home-assistant._tcp, within `wait`.
std::vector<std::string> mdnsHosts(std::chrono::milliseconds wait);

/// This PC's own IPv4 addresses (not loopback).
std::vector<std::string> localAddresses();
/// Every other address of the /24 of each of `own`.
std::vector<std::string> subnetOf(const std::vector<std::string>& own);

/// Those of `hosts` that accept a TCP connection on `port` within `wait`.
std::vector<std::string> acceptingOn(const std::vector<std::string>& hosts, int port, std::chrono::milliseconds wait);

/// One probe hello (X-Hms-Probe): what answered, or nullopt.
std::optional<Found> probe(const Helper::LinkFactory& factory, const std::string& host, int port,
                           const std::string& pc_name, std::chrono::milliseconds wait);

/// D3 steps 1 and 2. Every hms-cpap that answered, by address.
std::vector<Found> discover(const Helper::LinkFactory& factory, const std::string& pc_name, int port = kDefaultPort);

/// hms-cpap's _hms-cpap._tcp advertisement (D3.1), on a native or a
/// host-network install. It answers mDNS questions as they come (a thread
/// blocked on its sockets) and announces once at start. Where 5353 cannot be
/// bound it says so and does nothing.
class Advertiser {
public:
    Advertiser(std::string instance, int port, std::string install_id, std::string version);
    ~Advertiser();
    void start();
    void stop();

private:
    void run();
    std::string instance_, install_id_, version_;
    int port_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    int wake_{-1};      ///< a loopback UDP socket stop() writes to
    int wake_port_{0};
};

}  // namespace hms_cpap::oscar::helper
