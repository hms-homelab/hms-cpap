#include "model/LocalModelService.h"

#include "model/NarrationService.h"
#include "utils/AppConfig.h"

#include <cctype>
#include <ctime>
#include <filesystem>
#include <iostream>

namespace hms_cpap::model {

namespace {

// DATALOG folder YYYYMMDD -> sleep day YYYY-MM-DD; "" when it is not one.
std::string sleepDayOf(const std::string& folder) {
    if (folder.size() != 8) return "";
    for (char c : folder)
        if (!std::isdigit(static_cast<unsigned char>(c))) return "";
    return folder.substr(0, 4) + "-" + folder.substr(4, 2) + "-" + folder.substr(6, 2);
}

std::string nowLocal() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

}  // namespace

LocalModelService::LocalModelService(Deps deps) : deps_(std::move(deps)) {}

LocalModelService::~LocalModelService() { stop(); }

void LocalModelService::start() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (worker_.joinable()) return;
        stopping_ = false;
    }
    worker_ = std::thread([this] { run(); });
    enqueue(Event::Startup, "");
}

void LocalModelService::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void LocalModelService::run() {
    for (;;) {
        Queued next;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            next = std::move(queue_.front());
            queue_.pop_front();
        }
        handle(next.event, next.arg, next.str);
    }
}

void LocalModelService::enqueue(Event e, const std::string& arg,
                                std::optional<STRDailyRecord> str) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back({e, arg, std::move(str)});
    }
    cv_.notify_one();
}

bool LocalModelService::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return deps_.config && deps_.config->local_model.enabled;
}

void LocalModelService::setEnabled(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!deps_.config) return;
    deps_.config->local_model.enabled = on;
    if (!deps_.config_path.empty()) deps_.config->save(deps_.config_path);
}

std::string LocalModelService::dir() const {
    std::lock_guard<std::mutex> lk(mu_);
    return modelDir(deps_.config ? deps_.config->local_model.model_dir : std::string());
}

std::unique_ptr<ILocalModel> LocalModelService::makeModel() const {
    const auto path = modelPath(dir(), deps_.pin);
    if (deps_.make_model) return deps_.make_model(path);
    return std::make_unique<GgufModel>(path, deps_.pin.sha256);
}

void LocalModelService::noteInstalled() {
    const auto why = makeModel()->whyNotReady();
    std::lock_guard<std::mutex> lk(mu_);
    installed_ = true;
    runtime_ready_ = why.empty();
    runtime_why_ = why;
    state_.clear();
    message_.clear();
}

// -- The events ---------------------------------------------------------------

void LocalModelService::onNightClosed(const std::string& date_folder,
                                      std::optional<STRDailyRecord> str) {
    if (enabled()) enqueue(Event::NightClosed, date_folder, std::move(str));
}

std::string LocalModelService::requestEnable() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == "downloading" || state_ == "installing") return "";
    }
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        have = installed_;
    }
    if (have) {
        setEnabled(true);
        return "";
    }
    if (auto why = verifyRefusal(deps_.pin); !why.empty()) return why;
    if (deps_.pin.url.empty())
        return "This build has no download address for the model: load the model from a file.";
    if (auto why = downloadRefusal(deps_.pin); !why.empty()) return why;
    {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = "downloading";
        message_.clear();
    }
    enqueue(Event::Download, "");
    return "";
}

void LocalModelService::disable() {
    setEnabled(false);
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ == "error") {
        state_.clear();
        message_.clear();
    }
}

std::string LocalModelService::installFile(const std::string& path) {
    std::error_code ec;
    if (auto why = verifyRefusal(deps_.pin); !why.empty()) {
        std::filesystem::remove(path, ec);
        return why;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == "downloading" || state_ == "installing") {
            std::filesystem::remove(path, ec);
            return "The model is already being put in place.";
        }
        state_ = "installing";
        message_.clear();
    }
    enqueue(Event::InstallFile, path);
    return "";
}

void LocalModelService::handle(Event e, const std::string& arg,
                               const std::optional<STRDailyRecord>& str) {
    std::lock_guard<std::mutex> work(work_mu_);
    struct Busy {
        LocalModelService* s;
        explicit Busy(LocalModelService* x) : s(x) { std::lock_guard<std::mutex> lk(s->mu_); s->busy_ = true; }
        ~Busy() { std::lock_guard<std::mutex> lk(s->mu_); s->busy_ = false; }
    } busy(this);

    switch (e) {
        case Event::Startup: {
            if (installedAndVerified(dir(), deps_.pin)) {
                noteInstalled();
            } else if (enabled()) {
                std::cerr << "Local model: on, but no verified model file is in place" << std::endl;
            }
            return;
        }
        case Event::Download: {
            const auto r = downloadModel(dir(), deps_.pin, deps_.download);
            if (r.ok) {
                noteInstalled();
                setEnabled(true);
                std::cout << "Local model: on (" << r.path << ")" << std::endl;
            } else {
                std::lock_guard<std::mutex> lk(mu_);
                state_ = "error";
                message_ = r.error;
                std::cerr << "Local model: download failed: " << r.error << std::endl;
            }
            tellState();
            return;
        }
        case Event::InstallFile: {
            const auto r = installFromFile(arg, dir(), deps_.pin);
            std::error_code ec;
            std::filesystem::remove(arg, ec);
            if (r.ok) {
                noteInstalled();
                setEnabled(true);
                std::cout << "Local model: loaded from a file and on (" << r.path << ")" << std::endl;
            } else {
                std::lock_guard<std::mutex> lk(mu_);
                state_ = "error";
                message_ = r.error;
                std::cerr << "Local model: the file was not loaded: " << r.error << std::endl;
            }
            tellState();
            return;
        }
        case Event::NightClosed: {
            if (!enabled() || !deps_.db) return;
            const auto day = sleepDayOf(arg);
            if (day.empty()) return;
            auto model = makeModel();
            const auto r = narrateAndStore(*deps_.db, deps_.device_id, day, *model,
                                           str ? &*str : nullptr);
            if (r.written)
                std::cout << "Local model: night " << day << " summarized (" << r.how << ")" << std::endl;
            else
                std::cout << "Local model: night " << day << " not summarized: " << r.why << std::endl;
            std::lock_guard<std::mutex> lk(mu_);
            last_night_ = day;
            last_how_ = r.written ? r.how : "";
            last_at_ = nowLocal();
            last_why_ = r.why;
            return;
        }
    }
}

void LocalModelService::tellState() {
    if (!state_listener_) return;
    // busy_ is still set (the Busy guard ends with handle()); the page is
    // told the outcome, so it is said as not busy.
    Json::Value v = status();
    v["busy"] = false;
    state_listener_(v);
}

Json::Value LocalModelService::status() const {
    const bool published = verifyRefusal(deps_.pin).empty();
    const bool on = enabled();
    Json::Value v(Json::objectValue);
    std::lock_guard<std::mutex> lk(mu_);
    v["enabled"] = on;
    v["published"] = published;
    v["version"] = deps_.pin.version;
    v["installed"] = installed_;
    v["runtime"] = runtime_ready_;
    v["busy"] = busy_;

    std::string state = state_, message = message_;
    if (state.empty()) {
        if (!published) {
            state = "not_published";
            message = verifyRefusal(deps_.pin);
        } else if (on && installed_) {
            state = runtime_ready_ ? "ready" : "no_runtime";
            if (!runtime_ready_) message = runtime_why_;
        } else if (on) {
            state = "error";
            message = "The model file is missing: turn the switch off and on to download it again.";
        } else {
            state = "off";
        }
    }
    v["state"] = state;
    v["message"] = message;

    Json::Value last(Json::objectValue);
    if (!last_night_.empty()) {
        last["night"] = last_night_;
        last["how"] = last_how_;
        last["at"] = last_at_;
        last["why"] = last_why_;
    }
    v["last"] = last;
    return v;
}

}  // namespace hms_cpap::model
