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

    // Entry point called from detached thread. Virtual for SDD-082: the
    // clinician and compliance reports draw their own documents.
    virtual void generate(int report_id,
                          const std::string& start,
                          const std::string& end,
                          const std::string& filepath,
                          const std::string& report_dir);

    /// SDD-082 B6: the PDF's language ("en", "es", "pt", "hu", "fr"); anything
    /// else is English. The range and daily reports stay English.
    void setLanguage(const std::string& lang) { language_ = lang; }

    /// SDD-082 B3: the disclaimer the clinician and compliance reports end with.
    static constexpr const char* kDisclaimerEn =
        "HMS-CPAP is a consumer tool for education and self-tracking, not a medical device and "
        "not medical advice. Always consult your clinician about your therapy.";

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

    /**
     * SDD-024: is the index on this row an apnea-HYPOPNEA index?
     *
     * A row with no index_kind is treated as one, which is what every row was
     * before the column existed.
     */
    static bool gradableIndex(const Json::Value& row);

    /**
     * What to CALL the index on this row: "AHI", or "Apnea Index" for a machine
     * that scores apneas and never marks a hypopnea.
     *
     * A PDF is the artefact a user takes to a clinician, so a number labelled
     * AHI there will be read against AHI severity bands by someone who cannot
     * ask what produced it. This is the one surface where the label has to be
     * right without a tooltip to fall back on.
     */
    static std::string indexLabel(const Json::Value& row);

    // Shared PDF section builders
    void addSummarySection (PdfRenderer& pdf, const Json::Value& st,
                            const std::string& start, const std::string& end, int nights);
    void addOximetrySection(PdfRenderer& pdf, const IDatabase::OxiRangeSummary& oxi);
    void addInsightsSection(PdfRenderer& pdf, const Json::Value& daily,
                            const std::string& start, const std::string& end);

    // Protected for SDD-082's generators, which run their own generate().
    void updateStatus    (int id, const std::string& status, const std::string& err = "");
    void updateNightsCount(int id, int n);
    /// `extra` is merged into the ready message (SDD-082: kind, verdict).
    void publishMqtt     (int id, const std::string& filename, int nights,
                          const Json::Value& extra = Json::Value());

    /// The machine's name for the cover, as the range report has always named it.
    std::string coverMachineName() const;

    /// SDD-082: the download name of a stored file, without the "<id>_" a
    /// clinician or compliance file is stored under.
    static std::string downloadName(const std::string& filepath);

    /// 0 for English, 1 to 4 for es, pt, hu, fr: the column of a label table.
    std::size_t languageIndex() const;

    std::string language_ = "en";
};

} // namespace hms_cpap
