// SDD-025: the Pi install script carries the same runtime package list as the
// Dockerfile's final stage. Two copies of one list drift, and a library added
// to the image but not to the script is a Pi that fails to start hms_cpap with
// a missing .so after an apparently successful install. This reads both files
// from the source tree and refuses to let them differ.
#include <gtest/gtest.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace {

std::filesystem::path sourceRoot() {
#ifdef HMS_CPAP_SOURCE_DIR
    return HMS_CPAP_SOURCE_DIR;
#else
    for (const char* p : {".", "..", "../.."}) {
        if (std::filesystem::exists(std::filesystem::path(p) / "Dockerfile")) return p;
    }
    return ".";
#endif
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    EXPECT_TRUE(in.good()) << "cannot read " << p;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool looksLikePackage(const std::string& tok) {
    if (tok.empty()) return false;
    if (tok == "&&" || tok == "\\" || tok[0] == '-') return false;
    return std::all_of(tok.begin(), tok.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '+';
    });
}

// The apt-get install line of the runtime stage, up to the `&& rm -rf` that
// closes it. Tokens are package names; flags, continuations and the command
// words themselves are dropped.
std::set<std::string> dockerfileRuntimePackages(const std::string& text) {
    const auto stage = text.find("Stage 3: Runtime");
    EXPECT_NE(stage, std::string::npos) << "the Dockerfile has no 'Stage 3: Runtime' marker";
    const auto install = text.find("apt-get install", stage);
    EXPECT_NE(install, std::string::npos);
    const auto end = text.find("rm -rf", install);
    EXPECT_NE(end, std::string::npos);

    std::string span = text.substr(install, end - install);
    std::replace(span.begin(), span.end(), '\\', ' ');
    std::istringstream in(span);
    std::set<std::string> pkgs;
    std::string tok;
    bool past_command = false;
    while (in >> tok) {
        if (!past_command) {                 // skip "apt-get install -y --no-install-recommends"
            if (tok[0] == '-') continue;
            if (tok == "apt-get" || tok == "install") continue;
            past_command = true;
        }
        if (looksLikePackage(tok)) pkgs.insert(tok);
    }
    return pkgs;
}

// Everything between the two marker comments in install.sh that is not the
// variable assignment itself.
std::set<std::string> installScriptRuntimePackages(const std::string& text) {
    const auto begin = text.find("# runtime-packages-begin");
    const auto end = text.find("# runtime-packages-end");
    EXPECT_NE(begin, std::string::npos) << "install.sh has no runtime-packages-begin marker";
    EXPECT_NE(end, std::string::npos) << "install.sh has no runtime-packages-end marker";

    std::istringstream in(text.substr(begin, end - begin));
    std::set<std::string> pkgs;
    std::string line;
    while (std::getline(in, line)) {
        // trim
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        if (line.empty() || line[0] == '#' || line[0] == '"') continue;
        if (line.find('=') != std::string::npos) continue;   // RUNTIME_PACKAGES="
        if (looksLikePackage(line)) pkgs.insert(line);
    }
    return pkgs;
}

std::string join(const std::set<std::string>& s) {
    std::string out;
    for (const auto& x : s) out += (out.empty() ? "" : ", ") + x;
    return out;
}

}  // namespace

TEST(PiInstallRuntimeList, MatchesTheDockerfileRuntimeStage) {
    const auto root = sourceRoot();
    const auto docker = dockerfileRuntimePackages(slurp(root / "Dockerfile"));
    const auto script = installScriptRuntimePackages(slurp(root / "packaging" / "pi" / "install.sh"));

    ASSERT_FALSE(docker.empty()) << "no runtime packages parsed from the Dockerfile";
    ASSERT_FALSE(script.empty()) << "no runtime packages parsed from install.sh";

    std::set<std::string> only_docker, only_script;
    std::set_difference(docker.begin(), docker.end(), script.begin(), script.end(),
                        std::inserter(only_docker, only_docker.end()));
    std::set_difference(script.begin(), script.end(), docker.begin(), docker.end(),
                        std::inserter(only_script, only_script.end()));

    EXPECT_TRUE(only_docker.empty())
        << "in the Dockerfile runtime stage but not in packaging/pi/install.sh: " << join(only_docker);
    EXPECT_TRUE(only_script.empty())
        << "in packaging/pi/install.sh but not in the Dockerfile runtime stage: " << join(only_script);
}

TEST(PiInstallRuntimeList, TheUnitTemplateHasBothPlaceholders) {
    const auto unit = slurp(sourceRoot() / "packaging" / "pi" / "hms-cpap.service");
    EXPECT_NE(unit.find("User=__USER__"), std::string::npos);
    EXPECT_NE(unit.find("Environment=HOME=__HOME__"), std::string::npos);
    EXPECT_NE(unit.find("HMS_CPAP_SUPERVISED=1"), std::string::npos)
        << "without HMS_CPAP_SUPERVISED the service tries to open a browser on a headless Pi";
}
