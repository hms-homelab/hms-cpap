#include "services/reports/ClinicianReportGenerator.h"

#include "services/GnuplotService.h"
#include "services/PdfRenderer.h"
#include "utils/AppConfig.h"
#include "utils/ConfigManager.h"
#include "utils/MachineIdentity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <unordered_map>

namespace hms_cpap {

namespace fs = std::filesystem;
using namespace clinician;

namespace {

// { es, pt, hu, fr }, accent-free on purpose: the PDF is drawn with a core PDF
// font that has no reliable glyph coverage beyond Latin-1, and Hungarian needs
// letters outside it. Keyed on the English text. CpapDash's table, with
// "CpapDash" read as "HMS-CPAP" where the sentence names the product (B3).
using Loc = std::array<const char*, 4>;
const std::unordered_map<std::string, Loc>& labels() {
    static const std::unordered_map<std::string, Loc> T = {
        {"HMS-CPAP Clinician Report", {"Informe clinico de HMS-CPAP", "Relatorio clinico do HMS-CPAP", "HMS-CPAP klinikai jelentes", "Rapport clinique HMS-CPAP"}},
        {"Therapy statistics: ", {"Estadisticas de la terapia: ", "Estatisticas da terapia: ", "Terapias statisztika: ", "Statistiques du traitement : "}},
        {"Patient and device", {"Paciente y equipo", "Paciente e equipamento", "Beteg es keszulek", "Patient et appareil"}},
        {"Reporting period", {"Periodo del informe", "Periodo do relatorio", "Jelentesi idoszak", "Periode du rapport"}},
        {"Generated", {"Generado", "Gerado", "Keszult", "Genere le"}},
        {"Machine", {"Equipo", "Equipamento", "Keszulek", "Appareil"}},
        {"Serial number", {"Numero de serie", "Numero de serie", "Sorozatszam", "Numero de serie"}},
        {"Nights on record", {"Noches registradas", "Noites registadas", "Rogzitett ejszakak", "Nuits enregistrees"}},
        {"Times shown in", {"Horas mostradas en", "Horas mostradas em", "Idopontok idozonaja", "Heures affichees en"}},
        {"machine clock", {"reloj del equipo", "relogio do equipamento", "a keszulek oraja", "horloge de l'appareil"}},
        {" to ", {" a ", " a ", " - ", " au "}},
        {"CPAP statistics", {"Estadisticas de CPAP", "Estatisticas de CPAP", "CPAP statisztika", "Statistiques CPAP"}},
        {"All windows end on ", {"Todos los periodos terminan el ", "Todos os periodos terminam em ", "Minden idoszak vege: ", "Toutes les periodes se terminent le "}},
        {"since ", {"desde ", "desde ", "ota: ", "depuis "}},
        {"Last night", {"Ultima noche", "Ultima noite", "Utolso ejszaka", "Derniere nuit"}},
        {"Last 7 days", {"Ultimos 7 dias", "Ultimos 7 dias", "Utolso 7 nap", "7 derniers jours"}},
        {"Last 30 days", {"Ultimos 30 dias", "Ultimos 30 dias", "Utolso 30 nap", "30 derniers jours"}},
        {"Last 6 months", {"Ultimos 6 meses", "Ultimos 6 meses", "Utolso 6 honap", "6 derniers mois"}},
        {"Last year", {"Ultimo ano", "Ultimo ano", "Utolso ev", "Derniere annee"}},
        {"Your range", {"Su periodo", "O seu periodo", "Valasztott idoszak", "Votre periode"}},
        {"CPAP usage", {"Uso de CPAP", "Uso de CPAP", "CPAP hasznalat", "Utilisation CPAP"}},
        {"Therapy efficacy", {"Eficacia de la terapia", "Eficacia da terapia", "Terapia hatekonysaga", "Efficacite du traitement"}},
        {"Leak", {"Fuga", "Fuga", "Szivargas", "Fuite"}},
        {"Pressure", {"Presion", "Pressao", "Nyomas", "Pression"}},
        {"Total days", {"Dias totales", "Dias totais", "Osszes nap", "Jours au total"}},
        {"Used days", {"Dias de uso", "Dias de uso", "Hasznalati napok", "Jours d'utilisation"}},
        {"Days not used", {"Dias sin uso", "Dias sem uso", "Hasznalat nelkuli napok", "Jours sans utilisation"}},
        {"Used days >= 4 h", {"Dias de uso >= 4 h", "Dias de uso >= 4 h", "Hasznalati napok >= 4 ora", "Jours d'utilisation >= 4 h"}},
        {"Used days < 4 h", {"Dias de uso < 4 h", "Dias de uso < 4 h", "Hasznalati napok < 4 ora", "Jours d'utilisation < 4 h"}},
        {"% of all days >= 4 h", {"% de todos los dias >= 4 h", "% de todos os dias >= 4 h", "Osszes nap %-a >= 4 ora", "% de tous les jours >= 4 h"}},
        {"% of used days >= 4 h", {"% de los dias de uso >= 4 h", "% dos dias de uso >= 4 h", "Hasznalati napok %-a >= 4 ora", "% des jours d'utilisation >= 4 h"}},
        {"Average hours (used days)", {"Horas medias (dias de uso)", "Horas medias (dias de uso)", "Atlagos ora (hasznalati napok)", "Heures moyennes (jours d'utilisation)"}},
        {"Median hours (used days)", {"Mediana de horas (dias de uso)", "Mediana de horas (dias de uso)", "Median ora (hasznalati napok)", "Heures medianes (jours d'utilisation)"}},
        {"AHI", {"IAH", "IAH", "AHI", "IAH"}},
        {"AHI median", {"Mediana del IAH", "Mediana do IAH", "AHI medianja", "IAH median"}},
        // B7: the same rows when the machine scores apneas only (SDD-024).
        {"Apnea index", {"Indice de apnea", "Indice de apneia", "Apnoe index", "Indice d'apnee"}},
        {"Apnea index median", {"Mediana del indice de apnea", "Mediana do indice de apneia", "Apnoe index medianja", "Indice d'apnee median"}},
        {"Obstructive apnea index (OA)", {"Indice de apnea obstructiva (OA)", "Indice de apneia obstrutiva (OA)", "Obstruktiv apnoe index (OA)", "Indice d'apnee obstructive (OA)"}},
        {"Hypopnea index (H)", {"Indice de hipopnea (H)", "Indice de hipopneia (H)", "Hipopnoe index (H)", "Indice d'hypopnee (H)"}},
        {"Unclassified apnea index (UA)", {"Indice de apnea no clasificada (UA)", "Indice de apneia nao classificada (UA)", "Nem osztalyozott apnoe index (UA)", "Indice d'apnee non classee (UA)"}},
        {"Clear airway index (CA)", {"Indice de via aerea abierta (CA)", "Indice de via aerea aberta (CA)", "Nyitott legut index (CA)", "Indice voies aeriennes libres (CA)"}},
        {"RERA index", {"Indice RERA", "Indice RERA", "RERA index", "Indice RERA"}},
        {"Flow limitation (95th)", {"Limitacion de flujo (P95)", "Limitacao de fluxo (P95)", "Aramlaskorlatozas (P95)", "Limitation de debit (P95)"}},
        {"% of time in Cheyne-Stokes (CSR)", {"% del tiempo en Cheyne-Stokes (CSR)", "% do tempo em Cheyne-Stokes (CSR)", "Cheyne-Stokes ido %-a (CSR)", "% du temps en Cheyne-Stokes (CSR)"}},
        {"Median leak (L/min)", {"Mediana de fuga (L/min)", "Mediana de fuga (L/min)", "Median szivargas (L/perc)", "Fuite mediane (L/min)"}},
        {"95th leak (L/min)", {"Fuga P95 (L/min)", "Fuga P95 (L/min)", "Szivargas P95 (L/perc)", "Fuite P95 (L/min)"}},
        {"% of time in large leak", {"% del tiempo con fuga grande", "% do tempo com fuga grande", "Nagy szivargas ido %-a", "% du temps en grande fuite"}},
        {"Median pressure (cmH2O)", {"Mediana de presion (cmH2O)", "Mediana de pressao (cmH2O)", "Median nyomas (cmH2O)", "Pression mediane (cmH2O)"}},
        {"95th pressure (cmH2O)", {"Presion P95 (cmH2O)", "Pressao P95 (cmH2O)", "Nyomas P95 (cmH2O)", "Pression P95 (cmH2O)"}},
        {"Max pressure (cmH2O)", {"Presion maxima (cmH2O)", "Pressao maxima (cmH2O)", "Maximalis nyomas (cmH2O)", "Pression maximale (cmH2O)"}},
        {"Min pressure (cmH2O)", {"Presion minima (cmH2O)", "Pressao minima (cmH2O)", "Minimalis nyomas (cmH2O)", "Pression minimale (cmH2O)"}},
        {"Average EPAP (cmH2O)", {"EPAP media (cmH2O)", "EPAP media (cmH2O)", "Atlagos EPAP (cmH2O)", "EPAP moyenne (cmH2O)"}},
        {"Min EPAP (cmH2O)", {"EPAP minima (cmH2O)", "EPAP minima (cmH2O)", "Minimalis EPAP (cmH2O)", "EPAP minimale (cmH2O)"}},
        {"Max EPAP (cmH2O)", {"EPAP maxima (cmH2O)", "EPAP maxima (cmH2O)", "Maximalis EPAP (cmH2O)", "EPAP maximale (cmH2O)"}},
        {"Indices are weighted by hours of use. Large leak: the machine's own large leak flags where it records them, otherwise the minutes with leak above 24 L/min.",
         {"Los indices se ponderan por horas de uso. Fuga grande: los avisos de fuga grande del propio equipo cuando los registra; si no, los minutos con fuga por encima de 24 L/min.",
          "Os indices sao ponderados pelas horas de uso. Fuga grande: os alertas de fuga grande do proprio equipamento quando os regista; caso contrario, os minutos com fuga acima de 24 L/min.",
          "Az indexek a hasznalati orakkal sulyozottak. Nagy szivargas: a keszulek sajat nagy szivargas jelzesei, ha rogziti oket, kulonben a 24 L/perc feletti szivargasu percek.",
          "Les indices sont ponderes par les heures d'utilisation. Grande fuite : les signalements de grande fuite de l'appareil lorsqu'il les enregistre, sinon les minutes de fuite au-dessus de 24 L/min."}},
        {"Monthly statistics", {"Estadisticas mensuales", "Estatisticas mensais", "Havi statisztika", "Statistiques mensuelles"}},
        {"Changes to device settings", {"Cambios en la configuracion del equipo", "Alteracoes nas definicoes do equipamento", "A keszulek beallitasainak valtozasai", "Modifications des reglages de l'appareil"}},
        {"First", {"Desde", "Desde", "Elso", "Debut"}},
        {"Last", {"Hasta", "Ate", "Utolso", "Fin"}},
        {"Nights", {"Noches", "Noites", "Ejszakak", "Nuits"}},
        {"Mode", {"Modo", "Modo", "Mod", "Mode"}},
        {"Relief", {"Alivio", "Alivio", "Konnyites", "Confort"}},
        {"FL 95th", {"LF P95", "LF P95", "AK P95", "LD P95"}},
        {"Leak 95th", {"Fuga P95", "Fuga P95", "Sziv P95", "Fuite P95"}},
        {"Hours", {"Horas", "Horas", "Ora", "Heures"}},
        {"Adherence (CMS rule)", {"Adherencia (regla CMS)", "Adesao (regra CMS)", "Terapiahuseg (CMS szabaly)", "Observance (regle CMS)"}},
        {"Rule", {"Regla", "Regra", "Szabaly", "Regle"}},
        {"4 h or more on at least 70% of nights in 30 consecutive days within the first 90 days of therapy",
         {"4 h o mas en al menos el 70% de las noches en 30 dias consecutivos dentro de los primeros 90 dias de terapia",
          "4 h ou mais em pelo menos 70% das noites em 30 dias consecutivos nos primeiros 90 dias de terapia",
          "Legalabb 4 ora az ejszakak legalabb 70%-aban 30 egymast koveto napon belul, a terapia elso 90 napjaban",
          "4 h ou plus sur au moins 70% des nuits pendant 30 jours consecutifs au cours des 90 premiers jours de traitement"}},
        {"Day 1 (first night on record)", {"Dia 1 (primera noche registrada)", "Dia 1 (primeira noite registada)", "1. nap (elso rogzitett ejszaka)", "Jour 1 (premiere nuit enregistree)"}},
        {"Day 90", {"Dia 90", "Dia 90", "90. nap", "Jour 90"}},
        {"Best 30-day window", {"Mejor periodo de 30 dias", "Melhor periodo de 30 dias", "Legjobb 30 napos idoszak", "Meilleure periode de 30 jours"}},
        {"Nights with 4 h or more", {"Noches con 4 h o mas", "Noites com 4 h ou mais", "Legalabb 4 oras ejszakak", "Nuits de 4 h ou plus"}},
        {" of 30", {" de 30", " de 30", " / 30", " sur 30"}},
        {"Result", {"Resultado", "Resultado", "Eredmeny", "Resultat"}},
        {"Meets the rule", {"Cumple la regla", "Cumpre a regra", "Megfelel a szabalynak", "Respecte la regle"}},
        {"Does not meet the rule", {"No cumple la regla", "Nao cumpre a regra", "Nem felel meg a szabalynak", "Ne respecte pas la regle"}},
        {"In progress: the first 90 days end on ", {"En curso: los primeros 90 dias terminan el ", "Em curso: os primeiros 90 dias terminam em ", "Folyamatban: az elso 90 nap vege: ", "En cours : les 90 premiers jours se terminent le "}},
        {"A calculation from the nights HMS-CPAP holds, not a compliance certification.",
         {"Un calculo a partir de las noches que tiene HMS-CPAP, no una certificacion de cumplimiento.",
          "Um calculo a partir das noites que o HMS-CPAP tem, nao uma certificacao de adesao.",
          "Szamitas a HMS-CPAP altal tarolt ejszakakbol, nem megfelelosegi igazolas.",
          "Un calcul a partir des nuits que HMS-CPAP conserve, pas une certification d'observance."}},
        {"Night of ", {"Noche del ", "Noite de ", "Ejszaka: ", "Nuit du "}},
        {"most recent night", {"noche mas reciente", "noite mais recente", "legutobbi ejszaka", "nuit la plus recente"}},
        {"highest AHI in the range", {"IAH mas alto del periodo", "IAH mais alto do periodo", "legmagasabb AHI az idoszakban", "IAH le plus eleve de la periode"}},
        {"highest apnea index in the range", {"indice de apnea mas alto del periodo", "indice de apneia mais alto do periodo", "legmagasabb apnoe index az idoszakban", "indice d'apnee le plus eleve de la periode"}},
        {"Usage", {"Uso", "Uso", "Hasznalat", "Utilisation"}},
        {"Bedtime", {"Hora de acostarse", "Hora de deitar", "Lefekves", "Coucher"}},
        {"Wake-up", {"Hora de levantarse", "Hora de acordar", "Ebredes", "Lever"}},
        {"Settings", {"Configuracion", "Definicoes", "Beallitasok", "Reglages"}},
        {"Event", {"Evento", "Evento", "Esemeny", "Evenement"}},
        {"Count", {"Numero", "Numero", "Darab", "Nombre"}},
        {"Per hour", {"Por hora", "Por hora", "Orankent", "Par heure"}},
        {"Event flags", {"Marcas de eventos", "Marcas de eventos", "Esemenyjelek", "Marqueurs d'evenements"}},
        {"Flow rate, min and max per minute (L/min)", {"Flujo, minimo y maximo por minuto (L/min)", "Fluxo, minimo e maximo por minuto (L/min)", "Aramlas, percenkenti min. es max. (L/perc)", "Debit, min et max par minute (L/min)"}},
        {"Pressure and EPAP (cmH2O)", {"Presion y EPAP (cmH2O)", "Pressao e EPAP (cmH2O)", "Nyomas es EPAP (cmH2O)", "Pression et EPAP (cmH2O)"}},
        {"EPAP", {"EPAP", "EPAP", "EPAP", "EPAP"}},
        {"Leak (L/min)", {"Fuga (L/min)", "Fuga (L/min)", "Szivargas (L/perc)", "Fuite (L/min)"}},
        {"24 L/min", {"24 L/min", "24 L/min", "24 L/perc", "24 L/min"}},
        {"Flow limitation", {"Limitacion de flujo", "Limitacao de fluxo", "Aramlaskorlatozas", "Limitation de debit"}},
        {"Snore", {"Ronquido", "Ronco", "Horkolas", "Ronflement"}},
        {"Tidal volume (mL)", {"Volumen corriente (mL)", "Volume corrente (mL)", "Legzesi terfogat (mL)", "Volume courant (mL)"}},
        {"Respiratory rate (br/min)", {"Frecuencia respiratoria (resp/min)", "Frequencia respiratoria (resp/min)", "Legzesszam (legvetel/perc)", "Frequence respiratoire (resp/min)"}},
        {"Minute ventilation (L/min)", {"Ventilacion minuto (L/min)", "Ventilacao minuto (L/min)", "Percventillacio (L/perc)", "Ventilation minute (L/min)"}},
        {"AHI through the night", {"IAH a lo largo de la noche", "IAH ao longo da noite", "AHI az ejszaka soran", "IAH au cours de la nuit"}},
        {"Apnea index through the night", {"Indice de apnea a lo largo de la noche", "Indice de apneia ao longo da noite", "Apnoe index az ejszaka soran", "Indice d'apnee au cours de la nuit"}},
        {"Time at pressure", {"Tiempo a cada presion", "Tempo em cada pressao", "Ido nyomasonkent", "Temps par pression"}},
        {"Minutes", {"Minutos", "Minutos", "Perc", "Minutes"}},
        {"No per-minute data is stored for this night.", {"No hay datos por minuto guardados para esta noche.", "Nao ha dados por minuto guardados para esta noite.", "Ehhez az ejszakahoz nincs percenkenti adat.", "Aucune donnee par minute n'est enregistree pour cette nuit."}},
        {"Cheyne-Stokes (CSR)", {"Cheyne-Stokes (CSR)", "Cheyne-Stokes (CSR)", "Cheyne-Stokes (CSR)", "Cheyne-Stokes (CSR)"}},
        {"Large leak (LL)", {"Fuga grande (LL)", "Fuga grande (LL)", "Nagy szivargas (LL)", "Grande fuite (LL)"}},
        {"Clear airway (CA)", {"Via aerea abierta (CA)", "Via aerea aberta (CA)", "Nyitott legut (CA)", "Voies aeriennes libres (CA)"}},
        {"Obstructive apnea (OA)", {"Apnea obstructiva (OA)", "Apneia obstrutiva (OA)", "Obstruktiv apnoe (OA)", "Apnee obstructive (OA)"}},
        {"Unclassified apnea (UA)", {"Apnea no clasificada (UA)", "Apneia nao classificada (UA)", "Nem osztalyozott apnoe (UA)", "Apnee non classee (UA)"}},
        {"Hypopnea (H)", {"Hipopnea (H)", "Hipopneia (H)", "Hipopnoe (H)", "Hypopnee (H)"}},
        {"RERA (RE)", {"RERA (RE)", "RERA (RE)", "RERA (RE)", "RERA (RE)"}},
        {"Flow limitation (FL)", {"Limitacion de flujo (FL)", "Limitacao de fluxo (FL)", "Aramlaskorlatozas (FL)", "Limitation de debit (FL)"}},
        {"Vibratory snore (VS)", {"Ronquido vibratorio (VS)", "Ronco vibratorio (VS)", "Vibracios horkolas (VS)", "Ronflement vibratoire (VS)"}},
    };
    return T;
}

// The disclaimer, CpapDash's sentence naming HMS-CPAP (B3).
const Loc kDisclaimerLoc = {
    "HMS-CPAP es una herramienta de consumo para fines educativos y de autoseguimiento, no un dispositivo medico ni asesoramiento medico. Consulta siempre a tu profesional de salud sobre tu terapia.",
    "O HMS-CPAP e uma ferramenta de consumo para educacao e acompanhamento pessoal, nao um dispositivo medico e nao uma orientacao medica. Consulte sempre o seu medico sobre a sua terapia.",
    "A HMS-CPAP oktatasi es onkovetesi celu fogyasztoi eszkoz, nem orvostechnikai eszkoz es nem orvosi tanacs. A terapiajarol mindig egyeztessen a kezeloorvosaval.",
    "HMS-CPAP est un outil grand public d'information et de suivi personnel, pas un dispositif medical et pas un avis medical. Consultez toujours votre medecin au sujet de votre traitement."};

std::string clockOf(long epoch_local) {
    long s = epoch_local % 86400;
    if (s < 0) s += 86400;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld", s / 3600, (s % 3600) / 60);
    return buf;
}

std::string hm(double hours) {
    if (!(hours > 0)) return "00:00";
    const long minutes = std::lround(hours * 60.0);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld", minutes / 60, minutes % 60);
    return buf;
}

std::string pct(double v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f%%", v);
    return buf;
}

std::optional<double> num(const Json::Value& v) {
    if (v.isNull()) return std::nullopt;
    if (v.isNumeric()) return std::isfinite(v.asDouble()) ? std::optional<double>(v.asDouble()) : std::nullopt;
    if (v.isString()) {
        const std::string s = v.asString();
        if (s.empty()) return std::nullopt;
        char* e = nullptr;
        const double d = std::strtod(s.c_str(), &e);
        if (e == s.c_str() || !std::isfinite(d)) return std::nullopt;
        return d;
    }
    return std::nullopt;
}

/// One time series off getSessionSignals: (epoch, value) for `key`.
std::vector<std::pair<long, double>> signal(const Json::Value& sig, const char* key,
                                            bool positive_only = false) {
    std::vector<std::pair<long, double>> out;
    const auto& ts = sig["timestamps"];
    const auto& col = sig[key];
    if (!ts.isArray() || !col.isArray()) return out;
    for (Json::ArrayIndex i = 0; i < ts.size() && i < col.size(); ++i) {
        const auto t = epochOf(ts[i].asString());
        const auto v = num(col[i]);
        if (!t || !v) continue;
        if (positive_only && !(*v > 0)) continue;
        out.push_back({*t, *v});
    }
    return out;
}

}  // namespace

// ---- Labels and cells --------------------------------------------------------

std::string ClinicianReportGenerator::ct(const std::string& en) const {
    const std::size_t li = languageIndex();
    if (li == 0) return en;
    const auto& T = labels();
    const auto it = T.find(en);
    if (it == T.end()) return en;
    return it->second[li - 1];
}

std::string ClinicianReportGenerator::ix(const std::string& en) const {
    if (!ungraded_) return ct(en);
    static const std::map<std::string, std::string> kUngraded = {
        {"AHI", "Apnea index"},
        {"AHI median", "Apnea index median"},
        {"highest AHI in the range", "highest apnea index in the range"},
        {"AHI through the night", "Apnea index through the night"},
    };
    const auto it = kUngraded.find(en);
    return ct(it == kUngraded.end() ? en : it->second);
}

const std::vector<std::string>& ClinicianReportGenerator::labelsForTest() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (const auto& [en, loc] : labels()) k.push_back(en);
        return k;
    }();
    return keys;
}

std::string ClinicianReportGenerator::cell(const std::optional<double>& v, int decimals,
                                           const std::string& suffix) {
    if (!v) return "-";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, *v);
    return std::string(buf) + suffix;
}

std::string ClinicianReportGenerator::disclaimerIn(std::size_t li) {
    if (li == 0 || li > kDisclaimerLoc.size()) return kDisclaimerEn;
    return kDisclaimerLoc[li - 1];
}

std::string ClinicianReportGenerator::title() const { return ct("HMS-CPAP Clinician Report"); }
std::string ClinicianReportGenerator::subtitle(const std::string& p) const {
    return ct("Therapy statistics: ") + p;
}
std::string ClinicianReportGenerator::sectionSummaryHeading() const { return ct("CPAP statistics"); }

// ---- The grid -------------------------------------------------------------------

std::vector<PdfRow> ClinicianReportGenerator::gridRows(const std::vector<WindowFigures>& figs) const {
    std::vector<PdfRow> rows;
    const auto section = [&](const std::string& name) {
        PdfRow r;
        r.cells = {ct(name)};
        r.section = true;
        rows.push_back(r);
    };
    const auto line = [&](const std::string& label, auto value) {
        PdfRow r;
        r.cells.push_back(label);
        for (const auto& f : figs) r.cells.push_back(value(f));
        rows.push_back(r);
    };
    section("CPAP usage");
    line(ct("Total days"), [](const WindowFigures& f) { return std::to_string(f.total_days); });
    line(ct("Used days"), [](const WindowFigures& f) { return std::to_string(f.used_days); });
    line(ct("Days not used"), [](const WindowFigures& f) { return std::to_string(f.total_days - f.used_days); });
    line(ct("Used days >= 4 h"), [](const WindowFigures& f) { return std::to_string(f.days_4h); });
    line(ct("Used days < 4 h"), [](const WindowFigures& f) { return std::to_string(f.days_under_4h); });
    line(ct("% of all days >= 4 h"), [](const WindowFigures& f) { return pct(f.pct_total_4h); });
    line(ct("% of used days >= 4 h"), [](const WindowFigures& f) { return pct(f.pct_used_4h); });
    line(ct("Average hours (used days)"), [](const WindowFigures& f) { return hm(f.avg_hours); });
    line(ct("Median hours (used days)"), [](const WindowFigures& f) { return hm(f.median_hours); });
    section("Therapy efficacy");
    line(ix("AHI"), [](const WindowFigures& f) { return cell(f.ahi); });
    line(ix("AHI median"), [](const WindowFigures& f) { return cell(f.ahi_median); });
    line(ct("Obstructive apnea index (OA)"), [](const WindowFigures& f) { return cell(f.oai); });
    line(ct("Hypopnea index (H)"), [](const WindowFigures& f) { return cell(f.hi); });
    line(ct("Unclassified apnea index (UA)"), [](const WindowFigures& f) { return cell(f.uai); });
    line(ct("Clear airway index (CA)"), [](const WindowFigures& f) { return cell(f.cai); });
    line(ct("RERA index"), [](const WindowFigures& f) { return cell(f.rin); });
    line(ct("Flow limitation (95th)"), [](const WindowFigures& f) { return cell(f.flow_lim_95); });
    line(ct("% of time in Cheyne-Stokes (CSR)"), [](const WindowFigures& f) { return cell(f.csr_pct, 2, "%"); });
    section("Leak");
    line(ct("Median leak (L/min)"), [](const WindowFigures& f) { return cell(f.leak_median); });
    line(ct("95th leak (L/min)"), [](const WindowFigures& f) { return cell(f.leak_95); });
    line(ct("% of time in large leak"), [](const WindowFigures& f) { return cell(f.large_leak_pct, 2, "%"); });
    section("Pressure");
    line(ct("Median pressure (cmH2O)"), [](const WindowFigures& f) { return cell(f.press_median); });
    line(ct("95th pressure (cmH2O)"), [](const WindowFigures& f) { return cell(f.press_95); });
    line(ct("Max pressure (cmH2O)"), [](const WindowFigures& f) { return cell(f.press_max); });
    line(ct("Min pressure (cmH2O)"), [](const WindowFigures& f) { return cell(f.press_min); });
    line(ct("Average EPAP (cmH2O)"), [](const WindowFigures& f) { return cell(f.epap_avg); });
    line(ct("Min EPAP (cmH2O)"), [](const WindowFigures& f) { return cell(f.epap_min); });
    line(ct("Max EPAP (cmH2O)"), [](const WindowFigures& f) { return cell(f.epap_max); });
    return rows;
}

void ClinicianReportGenerator::addGrid(PdfRenderer& pdf, const std::vector<Window>& windows,
                                       const std::vector<Night>& nights,
                                       bool header_dates) const {
    std::vector<std::string> headers = {""};
    std::vector<WindowFigures> figs;
    for (const auto& w : windows) {
        std::string h = header_dates ? ct(w.label) : w.label;
        if (header_dates) h += "\n" + (w.start == w.end ? w.start : ct("since ") + w.start);
        headers.push_back(h);
        figs.push_back(figuresFor(nights, w, 240.0));
    }
    pdf.addGridTable(headers, gridRows(figs), 165.0f, windows.size() > 6 ? 7.0f : 8.0f);
}

// ---- One night's page ----------------------------------------------------------

void ClinicianReportGenerator::addNightPage(PdfRenderer& pdf, const Night& night,
                                            const std::string& why, const std::string& tmpDir,
                                            int index) const {
    pdf.addPageBreak();
    pdf.addSectionHeading(ct("Night of ") + night.date + " (" + why + ")");

    const Json::Value sig = qs_->getSessionSignals(night.date);
    const auto events = eventsFrom(qs_->getSessionEvents(night.date));

    // The night's first and last minute, on the machine's clock (the stored
    // stamps carry no zone, SDD-074), so no offset is applied.
    long t0 = 0, t1 = 0;
    const auto ts = signal(sig, "therapy_pressure");
    const auto anyTs = signal(sig, "leak_rate");
    const auto& clockSrc = !ts.empty() ? ts : anyTs;
    if (!clockSrc.empty()) {
        t0 = clockSrc.front().first;
        t1 = clockSrc.back().first;
    } else if (!events.empty()) {
        t0 = events.front().t;
        t1 = events.back().t;
    }

    std::vector<std::pair<std::string, std::string>> kv;
    kv.push_back({ct("Usage"), hm(night.minutes / 60.0)});
    kv.push_back({ix("AHI"), cell(night.ahi)});
    if (t0 > 0) {
        kv.push_back({ct("Bedtime"), clockOf(t0)});
        kv.push_back({ct("Wake-up"), clockOf(t1)});
    }
    {
        const auto periods = settingsPeriods({night});
        if (!periods.empty()) {
            std::string s = periods[0].mode_label;
            if (!periods[0].pressure.empty()) s += (s.empty() ? "" : ", ") + periods[0].pressure;
            if (!periods[0].relief.empty()) s += (s.empty() ? "" : ", ") + periods[0].relief;
            if (!s.empty()) kv.push_back({ct("Settings"), s});
        }
    }
    kv.push_back({ct("95th pressure (cmH2O)"), cell(night.press_95)});
    kv.push_back({ct("95th leak (L/min)"), cell(night.leak_95)});
    kv.push_back({ct("Flow limitation (95th)"), cell(night.flow_lim_95)});
    pdf.addKeyValueTable(kv);
    pdf.addSpacer(8);

    // Event counts, every flag row the night has.
    const auto& rows = flagRows();
    const auto counts = flagCounts(events);
    static const char* kNames[] = {"Cheyne-Stokes (CSR)", "Large leak (LL)", "Clear airway (CA)",
                                   "Obstructive apnea (OA)", "Unclassified apnea (UA)",
                                   "Hypopnea (H)", "RERA (RE)", "Flow limitation (FL)",
                                   "Vibratory snore (VS)"};
    std::vector<PdfRow> countRows;
    const double hours = night.minutes / 60.0;
    for (std::size_t i = 0; i < rows.size() && i < counts.size(); ++i) {
        if (counts[i] == 0) continue;
        PdfRow r;
        r.cells = {ct(kNames[i]), std::to_string(counts[i]),
                   hours > 0 ? cell(counts[i] / hours) : std::string("-")};
        countRows.push_back(r);
    }
    if (!countRows.empty())
        pdf.addGridTable({ct("Event"), ct("Count"), ct("Per hour")}, countRows, 200.0f);

    if (t0 <= 0 || t1 <= t0) {
        pdf.addNote(ct("No per-minute data is stored for this night."));
        return;
    }
    const double x0 = static_cast<double>((t0 % 86400 + 86400) % 86400);
    const double span = static_cast<double>(t1 - t0);
    const std::string p = tmpDir + "/n" + std::to_string(index) + "_";
    const auto rel = [&](const std::vector<std::pair<long, double>>& s) {
        std::vector<std::pair<double, double>> out;
        for (const auto& [t, v] : s) out.push_back({static_cast<double>(t - t0), v});
        return out;
    };

    // The event strip.
    static const char* kColors[] = {"#7cb342", "#9e9e9e", "#8e24aa", "#29b6f6", "#2e7d32",
                                    "#1e40af", "#f9a825", "#616161", "#ff7043"};
    std::vector<GnuplotService::FlagSeries> flags;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        GnuplotService::FlagSeries f{rows[i].label, kColors[i], {}};
        for (const auto& e : events)
            if (std::find(rows[i].types.begin(), rows[i].types.end(), e.type) != rows[i].types.end())
                f.offsets.push_back(static_cast<double>(e.t - t0));
        flags.push_back(f);
    }
    if (GnuplotService::renderEventStrip(flags, p + "flags.png", ct("Event flags"), x0, span))
        pdf.addChartSized(p + "flags.png", "", 120.0f);

    const auto chart = [&](const std::string& file, std::vector<GnuplotService::TimeSeries> series,
                           const std::string& title, const std::string& unit, double ymin,
                           double ymax, bool bands = false) {
        if (GnuplotService::renderTimeChart(series, p + file, title, unit, x0, span, ymin, ymax,
                                            bands, 190))
            pdf.addChartSized(p + file, "", 92.0f);
    };

    chart("flow.png",
          {{"min", "#1f2937", rel(signal(sig, "flow_min"))}, {"max", "#1f2937", rel(signal(sig, "flow_max"))}},
          ct("Flow rate, min and max per minute (L/min)"), "L/min", -100.0, -1.0, true);
    chart("pressure.png",
          {{ct("Pressure"), "#dc2626", rel(signal(sig, "therapy_pressure", true))},
           {ct("EPAP"), "#16a34a", rel(signal(sig, "epr_pressure", true))}},
          ct("Pressure and EPAP (cmH2O)"), "cmH2O", 0.0, -1.0);
    {
        auto leak = rel(signal(sig, "leak_rate"));
        // A point every five minutes: the chart breaks a line wherever two
        // points are more than ten minutes apart (the machine was off), and a
        // two-point line across the night would be one such gap.
        std::vector<std::pair<double, double>> line;
        if (!leak.empty())
            for (double x = 0; x <= span + 1; x += 300) line.push_back({x, 24.0});
        chart("leak.png", {{ct("Leak"), "#15803d", leak}, {ct("24 L/min"), "#ef4444", line}},
              ct("Leak (L/min)"), "L/min", 0.0, -1.0);
    }
    chart("fl.png", {{ct("Flow limitation"), "#4b5563", rel(signal(sig, "flow_limitation"))}},
          ct("Flow limitation"), "", 0.0, 1.0);
    chart("snore.png", {{ct("Snore"), "#6b7280", rel(signal(sig, "snore_index"))}}, ct("Snore"), "",
          0.0, -1.0);
    chart("tv.png", {{"", "#c026d3", rel(signal(sig, "tidal_volume", true))}},
          ct("Tidal volume (mL)"), "mL", 0.0, -1.0);
    chart("rr.png", {{"", "#7e22ce", rel(signal(sig, "respiratory_rate", true))}},
          ct("Respiratory rate (br/min)"), "br/min", 0.0, -1.0);
    chart("mv.png", {{"", "#0f766e", rel(signal(sig, "minute_ventilation", true))}},
          ct("Minute ventilation (L/min)"), "L/min", 0.0, -1.0);
    {
        std::vector<std::pair<double, double>> ahi;
        for (const auto& [t, v] : runningAhi(events, t0)) ahi.push_back({static_cast<double>(t - t0), v});
        chart("ahi.png", {{"", "#991b1b", ahi}}, ix("AHI through the night"), ix("AHI"), 0.0, -1.0);
    }
    {
        std::vector<double> perMinute;
        for (const auto& [t, v] : signal(sig, "therapy_pressure", true)) perMinute.push_back(v);
        const auto bars = timeAtPressure(perMinute);
        if (GnuplotService::renderBars(bars, p + "tap.png", ct("Time at pressure"), "cmH2O",
                                       ct("Minutes"), "#dc2626", 0.5))
            pdf.addChartSized(p + "tap.png", "", 100.0f);
    }
}

// ---- The document ---------------------------------------------------------------

void ClinicianReportGenerator::generate(int report_id, const std::string& start,
                                        const std::string& end, const std::string& filepath,
                                        const std::string& /*report_dir*/) {
    updateStatus(report_id, "generating");
    const auto fail = [&](const std::string& why) { updateStatus(report_id, "error", why); };
    if (!dayNumber(start) || !dayNumber(end) || start > end) return fail("Invalid date range");

    // Who the machine is, and day 1: the first night on record.
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

    // The nights: the user's range, and the year before `end` the fixed
    // windows and the monthly page reach.
    std::string from = dateOf(*dayNumber(addMonths(end, -12)) + 1);
    if (start < from) from = start;
    const auto nights = nightsFrom(qs_->getClinicalNights(from, end));

    int inRange = 0, gradedInRange = 0;
    for (const auto& n : nights)
        if (n.date >= start && n.date <= end && n.minutes > 0) {
            ++inRange;
            if (!n.ungraded) ++gradedInRange;
        }
    updateNightsCount(report_id, inRange);
    if (inRange == 0) return fail("No therapy nights in range");
    // B7 (SDD-024): only when no night of the range is an AHI does the report
    // call its index something else.
    ungraded_ = gradedInRange == 0;

    // CMS: the first 90 days, which may lie long before the year above.
    std::vector<Night> first90;
    if (!first_night.empty()) {
        const std::string day90 = dateOf(*dayNumber(first_night) + 89);
        first90 = first_night >= from ? nights : nightsFrom(qs_->getDailySummary(first_night, day90));
    }
    const std::string today = nowStr().substr(0, 10);
    const auto cms = cmsAdherence(first90, first_night, today);

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
    kv.push_back({ct("Times shown in"), ct("machine clock")});
    pdf.addKeyValueTable(kv);
    pdf.addSpacer(10);

    // CPAP statistics.
    pdf.addSectionHeading(ct("CPAP statistics"));
    pdf.addNote(ct("All windows end on ") + end + ".");
    const auto windows = standardWindows(nights, start, end, first_night);
    addGrid(pdf, windows, nights, true);
    pdf.addNote(ct("Indices are weighted by hours of use. Large leak: the machine's own large "
                   "leak flags where it records them, otherwise the minutes with leak above 24 L/min."));
    pdf.addSpacer(10);

    // Adherence.
    if (cms.known) {
        pdf.addSectionHeading(ct("Adherence (CMS rule)"));
        std::string result;
        if (cms.met) result = ct("Meets the rule");
        else if (!cms.complete) result = ct("In progress: the first 90 days end on ") + cms.day_90;
        else result = ct("Does not meet the rule");
        // The rule is a sentence: a paragraph, not a table cell it would overrun.
        pdf.addParagraph(ct("Rule") + ": " +
                         ct("4 h or more on at least 70% of nights in 30 consecutive days within "
                            "the first 90 days of therapy"));
        pdf.addKeyValueTable({
            {ct("Day 1 (first night on record)"), cms.first_night},
            {ct("Day 90"), cms.day_90},
            {ct("Best 30-day window"), cms.best_start + ct(" to ") + cms.best_end},
            {ct("Nights with 4 h or more"),
             std::to_string(cms.best_nights_4h) + ct(" of 30") + " (" + pct(cms.best_pct) + ")"},
            {ct("Result"), result},
        });
        pdf.addNote(ct("A calculation from the nights HMS-CPAP holds, not a compliance certification."));
        pdf.addSpacer(10);
    }

    // Monthly statistics, six months to a table.
    const auto months = monthlyWindows(end, first_night);
    if (!months.empty()) {
        pdf.addPageBreak();
        pdf.addSectionHeading(ct("Monthly statistics"));
        for (std::size_t i = 0; i < months.size(); i += 6) {
            const std::vector<Window> part(months.begin() + i,
                                           months.begin() + std::min(months.size(), i + 6));
            addGrid(pdf, part, nights, false);
        }
    }

    // Settings periods.
    const auto periods = settingsPeriods(nights);
    if (!periods.empty()) {
        pdf.addSectionHeading(ct("Changes to device settings"));
        std::vector<PdfRow> rows;
        for (auto it = periods.rbegin(); it != periods.rend(); ++it) {
            PdfRow r;
            r.cells = {it->first, it->last, std::to_string(it->nights), it->mode_label,
                       it->pressure, it->relief, cell(it->ahi), cell(it->flow_lim_95),
                       cell(it->leak_95, 1), hm(it->avg_hours)};
            rows.push_back(r);
        }
        pdf.addGridTable({ct("First"), ct("Last"), ct("Nights"), ct("Mode"), ct("Pressure"),
                          ct("Relief"), ix("AHI"), ct("FL 95th"), ct("Leak 95th"), ct("Hours")},
                         rows,
                         std::vector<float>{50, 50, 30, 34, 92, 36, 30, 34, 38, 34}, 7.0f);
    }

    // The last night and the worst night: one page when they are one night.
    const auto last = lastNight(nights, end);
    const auto worst = worstNight(nights, start, end);
    int index = 0;
    if (last) {
        std::string why = ct("most recent night");
        if (worst && worst->date == last->date) why += ", " + ix("highest AHI in the range");
        addNightPage(pdf, *last, why, tmpDir, index++);
    }
    if (worst && (!last || worst->date != last->date))
        addNightPage(pdf, *worst, ix("highest AHI in the range"), tmpDir, index++);
    pdf.addDisclaimer(disclaimerIn(languageIndex()));

    if (!pdf.save(filepath)) {
        fs::remove_all(tmpDir);
        return fail("PDF save failed");
    }
    fs::remove_all(tmpDir);
    updateStatus(report_id, "ready");
    Json::Value extra(Json::objectValue);
    extra["kind"] = "clinician";
    publishMqtt(report_id, downloadName(filepath), inRange, extra);
    std::cout << "[ClinicianReport] report " << report_id << " ready: " << filepath << "\n";
}

}  // namespace hms_cpap
