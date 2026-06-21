#include "services/SleepHqExportService.h"
#include "services/SleepHqClient.h"
#include "utils/AppConfig.h"
#include "utils/ConfigManager.h"

#include <filesystem>
#include <thread>
#include <iostream>

namespace hms_cpap {

SleepHqExportService& SleepHqExportService::getInstance() {
    static SleepHqExportService instance;
    return instance;
}

static bool sleephqReady(const AppConfig* cfg) {
    return cfg && cfg->sleephq.enabled && !cfg->sleephq.client_id.empty();
}

void SleepHqExportService::exportDateAsync(const std::string& date_folder) {
    if (!sleephqReady(config_) || date_folder.empty()) return;
    std::string d = date_folder;
    std::thread([this, d]() { exportDate(d); }).detach();
}

bool SleepHqExportService::exportDate(const std::string& date_folder) {
    std::string archive_base = ConfigManager::get("CPAP_ARCHIVE_DIR", "");
    if (archive_base.empty()) {
        std::cerr << "[sleephq] CPAP_ARCHIVE_DIR not set; cannot export " << date_folder << std::endl;
        return false;
    }
    return exportFolder(date_folder, archive_base + "/DATALOG/" + date_folder, archive_base);
}

void SleepHqExportService::exportFolderAsync(const std::string& date_folder,
                                             const std::string& datalog_dir,
                                             const std::string& root_dir) {
    if (!sleephqReady(config_) || date_folder.empty()) return;
    std::string d = date_folder, dl = datalog_dir, rd = root_dir;
    std::thread([this, d, dl, rd]() { exportFolder(d, dl, rd); }).detach();
}

bool SleepHqExportService::exportFolder(const std::string& date_folder,
                                        const std::string& datalog_dir,
                                        const std::string& root_dir) {
    namespace fs = std::filesystem;
    if (!sleephqReady(config_)) return true;

    SleepHqClient client(config_->sleephq.client_id, config_->sleephq.client_secret);
    std::string err;
    if (!client.connect(err)) { std::cerr << "[sleephq] connect failed: " << err << std::endl; return false; }

    std::string import_id = client.createImport(err);
    if (import_id.empty()) { std::cerr << "[sleephq] createImport failed: " << err << std::endl; return false; }

    int count = 0;
    std::error_code ec;
    if (fs::exists(datalog_dir, ec)) {
        for (const auto& e : fs::directory_iterator(datalog_dir, ec)) {
            if (!e.is_regular_file()) continue;
            std::string name = e.path().filename().string();
            if (!client.uploadFile(import_id, name, "DATALOG/" + date_folder, e.path().string(), err)) {
                std::cerr << "[sleephq] " << err << std::endl; return false;
            }
            ++count;
        }
    }
    const char* rootFiles[] = {"STR.edf", "Identification.tgt",
                               "Identification.json", "Identification.crc"};
    for (const char* name : rootFiles) {
        std::string p = root_dir + "/" + name;
        if (!fs::exists(p, ec)) continue;
        if (!client.uploadFile(import_id, name, "", p, err)) {
            std::cerr << "[sleephq] " << err << std::endl; return false;
        }
        ++count;
    }

    if (count == 0) { std::cerr << "[sleephq] no files to export for " << date_folder << std::endl; return false; }
    if (!client.processFiles(import_id, err)) { std::cerr << "[sleephq] " << err << std::endl; return false; }
    std::cout << "[sleephq] exported " << count << " files for " << date_folder
              << " (import " << import_id << ")" << std::endl;
    return true;
}

} // namespace hms_cpap
