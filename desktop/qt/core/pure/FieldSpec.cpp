#include "FieldSpec.h"

#include <regex>

namespace cpapdash::supervisor {

namespace {

/// Reads as a predicate at the call site: enabledBy("mqtt.enabled").
std::function<bool(const ConfigModel&)> enabledBy(const std::string& path) {
    return [path](const ConfigModel& m) { return m.getBool(path); };
}

std::function<bool(const ConfigModel&)> sourceIs(const std::string& value) {
    return [value](const ConfigModel& m) { return m.getString("source") == value; };
}

std::function<bool(const ConfigModel&)> allOf(
        std::function<bool(const ConfigModel&)> a,
        std::function<bool(const ConfigModel&)> b) {
    return [a, b](const ConfigModel& m) { return a(m) && b(m); };
}

/// Mirrors PreflightService::sourceNeedsArchive.
bool sourceNeedsArchive(const ConfigModel& m) {
    const auto s = m.getString("source");
    return s == "ezshare" || s == "fysetc";
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

const std::vector<Group>& settingsGroups() {
    static const std::vector<Group> groups = {
        {"source",      "Data Source",          "Where therapy files come from.", ""},
        {"database",    "Database",             "Where sessions are stored.", ""},
        {"device",      "Device",               "How this machine identifies itself.", ""},
        {"timing",      "Collection Timing",    "", ""},
        {"sleephq",     "SleepHQ Sync",         "Upload nights to SleepHQ. Credentials come from SleepHQ Account, API.", "sleephq.enabled"},
        {"cpapdash",    "CpapDash Cloud",       "Sync to the CpapDash service.", "cpapdash.enabled"},
        {"o2ring",      "O2 Ring Oximetry",     "Pulse oximetry from a Wellue O2 Ring.", "o2ring.enabled"},
        {"mqtt",        "MQTT",                 "Publish sensors to Home Assistant.", "mqtt.enabled"},
        {"llm",         "AI Summaries",         "Nightly summaries from a language model.", "llm.enabled"},
        {"agent",       "Agent",                "Conversational agent over your therapy history.", "agent.enabled"},
        {"sleep_stage", "Sleep Stage Inference","Estimate sleep stages from flow data.", "sleep_stage.enabled"},
        {"ml_training", "ML Training",          "Retrain the prediction models on your own data.", "ml_training.enabled"},
        {"fysetc",      "Fysetc TCP",           "", ""},
        {"logging",     "Support Log",          "A log file to attach to a support request.", ""},
        {"advanced",    "Advanced",             "", ""},
    };
    return groups;
}

const std::vector<FieldSpec>& settingsFields() {
    static const std::vector<FieldSpec> fields = [] {
        std::vector<FieldSpec> f;

        // ── Data Source ────────────────────────────────────────────────────
        f.push_back({"source", "source", "Source Type", "", FieldKind::Choice, "",
            {{"ezshare", "ezShare WiFi SD card"},
             {"local",   "A folder on this computer"},
             {"fysetc",  "Fysetc TCP"}},
            {}, {}, false, nullptr});

        f.push_back({"ezshare_url", "source", "ezShare URL", "", FieldKind::Text,
            "http://192.168.4.1", {}, {}, {}, false, sourceIs("ezshare")});

        f.push_back({"ezshare_range", "source", "Use range requests", "", FieldKind::Bool,
            "", {}, {}, {}, true, sourceIs("ezshare")});

        f.push_back({"local_dir", "source", "SD Card Folder",
            "The card ROOT, holding both STR.edf and DATALOG. Not the DATALOG folder itself.",
            FieldKind::Directory, "", {}, {}, {}, false, sourceIs("local")});

        f.push_back({"archive_dir", "source", "Archive Folder",
            "Where the card is copied to. OSCAR imports from here and SleepHQ export reads it.",
            FieldKind::Directory, "", {}, {}, {}, false, nullptr});

        // ── Database ───────────────────────────────────────────────────────
        f.push_back({"database.type", "database", "Type", "", FieldKind::Choice, "",
            {{"sqlite", "SQLite (built in, no setup)"},
             {"postgresql", "PostgreSQL"},
             {"mysql", "MySQL"}},
            {}, {}, true, nullptr});

        auto notSqlite = [](const ConfigModel& m) { return m.getString("database.type") != "sqlite"; };

        f.push_back({"database.sqlite_path", "database", "Database File", "", FieldKind::File,
            "Leave empty for the default in your data folder", {}, {}, {}, true,
            [](const ConfigModel& m) { return m.getString("database.type") == "sqlite"; }});

        f.push_back({"database.host", "database", "Host", "", FieldKind::Text, "localhost", {}, {}, {}, true, notSqlite});
        f.push_back({"database.port", "database", "Port", "", FieldKind::Int, "", {}, 1, 65535, true, notSqlite});
        f.push_back({"database.name", "database", "Database Name", "", FieldKind::Text, "cpap", {}, {}, {}, true, notSqlite});
        f.push_back({"database.user", "database", "User", "", FieldKind::Text, "cpap_user", {}, {}, {}, true, notSqlite});
        f.push_back({"database.password", "database", "Password", "", FieldKind::Secret, "", {}, {}, {}, true, notSqlite});

        // ── Device ─────────────────────────────────────────────────────────
        f.push_back({"device_id", "device", "Device ID", "", FieldKind::Text, "23243570851", {}, {}, {}, false, nullptr});
        f.push_back({"device_name", "device", "Device Name", "", FieldKind::Text, "ResMed AirSense 11", {}, {}, {}, false, nullptr});

        // ── Timing ─────────────────────────────────────────────────────────
        f.push_back({"burst_interval", "timing", "Collection Interval (seconds)",
            "How often the card is polled while therapy is running.",
            FieldKind::Int, "", {}, 1, 86400, false, nullptr});

        // ── SleepHQ ────────────────────────────────────────────────────────
        f.push_back({"sleephq.enabled", "sleephq", "Enabled", "", FieldKind::Bool, "", {}, {}, {}, true, nullptr});
        f.push_back({"sleephq.client_id", "sleephq", "Client ID", "", FieldKind::Text,
            "SleepHQ API client ID", {}, {}, {}, true, enabledBy("sleephq.enabled")});
        f.push_back({"sleephq.client_secret", "sleephq", "Client Secret", "", FieldKind::Secret,
            "", {}, {}, {}, true, enabledBy("sleephq.enabled")});
        f.push_back({"sleephq.auto_on_session", "sleephq", "Upload when a session completes", "",
            FieldKind::Bool, "", {}, {}, {}, true, enabledBy("sleephq.enabled")});
        f.push_back({"sleephq.auto_on_backfill", "sleephq", "Upload when importing history", "",
            FieldKind::Bool, "", {}, {}, {}, true, enabledBy("sleephq.enabled")});
        f.push_back({"sleephq.quiet_minutes", "sleephq", "Quiet Period (minutes)",
            "How long the card must be idle before a night is considered finished.",
            FieldKind::Int, "", {}, 1, 1440, true, enabledBy("sleephq.enabled")});

        // ── CpapDash Cloud ─────────────────────────────────────────────────
        f.push_back({"cpapdash.enabled", "cpapdash", "Enabled", "", FieldKind::Bool, "", {}, {}, {}, false, nullptr});
        f.push_back({"cpapdash.api_url", "cpapdash", "API URL", "", FieldKind::Text,
            "https://api.cpapdash.com", {}, {}, {}, false, enabledBy("cpapdash.enabled")});
        f.push_back({"cpapdash.token", "cpapdash", "Token", "", FieldKind::Secret,
            "", {}, {}, {}, false, enabledBy("cpapdash.enabled")});
        f.push_back({"cpapdash.auto_sync", "cpapdash", "Sync after each collection sweep", "",
            FieldKind::Bool, "", {}, {}, {}, false, enabledBy("cpapdash.enabled")});

        // ── O2 Ring ────────────────────────────────────────────────────────
        f.push_back({"o2ring.enabled", "o2ring", "Enabled", "", FieldKind::Bool, "", {}, {}, {}, false, nullptr});
        f.push_back({"o2ring.mode", "o2ring", "Mode", "", FieldKind::Choice, "",
            {{"http", "Over the network, via a Mule"}, {"ble", "Bluetooth, direct"}},
            {}, {}, false, enabledBy("o2ring.enabled")});
        f.push_back({"o2ring.mule_url", "o2ring", "Mule URL",
            "Address of the Mule bridging the ring over Bluetooth.",
            FieldKind::Text, "http://192.168.2.74", {}, {}, {}, false,
            allOf(enabledBy("o2ring.enabled"),
                  [](const ConfigModel& m) { return m.getString("o2ring.mode") == "http"; })});

        // ── MQTT ───────────────────────────────────────────────────────────
        f.push_back({"mqtt.enabled", "mqtt", "Enabled", "", FieldKind::Bool, "", {}, {}, {}, false, nullptr});
        f.push_back({"mqtt.broker", "mqtt", "Broker", "", FieldKind::Text, "127.0.0.1", {}, {}, {}, false, enabledBy("mqtt.enabled")});
        f.push_back({"mqtt.port", "mqtt", "Port", "", FieldKind::Int, "", {}, 1, 65535, false, enabledBy("mqtt.enabled")});
        f.push_back({"mqtt.username", "mqtt", "Username", "", FieldKind::Text, "", {}, {}, {}, false, enabledBy("mqtt.enabled")});
        f.push_back({"mqtt.password", "mqtt", "Password", "", FieldKind::Secret, "", {}, {}, {}, false, enabledBy("mqtt.enabled")});
        f.push_back({"mqtt.client_id", "mqtt", "Client ID", "", FieldKind::Text, "hms_cpap", {}, {}, {}, true, enabledBy("mqtt.enabled")});

        // ── LLM ────────────────────────────────────────────────────────────
        f.push_back({"llm.enabled", "llm", "Enabled", "", FieldKind::Bool, "", {}, {}, {}, false, nullptr});
        f.push_back({"llm.provider", "llm", "Provider", "", FieldKind::Choice, "",
            {{"ollama", "Ollama"},
             {"openai", "OpenAI-compatible"},
             {"gemini", "Gemini"},
             {"anthropic", "Anthropic"}},
            {}, {}, false, enabledBy("llm.enabled")});
        f.push_back({"llm.endpoint", "llm", "Endpoint",
            "The base URL only, with no path. Any server speaking the OpenAI chat API works here.",
            FieldKind::Text, "http://127.0.0.1:11434", {}, {}, {}, false, enabledBy("llm.enabled")});
        f.push_back({"llm.model", "llm", "Model", "", FieldKind::Text, "llama3.1:8b", {}, {}, {}, false, enabledBy("llm.enabled")});
        f.push_back({"llm.api_key", "llm", "API Key",
            "Not needed for Ollama or most local servers.",
            FieldKind::Secret, "", {}, {}, {}, false, enabledBy("llm.enabled")});
        f.push_back({"llm.max_tokens", "llm", "Max Tokens", "", FieldKind::Int, "", {}, 1, 128000, true, enabledBy("llm.enabled")});
        f.push_back({"llm.prompt_file", "llm", "Prompt File",
            "Leave empty to use the built-in prompt.",
            FieldKind::File, "", {}, {}, {}, true, enabledBy("llm.enabled")});

        // ── Agent ──────────────────────────────────────────────────────────
        f.push_back({"agent.enabled", "agent", "Enabled", "", FieldKind::Bool, "", {}, {}, {}, true, nullptr});
        f.push_back({"agent.embed_model", "agent", "Embedding Model", "", FieldKind::Text,
            "nomic-embed-text", {}, {}, {}, true, enabledBy("agent.enabled")});
        f.push_back({"agent.temperature", "agent", "Temperature", "", FieldKind::Double, "", {}, 0.0, 2.0, true, enabledBy("agent.enabled")});
        f.push_back({"agent.max_iterations", "agent", "Max Iterations", "", FieldKind::Int, "", {}, 1, 100, true, enabledBy("agent.enabled")});

        // ── Sleep stage ────────────────────────────────────────────────────
        f.push_back({"sleep_stage.enabled", "sleep_stage", "Enabled", "", FieldKind::Bool, "", {}, {}, {}, true, nullptr});
        f.push_back({"sleep_stage.live_inference", "sleep_stage", "Infer during the night", "",
            FieldKind::Bool, "", {}, {}, {}, true, enabledBy("sleep_stage.enabled")});
        f.push_back({"sleep_stage.model_dir", "sleep_stage", "Model Folder",
            "Leave empty to use the data folder.", FieldKind::Directory, "", {}, {}, {}, true,
            enabledBy("sleep_stage.enabled")});
        f.push_back({"sleep_stage.model_version", "sleep_stage", "Model Version", "", FieldKind::Text,
            "shhs-rf-v1", {}, {}, {}, true, enabledBy("sleep_stage.enabled")});

        // ── ML training ────────────────────────────────────────────────────
        f.push_back({"ml_training.enabled", "ml_training", "Enabled", "", FieldKind::Bool, "", {}, {}, {}, false, nullptr});
        f.push_back({"ml_training.schedule", "ml_training", "Retrain", "", FieldKind::Choice, "",
            {{"daily", "Daily"}, {"weekly", "Weekly"}, {"monthly", "Monthly"}},
            {}, {}, false, enabledBy("ml_training.enabled")});
        f.push_back({"ml_training.min_days", "ml_training", "Minimum Therapy Days", "", FieldKind::Int,
            "", {}, 7, 3650, false, enabledBy("ml_training.enabled")});
        f.push_back({"ml_training.max_training_days", "ml_training", "Training Lookback (days)",
            "0 means use every night available.", FieldKind::Int, "", {}, 0, 3650, false,
            enabledBy("ml_training.enabled")});
        f.push_back({"ml_training.model_dir", "ml_training", "Model Folder", "", FieldKind::Directory,
            "", {}, {}, {}, false, enabledBy("ml_training.enabled")});

        // ── Fysetc ─────────────────────────────────────────────────────────
        f.push_back({"fysetc.enabled", "fysetc", "Enabled", "", FieldKind::Bool, "", {}, {}, {}, true, sourceIs("fysetc")});
        f.push_back({"fysetc.listen_port", "fysetc", "Listen Port", "", FieldKind::Int, "", {}, 1, 65535, true, sourceIs("fysetc")});
        f.push_back({"fysetc.listen_bind", "fysetc", "Listen Address", "", FieldKind::Text, "0.0.0.0", {}, {}, {}, true, sourceIs("fysetc")});
        f.push_back({"fysetc.connection_timeout_s", "fysetc", "Connection Timeout (seconds)", "",
            FieldKind::Int, "", {}, 1, 3600, true, sourceIs("fysetc")});
        f.push_back({"fysetc.archive_dir", "fysetc", "Reconstructed Card Folder", "", FieldKind::Directory,
            "", {}, {}, {}, true, sourceIs("fysetc")});
        f.push_back({"fysetc.log_dir", "fysetc", "Log Folder", "", FieldKind::Directory, "", {}, {}, {}, true, sourceIs("fysetc")});

        // ── Logging ────────────────────────────────────────────────────────
        f.push_back({"logging.enabled", "logging", "Write a log file", "", FieldKind::Bool, "", {}, {}, {}, true, nullptr});
        f.push_back({"logging.file", "logging", "Log File",
            "Leave empty for the default location in your data folder.",
            FieldKind::File, "", {}, {}, {}, true, enabledBy("logging.enabled")});
        f.push_back({"logging.max_mb", "logging", "Rotate After (MB)", "", FieldKind::Int, "", {}, 1, 1024, true, enabledBy("logging.enabled")});
        f.push_back({"logging.keep", "logging", "Old Logs Kept", "", FieldKind::Int, "", {}, 0, 100, true, enabledBy("logging.enabled")});

        // ── Advanced ───────────────────────────────────────────────────────
        f.push_back({"web_port", "advanced", "Web Port",
            "The dashboard moves to this port. A port already in use stops the service from starting.",
            FieldKind::Int, "", {}, 1, 65535, true, nullptr});

        return f;
    }();
    return fields;
}

const std::vector<Rule>& settingsRules() {
    static const std::vector<Rule> rules = {
        // The card root, not DATALOG. This is the one validator that cannot be
        // expressed as a range or a regex, and there is no HTTP endpoint for
        // it -- classifyLocalDir is compiled into the supervisor for exactly
        // this. Checked in the UI layer where the filesystem is reachable; here
        // we only require that something was chosen.
        [](const ConfigModel& m) -> std::optional<Issue> {
            if (m.getString("source") != "local") return std::nullopt;
            if (!trim(m.getString("local_dir")).empty()) return std::nullopt;
            return Issue{"local_dir", "Choose the folder holding your SD card data.",
                         "Use Browse and pick the card ROOT: the folder that contains "
                         "both STR.edf and DATALOG.", Severity::Error};
        },

        // Advisory, NOT blocking -- PreflightService::checkArchiveDir is
        // explicitly non-fatal, and refusing a Save here would lock someone out
        // of the settings dialog over a condition the service itself starts up
        // with.
        [](const ConfigModel& m) -> std::optional<Issue> {
            if (!sourceNeedsArchive(m)) return std::nullopt;
            if (!trim(m.getString("archive_dir")).empty()) return std::nullopt;
            return Issue{"archive_dir",
                         "Nothing is being written to disk.",
                         "Without an archive folder, OSCAR has nothing to import and "
                         "SleepHQ export stays blocked. Nights already collected are "
                         "safe in the database.", Severity::Warning};
        },

        // The one hard gate the web page already enforces.
        [](const ConfigModel& m) -> std::optional<Issue> {
            if (!m.getBool("agent.enabled")) return std::nullopt;
            const bool llm = m.getBool("llm.enabled");
            const bool pg  = m.getString("database.type") == "postgresql";
            if (llm && pg) return std::nullopt;

            std::string why;
            if (!llm && !pg)      why = "AI Summaries is off and the database is not PostgreSQL";
            else if (!llm)        why = "AI Summaries is off";
            else                  why = "the database is not PostgreSQL";
            return Issue{"agent.enabled", "The agent cannot run: " + why + ".",
                         "Turn on AI Summaries and use a PostgreSQL database, or turn "
                         "the agent off.", Severity::Error};
        },

        // A server database with no host or name will not open, and preflight
        // refuses to start the service. Catching it here means the user finds
        // out while they are looking at the field.
        [](const ConfigModel& m) -> std::optional<Issue> {
            if (m.getString("database.type") == "sqlite") return std::nullopt;
            for (const auto& [path, what] : std::vector<std::pair<std::string, std::string>>{
                     {"database.host", "host"},
                     {"database.name", "name"},
                     {"database.user", "user"}}) {
                if (trim(m.getString(path)).empty())
                    return Issue{path, "The database " + what + " is required.",
                                 "", Severity::Error};
            }
            return std::nullopt;
        },

        // Matches SetupService::isSafeIdentifier. Postgres truncates past 63,
        // so a longer name silently becomes a different database.
        [](const ConfigModel& m) -> std::optional<Issue> {
            if (m.getString("database.type") == "sqlite") return std::nullopt;
            static const std::regex ok("^[A-Za-z_][A-Za-z0-9_]{0,62}$");
            const auto name = trim(m.getString("database.name"));
            if (name.empty() || std::regex_match(name, ok)) return std::nullopt;
            return Issue{"database.name", "That database name will not work.",
                         "Use letters, digits and underscore, starting with a letter, "
                         "at most 63 characters.", Severity::Error};
        },

        // An endpoint that is a full path rather than a base URL is the most
        // common way to configure an LLM that never answers.
        [](const ConfigModel& m) -> std::optional<Issue> {
            if (!m.getBool("llm.enabled")) return std::nullopt;
            const auto ep = trim(m.getString("llm.endpoint"));
            if (ep.empty())
                return Issue{"llm.endpoint", "An endpoint is required.", "", Severity::Error};
            if (ep.find("/v1/") != std::string::npos ||
                ep.find("/api/generate") != std::string::npos)
                return Issue{"llm.endpoint", "This looks like a full path, not a base URL.",
                             "Use just the scheme, host and port, for example "
                             "http://127.0.0.1:11434. The provider adds its own path.",
                             Severity::Warning};
            if (ep.rfind("http://", 0) != 0 && ep.rfind("https://", 0) != 0)
                return Issue{"llm.endpoint", "The endpoint must start with http:// or https://.",
                             "", Severity::Error};
            return std::nullopt;
        },
    };
    return rules;
}

std::vector<Issue> validate(const ConfigModel& model) {
    std::vector<Issue> issues;

    for (const auto& spec : settingsFields()) {
        // A field the user cannot see must not block a Save. Turning MQTT off
        // with a bad port left in the box would otherwise wedge the dialog on
        // an error with nothing visible to fix.
        if (spec.visible && !spec.visible(model)) continue;

        if (spec.kind == FieldKind::Int || spec.kind == FieldKind::Double) {
            const auto raw = model.get(spec.path);
            if (raw.is_null()) continue;
            const double v = raw.is_number() ? raw.get<double>()
                                             : static_cast<double>(model.getInt(spec.path));
            if (spec.min && v < *spec.min) {
                issues.push_back({spec.path,
                    spec.label + " must be at least " +
                        (spec.kind == FieldKind::Int ? std::to_string((long long)*spec.min)
                                                     : std::to_string(*spec.min)) + ".",
                    "", Severity::Error});
            } else if (spec.max && v > *spec.max) {
                issues.push_back({spec.path,
                    spec.label + " must be at most " +
                        (spec.kind == FieldKind::Int ? std::to_string((long long)*spec.max)
                                                     : std::to_string(*spec.max)) + ".",
                    "", Severity::Error});
            }
        }
    }

    for (const auto& rule : settingsRules())
        if (auto issue = rule(model)) issues.push_back(*issue);

    return issues;
}

std::vector<Issue> blockingIssues(const ConfigModel& model) {
    std::vector<Issue> out;
    for (auto& i : validate(model))
        if (i.severity == Severity::Error) out.push_back(i);
    return out;
}

std::vector<std::string> restartReasons(const ConfigModel& model) {
    std::vector<std::string> reasons;
    for (const auto& spec : settingsFields()) {
        if (!spec.restart_required) continue;
        if (!model.touched(spec.path)) continue;
        reasons.push_back(spec.label);
    }
    return reasons;
}

}  // namespace cpapdash::supervisor
