#pragma once
#include "database/IDatabase.h"
#include "web/QueryService.h"
#include "services/PdfRenderer.h"
#include "services/GnuplotService.h"
#include "mqtt_client.h"
#include <json/json.h>
#include <string>
#include <memory>

namespace hms_cpap {

struct ChartPaths {
    std::string slot_a;      // AHI (range) or Heart Rate (daily)
    std::string slot_b;      // Usage (range only)
    std::string leak;
    std::string pressure;
    std::string spo2;
    std::string resp_rate;   // daily only
    std::string tidal_vol;   // daily only
    std::string minute_vent; // daily only
    std::string snore;       // daily only
};

class BaseReportGenerator {
public:
    BaseReportGenerator(std::shared_ptr<IDatabase>       db,
                        std::shared_ptr<QueryService>    qs,
                        std::shared_ptr<hms::MqttClient> mqtt,
                        const std::string& device_id,
                        const std::string& logo_path);

    virtual ~BaseReportGenerator() = default;

    // Entry point called from detached thread.
    void generate(int report_id,
                  const std::string& start,
                  const std::string& end,
                  const std::string& filepath,
                  const std::string& report_dir);

protected:
    // Subclasses override these three
    virtual void   buildCharts(const std::string& tmpDir,
                               const std::string& start,
                               const std::string& end,
                               const Json::Value& daily,
                               ChartPaths& out) = 0;

    virtual void   addChartSection(PdfRenderer& pdf, const ChartPaths& charts) = 0;
    virtual void   addDataSection (PdfRenderer& pdf,
                                   const std::string& start,
                                   const std::string& end,
                                   const Json::Value& daily) = 0;

    virtual std::string title()                       const = 0;
    virtual std::string subtitle(const std::string& period) const = 0;
    virtual std::string sectionSummaryHeading()       const = 0;

    // Shared data members
    std::shared_ptr<IDatabase>       db_;
    std::shared_ptr<QueryService>    qs_;
    std::shared_ptr<hms::MqttClient> mqtt_;
    std::string device_id_;
    std::string logo_path_;

    // Shared helpers
    static double      jd(const Json::Value& o, const char* k);
    static std::string js(const Json::Value& o, const char* k);
    static std::string fmt1(double v);
    static std::string fmt0(double v);
    static std::string fmtHM(double hours);
    static std::string nowStr();
    static std::string toOxiDate(const std::string& yyyymmdd);

    // Shared PDF section builders
    void addSummarySection (PdfRenderer& pdf, const Json::Value& st,
                            const std::string& start, const std::string& end, int nights);
    void addOximetrySection(PdfRenderer& pdf, const IDatabase::OxiRangeSummary& oxi);
    void addInsightsSection(PdfRenderer& pdf, const Json::Value& daily,
                            const std::string& start, const std::string& end);

private:
    void updateStatus    (int id, const std::string& status, const std::string& err = "");
    void updateNightsCount(int id, int n);
    void publishMqtt     (int id, const std::string& filename, int nights);
};

} // namespace hms_cpap
