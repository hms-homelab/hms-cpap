#include "services/SetupService.h"

#include "utils/AppConfig.h"

#include <sqlite3.h>
#ifdef WITH_POSTGRESQL
  #include <pqxx/pqxx>
#endif
#ifdef WITH_MYSQL
  // Via the backend header rather than a bare <mysql.h>: vcpkg, Homebrew and
  // Debian each put the client headers somewhere different, and MySQLDatabase.h
  // already resolves that with __has_include. A second, weaker include here
  // broke the MSVC build with "cannot open include file: 'mysql.h'".
  #include "database/MySQLDatabase.h"
#endif

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <climits>
  #include <unistd.h>   // isatty/STDIN_FILENO on every POSIX, not just Linux
  #ifdef __APPLE__
    #include <mach-o/dyld.h>
  #endif
#endif

namespace hms_cpap {

namespace {

// Reported so the wizard can label the download the user is running and, more
// usefully, so a support ticket carries it without asking. Kept coarse on
// purpose: this is a label, not a feature test. Anything behaviour-changing
// belongs in Capabilities as its own flag.
const char* platformName() {
#if defined(_WIN32)
  #if defined(_M_ARM64)
    return "windows-arm64";
  #else
    return "windows-x64";
  #endif
#elif defined(__APPLE__)
  #if defined(__aarch64__) || defined(__arm64__)
    return "darwin-arm64";
  #else
    return "darwin-x64";
  #endif
#else
  #if defined(__aarch64__)
    return "linux-arm64";
  #else
    return "linux-x64";
  #endif
#endif
}

}  // namespace

SetupService::Capabilities SetupService::capabilities() {
    Capabilities c;
    c.version = HMS_CPAP_VERSION;

    // SQLite is unconditional: it is the embedded default and has no build flag.
    c.backends.emplace_back("sqlite");
#ifdef WITH_POSTGRESQL
    c.backends.emplace_back("postgresql");
#endif
#ifdef WITH_MYSQL
    c.backends.emplace_back("mysql");
#endif

    // gnuplot + libharu are wired only on POSIX (see main.cpp), so a Windows
    // build genuinely has no reports. Saying so lets the wizard stop implying a
    // feature that is not there.
#ifndef _WIN32
    c.pdf_reports = true;
#endif

    // Discovery ships whenever the binary does; it is not behind a build flag.
    c.mdns_discovery = true;

    c.platform = platformName();
    c.data_dir = AppConfig::dataDir();
    return c;
}

bool SetupService::supportsBackend(const std::string& type) {
    const auto caps = capabilities();
    for (const auto& b : caps.backends) {
        if (b == type) return true;
    }
    return false;
}

std::string SetupService::executablePath() {
#if defined(_WIN32)
    // MAX_PATH is not the ceiling on modern Windows; grow until it fits rather
    // than truncating a long install path into something that does not exist.
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(),
                                             static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size() - 1) {
            return std::filesystem::path(std::wstring(buf.data(), n)).string();
        }
        if (buf.size() > 65536) return {};   // refuse to grow without bound
        buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    // _NSGetExecutablePath reports the required size when the buffer is short,
    // and may hand back a path containing symlinks or "..", so canonicalise.
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(
        std::filesystem::path(buf.data()), ec);
    return ec ? std::string(buf.data()) : canonical.string();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return p.string();
#endif
}

std::string SetupService::executableDir() {
    const auto exe = executablePath();
    if (exe.empty()) return {};
    std::error_code ec;
    auto dir = std::filesystem::path(exe).parent_path();
    if (dir.empty()) return {};
    (void)ec;
    return dir.string();
}

std::string SetupService::resolveStaticDir(const std::string& configured) {
    // An explicit choice always wins. Empty and the historical default both
    // mean "the user never set this", since neither can be told apart from an
    // untouched config.
    if (!configured.empty() && configured != kLegacyStaticDir) return configured;

    const auto dir = executableDir();
    if (!dir.empty()) {
        std::error_code ec;
        auto candidate = std::filesystem::path(dir) / "static" / "browser";
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate.string();
        }
    }

    // Nothing next to the binary: fall back to the historical behaviour so a
    // developer running from the build directory is unaffected.
    return kLegacyStaticDir;
}

bool SetupService::shouldOpenBrowser(const LaunchContext& ctx) {
    if (ctx.setup_complete)  return false;   // already configured
    if (ctx.no_browser_flag) return false;   // explicitly declined
    if (ctx.supervised)      return false;   // the shell owns presentation
    if (!ctx.interactive)    return false;   // no human, no browser
    return true;
}

std::string SetupService::setupUrl(int web_port) {
    // localhost, not the configured bind address: 0.0.0.0 is not a thing a
    // browser can navigate to, and this URL is only ever opened on the machine
    // the service is running on.
    return "http://localhost:" + std::to_string(web_port) + "/setup";
}

bool SetupService::isInteractiveSession() {
#ifdef _WIN32
    // A service or a detached process has no console window.
    return ::GetConsoleWindow() != nullptr;
#else
    // stdin being a tty is the closest portable proxy. A Docker run without
    // -t, a systemd unit and a cron job all fail this, which is exactly the
    // set we want to skip.
    if (::isatty(STDIN_FILENO) == 1) return true;
    // A desktop session started from a launcher may have no tty but does have
    // a display, so treat that as interactive too.
    return std::getenv("DISPLAY") != nullptr ||
           std::getenv("WAYLAND_DISPLAY") != nullptr;
#endif
}

bool SetupService::openInBrowser(const std::string& url) {
    // The URL is built by setupUrl() from an int port, never from user input,
    // so there is nothing here to quote-escape. If that ever stops being true
    // this needs revisiting before it reaches a shell.
#if defined(_WIN32)
    const auto res = reinterpret_cast<INT_PTR>(
        ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return res > 32;   // ShellExecute's documented success threshold
#elif defined(__APPLE__)
    return std::system(("open " + url + " >/dev/null 2>&1 &").c_str()) == 0;
#else
    return std::system(("xdg-open " + url + " >/dev/null 2>&1 &").c_str()) == 0;
#endif
}

// ---------------------------------------------------------------------------
// Database provisioning (SDD-006 phase 2)
// ---------------------------------------------------------------------------

bool SetupService::isSafeIdentifier(const std::string& name) {
    // 63 rather than 64: PostgreSQL silently TRUNCATES at 63, so a 64-character
    // name would provision one database and then be configured to open another.
    if (name.empty() || name.size() > 63) return false;

    const unsigned char first = static_cast<unsigned char>(name[0]);
    if (!(std::isalpha(first) || first == '_')) return false;

    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || u == '_')) return false;
    }
    return true;
}

SetupService::ProvisionPlan SetupService::provisionPlan(const std::string& engine,
                                                        const std::string& db_name,
                                                        const std::string& owner_user) {
    ProvisionPlan plan;

    // Re-checked here even though the caller validates: this function emits raw
    // SQL, so it must not depend on someone else having been careful.
    if (!isSafeIdentifier(db_name)) {
        plan.error = "database name must be letters, digits and underscore, "
                     "not starting with a digit, at most 63 characters";
        return plan;
    }
    if (!isSafeIdentifier(owner_user)) {
        plan.error = "database user must be letters, digits and underscore, "
                     "not starting with a digit, at most 63 characters";
        return plan;
    }

    if (engine == "postgresql") {
        plan.maintenance_db = "postgres";
        // CREATE USER before CREATE DATABASE: OWNER has to name an existing
        // role. The reverse order fails on a fresh server, which is the only
        // case this endpoint exists for.
        plan.statements.push_back({std::string("CREATE ROLE ") + owner_user +
                                   " LOGIN PASSWORD " + kPasswordToken, true});
        plan.statements.push_back({"CREATE DATABASE " + db_name +
                                   " OWNER " + owner_user, false});
        // Belt and braces on a server where the role predates us and may not
        // own the database it was just handed.
        plan.statements.push_back({"GRANT ALL PRIVILEGES ON DATABASE " + db_name +
                                   " TO " + owner_user, false});
        return plan;
    }

    if (engine == "mysql") {
        plan.maintenance_db = "mysql";
        plan.statements.push_back({"CREATE DATABASE IF NOT EXISTS " + db_name +
                                   " CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci",
                                   false});
        // '%' rather than 'localhost': the wizard's whole purpose is pointing at
        // a database on another box, and a localhost-only grant authenticates
        // during setup (run from the server) and then fails from the app host.
        plan.statements.push_back({std::string("CREATE USER IF NOT EXISTS '") + owner_user +
                                   "'@'%' IDENTIFIED BY " + kPasswordToken, true});
        plan.statements.push_back({"GRANT ALL PRIVILEGES ON " + db_name +
                                   ".* TO '" + owner_user + "'@'%'", false});
        plan.statements.push_back({"FLUSH PRIVILEGES", false});
        return plan;
    }

    // sqlite lands here on purpose: it has no server to provision against, and
    // the file is created on first open.
    plan.error = "engine '" + engine + "' cannot be provisioned; "
                 "only postgresql and mysql have a server to create a database on";
    return plan;
}

SetupService::RestartMode SetupService::restartMode(bool supervised,
                                                   const std::string& exe_path) {
    // Checked first, and deliberately without consulting exe_path: under the
    // shell, exiting 0 IS the restart, so a platform whose executable-path call
    // failed still restarts correctly when supervised.
    if (supervised) return RestartMode::SupervisedExit;
    if (exe_path.empty()) return RestartMode::Unsupported;
    return RestartMode::ReExec;
}

bool SetupService::isSupervised() {
    const char* v = std::getenv("HMS_CPAP_SUPERVISED");
    return v && std::string(v) == "1";
}

// ---------------------------------------------------------------------------
// Live database checks (SDD-006 phase 2)
//
// These talk to the client libraries DIRECTLY rather than going through
// IDatabase, because IDatabase::connect() creates and migrates the schema as a
// side effect. A probe has to be able to report "that is not the database you
// meant" without having already written to it.
// ---------------------------------------------------------------------------

SetupService::DbProbe SetupService::probeDatabase(const std::string& type,
                                                  const std::string& host, int port,
                                                  const std::string& name,
                                                  const std::string& user,
                                                  const std::string& password,
                                                  const std::string& sqlite_path) {
    DbProbe out;

    if (type == "sqlite") {
        // A missing file is a perfectly good answer here: it is created on first
        // open, so "not there yet" means new, not broken.
        const std::string path = sqlite_path.empty()
            ? AppConfig::dataDir() + "/cpap.db" : sqlite_path;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            out.ok = true;
            return out;
        }
        sqlite3* raw = nullptr;
        // READONLY: a probe must not create the file it is asked about.
        if (sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            out.error = raw ? sqlite3_errmsg(raw) : "cannot open database file";
            if (raw) sqlite3_close(raw);
            return out;
        }
        out.ok = true;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM cpap_sessions", -1, &st, nullptr)
                == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                out.schema_present = true;
                out.session_count = sqlite3_column_int64(st, 0);
            }
        }
        if (st) sqlite3_finalize(st);
        sqlite3_close(raw);
        return out;
    }

#ifdef WITH_POSTGRESQL
    if (type == "postgresql") {
        try {
            pqxx::connection c("host=" + host +
                               " port=" + std::to_string(port > 0 ? port : 5432) +
                               " dbname=" + name +
                               " user=" + user +
                               " password=" + password +
                               " connect_timeout=5");
            if (!c.is_open()) { out.error = "connection refused"; return out; }
            out.ok = true;
            pqxx::work txn(c);
            auto r = txn.exec("SELECT COUNT(*) FROM information_schema.tables"
                              " WHERE table_name = 'cpap_sessions'");
            if (!r.empty() && r[0][0].as<int>(0) > 0) {
                out.schema_present = true;
                auto n = txn.exec("SELECT COUNT(*) FROM cpap_sessions");
                if (!n.empty()) out.session_count = n[0][0].as<long long>(0);
            }
            txn.commit();
        } catch (const std::exception& e) {
            out.ok = false;
            out.error = e.what();
        }
        return out;
    }
#endif

#ifdef WITH_MYSQL
    if (type == "mysql") {
        MYSQL* c = mysql_init(nullptr);
        if (!c) { out.error = "mysql_init failed"; return out; }
        unsigned int timeout = 5;
        mysql_options(c, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        if (!mysql_real_connect(c, host.c_str(), user.c_str(), password.c_str(),
                                name.c_str(), port > 0 ? port : 3306, nullptr, 0)) {
            out.error = mysql_error(c);
            mysql_close(c);
            return out;
        }
        out.ok = true;
        if (mysql_query(c, "SELECT COUNT(*) FROM cpap_sessions") == 0) {
            if (MYSQL_RES* res = mysql_store_result(c)) {
                if (MYSQL_ROW row = mysql_fetch_row(res)) {
                    out.schema_present = true;
                    out.session_count = row[0] ? std::atoll(row[0]) : 0;
                }
                mysql_free_result(res);
            }
        }
        mysql_close(c);
        return out;
    }
#endif

    out.error = "backend '" + type + "' is not available in this build";
    return out;
}

SetupService::DbProbe SetupService::provisionDatabase(const std::string& engine,
                                                      const std::string& host, int port,
                                                      const std::string& db_name,
                                                      const std::string& owner_user,
                                                      const std::string& owner_password,
                                                      const std::string& admin_user,
                                                      const std::string& admin_password) {
    DbProbe out;

    const auto plan = provisionPlan(engine, db_name, owner_user);
    if (!plan.error.empty()) { out.error = plan.error; return out; }

#ifdef WITH_POSTGRESQL
    if (engine == "postgresql") {
        try {
            pqxx::connection admin("host=" + host +
                                   " port=" + std::to_string(port > 0 ? port : 5432) +
                                   " dbname=" + plan.maintenance_db +
                                   " user=" + admin_user +
                                   " password=" + admin_password +
                                   " connect_timeout=5");
            // Skip CREATE ROLE when the role is already there. PostgreSQL checks
            // the CREATEROLE privilege BEFORE it checks existence, so an admin
            // who can create databases but not roles is refused outright even
            // when there is nothing to create. Pointing the wizard at an
            // existing user is an ordinary thing to do, and it should not
            // require a privilege it never actually uses.
            bool role_exists = false;
            try {
                pqxx::nontransaction probe(admin);
                auto r = probe.exec_params(
                    "SELECT 1 FROM pg_roles WHERE rolname = $1", owner_user);
                role_exists = !r.empty();
            } catch (...) {
                // Not fatal: fall through and let CREATE ROLE report for itself.
            }

            for (const auto& st : plan.statements) {
                if (role_exists && st.sql.rfind("CREATE ROLE", 0) == 0) continue;
                // CREATE DATABASE cannot run inside a transaction block, so each
                // statement gets its own nontransaction.
                pqxx::nontransaction tx(admin);
                try {
                    std::string sql = st.sql;
                    if (st.binds_password) {
                        // quote() returns a fully-quoted, escaped literal. Doing
                        // this by hand is how a quote in a password becomes an
                        // injection; the driver knows the server's rules.
                        const auto pos = sql.find(kPasswordToken);
                        if (pos != std::string::npos) {
                            sql.replace(pos, std::strlen(kPasswordToken),
                                        admin.quote(owner_password));
                        }
                    }
                    tx.exec(sql);
                } catch (const std::exception& e) {
                    // "already exists" is a fine outcome for a wizard someone ran
                    // twice; anything else is real.
                    const std::string msg = e.what();
                    if (msg.find("already exists") == std::string::npos) throw;
                }
            }
        } catch (const std::exception& e) {
            out.error = e.what();
            return out;
        }
    }
#endif
#ifdef WITH_MYSQL
    if (engine == "mysql") {
        MYSQL* c = mysql_init(nullptr);
        if (!c) { out.error = "mysql_init failed"; return out; }
        unsigned int timeout = 5;
        mysql_options(c, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        if (!mysql_real_connect(c, host.c_str(), admin_user.c_str(),
                                admin_password.c_str(), plan.maintenance_db.c_str(),
                                port > 0 ? port : 3306, nullptr, 0)) {
            out.error = mysql_error(c);
            mysql_close(c);
            return out;
        }
        for (const auto& st : plan.statements) {
            std::string sql = st.sql;
            if (st.binds_password) {
                // CREATE USER will not take a placeholder either, so escape with
                // the driver (which knows the connection charset) and inline.
                std::vector<char> esc(owner_password.size() * 2 + 1);
                const unsigned long n = mysql_real_escape_string(
                    c, esc.data(), owner_password.c_str(),
                    static_cast<unsigned long>(owner_password.size()));
                const auto pos = sql.find(kPasswordToken);
                if (pos != std::string::npos) {
                    sql.replace(pos, std::strlen(kPasswordToken),
                                "'" + std::string(esc.data(), n) + "'");
                }
            }
            if (mysql_query(c, sql.c_str()) != 0) {
                out.error = mysql_error(c);
                mysql_close(c);
                return out;
            }
        }
        mysql_close(c);
    }
#endif

    // Reconnect as the ORDINARY user. This is the whole point: a successful
    // CREATE DATABASE followed by a failed GRANT looks like success right up
    // until first boot, and this is where that gets caught.
    auto verify = probeDatabase(engine, host, port, db_name, owner_user,
                                owner_password, "");
    if (!verify.ok) {
        out.error = "created, but the new credentials could not open it: " + verify.error;
        return out;
    }
    return verify;
}

// ---------------------------------------------------------------------------
// Start at login (SDD-006 phase 4)
// ---------------------------------------------------------------------------

bool SetupService::canManageAutostart(bool supervised) {
    // The installer owns it when there is an installer. Writing our own entry
    // under the shell would mean two things racing to start one service.
    return !supervised;
}

namespace {

/// The account a boot service runs as.
///
/// A system service starts with no user context: on macOS a LaunchDaemon runs as
/// root with HOME=/var/root, and a systemd system unit runs as root with no HOME
/// at all. Both would then resolve ~/.hms-cpap somewhere the user cannot see,
/// build a SECOND empty database, and look like the app lost every night of
/// data. Pinning the account and HOME is what stops that.
std::string currentUser(const std::string& override_name) {
    if (!override_name.empty()) return override_name;
#ifdef _WIN32
    const char* u = std::getenv("USERNAME");
#else
    const char* u = std::getenv("USER");
    if (!u || !*u) u = std::getenv("LOGNAME");
#endif
    return (u && *u) ? std::string(u) : std::string();
}

}  // namespace

SetupService::AutostartEntry SetupService::bootEntry(const std::string& exe_path,
                                                     const std::string& home_dir,
                                                     const std::string& user_name) {
    AutostartEntry e;
    e.needs_elevation = true;   // true on every platform for a real boot service
    const std::string user = currentUser(user_name);

#if defined(_WIN32)
    (void)home_dir;
    // A plain console program CANNOT be a Windows service. The Service Control
    // Manager expects StartServiceCtrlDispatcher and a running-status report
    // within its timeout; a binary that never answers is killed after about
    // thirty seconds, which presents as "the service starts and immediately
    // stops". Rather than build SCM support into hms_cpap, wrap it: shawl is a
    // single portable MIT-licensed executable that speaks the service protocol
    // and forwards Ctrl+C to the child, which this program already handles.
    e.requires_helper = "shawl (MIT, https://github.com/mtkennerly/shawl) placed "
                        "next to hms_cpap.exe";
    e.install_command =
        "sc create HmsCpap binPath= \"\\\"%~dp0shawl.exe\\\" run --name HmsCpap -- "
        "\\\"" + exe_path + "\\\" --no-browser\" start= auto";
    e.uninstall_command = "sc delete HmsCpap";
    return e;

#elif defined(__APPLE__)
    if (user.empty()) { e.error = "cannot determine which account to run as"; return e; }
    // Label distinct from BOTH the login agent and SDD-005's, so the three can
    // coexist rather than overwrite one another.
    e.path = "/Library/LaunchDaemons/com.hms.cpap.daemon.plist";
    e.content =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>Label</key>\n"
        "  <string>com.hms.cpap.daemon</string>\n"
        "  <key>ProgramArguments</key>\n"
        "  <array>\n"
        "    <string>" + exe_path + "</string>\n"
        "    <string>--no-browser</string>\n"
        "  </array>\n"
        // Without UserName this runs as root and writes a root-owned database
        // the user cannot read.
        "  <key>UserName</key>\n"
        "  <string>" + user + "</string>\n"
        // Without HOME a daemon resolves ~/.hms-cpap under /var/root and starts
        // from an empty database, which looks exactly like data loss.
        "  <key>EnvironmentVariables</key>\n"
        "  <dict>\n"
        "    <key>HOME</key>\n"
        "    <string>" + home_dir + "</string>\n"
        "  </dict>\n"
        "  <key>RunAtLoad</key>\n"
        "  <true/>\n"
        "  <key>KeepAlive</key>\n"
        "  <true/>\n"
        "</dict>\n"
        "</plist>\n";
    e.install_command =
        "sudo cp <generated> " + e.path +
        " && sudo chown root:wheel " + e.path +
        " && sudo launchctl load -w " + e.path;
    e.uninstall_command = "sudo launchctl unload -w " + e.path +
                          " && sudo rm " + e.path;
    return e;

#else
    if (user.empty()) { e.error = "cannot determine which account to run as"; return e; }
    e.path = "/etc/systemd/system/hms-cpap.service";
    e.content =
        "[Unit]\n"
        "Description=HMS-CPAP\n"
        "After=network-online.target\n"
        "Wants=network-online.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "User=" + user + "\n"
        // Same reason as the macOS EnvironmentVariables block: a system unit has
        // no HOME, so the data directory would resolve somewhere else.
        "Environment=HOME=" + home_dir + "\n"
        "ExecStart=" + exe_path + " --no-browser\n"
        "Restart=on-failure\n"
        "RestartSec=5\n"
        "\n"
        "[Install]\n"
        // multi-user.target here, unlike the user unit: this one genuinely is a
        // system service and must come up without anyone logging in.
        "WantedBy=multi-user.target\n";
    e.install_command =
        "sudo cp <generated> " + e.path +
        " && sudo systemctl daemon-reload"
        " && sudo systemctl enable --now hms-cpap";
    e.uninstall_command =
        "sudo systemctl disable --now hms-cpap && sudo rm " + e.path;
    // The no-root route: a USER unit plus lingering also survives a reboot
    // without anyone logging in, and needs no elevation on most distributions.
    e.alternative_command = "loginctl enable-linger " + user +
                            "  (keeps the per-user unit running at boot, no root)";
    return e;
#endif
}

SetupService::AutostartEntry SetupService::autostartEntry(const std::string& exe_path,
                                                          const std::string& home_dir,
                                                          AutostartScope scope,
                                                          const std::string& user_name) {
    AutostartEntry e;
    if (exe_path.empty()) {
        e.error = "cannot determine where this program lives, so it cannot be "
                  "registered to start automatically";
        return e;
    }

    if (scope == AutostartScope::Boot) return bootEntry(exe_path, home_dir, user_name);

#if defined(_WIN32)
    (void)home_dir;
    e.registry_key   = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    // Distinct from SDD-005's value name on purpose, so a zip install and the
    // desktop app can coexist instead of overwriting one another.
    e.registry_value = "HmsCpapCli";
    // Quoted: Program Files has a space in it, and an unquoted path there starts
    // the wrong executable or nothing at all.
    e.registry_data  = "\"" + exe_path + "\" --no-browser";
    return e;

#elif defined(__APPLE__)
    if (home_dir.empty()) { e.error = "no home directory"; return e; }
    // Distinct label from SDD-005's agent, same reason as the Windows value name.
    e.path = home_dir + "/Library/LaunchAgents/com.hms.cpap.cli.plist";
    e.content =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>Label</key>\n"
        "  <string>com.hms.cpap.cli</string>\n"
        "  <key>ProgramArguments</key>\n"
        "  <array>\n"
        "    <string>" + exe_path + "</string>\n"
        "    <string>--no-browser</string>\n"
        "  </array>\n"
        "  <key>RunAtLoad</key>\n"
        "  <true/>\n"
        "  <key>KeepAlive</key>\n"
        "  <true/>\n"
        "</dict>\n"
        "</plist>\n";
    return e;

#else
    if (home_dir.empty()) { e.error = "no home directory"; return e; }
    e.path = home_dir + "/.config/systemd/user/hms-cpap.service";
    e.content =
        "[Unit]\n"
        "Description=HMS-CPAP\n"
        "After=network-online.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=" + exe_path + " --no-browser\n"
        "Restart=on-failure\n"
        "RestartSec=5\n"
        "\n"
        "[Install]\n"
        // default.target, not multi-user.target: this is a USER unit and starts
        // at login. Claiming boot-time start would be a promise it cannot keep
        // without lingering enabled, which needs a separate opt-in.
        "WantedBy=default.target\n";
    return e;
#endif
}

}  // namespace hms_cpap
