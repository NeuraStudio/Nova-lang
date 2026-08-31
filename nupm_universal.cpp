/*
 * Nova Universal Package Manager (NUPM)
 * File: nupm_universal.cpp
 * Standard: C++17
 *
 * Usage:
 *   nupm install pip::requests
 *   nupm install npm::lodash
 *   nupm install cargo::ripgrep
 *   nupm install go::example.com/user/tool@latest
 *   nupm install gem::rails
 *   nupm install nuget::Newtonsoft.Json
 *   nupm install mvn::org.apache.commons:commons-lang3:3.14.0
 *   nupm install composer::vendor/package
 *   nupm install vcpkg::https://github.com/user/project.git
 *   nupm install git::https://github.com/user/project.git
 *   nupm install nova::package-name
 *
 * NOTE:
 * This tool delegates to native package managers through std::system().
 * Shell command arguments are quoted to reduce accidental shell parsing.
 */

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace nova {
namespace nupm {

static constexpr const char* RESET  = "\033[0m";
static constexpr const char* BOLD   = "\033[1m";
static constexpr const char* RED    = "\033[31m";
static constexpr const char* GREEN  = "\033[32m";
static constexpr const char* YELLOW = "\033[33m";
static constexpr const char* BLUE   = "\033[34m";
static constexpr const char* CYAN   = "\033[36m";
static constexpr const char* MAGENTA= "\033[35m";

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string shellQuote(const std::string& value) {
#ifdef _WIN32
    std::string result = "\"";
    for (char c : value) {
        if (c == '"') result += "\\\"";
        else result += c;
    }
    result += "\"";
    return result;
#else
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') result += "'\\''";
        else result += c;
    }
    result += "'";
    return result;
#endif
}

std::string pathQuote(const fs::path& path) {
    return shellQuote(path.string());
}

void printBanner() {
    std::cout
        << CYAN << BOLD
        << "\n"
        << "╔══════════════════════════════════════════════╗\n"
        << "║       NOVA UNIVERSAL PACKAGE MANAGER         ║\n"
        << "║            POLYGLOT DISPATCHER               ║\n"
        << "╚══════════════════════════════════════════════╝\n"
        << RESET;
}

void info(const std::string& message) {
    std::cout << BLUE << "[NUPM] " << RESET << message << '\n';
}

void success(const std::string& message) {
    std::cout << GREEN << "[SUCCESS] " << RESET << message << '\n';
}

void warning(const std::string& message) {
    std::cout << YELLOW << "[WARNING] " << RESET << message << '\n';
}

void error(const std::string& message) {
    std::cerr << RED << "[ERROR] " << RESET << message << '\n';
}

void ecosystemBanner(const std::string& ecosystem) {
    std::cout << "\n"
              << MAGENTA << BOLD
              << "═══ Dispatching to " << ecosystem << " ecosystem ═══"
              << RESET << "\n";
}

int runCommand(const std::string& command) {
    info("Executing native package manager...");
    std::cout << CYAN << "  $ " << command << RESET << "\n";

    const int result = std::system(command.c_str());
    if (result == 0) {
        success("Command completed successfully.");
    } else {
        error("Native package manager returned code: " + std::to_string(result));
    }
    return result;
}

fs::path environmentRoot() {
    return fs::current_path() / ".nova" / "environments";
}

bool createEnvironment(const fs::path& directory) {
    try {
        fs::create_directories(directory);
        info("Local environment: " + directory.string());
        return true;
    } catch (const fs::filesystem_error& e) {
        error(std::string("Unable to create directory: ") + e.what());
        return false;
    }
}

std::string currentWorkingDirectory() {
    return fs::current_path().string();
}

std::string basenameFromPackage(const std::string& package) {
    std::string value = package;

    const std::size_t query = value.find_first_of("?#");
    if (query != std::string::npos) value = value.substr(0, query);

    while (!value.empty() && (value.back() == '/' || value.back() == '\\')) {
        value.pop_back();
    }

    const std::size_t slash = value.find_last_of("/\\");
    if (slash != std::string::npos) value = value.substr(slash + 1);

    if (value.size() > 4 && value.substr(value.size() - 4) == ".git") {
        value.resize(value.size() - 4);
    }

    if (value.empty()) value = "package";
    return value;
}

int handlePip(const std::string& package, const fs::path& root) {
    ecosystemBanner("Python / pip");
    const fs::path target = root / "python";
    if (!createEnvironment(target)) return 1;

    return runCommand(
        "pip install " + shellQuote(package) +
        " -t " + pathQuote(target)
    );
}

int handleNpm(const std::string& package, const fs::path& root) {
    ecosystemBanner("Node.js / JavaScript / TypeScript");
    const fs::path target = root / "node";
    if (!createEnvironment(target)) return 1;

    return runCommand(
        "npm install " + shellQuote(package) +
        " --prefix " + pathQuote(target)
    );
}

int handleCargo(const std::string& package, const fs::path& root) {
    ecosystemBanner("Rust / Cargo");
    const fs::path target = root / "cargo";
    if (!createEnvironment(target)) return 1;

    return runCommand(
        "cargo install " + shellQuote(package) +
        " --root " + pathQuote(target)
    );
}

int handleGo(const std::string& package, const fs::path& root) {
    ecosystemBanner("Go");
    const fs::path target = root / "go";
    if (!createEnvironment(target)) return 1;

#ifdef _WIN32
    const std::string command =
        "set \"GOPATH=" + target.string() + "\" && go install " +
        shellQuote(package);
#else
    const std::string command =
        "GOPATH=" + shellQuote(target.string()) +
        " go install " + shellQuote(package);
#endif

    return runCommand(command);
}

int handleGem(const std::string& package, const fs::path& root) {
    ecosystemBanner("Ruby / Gem");
    const fs::path target = root / "ruby";
    if (!createEnvironment(target)) return 1;

    return runCommand(
        "gem install " + shellQuote(package) +
        " --install-dir " + pathQuote(target)
    );
}

int handleNuget(const std::string& package, const fs::path& root) {
    ecosystemBanner("C# / .NET / NuGet");
    const fs::path target = root / "dotnet";
    if (!createEnvironment(target)) return 1;

    warning("dotnet add package normally requires a .NET project in the current directory.");
    return runCommand(
        "dotnet add package " + shellQuote(package) +
        " --package-directory " + pathQuote(target)
    );
}

int handleMaven(const std::string& package, const fs::path& root) {
    ecosystemBanner("Java / Maven");
    const fs::path target = root / "java";
    if (!createEnvironment(target)) return 1;

    return runCommand(
        "mvn dependency:get -Dartifact=" + shellQuote(package) +
        " -Dmaven.repo.local=" + pathQuote(target)
    );
}

int handleComposer(const std::string& package, const fs::path& root) {
    ecosystemBanner("PHP / Composer");
    const fs::path target = root / "php";
    if (!createEnvironment(target)) return 1;

    // Composer expects a project directory. Create a minimal project file if absent.
    const fs::path composerJson = target / "composer.json";
    if (!fs::exists(composerJson)) {
        try {
            std::ofstream file(composerJson);
            file << "{\n"
                 << "  \"name\": \"nova/local-environment\",\n"
                 << "  \"description\": \"NUPM isolated PHP environment\",\n"
                 << "  \"require\": {}\n"
                 << "}\n";
        } catch (...) {
            error("Unable to create local composer.json.");
            return 1;
        }
    }

    return runCommand(
        "composer require " + shellQuote(package) +
        " --working-dir=" + pathQuote(target)
    );
}

int handleCppGit(const std::string& package,
                 const fs::path& root,
                 bool vcpkgMode) {
    ecosystemBanner(vcpkgMode ? "C / C++ / vcpkg-git" : "C / C++ / Git");

    const fs::path target = root / "cpp";
    if (!createEnvironment(target)) return 1;

    std::string repository = package;

    if (vcpkgMode && package.find("://") == std::string::npos &&
        package.rfind("git@", 0) != 0) {
        warning(
            "vcpkg::<package> without a repository URL cannot be directly "
            "cloned. Treating the value as a GitHub repository name."
        );
        repository = "https://github.com/" + package + ".git";
    }

    const fs::path sourceDir = target / basenameFromPackage(repository);

    if (fs::exists(sourceDir)) {
        warning("Source directory already exists. Skipping clone: " +
                sourceDir.string());
    } else {
        const int cloneResult = runCommand(
            "git clone " + shellQuote(repository) +
            " " + pathQuote(sourceDir)
        );
        if (cloneResult != 0) return cloneResult;
    }

    const fs::path buildDir = sourceDir / "build";
    if (!createEnvironment(buildDir)) return 1;

    info("Attempting isolated CMake configuration...");
    int result = runCommand(
        "cmake -S " + pathQuote(sourceDir) +
        " -B " + pathQuote(buildDir) +
        " -DCMAKE_INSTALL_PREFIX=" + pathQuote(target / "installed")
    );

    if (result != 0) {
        warning("CMake configuration failed. Repository was still cloned locally.");
        return result;
    }

    info("Attempting CMake build...");
    result = runCommand(
        "cmake --build " + pathQuote(buildDir)
    );

    if (result != 0) {
        warning("Build failed. Sources remain available in the local workspace.");
    }

    return result;
}

int handleNova(const std::string& package, const fs::path& root) {
    ecosystemBanner("Nova Native Packages");

    const fs::path target = root / "nova";
    if (!createEnvironment(target)) return 1;

    info("Native Nova package resolution selected.");
    warning("No standalone Nova registry backend is embedded in this dispatcher.");

    // Fallback contract: preserve package intent locally so a future Nova
    // resolver can consume it without changing this CLI routing interface.
    const fs::path requestFile = target / (basenameFromPackage(package) + ".request");

    try {
        std::ofstream out(requestFile);
        out << "package=" << package << '\n';
        out << "status=pending-native-resolution\n";
        out.close();

        success("Nova package request recorded locally: " + requestFile.string());
        info("A future Nova registry resolver can process this request.");
        return 0;
    } catch (...) {
        error("Failed to write Nova package request.");
        return 1;
    }
}

int dispatchInstall(const std::string& ecosystem,
                    const std::string& package) {
    if (package.empty()) {
        error("Package name cannot be empty.");
        return 1;
    }

    const fs::path root = environmentRoot();

    if (ecosystem == "pip" || ecosystem == "python") {
        return handlePip(package, root);
    }
    if (ecosystem == "npm" || ecosystem == "node" ||
        ecosystem == "js" || ecosystem == "ts") {
        return handleNpm(package, root);
    }
    if (ecosystem == "cargo" || ecosystem == "rust") {
        return handleCargo(package, root);
    }
    if (ecosystem == "go" || ecosystem == "golang") {
        return handleGo(package, root);
    }
    if (ecosystem == "gem" || ecosystem == "ruby") {
        return handleGem(package, root);
    }
    if (ecosystem == "nuget" || ecosystem == "dotnet" ||
        ecosystem == "csharp") {
        return handleNuget(package, root);
    }
    if (ecosystem == "mvn" || ecosystem == "maven" ||
        ecosystem == "java") {
        return handleMaven(package, root);
    }
    if (ecosystem == "composer" || ecosystem == "php") {
        return handleComposer(package, root);
    }
    if (ecosystem == "git") {
        return handleCppGit(package, root, false);
    }
    if (ecosystem == "vcpkg" || ecosystem == "cpp" ||
        ecosystem == "cxx" || ecosystem == "c++") {
        return handleCppGit(package, root, true);
    }
    if (ecosystem == "nova") {
        return handleNova(package, root);
    }

    error("Unknown ecosystem prefix: " + ecosystem);
    std::cerr << "Supported prefixes: pip, npm, cargo, go, gem, nuget, "
              << "mvn, composer, vcpkg, git, nova\n";
    return 1;
}

void printUsage(const char* program) {
    std::cout
        << "\nUsage:\n"
        << "  " << program << " install <ecosystem>::<package>\n\n"
        << "Examples:\n"
        << "  " << program << " install pip::requests\n"
        << "  " << program << " install npm::lodash\n"
        << "  " << program << " install cargo::ripgrep\n"
        << "  " << program << " install go::example.com/user/tool@latest\n"
        << "  " << program << " install gem::rails\n"
        << "  " << program << " install nuget::Newtonsoft.Json\n"
        << "  " << program << " install mvn::org.example:library:1.0.0\n"
        << "  " << program << " install composer::vendor/package\n"
        << "  " << program << " install git::https://github.com/user/repo.git\n"
        << "  " << program << " install nova::my-package\n\n"
        << "All supported downloads are isolated under:\n"
        << "  ./.nova/environments/\n\n";
}

} // namespace nupm
} // namespace nova

int main(int argc, char* argv[]) {
    using namespace nova::nupm;

    printBanner();

    if (argc == 2) {
        const std::string command = toLower(argv[1]);
        if (command == "--help" || command == "-h" || command == "help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (argc != 3) {
        error("Invalid command syntax.");
        printUsage(argv[0]);
        return 1;
    }

    const std::string command = toLower(argv[1]);
    if (command != "install") {
        error("Unsupported command: " + std::string(argv[1]));
        printUsage(argv[0]);
        return 1;
    }

    const std::string specification = argv[2];

    // Required routing parser: ecosystem::package
    const std::size_t separator = specification.find("::");
    if (separator == std::string::npos) {
        error("Missing ecosystem prefix. Expected <ecosystem>::<package>.");
        printUsage(argv[0]);
        return 1;
    }

    if (separator == 0 || separator + 2 >= specification.size()) {
        error("Invalid package specification: " + specification);
        return 1;
    }

    const std::string ecosystem =
        toLower(specification.substr(0, separator));
    const std::string package =
        specification.substr(separator + 2);

    info("Requested package: " + package);
    info("Selected ecosystem: " + ecosystem);

    return dispatchInstall(ecosystem, package);
}
