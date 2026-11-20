#include "HelperSetup.h"

namespace cpapdash::supervisor {

namespace {

struct Row {
    HTxt        key;
    const char* w[5];   // en, es, fr, hu, pt
};

// One row per word, in HTxt order (run_tests checks the order and that none
// is empty).
const Row kRows[] = {
    {HTxt::RoleTitle, {"HmsCpap Setup", "Configuración de HmsCpap", "Configuration de HmsCpap", "HmsCpap beállítása",
                       "Configuração do HmsCpap"}},
    {HTxt::RoleQuestion, {"What is this computer for?", "¿Para qué es este ordenador?", "À quoi sert cet ordinateur ?",
                          "Mire szolgál ez a számítógép?", "Para que serve este computador?"}},
    {HTxt::RoleHms, {"Read my CPAP machine", "Leer mi máquina CPAP", "Lire mon appareil PPC",
                     "A CPAP-készülékem olvasása", "Ler a minha máquina CPAP"}},
    {HTxt::RoleHmsHint, {"hms-cpap runs here: it reads the machine's card, keeps the nights and shows them.",
                         "hms-cpap funciona aquí: lee la tarjeta de la máquina, guarda las noches y las muestra.",
                         "hms-cpap tourne ici : il lit la carte de l'appareil, garde les nuits et les affiche.",
                         "A hms-cpap itt fut: olvassa a készülék kártyáját, megőrzi és megmutatja az éjszakákat.",
                         "O hms-cpap funciona aqui: lê o cartão da máquina, guarda as noites e mostra-as."}},
    {HTxt::RoleHelper, {"Read OSCAR for hms-cpap", "Leer OSCAR para hms-cpap", "Lire OSCAR pour hms-cpap",
                        "OSCAR olvasása a hms-cpap számára", "Ler o OSCAR para o hms-cpap"}},
    {HTxt::RoleHelperHint,
     {"hms-cpap runs elsewhere (Home Assistant, Docker, another computer). This computer has OSCAR 2.1.1 and reads "
      "an encrypted DreamStation 2 card for it.",
      "hms-cpap funciona en otro sitio (Home Assistant, Docker, otro ordenador). Este ordenador tiene OSCAR 2.1.1 y "
      "lee para él una tarjeta cifrada de DreamStation 2.",
      "hms-cpap tourne ailleurs (Home Assistant, Docker, un autre ordinateur). Cet ordinateur a OSCAR 2.1.1 et lit "
      "pour lui une carte chiffrée de DreamStation 2.",
      "A hms-cpap máshol fut (Home Assistant, Docker, egy másik számítógép). Ezen a számítógépen OSCAR 2.1.1 van, és "
      "beolvassa neki a DreamStation 2 titkosított kártyáját.",
      "O hms-cpap funciona noutro sítio (Home Assistant, Docker, outro computador). Este computador tem o OSCAR 2.1.1 e "
      "lê para ele um cartão encriptado da DreamStation 2."}},
    {HTxt::Continue, {"Continue", "Continuar", "Continuer", "Tovább", "Continuar"}},
    {HTxt::WindowTitle, {"Read OSCAR for hms-cpap", "Leer OSCAR para hms-cpap", "Lire OSCAR pour hms-cpap",
                         "OSCAR olvasása a hms-cpap számára", "Ler o OSCAR para o hms-cpap"}},
    {HTxt::HmsHeading, {"hms-cpap", "hms-cpap", "hms-cpap", "hms-cpap", "hms-cpap"}},
    {HTxt::Looking, {"Looking for hms-cpap on this network...", "Buscando hms-cpap en esta red...",
                     "Recherche de hms-cpap sur ce réseau...", "A hms-cpap keresése ezen a hálózaton...",
                     "À procura do hms-cpap nesta rede..."}},
    {HTxt::FoundOne, {"Found hms-cpap at %1", "hms-cpap encontrado en %1", "hms-cpap trouvé à %1",
                      "A hms-cpap itt található: %1", "hms-cpap encontrado em %1"}},
    {HTxt::FoundMany, {"Found more than one hms-cpap. Choose yours:", "Se encontró más de un hms-cpap. Elija el suyo:",
                       "Plusieurs hms-cpap trouvés. Choisissez le vôtre :",
                       "Több hms-cpap is van. Válassza ki a sajátját:",
                       "Foi encontrado mais de um hms-cpap. Escolha o seu:"}},
    {HTxt::FoundNone, {"hms-cpap was not found on this network.", "No se encontró hms-cpap en esta red.",
                       "hms-cpap n'a pas été trouvé sur ce réseau.", "A hms-cpap nem található ezen a hálózaton.",
                       "O hms-cpap não foi encontrado nesta rede."}},
    {HTxt::TypeAddress, {"Type the address of hms-cpap", "Escriba la dirección de hms-cpap",
                         "Tapez l'adresse de hms-cpap", "Írja be a hms-cpap címét", "Escreva o endereço do hms-cpap"}},
    {HTxt::AnotherAddress, {"Another address", "Otra dirección", "Une autre adresse", "Másik cím", "Outro endereço"}},
    {HTxt::AlreadyReads, {"hms-cpap already reads OSCAR on %1", "hms-cpap ya lee OSCAR en %1",
                          "hms-cpap lit déjà OSCAR sur %1", "A hms-cpap már a(z) %1 gépen olvassa az OSCAR-t",
                          "O hms-cpap já lê o OSCAR em %1"}},
    {HTxt::OscarHeading, {"OSCAR on this computer", "OSCAR en este ordenador", "OSCAR sur cet ordinateur",
                          "OSCAR ezen a számítógépen", "OSCAR neste computador"}},
    {HTxt::OscarHint,
     {"hms-cpap runs this OSCAR with no window to import the card into a profile of its own. Your own profile is not "
      "touched.",
      "hms-cpap ejecuta este OSCAR sin ventana para importar la tarjeta en un perfil propio. Su perfil no se toca.",
      "hms-cpap lance cet OSCAR sans fenêtre pour importer la carte dans un profil à lui. Votre profil n'est pas "
      "touché.",
      "A hms-cpap ablak nélkül futtatja ezt az OSCAR-t, és egy saját profilba importálja a kártyát. Az Ön profiljához "
      "nem nyúl.",
      "O hms-cpap executa este OSCAR sem janela para importar o cartão para um perfil próprio. O seu perfil não é "
      "tocado."}},
    {HTxt::Program, {"OSCAR program", "Programa OSCAR", "Programme OSCAR", "OSCAR program", "Programa OSCAR"}},
    {HTxt::DataDir, {"OSCAR data folder", "Carpeta de datos de OSCAR", "Dossier de données OSCAR",
                     "OSCAR adatmappa", "Pasta de dados do OSCAR"}},
    {HTxt::Profile, {"Profile for hms-cpap", "Perfil para hms-cpap", "Profil pour hms-cpap", "Profil a hms-cpap számára",
                     "Perfil para o hms-cpap"}},
    {HTxt::ProfileHint, {"Make it once in OSCAR with this name.", "Créelo una vez en OSCAR con este nombre.",
                         "Créez-le une fois dans OSCAR avec ce nom.", "Egyszer hozza létre az OSCAR-ban ezzel a névvel.",
                         "Crie-o uma vez no OSCAR com este nome."}},
    {HTxt::Browse, {"Browse...", "Examinar...", "Parcourir...", "Tallózás...", "Procurar..."}},
    {HTxt::Check, {"Check", "Comprobar", "Vérifier", "Ellenőrzés", "Verificar"}},
    {HTxt::Checking, {"Checking...", "Comprobando...", "Vérification...", "Ellenőrzés...", "A verificar..."}},
    {HTxt::Save, {"Save and start", "Guardar e iniciar", "Enregistrer et démarrer", "Mentés és indítás",
                  "Guardar e iniciar"}},
    {HTxt::Cancel, {"Cancel", "Cancelar", "Annuler", "Mégse", "Cancelar"}},
    {HTxt::StateOk, {"Right.", "Correcto.", "Correct.", "Rendben.", "Correto."}},
    {HTxt::StateEmpty, {"Not filled in.", "Sin rellenar.", "Non renseigné.", "Nincs kitöltve.", "Por preencher."}},
    {HTxt::StateNotFound, {"There is no program at this path.", "No hay ningún programa en esta ruta.",
                           "Il n'y a pas de programme à cet endroit.", "Ezen az útvonalon nincs program.",
                           "Não há nenhum programa neste caminho."}},
    {HTxt::StateNotRunnable, {"This program did not start.", "Este programa no arrancó.",
                              "Ce programme n'a pas démarré.", "Ez a program nem indult el.",
                              "Este programa não arrancou."}},
    {HTxt::StateNoBatch, {"This OSCAR cannot import without a window. OSCAR 2.1.1 or later is needed.",
                          "Este OSCAR no puede importar sin ventana. Se necesita OSCAR 2.1.1 o posterior.",
                          "Cet OSCAR ne peut pas importer sans fenêtre. OSCAR 2.1.1 ou plus récent est nécessaire.",
                          "Ez az OSCAR nem tud ablak nélkül importálni. OSCAR 2.1.1 vagy újabb szükséges.",
                          "Este OSCAR não consegue importar sem janela. É preciso o OSCAR 2.1.1 ou mais recente."}},
    {HTxt::StateNoDb, {"There is no oscar.db in this folder.", "No hay oscar.db en esta carpeta.",
                       "Il n'y a pas d'oscar.db dans ce dossier.", "Ebben a mappában nincs oscar.db.",
                       "Não há oscar.db nesta pasta."}},
    {HTxt::StateNotSupported, {"This OSCAR version is not supported yet. Supported: OSCAR 2.1.1.",
                               "Esta versión de OSCAR aún no es compatible. Compatible: OSCAR 2.1.1.",
                               "Cette version d'OSCAR n'est pas encore prise en charge. Prise en charge : OSCAR 2.1.1.",
                               "Ez az OSCAR-verzió még nem támogatott. Támogatott: OSCAR 2.1.1.",
                               "Esta versão do OSCAR ainda não é suportada. Suportada: OSCAR 2.1.1."}},
    {HTxt::StateMissing, {"This profile is not in OSCAR yet. Make it in OSCAR, then check again.",
                          "Este perfil aún no está en OSCAR. Créelo en OSCAR y compruebe de nuevo.",
                          "Ce profil n'est pas encore dans OSCAR. Créez-le dans OSCAR, puis vérifiez à nouveau.",
                          "Ez a profil még nincs az OSCAR-ban. Hozza létre, majd ellenőrizze újra.",
                          "Este perfil ainda não está no OSCAR. Crie-o no OSCAR e verifique de novo."}},
    {HTxt::StateUnknown, {"Checked once the data folder is right.", "Se comprueba cuando la carpeta de datos sea correcta.",
                          "Vérifié une fois le dossier de données correct.", "Az adatmappa javítása után ellenőrizhető.",
                          "Verificado quando a pasta de dados estiver correta."}},
    {HTxt::TrayConnecting, {"Looking for hms-cpap at %1...", "Buscando hms-cpap en %1...",
                            "Recherche de hms-cpap à %1...", "A hms-cpap keresése itt: %1...",
                            "À procura do hms-cpap em %1..."}},
    {HTxt::TrayConnected, {"Reading OSCAR for hms-cpap at %1: connected", "Leyendo OSCAR para hms-cpap en %1: conectado",
                           "Lecture d'OSCAR pour hms-cpap à %1 : connecté",
                           "OSCAR olvasása a hms-cpap számára (%1): csatlakozva",
                           "A ler o OSCAR para o hms-cpap em %1: ligado"}},
    {HTxt::TrayConnectedLast, {"Reading OSCAR for hms-cpap at %1: connected, last import %2",
                               "Leyendo OSCAR para hms-cpap en %1: conectado, última importación %2",
                               "Lecture d'OSCAR pour hms-cpap à %1 : connecté, dernier import %2",
                               "OSCAR olvasása a hms-cpap számára (%1): csatlakozva, utolsó importálás %2",
                               "A ler o OSCAR para o hms-cpap em %1: ligado, última importação %2"}},
    {HTxt::TrayImporting, {"Reading OSCAR for hms-cpap at %1: OSCAR is importing...",
                           "Leyendo OSCAR para hms-cpap en %1: OSCAR está importando...",
                           "Lecture d'OSCAR pour hms-cpap à %1 : OSCAR importe...",
                           "OSCAR olvasása a hms-cpap számára (%1): az OSCAR importál...",
                           "A ler o OSCAR para o hms-cpap em %1: o OSCAR está a importar..."}},
    {HTxt::TrayUnreachable, {"hms-cpap at %1 is not answering. It is tried again by itself.",
                             "hms-cpap en %1 no responde. Se vuelve a intentar solo.",
                             "hms-cpap à %1 ne répond pas. Nouvel essai automatique.",
                             "A hms-cpap (%1) nem válaszol. Magától újra próbálkozik.",
                             "O hms-cpap em %1 não responde. Volta a tentar sozinho."}},
    {HTxt::TrayForgotten, {"hms-cpap forgot this PC", "hms-cpap olvidó este PC", "hms-cpap a oublié ce PC",
                           "A hms-cpap elfelejtette ezt a PC-t", "O hms-cpap esqueceu este PC"}},
    {HTxt::TrayRefused, {"hms-cpap already reads OSCAR on %1", "hms-cpap ya lee OSCAR en %1",
                         "hms-cpap lit déjà OSCAR sur %1", "A hms-cpap már a(z) %1 gépen olvassa az OSCAR-t",
                         "O hms-cpap já lê o OSCAR em %1"}},
    {HTxt::TrayUpdate, {"This hms-cpap and the one at %1 do not match. Update hms-cpap here.",
                        "Este hms-cpap y el de %1 no coinciden. Actualice hms-cpap aquí.",
                        "Ce hms-cpap et celui de %1 ne correspondent pas. Mettez à jour hms-cpap ici.",
                        "Ez a hms-cpap és a(z) %1 címen lévő nem illik össze. Frissítse itt a hms-cpap-ot.",
                        "Este hms-cpap e o de %1 não coincidem. Atualize o hms-cpap aqui."}},
    {HTxt::TrayNoAddress, {"No hms-cpap address yet. Open Settings.", "Aún no hay dirección de hms-cpap. Abra Ajustes.",
                           "Pas encore d'adresse hms-cpap. Ouvrez les Réglages.",
                           "Még nincs hms-cpap cím. Nyissa meg a Beállításokat.",
                           "Ainda sem endereço do hms-cpap. Abra as Definições."}},
    {HTxt::TrayStopped, {"The OSCAR helper is stopped", "El ayudante de OSCAR está detenido",
                         "L'assistant OSCAR est arrêté", "Az OSCAR segéd le van állítva",
                         "O assistente OSCAR está parado"}},
    {HTxt::ConnectAgain, {"Connect again", "Conectar de nuevo", "Se reconnecter", "Csatlakozás újra",
                          "Ligar de novo"}},
    {HTxt::ChangeSettings, {"Settings...", "Ajustes...", "Réglages...", "Beállítások...", "Definições..."}},
    {HTxt::StartAtLogin, {"Start at login", "Iniciar al iniciar sesión", "Démarrer à l'ouverture de session",
                          "Indítás bejelentkezéskor", "Iniciar ao entrar"}},
    {HTxt::Quit, {"Quit", "Salir", "Quitter", "Kilépés", "Sair"}},
};

static_assert(sizeof(kRows) / sizeof(kRows[0]) == static_cast<size_t>(HTxt::Count_), "one row per word");

std::string str(const nlohmann::json& j, const char* k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}

}  // namespace

const char* htext(HTxt key, Lang lang) {
    const auto i = static_cast<size_t>(key);
    if (i >= static_cast<size_t>(HTxt::Count_) || kRows[i].key != key) return "";
    return kRows[i].w[static_cast<int>(lang)];
}

std::string roleOf(const std::optional<nlohmann::json>& config) {
    if (!config || !config->is_object()) return "";
    return str(*config, "role") == kRoleHelper ? kRoleHelper : "";
}

bool helperReady(const nlohmann::json& config) {
    if (!config.is_object() || !config.contains("oscar_helper") || !config.contains("oscar")) return false;
    return !str(config["oscar_helper"], "host").empty() && !str(config["oscar"], "data_dir").empty();
}

std::vector<FoundHms> parseDiscover(const std::string& text) {
    std::vector<FoundHms> out;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (...) {
        return out;
    }
    if (!j.is_object() || !j.contains("found") || !j["found"].is_array()) return out;
    for (const auto& f : j["found"]) {
        FoundHms h;
        h.address = str(f, "address");
        if (h.address.empty()) continue;
        if (f.contains("port") && f["port"].is_number_integer()) h.port = f["port"].get<int>();
        h.name = str(f, "name");
        h.helper = str(f, "helper");
        out.push_back(h);
    }
    return out;
}

void applyHelperInputs(nlohmann::json& config, const HelperInputs& in) {
    if (!config.is_object()) config = nlohmann::json::object();
    config["role"] = kRoleHelper;
    auto& h = config["oscar_helper"];
    if (!h.is_object()) h = nlohmann::json::object();
    const bool same = str(h, "host") == in.host && h.value("port", 8894) == in.port;
    h["host"] = in.host;
    h["port"] = in.port;
    if (!same || !h.contains("token")) h["token"] = "";
    auto& o = config["oscar"];
    if (!o.is_object()) o = nlohmann::json::object();
    o["program"] = in.program;
    o["data_dir"] = in.data_dir;
    o["profile"] = in.profile.empty() ? "hms-cpap" : in.profile;
}

void dropHelperToken(nlohmann::json& config) {
    if (config.is_object() && config.contains("oscar_helper") && config["oscar_helper"].is_object())
        config["oscar_helper"]["token"] = "";
}

HTxt checkWord(const std::string& state) {
    if (state == "ok") return HTxt::StateOk;
    if (state == "empty") return HTxt::StateEmpty;
    if (state == "not_found") return HTxt::StateNotFound;
    if (state == "not_runnable") return HTxt::StateNotRunnable;
    if (state == "no_batch") return HTxt::StateNoBatch;
    if (state == "no_db") return HTxt::StateNoDb;
    if (state == "not_supported") return HTxt::StateNotSupported;
    if (state == "missing") return HTxt::StateMissing;
    return HTxt::StateUnknown;
}

TrayLine trayLine(const nlohmann::json& status, const std::string& last_import_hhmm) {
    TrayLine t;
    const std::string state = str(status, "state");
    const std::string host = str(status, "host");
    t.arg1 = host;
    if (state == "connected") {
        t.key = last_import_hhmm.empty() ? HTxt::TrayConnected : HTxt::TrayConnectedLast;
        t.arg2 = last_import_hhmm;
    } else if (state == "importing") {
        t.key = HTxt::TrayImporting;
    } else if (state == "unreachable") {
        t.key = HTxt::TrayUnreachable;
        t.problem = true;
    } else if (state == "forgotten") {
        t.key = HTxt::TrayForgotten;
        t.connect_again = true;
        t.problem = true;
    } else if (state == "refused") {
        t.key = HTxt::TrayRefused;
        t.arg1 = str(status, "helper");
        t.connect_again = true;
        t.problem = true;
    } else if (state == "update") {
        t.key = HTxt::TrayUpdate;
        t.problem = true;
    } else if (state == "no_address") {
        t.key = HTxt::TrayNoAddress;
        t.problem = true;
    } else {
        t.key = HTxt::TrayConnecting;
    }
    return t;
}

}  // namespace cpapdash::supervisor
