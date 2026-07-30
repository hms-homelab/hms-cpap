#include "services/PreflightService.h"

#include "services/SetupService.h"
#include "utils/AppConfig.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

namespace hms_cpap {

namespace {

/// Close a socket without caring which platform's spelling applies.
void closeSock(long long s) {
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(s));
#else
    ::close(static_cast<int>(s));
#endif
}

}  // namespace

PreflightService::Check PreflightService::checkPort(const std::string& bind_addr, int port) {
    Check c;
    c.name = "web_port";

    if (port <= 0 || port > 65535) {
        c.ok = false;
        c.detail = "web_port is " + std::to_string(port) + ", which is not a usable port";
        c.remedy = "Set \"web_port\" in config.json to a number between 1 and 65535.";
        return c;
    }

#ifdef _WIN32
    WSADATA wsa;
    // Idempotent and refcounted, so calling it here is safe even though the
    // server will call it again later.
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        c.ok = false;
        c.detail = "Windows networking could not be initialised";
        c.remedy = "Restart the machine, then try again.";
        return c;
    }
#endif

    const auto sock = ::socket(AF_INET, SOCK_STREAM, 0);
    bool created = true;
#ifdef _WIN32
    created = (sock != INVALID_SOCKET);
#else
    created = (sock >= 0);
#endif
    if (!created) {
        c.ok = false;
        c.detail = "could not create a socket to test port " + std::to_string(port);
        c.remedy = "Check that networking is available on this machine.";
        return c;
    }

    // NOTE: no SO_REUSEADDR on purpose. The question being asked is "will the
    // real listener bind?", and enabling reuse answers a more optimistic
    // question, which would let the check pass and the service still fail.

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    const std::string host = (bind_addr.empty() || bind_addr == "0.0.0.0")
                             ? "0.0.0.0" : bind_addr;
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    const int rc = ::bind(static_cast<
#ifdef _WIN32
        SOCKET
#else
        int
#endif
        >(sock), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    closeSock(static_cast<long long>(sock));

    if (rc != 0) {
        c.ok = false;
        c.detail = "port " + std::to_string(port) + " is already in use on " + host;
        // Named remedy, not "try again": this is deterministic and retrying
        // changes nothing.
        c.remedy = "Another program is using this port. Either stop it, or set "
                   "\"web_port\" in config.json to a free port and start again.";
        return c;
    }

    c.ok = true;
    c.detail = "port " + std::to_string(port) + " is free";
    return c;
}

PreflightService::Check PreflightService::checkWritableDir(const std::string& name,
                                                           const std::string& path) {
    Check c;
    c.name = name;

    if (path.empty()) {
        c.ok = true;                 // nothing configured is not a failure
        c.detail = "not configured";
        return c;
    }

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (!std::filesystem::exists(path, ec)) {
        c.ok = false;
        c.detail = path + " does not exist and could not be created";
        c.remedy = "Create the folder yourself, or point this setting at one that exists.";
        return c;
    }

    // Actually write. Permission bits lie often enough (read-only mounts, ACLs,
    // full disks) that the only honest test is the operation itself.
    const auto probe = std::filesystem::path(path) / ".hms-write-test";
    {
        std::ofstream out(probe);
        if (!out) {
            c.ok = false;
            c.detail = path + " is not writable";
            c.remedy = "Grant write permission to this folder, or choose another one.";
            return c;
        }
        out << "ok";
    }
    std::filesystem::remove(probe, ec);

    c.ok = true;
    c.detail = path + " is writable";
    return c;
}

PreflightService::Check PreflightService::checkDatabase(const AppConfig& cfg) {
    Check c;
    c.name = "database";

    const auto& d = cfg.database;

    if (!SetupService::supportsBackend(d.type)) {
        c.ok = false;
        c.detail = "this build cannot open a '" + d.type + "' database";
        c.remedy = "Set \"database.type\" to one of the backends this build "
                   "supports, or install a build that includes it.";
        return c;
    }

    // Same probe the setup wizard's "test connection" uses, so the two can
    // never disagree about what reachable means.
    const auto probe = SetupService::probeDatabase(
        d.type, d.host, d.port, d.name, d.user, d.password, d.sqlite_path);

    if (!probe.ok) {
        c.ok = false;
        c.detail = "cannot open the " + d.type + " database: " + probe.error;
        c.remedy = d.type == "sqlite"
            ? "Check that the file in \"database.sqlite_path\" is readable."
            : "Check host, port, database name, user and password under "
              "\"database\" in config.json, and that the server is running.";
        return c;
    }

    c.ok = true;
    c.detail = probe.schema_present
        ? d.type + " reachable, " + std::to_string(probe.session_count) + " session(s) stored"
        : d.type + " reachable, schema will be created on first start";
    return c;
}

PreflightService::Check PreflightService::checkSource(const AppConfig& cfg) {
    Check c;
    c.name = "source";

    if (cfg.source == "local") {
        if (cfg.local_dir.empty()) {
            c.ok = false;
            c.detail = "source is 'local' but no folder is configured";
            c.remedy = "Set \"local_dir\" in config.json to the DATALOG folder.";
            return c;
        }
        std::error_code ec;
        if (!std::filesystem::exists(cfg.local_dir, ec)) {
            c.ok = false;
            c.detail = "local_dir does not exist: " + cfg.local_dir;
            c.remedy = "Point \"local_dir\" at a folder that exists, or insert the card.";
            return c;
        }
        c.ok = true;
        c.detail = "reading from " + cfg.local_dir;
        return c;
    }

    // ezShare and the Mule and Miner are NETWORK sources. Their reachability is
    // deliberately not a startup condition: the card is powered by the CPAP
    // machine and is usually unreachable at boot, so failing here would refuse
    // to start for the entire population of laptop users. That is the
    // collector's problem, and it already retries.
    c.ok = true;
    c.fatal = false;
    c.detail = "source is '" + cfg.source + "' (network); reachability is checked "
               "while running, not at startup";
    return c;
}

PreflightService::Report PreflightService::run(const AppConfig& cfg) {
    Report r;
    r.checks.push_back(checkWritableDir("data_dir", AppConfig::dataDir()));
    r.checks.push_back(checkPort("0.0.0.0", cfg.web_port));
    r.checks.push_back(checkDatabase(cfg));
    r.checks.push_back(checkSource(cfg));
    if (!cfg.archive_dir.empty())
        r.checks.push_back(checkWritableDir("archive_dir", cfg.archive_dir));
    return r;
}

std::string PreflightService::Report::render() const {
    std::ostringstream os;
    for (const auto& c : checks) {
        os << (c.ok ? "  ok    " : (c.fatal ? "  FAIL  " : "  warn  "))
           << c.name << ": " << c.detail << "\n";
        if (!c.ok && !c.remedy.empty()) os << "        -> " << c.remedy << "\n";
    }
    return os.str();
}

}  // namespace hms_cpap
