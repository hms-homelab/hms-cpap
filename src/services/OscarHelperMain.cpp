#include "services/OscarHelperMain.h"

#include "services/CpapDashLink.h"
#include "services/OscarCardReader.h"
#include "services/OscarHelper.h"
#include "services/OscarHelperDiscovery.h"
#include "utils/FileLogger.h"
#include "utils/ParentWatch.h"

#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <pthread.h>
#endif

namespace hms_cpap::oscar::helper {

namespace fs = std::filesystem;

namespace {

std::mutex              g_mu;
std::condition_variable g_cv;
bool                    g_done = false;

void quit() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_done = true;
    }
    g_cv.notify_all();
}

#ifdef _WIN32
BOOL WINAPI onConsole(DWORD) {
    quit();
    return TRUE;
}
#endif

bool has(const std::vector<std::string>& a, const char* flag) {
    for (const auto& x : a)
        if (x == flag) return true;
    return false;
}

std::string valueOf(const std::vector<std::string>& a, const char* flag) {
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == flag) return a[i + 1];
    return "";
}

Helper::LinkFactory systemLinks() {
    return [](const CpapDashEndpoint& ep) { return makeCpapDashLink(ep); };
}

void writeStatus(const std::string& path, const Json::Value& v) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << frame(v) << "\n";
    }
    fs::rename(tmp, path, ec);
}

std::string ask(const std::string& q, const std::string& def) {
    std::cout << q << (def.empty() ? "" : " [" + def + "]") << ": " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return def;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line.empty() ? def : line;
}

const char* checkWord(const std::string& state) {
    if (state == "ok") return "ok";
    if (state == "empty") return "not filled in";
    if (state == "not_found") return "not found";
    if (state == "not_runnable") return "does not run";
    if (state == "no_batch") return "has no batch import (OSCAR 2.1.1 or later is needed)";
    if (state == "no_db") return "holds no oscar.db";
    if (state == "not_supported") return "is from an OSCAR not supported yet";
    if (state == "missing") return "is not in OSCAR: make it once in OSCAR (File > New Profile)";
    if (state == "unknown") return "cannot be checked until the data folder is right";
    return state.c_str();
}

int runDaemon(const std::string& data_dir, const std::string& config_path, const std::string& version,
              const std::vector<std::string>& args) {
#ifndef _WIN32
    // Before any thread: SIGINT and SIGTERM are taken by sigwait below.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread([set]() mutable {
        int sig = 0;
        sigwait(&set, &sig);
        quit();
    }).detach();
#else
    SetConsoleCtrlHandler(onConsole, TRUE);
#endif
    FileLogger::start(FileLogger::defaultPath(), 5 * 1024 * 1024, 3);
    if (has(args, "--exit-with-parent")) watchParentPipe(0, [] { quit(); });

    const std::string status = statusPath(data_dir);
    Helper::Options o;
    o.data_dir = data_dir;
    o.load = [config_path] { return loadHelperConfig(config_path); };
    o.save = [config_path](const HelperConfig& c) { saveHelperConfig(config_path, c); };
    o.factory = systemLinks();
    o.version = version;
    o.pc_name = hostName();
    o.on_state = [status](const Json::Value& v) {
        Json::Value out = v;
        out["updated_at"] = static_cast<Json::Int64>(std::time(nullptr));
        writeStatus(status, out);
    };
    std::cout << "OSCAR helper: hms-cpap " << version << " reads OSCAR on " << o.pc_name << " for hms-cpap"
              << std::endl;
    Helper helper(o);
    helper.start();
    {
        std::unique_lock<std::mutex> lk(g_mu);
        g_cv.wait(lk, [] { return g_done; });
    }
    helper.stop();
    std::cout << "OSCAR helper: stopped" << std::endl;
    FileLogger::stop();
    return 0;
}

int runSetup(const std::string& data_dir, const std::string& config_path) {
    (void)data_dir;
    HelperConfig c = loadHelperConfig(config_path);
    std::cout << "Read OSCAR for hms-cpap\n\nLooking for hms-cpap on this network..." << std::endl;
    const auto found = discover(systemLinks(), hostName(), c.port);
    std::string host;
    if (found.size() == 1) {
        std::cout << "Found hms-cpap at " << found[0].address
                  << (found[0].name.empty() ? "" : " (" + found[0].name + ")") << std::endl;
        if (!found[0].helper.empty() && found[0].helper != hostName())
            std::cout << "hms-cpap already reads OSCAR on " << found[0].helper << std::endl;
        host = ask("Use it? Type another address, or press Enter", found[0].address);
    } else if (found.size() > 1) {
        for (size_t i = 0; i < found.size(); ++i)
            std::cout << "  " << (i + 1) << ". " << found[i].name << " at " << found[i].address << std::endl;
        const std::string pick = ask("Which one (a number, or an address)", "1");
        try {
            const size_t n = std::stoul(pick);
            host = n >= 1 && n <= found.size() ? found[n - 1].address : pick;
        } catch (...) {
            host = pick;
        }
    } else {
        host = ask("Type the address of hms-cpap", c.host);
    }
    if (host.empty()) {
        std::cerr << "No address: nothing saved." << std::endl;
        return 1;
    }
    if (host != c.host) c.token.clear();
    c.host = host;

    std::cout << "\nOSCAR on this computer" << std::endl;
    c.oscar.program = ask("The OSCAR program", c.oscar.program.empty() ? findProgram() : c.oscar.program);
    c.oscar.data_dir = ask("OSCAR's data folder (holds oscar.db)", c.oscar.data_dir);
    c.oscar.profile = ask("The profile hms-cpap imports into", c.oscar.profile);
    SystemProcessRunner runner;
    const auto v = OscarCardReader::check(c.oscar, runner, false);
    std::cout << "  program:     " << checkWord(v["program"]["state"].asString()) << "\n"
              << "  data folder: " << checkWord(v["data_dir"]["state"].asString()) << "\n"
              << "  profile:     " << checkWord(v["profile"]["state"].asString()) << std::endl;
    if (!saveHelperConfig(config_path, c)) {
        std::cerr << "Could not save " << config_path << std::endl;
        return 1;
    }
    std::cout << "\nSaved to " << config_path << ". The helper reads it when it starts:\n"
              << "  sudo systemctl restart hms-cpap-oscar-helper" << std::endl;
    return v["ok"].asBool() ? 0 : 2;
}

}  // namespace

std::string statusPath(const std::string& data_dir) {
    return (fs::path(data_dir) / "oscar-helper" / "status.json").string();
}

int runHelperMain(const std::vector<std::string>& args, const std::string& data_dir, const std::string& config_path,
                  const std::string& version) {
    int port = kDefaultPort;
    try {
        if (!valueOf(args, "--port").empty()) port = std::stoi(valueOf(args, "--port"));
    } catch (...) {
    }
    if (has(args, "--discover")) {
        Json::Value v(Json::objectValue);
        v["found"] = Json::Value(Json::arrayValue);
        for (const auto& f : discover(systemLinks(), hostName(), port)) v["found"].append(toJson(f));
        v["pc_name"] = hostName();
        std::cout << frame(v) << std::endl;
        return 0;
    }
    if (has(args, "--probe")) {   // one typed address
        Json::Value v(Json::objectValue);
        const std::string host = valueOf(args, "--probe");
        if (auto f = probe(systemLinks(), host, port, hostName(), std::chrono::milliseconds(4000)))
            v["found"] = toJson(*f);
        std::cout << frame(v) << std::endl;
        return v.isMember("found") ? 0 : 1;
    }
    if (has(args, "--check")) {
        Settings s;
        s.program = valueOf(args, "--program");
        s.data_dir = valueOf(args, "--data-dir");
        if (!valueOf(args, "--profile").empty()) s.profile = valueOf(args, "--profile");
        Json::Value v(Json::objectValue);
        SystemProcessRunner runner;
        v = OscarCardReader::check(s, runner, false);
        std::cout << frame(v) << std::endl;
        return v["ok"].asBool() ? 0 : 2;
    }
    if (has(args, "--connect-again")) {
        HelperConfig c = loadHelperConfig(config_path);
        c.token.clear();
        return saveHelperConfig(config_path, c) ? 0 : 1;
    }
    if (has(args, "--setup")) return runSetup(data_dir, config_path);
    return runDaemon(data_dir, config_path, version, args);
}

}  // namespace hms_cpap::oscar::helper
