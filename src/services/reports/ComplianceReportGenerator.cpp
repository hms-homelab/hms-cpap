#include "services/reports/ComplianceReportGenerator.h"

#include "services/GnuplotService.h"
#include "services/PdfRenderer.h"
#include "services/reports/ClinicianReportGenerator.h"
#include "utils/AppConfig.h"
#include "utils/ConfigManager.h"
#include "utils/MachineIdentity.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <unordered_map>

namespace hms_cpap {

namespace fs = std::filesystem;
using namespace clinician;
using namespace compliance;

namespace {

// { es, pt, hu, fr }, accent-free for the core PDF font, keyed on the English
// text. CpapDash's table, with "CpapDash" read as "HMS-CPAP" where the
// sentence names the product (B3). The placeholders {H} (the rule's hours),
// {P} (its share), {N} (a count), {START} and {END} let a language order the
// sentence its own way.
using Loc = std::array<const char*, 4>;
const std::unordered_map<std::string, Loc>& labels() {
    static const std::unordered_map<std::string, Loc> T = {
        {"HMS-CPAP Compliance Report", {"Informe de adherencia de HMS-CPAP", "Relatorio de adesao do HMS-CPAP", "HMS-CPAP terapiahusegi jelentes", "Rapport d'observance HMS-CPAP"}},
        {"Usage against a rule: ", {"Uso frente a una regla: ", "Uso face a uma regra: ", "Hasznalat egy szabalyhoz merve: ", "Utilisation au regard d'une regle : "}},
        {"Patient and device", {"Paciente y equipo", "Paciente e equipamento", "Beteg es keszulek", "Patient et appareil"}},
        {"Reporting period", {"Periodo del informe", "Periodo do relatorio", "Jelentesi idoszak", "Periode du rapport"}},
        {"Generated", {"Generado", "Gerado", "Keszult", "Genere le"}},
        {"Machine", {"Equipo", "Equipamento", "Keszulek", "Appareil"}},
        {"Serial number", {"Numero de serie", "Numero de serie", "Sorozatszam", "Numero de serie"}},
        {"Nights on record", {"Noches registradas", "Noites registadas", "Rogzitett ejszakak", "Nuits enregistrees"}},
        {" to ", {" a ", " a ", " - ", " au "}},
        {"The rule", {"La regla", "A regra", "A szabaly", "La regle"}},
        {"At least {H} hours on at least {P} % of nights, {START} to {END}",
         {"Al menos {H} horas en al menos el {P} % de las noches, {START} a {END}",
          "Pelo menos {H} horas em pelo menos {P} % das noites, {START} a {END}",
          "Legalabb {H} ora az ejszakak legalabb {P} %-aban, {START} - {END}",
          "Au moins {H} heures sur au moins {P} % des nuits, du {START} au {END}"}},
        {"Every calendar night in the range counts; a night with no use is a miss.",
         {"Cuentan todas las noches del calendario del periodo; una noche sin uso es un fallo.",
          "Contam todas as noites de calendario do periodo; uma noite sem uso e uma falha.",
          "Az idoszak minden naptari ejszakaja szamit; a hasznalat nelkuli ejszaka hianyzik.",
          "Chaque nuit du calendrier de la periode compte ; une nuit sans utilisation est un manquement."}},
        {"MEETS THE RULE", {"CUMPLE LA REGLA", "CUMPRE A REGRA", "MEGFELEL A SZABALYNAK", "RESPECTE LA REGLE"}},
        {"DOES NOT MEET THE RULE", {"NO CUMPLE LA REGLA", "NAO CUMPRE A REGRA", "NEM FELEL MEG A SZABALYNAK", "NE RESPECTE PAS LA REGLE"}},
        {"The figures", {"Las cifras", "Os numeros", "A szamok", "Les chiffres"}},
        {"Nights in range", {"Noches del periodo", "Noites do periodo", "Ejszakak az idoszakban", "Nuits de la periode"}},
        {"Nights with any use", {"Noches con algun uso", "Noites com algum uso", "Ejszakak barmilyen hasznalattal", "Nuits avec utilisation"}},
        {"Nights at or over {H} hours", {"Noches con {H} horas o mas", "Noites com {H} horas ou mais", "Legalabb {H} oras ejszakak", "Nuits de {H} heures ou plus"}},
        {"Share of nights", {"Proporcion de noches", "Proporcao de noites", "Ejszakak aranya", "Part des nuits"}},
        {"Nights needed for {P} %", {"Noches necesarias para el {P} %", "Noites necessarias para {P} %", "A {P} %-hoz szukseges ejszakak", "Nuits necessaires pour {P} %"}},
        {"Margin", {"Margen", "Margem", "Tartalek", "Marge"}},
        {"{N} night(s) to spare", {"{N} noche(s) de margen", "{N} noite(s) de margem", "{N} ejszaka tartalek", "{N} nuit(s) de marge"}},
        {"{N} more night(s) at {H} hours would have met it",
         {"{N} noche(s) mas con {H} horas lo habrian cumplido",
          "Mais {N} noite(s) com {H} horas teriam cumprido",
          "Meg {N} legalabb {H} oras ejszaka kellett volna",
          "{N} nuit(s) de plus a {H} heures l'auraient respectee"}},
        {"Calendar", {"Calendario", "Calendario", "Naptar", "Calendrier"}},
        {"No use", {"Sin uso", "Sem uso", "Nincs hasznalat", "Pas d'utilisation"}},
        {"Under {H} hours", {"Menos de {H} horas", "Menos de {H} horas", "{H} ora alatt", "Moins de {H} heures"}},
        {"At or over {H} hours", {"{H} horas o mas", "{H} horas ou mais", "Legalabb {H} ora", "{H} heures ou plus"}},
        {"Rolling 30-night share", {"Proporcion movil de 30 noches", "Proporcao movel de 30 noites", "Gordulo 30 ejszakas arany", "Part glissante sur 30 nuits"}},
        {"Share of the 30 nights ending on each date with at least {H} hours, against the {P} % line.",
         {"Proporcion de las 30 noches que terminan en cada fecha con al menos {H} horas, frente a la linea del {P} %.",
          "Proporcao das 30 noites que terminam em cada data com pelo menos {H} horas, face a linha dos {P} %.",
          "A minden datummal vegzodo 30 ejszaka legalabb {H} oras aranya, a {P} % vonalhoz merve.",
          "Part des 30 nuits se terminant a chaque date avec au moins {H} heures, face a la ligne des {P} %."}},
        {"% of nights", {"% de noches", "% de noites", "ejszakak %-a", "% des nuits"}},
        {"Nights", {"Noches", "Noites", "Ejszakak", "Nuits"}},
        {"Date", {"Fecha", "Data", "Datum", "Date"}},
        {"Hours", {"Horas", "Horas", "Ora", "Heures"}},
        {"Met", {"Cumple", "Cumpre", "Megfelel", "Respectee"}},
        {"yes", {"si", "sim", "igen", "oui"}},
        {"no", {"no", "nao", "nem", "non"}},
        {"AHI", {"IAH", "IAH", "AHI", "IAH"}},
        // B7: the same column when the machine scores apneas only (SDD-024).
        {"Apnea index", {"Indice de apnea", "Indice de apneia", "Apnoe index", "Indice d'apnee"}},
        {"Leak median", {"Mediana de fuga", "Mediana de fuga", "Median szivargas", "Fuite mediane"}},
        {"A record of usage from the machine's card as HMS-CPAP holds it, not a compliance certification by any payer or programme.",
         {"Un registro de uso a partir de la tarjeta del equipo tal como lo tiene HMS-CPAP, no una certificacion de cumplimiento de ningun pagador ni programa.",
          "Um registo de uso a partir do cartao do equipamento tal como o HMS-CPAP o tem, nao uma certificacao de adesao de qualquer pagador ou programa.",
          "A keszulek kartyajarol szarmazo hasznalati nyilvantartas, ahogy a HMS-CPAP tarolja; nem megfelelosegi igazolas semmilyen finanszirozo vagy program reszerol.",
          "Un releve d'utilisation issu de la carte de l'appareil tel que HMS-CPAP le conserve, pas une certification d'observance par un payeur ou un programme."}},
        {"Mon", {"Lun", "Seg", "H", "Lun"}},
        {"Tue", {"Mar", "Ter", "K", "Mar"}},
        {"Wed", {"Mie", "Qua", "Sze", "Mer"}},
        {"Thu", {"Jue", "Qui", "Cs", "Jeu"}},
        {"Fri", {"Vie", "Sex", "P", "Ven"}},
        {"Sat", {"Sab", "Sab", "Szo", "Sam"}},
        {"Sun", {"Dom", "Dom", "V", "Dim"}},
    };
    return T;
}

std::string replaceAll(std::string s, const std::string& what, const std::string& with) {
    for (auto at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size()))
        s.replace(at, what.size(), with);
    return s;
}

std::string pct1(double v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f %%", v);
    return buf;
}

std::string cellOf(const std::optional<double>& v) {
    if (!v) return "-";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", *v);
    return buf;
}

}  // namespace

ComplianceReportGenerator::ComplianceReportGenerator(std::shared_ptr<IDatabase> db,
                                                     std::shared_ptr<QueryService> qs,
                                                     std::shared_ptr<hms::MqttClient> mqtt,
                                                     const std::string& device_id,
                                                     const std::string& logo_path,
                                                     compliance::Rule rule)
    : BaseReportGenerator(std::move(db), std::move(qs), std::move(mqtt), device_id, logo_path),
      rule_(rule) {}

// ---- Labels ----------------------------------------------------------------------

std::string ComplianceReportGenerator::ct(const std::string& en) const {
    const std::size_t li = languageIndex();
    if (li == 0) return en;
    const auto& T = labels();
    const auto it = T.find(en);
    if (it == T.end()) return en;
    return it->second[li - 1];
}

const std::vector<std::string>& ComplianceReportGenerator::labelsForTest() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (const auto& [en, loc] : labels()) k.push_back(en);
        return k;
    }();
    return keys;
}

std::string ComplianceReportGenerator::hoursOf(double minutes) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", minutes > 0 ? minutes / 60.0 : 0.0);
    return buf;
}

std::string ComplianceReportGenerator::fill(const std::string& label, const std::string& start,
                                            const std::string& end, int n) const {
    std::string s = ct(label);
    s = replaceAll(s, "{H}", hoursOf(rule_.hours * 60.0));
    s = replaceAll(s, "{P}", std::to_string(rule_.share));
    s = replaceAll(s, "{N}", std::to_string(n));
    s = replaceAll(s, "{START}", start);
    s = replaceAll(s, "{END}", end);
    return s;
}

std::string ComplianceReportGenerator::ruleSentence(const std::string& start,
                                                    const std::string& end) const {
    return fill("At least {H} hours on at least {P} % of nights, {START} to {END}", start, end);
}

std::vector<std::string> ComplianceReportGenerator::weekdays() const {
    return {ct("Mon"), ct("Tue"), ct("Wed"), ct("Thu"), ct("Fri"), ct("Sat"), ct("Sun")};
}

std::string ComplianceReportGenerator::title() const { return ct("HMS-CPAP Compliance Report"); }
std::string ComplianceReportGenerator::subtitle(const std::string& p) const {
    return ct("Usage against a rule: ") + p;
}
std::string ComplianceReportGenerator::sectionSummaryHeading() const { return ct("The figures"); }

// ---- The calendar ----------------------------------------------------------------

void ComplianceReportGenerator::addCalendarPages(PdfRenderer& pdf, const Calendar& calendar) const {
    if (calendar.empty()) return;
    const auto stateOf = [&](double minutes) {
        if (minutes <= 0) return 1;
        return minutes + 1e-6 >= rule_.hours * 60.0 ? 3 : 2;
    };
    // Up to about two months is one grid; longer wraps into calendar months,
    // each starting on its own weekday, so a quarter reads like a wall calendar.
    constexpr std::size_t kOneGrid = 62;
    if (calendar.size() <= kOneGrid) {
        std::vector<CalendarCell> cells;
        for (const auto& [date, minutes] : calendar)
            cells.push_back({date.substr(5), hoursOf(minutes), stateOf(minutes)});
        pdf.addCalendar(calendar.front().first + ct(" to ") + calendar.back().first, weekdays(),
                        weekdayOf(*dayNumber(calendar.front().first)), cells);
        return;
    }
    std::map<std::string, std::vector<std::pair<std::string, double>>> months;
    for (const auto& night : calendar) months[night.first.substr(0, 7)].push_back(night);
    for (const auto& [month, nights] : months) {
        std::vector<CalendarCell> cells;
        for (const auto& [date, minutes] : nights)
            cells.push_back({date.substr(8), hoursOf(minutes), stateOf(minutes)});
        pdf.addCalendar(month, weekdays(), weekdayOf(*dayNumber(nights.front().first)), cells);
    }
}

// ---- The rolling line ------------------------------------------------------------

void ComplianceReportGenerator::addRollingChart(PdfRenderer& pdf, const std::vector<Night>& nights,
                                                const std::string& start, const std::string& end,
                                                const std::string& tmpDir) const {
    const auto series = rollingShare(nights, start, end, rule_.hours);
    if (series.empty()) return;
    std::vector<ChartPoint> pts;
    for (const auto& [date, share] : series) pts.push_back({date.substr(5), share});
    const std::string png = tmpDir + "/rolling.png";
    pdf.addSectionHeading(ct("Rolling 30-night share"));
    if (GnuplotService::renderLineChart(pts, png, ct("Rolling 30-night share"), ct("% of nights"),
                                        "#2563eb", 0.0, 100.0, rule_.share,
                                        std::to_string(rule_.share) + " %") &&
        fs::exists(png))
        pdf.addChart(png, "");
    pdf.addNote(fill("Share of the 30 nights ending on each date with at least {H} hours, "
                     "against the {P} % line."));
    pdf.addSpacer(8);
}

// ---- The nights table -----------------------------------------------------------

void ComplianceReportGenerator::addNightsTable(PdfRenderer& pdf, const Calendar& calendar,
                                               const std::vector<Night>& nights) const {
    std::map<std::string, const Night*> byDate;
    for (const auto& n : nights) byDate[n.date] = &n;
    // The night's AHI and leak median only when the rows carry them at all.
    bool anyAhi = false, anyLeak = false;
    for (const auto& [date, minutes] : calendar) {
        const auto it = byDate.find(date);
        if (it == byDate.end()) continue;
        anyAhi |= it->second->ahi.has_value();
        anyLeak |= it->second->leak_50.has_value();
    }
    std::vector<std::string> headers = {ct("Date"), ct("Hours"), ct("Met")};
    std::vector<float> widths = {60, 40, 40};
    if (anyAhi) { headers.push_back(ct(ungraded_ ? "Apnea index" : "AHI")); widths.push_back(40); }
    if (anyLeak) { headers.push_back(ct("Leak median")); widths.push_back(60); }
    std::vector<PdfRow> rows;
    for (const auto& [date, minutes] : calendar) {
        const bool met = minutes > 0 && minutes + 1e-6 >= rule_.hours * 60.0;
        PdfRow r;
        r.cells = {date, hoursOf(minutes), met ? ct("yes") : ct("no")};
        const auto it = byDate.find(date);
        const Night* n = it == byDate.end() ? nullptr : it->second;
        if (anyAhi) r.cells.push_back(n ? cellOf(n->ahi) : "-");
        if (anyLeak) r.cells.push_back(n ? cellOf(n->leak_50) : "-");
        rows.push_back(r);
    }
    pdf.addSectionHeading(ct("Nights"));
    pdf.addGridTable(headers, rows, widths, 7.5f);
}

// ---- The document ---------------------------------------------------------------

void ComplianceReportGenerator::generate(int report_id, const std::string& start,
                                         const std::string& end, const std::string& filepath,
                                         const std::string& /*report_dir*/) {
    updateStatus(report_id, "generating");
    const auto fail = [&](const std::string& why) { updateStatus(report_id, "error", why); };
    if (!dayNumber(start) || !dayNumber(end) || start > end) return fail("Invalid date range");
    if (!validRule(rule_.hours, rule_.share)) return fail("Invalid compliance rule");

    // Who the machine is (the clinician report's cover).
    const auto identity = installIdentity(
        ConfigManager::get("CPAP_SOURCE", "") == "local" ? ConfigManager::get("CPAP_LOCAL_DIR", "") : "",
        ConfigManager::get("CPAP_ARCHIVE_DIR", ""), AppConfig::dataDir());
    const std::string cover = coverMachineName();
    std::string machine = identity.manufacturer;
    if (!identity.model.empty()) machine += (machine.empty() ? "" : " ") + identity.model;
    if (machine.empty()) machine = cover;
    const auto onRecord = qs_->getNightsOnRecord();
    const std::string first_night = onRecord.get("first_night", "").asString();
    const std::string last_on_record = onRecord.get("last_night", "").asString();

    // The nights: one row per used night; the calendar supplies the rest.
    const auto nights = nightsFrom(qs_->getDailySummary(start, end));
    const Judgement j = judge(nights, start, end, rule_);
    updateNightsCount(report_id, j.nights_used);
    if (j.nights_used == 0) return fail("No therapy nights in range");
    int graded = 0;
    for (const auto& n : nights)
        if (n.minutes > 0 && !n.ungraded) ++graded;
    ungraded_ = graded == 0;
    const Calendar calendar = calendarMinutes(nights, start, end);

    const std::string tmpDir = "/tmp/cpap_report_" + std::to_string(report_id);
    fs::create_directories(tmpDir);

    const std::string period = start == end ? start : start + ct(" to ") + end;
    PdfRenderer pdf;
    pdf.addCoverPage(title(), subtitle(period), cover, period, nowStr(), logo_path_);

    // Patient and device.
    pdf.addSectionHeading(ct("Patient and device"));
    std::vector<std::pair<std::string, std::string>> kv = {
        {ct("Reporting period"), period},
        {ct("Generated"), nowStr()},
        {ct("Machine"), machine},
    };
    if (!identity.serial.empty()) kv.push_back({ct("Serial number"), identity.serial});
    if (!first_night.empty())
        kv.push_back({ct("Nights on record"), first_night + ct(" to ") + last_on_record});
    pdf.addKeyValueTable(kv);
    pdf.addSpacer(10);

    // The rule, as it was asked, then the verdict.
    pdf.addSectionHeading(ct("The rule"));
    pdf.addParagraph(ruleSentence(start, end));
    pdf.addNote(ct("Every calendar night in the range counts; a night with no use is a miss."));
    pdf.addSpacer(6);
    pdf.addVerdict(j.meets ? ct("MEETS THE RULE") : ct("DOES NOT MEET THE RULE"), j.meets);

    // The figures.
    pdf.addSectionHeading(ct("The figures"));
    const std::string margin = j.margin >= 0
        ? fill("{N} night(s) to spare", "", "", j.margin)
        : fill("{N} more night(s) at {H} hours would have met it", "", "", -j.margin);
    pdf.addKeyValueTable({
        {ct("Nights in range"), std::to_string(j.nights)},
        {ct("Nights with any use"), std::to_string(j.nights_used)},
        {fill("Nights at or over {H} hours"), std::to_string(j.nights_met)},
        {ct("Share of nights"), pct1(j.pct)},
        {fill("Nights needed for {P} %"), std::to_string(j.nights_needed)},
        {ct("Margin"), margin},
    });
    pdf.addSpacer(10);

    // The calendar, on its own page.
    pdf.addPageBreak();
    pdf.addSectionHeading(ct("Calendar"));
    pdf.addNote(ct("No use") + " | " + fill("Under {H} hours") + " | " + fill("At or over {H} hours"));
    pdf.addSpacer(4);
    addCalendarPages(pdf, calendar);
    pdf.addSpacer(6);

    // The rolling line, only for a range of 30 nights or more.
    addRollingChart(pdf, nights, start, end, tmpDir);

    // The nights behind the cells.
    addNightsTable(pdf, calendar, nights);

    // The footer: the generation time and the not-a-certification sentence.
    pdf.addNote(ct("Generated") + ": " + nowStr());
    pdf.addNote(ct("A record of usage from the machine's card as HMS-CPAP holds it, not a "
                   "compliance certification by any payer or programme."));
    pdf.addDisclaimer(ClinicianReportGenerator::disclaimerIn(languageIndex()));

    if (!pdf.save(filepath)) {
        fs::remove_all(tmpDir);
        return fail("PDF save failed");
    }
    fs::remove_all(tmpDir);
    updateStatus(report_id, "ready");
    Json::Value extra(Json::objectValue);
    extra["kind"] = "compliance";
    extra["verdict"] = verdictOf(j, rule_);
    publishMqtt(report_id, downloadName(filepath), j.nights_used, extra);
    std::cout << "[ComplianceReport] report " << report_id << " ready: " << filepath << "\n";
}

}  // namespace hms_cpap
