/*
 * Nova Omni-Toolchain Bootstrapper
 * File: nova_toolchain.cpp
 * Standard: C++17
 *
 * Purpose
 * -------
 * Provision language/package-manager toolchains into:
 *
 *     ./.nova/toolchains/<ecosystem>/
 *
 * and execute package-manager commands with an isolated PATH.
 *
 * Supported ecosystems:
 *   python/pip, node/npm, rust/cargo, go/go, ruby/gem,
 *   dotnet/nuget, java/mvn, php/composer, cpp/cmake, git
 *
 * Design:
 *   - Never requires a globally-installed package manager when a local
 *     toolchain can be provisioned.
 *   - Uses curl + tar/unzip when available.
 *   - Detects Termux and provides a local-prefix fallback.
 *   - Uses file locks to prevent concurrent bootstrap races.
 *   - Uses only the C++17 standard library plus external commands invoked
 *     through std::system(), as required by the Nova bootstrap architecture.
 *
 * Public integration API:
 *
 *   namespace nova::toolchain {
 *       bool bootstrap_toolchain(Ecosystem ecosystem);
 *       int run_isolated_command(Ecosystem ecosystem,
 *                                const std::string& command);
 *       int run_isolated_command(const std::string& command);
 *       std::string executable(Ecosystem ecosystem,
 *                              const std::string& name);
 *   }
 *
 * The implementation intentionally keeps package-manager installation
 * separate from NUPM's package-resolution logic. NUPM can call:
 *
 *   bootstrap_toolchain(ecosystem);
 *   run_isolated_command(ecosystem, "pip install ...");
 *
 * before resolving/installing a package.
 */

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/wait.h>
#  include <sys/utsname.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace nova::toolchain {

enum class Ecosystem {
    Python,
    Node,
    Rust,
    Go,
    Ruby,
    DotNet,
    Java,
    PHP,
    Cpp,
    Git
};

struct ToolchainSpec {
    Ecosystem ecosystem;
    const char* key;
    const char* display_name;
    const char* executable_name;
    const char* manager_name;
    const char* manager_relative_path;
    const char* archive_kind;
    const char* local_version;
};

static const std::map<Ecosystem, ToolchainSpec>& registry() {
    static const std::map<Ecosystem, ToolchainSpec> r = {
        {Ecosystem::Python, {
            Ecosystem::Python, "python", "Python / pip",
            "python", "pip", "bin/pip", "tar.xz", "3.13.7"
        }},
        {Ecosystem::Node, {
            Ecosystem::Node, "node", "Node.js / npm",
            "node", "npm", "bin/npm", "tar.xz", "24.7.0"
        }},
        {Ecosystem::Rust, {
            Ecosystem::Rust, "rust", "Rust / Cargo",
            "rustc", "cargo", "bin/cargo", "tar.xz", "1.89.0"
        }},
        {Ecosystem::Go, {
            Ecosystem::Go, "go", "Go",
            "go", "go", "bin/go", "tar.gz", "1.25.0"
        }},
        {Ecosystem::Ruby, {
            Ecosystem::Ruby, "ruby", "Ruby / Gem",
            "ruby", "gem", "bin/gem", "tar.xz", "3.4.5"
        }},
        {Ecosystem::DotNet, {
            Ecosystem::DotNet, "dotnet", ".NET / NuGet",
            "dotnet", "dotnet", "dotnet", "tar.gz", "10.0.0"
        }},
        {Ecosystem::Java, {
            Ecosystem::Java, "java", "Java / Maven",
            "java", "mvn", "bin/mvn", "tar.gz", "25"
        }},
        {Ecosystem::PHP, {
            Ecosystem::PHP, "php", "PHP / Composer",
            "php", "composer", "composer", "tar.gz", "8.4"
        }},
        {Ecosystem::Cpp, {
            Ecosystem::Cpp, "cpp", "C++ / CMake",
            "cmake", "cmake", "bin/cmake", "tar.gz", "4.1.1"
        }},
        {Ecosystem::Git, {
            Ecosystem::Git, "git", "Git",
            "git", "git", "bin/git", "tar.gz", "2.50.1"
        }}
    };
    return r;
}

static std::mutex bootstrap_mutex;

static constexpr const char* RESET   = "\033[0m";
static constexpr const char* RED     = "\033[31m";
static constexpr const char* GREEN   = "\033[32m";
static constexpr const char* YELLOW  = "\033[33m";
static constexpr const char* BLUE    = "\033[34m";
static constexpr const char* CYAN    = "\033[36m";
static constexpr const char* MAGENTA = "\033[35m";

static void info(const std::string& s) {
    std::cout << BLUE << "[NOVA-TOOLCHAIN] " << RESET << s << '\n';
}

static void success(const std::string& s) {
    std::cout << GREEN << "[NOVA-TOOLCHAIN] " << RESET << s << '\n';
}

static void warning(const std::string& s) {
    std::cerr << YELLOW << "[NOVA-TOOLCHAIN] " << RESET << s << '\n';
}

static void error(const std::string& s) {
    std::cerr << RED << "[NOVA-TOOLCHAIN] " << RESET << s << '\n';
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string shell_quote(const std::string& value) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += '"';
    return out;
#else
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
#endif
}

static std::string path_string(const fs::path& p) {
    return p.lexically_normal().string();
}

static fs::path nova_root() {
    if (const char* root = std::getenv("NOVA_ROOT")) {
        if (*root) return fs::path(root);
    }
    return fs::current_path() / ".nova";
}

static fs::path toolchains_root() {
    return nova_root() / "toolchains";
}

static fs::path state_root() {
    return nova_root() / "state";
}

static fs::path downloads_root() {
    return nova_root() / "downloads";
}

static bool ensure_directory(const fs::path& p) {
    try {
        fs::create_directories(p);
        return true;
    } catch (const fs::filesystem_error& e) {
        error("Cannot create " + path_string(p) + ": " + e.what());
        return false;
    }
}

static bool command_exists(const std::string& command) {
#ifdef _WIN32
    const std::string probe = "where " + shell_quote(command) + " >NUL 2>NUL";
#else
    const std::string probe = "command -v " + shell_quote(command) + " >/dev/null 2>&1";
#endif
    return std::system(probe.c_str()) == 0;
}

static bool is_termux() {
    if (std::getenv("TERMUX_VERSION")) return true;
    if (std::getenv("PREFIX")) {
        const char* prefix = std::getenv("PREFIX");
        if (prefix && std::string(prefix).find("/com.termux/") != std::string::npos)
            return true;
    }
    return false;
}

static std::string host_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__x86_64__) || defined(_M_X64)
    return "amd64";
#elif defined(__i386__) || defined(_M_IX86)
    return "386";
#else
    return "unknown";
#endif
}

static std::string host_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

static std::string shell_os() {
#ifdef _WIN32
    return "windows";
#else
    return "posix";
#endif
}

static bool executable_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

static bool manager_ready(Ecosystem e) {
    const auto& spec = registry().at(e);
    const fs::path root = toolchains_root() / spec.key;

    if (e == Ecosystem::DotNet)
        return executable_file(root / "dotnet");
    if (e == Ecosystem::PHP)
        return executable_file(root / "bin" / "php") ||
               executable_file(root / "php");

    return executable_file(root / spec.manager_relative_path);
}

static fs::path manager_path(Ecosystem e) {
    const auto& spec = registry().at(e);
    const fs::path root = toolchains_root() / spec.key;

    if (e == Ecosystem::DotNet) {
#ifdef _WIN32
        return root / "dotnet.exe";
#else
        return root / "dotnet";
#endif
    }

    if (e == Ecosystem::PHP) {
        if (executable_file(root / "bin" / "php"))
            return root / "bin" / "php";
        return root / "php";
    }

    return root / spec.manager_relative_path;
}

/*
 * A deterministic architecture/platform tuple used by the bootstrap
 * templates. These templates can be overridden by environment variables:
 *
 *   NOVA_<ECOSYSTEM>_URL
 *   NOVA_<ECOSYSTEM>_SHA256
 *
 * This lets a release build pin exact vendor URLs/checksums without changing
 * the source file.
 */
static std::string override_key(Ecosystem e) {
    std::string key = "NOVA_";
    key += lower(registry().at(e).key);
    key += "_URL";
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return key;
}

static std::string configured_url(Ecosystem e) {
    const std::string key = override_key(e);
    if (const char* v = std::getenv(key.c_str()))
        return v;
    return {};
}

static std::string configured_sha256(Ecosystem e) {
    std::string key = "NOVA_";
    key += lower(registry().at(e).key);
    key += "_SHA256";
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    if (const char* v = std::getenv(key.c_str()))
        return v;
    return {};
}

static std::string make_download_url(Ecosystem e) {
    const auto override_url = configured_url(e);
    if (!override_url.empty()) return override_url;

    const std::string arch = host_arch();
    const std::string os = host_os();
    const auto& s = registry().at(e);

    /*
     * Official distribution endpoints are deliberately expressed using
     * vendor-supported archive layouts. Release engineering may override
     * any URL at runtime for mirrors or a fully pinned Nova distribution.
     */
    if (e == Ecosystem::Node) {
        std::string a = arch == "amd64" ? "x64" :
                        arch == "arm64" ? "arm64" :
                        arch;
        std::string platform = os == "darwin" ? "darwin" :
                               os == "windows" ? "win" : "linux";
        return "https://nodejs.org/dist/v" + std::string(s.local_version) +
               "/node-v" + s.local_version + "-" + platform + "-" + a + ".tar.xz";
    }

    if (e == Ecosystem::Go) {
        std::string a = arch == "amd64" ? "amd64" :
                        arch == "arm64" ? "arm64" : arch;
        std::string platform = os == "darwin" ? "darwin" :
                               os == "windows" ? "windows" : "linux";
        return "https://go.dev/dl/go" + std::string(s.local_version) +
               "." + platform + "-" + a + ".tar.gz";
    }

    if (e == Ecosystem::Rust) {
        /*
         * Rust upstream uses rustup for installation. The direct rustup
         * bootstrap is handled separately below because Rust archives are
         * target-triple and channel dependent.
         */
        return {};
    }

    if (e == Ecosystem::DotNet) {
        return "https://dot.net/v1/dotnet-install.sh";
    }

    if (e == Ecosystem::Cpp) {
        std::string a = arch == "amd64" ? "x86_64" :
                        arch == "arm64" ? "aarch64" : arch;
        std::string platform = os == "darwin" ? "macos" :
                               os == "windows" ? "windows" : "linux";
        return "https://github.com/Kitware/CMake/releases/download/v" +
               std::string(s.local_version) + "/cmake-" + s.local_version +
               "-" + platform + "-" + a + ".tar.gz";
    }

    /*
     * Python, Ruby, PHP, Java and Git have many platform-specific release
     * layouts. For those, Nova accepts a pinned NOVA_<NAME>_URL in production
     * and otherwise falls back to the host's package archive facilities
     * without installing globally.
     */
    return {};
}

static bool verify_sha256(const fs::path& file, const std::string& expected) {
    if (expected.empty()) return true;
    if (!command_exists("sha256sum")) {
        warning("SHA-256 requested but sha256sum is unavailable; refusing to "
                "silently skip verification.");
        return false;
    }

    const std::string command =
        "printf '%s  %s\\n' " + shell_quote(expected) + " " +
        shell_quote(path_string(file)) + " | sha256sum -c - >/dev/null 2>&1";

    return std::system(command.c_str()) == 0;
}

static int run_shell(const std::string& command) {
    return std::system(command.c_str());
}

static bool download_file(const std::string& url, const fs::path& destination) {
    if (!command_exists("curl")) {
        error("curl is required for automatic bootstrap.");
        return false;
    }

    if (!ensure_directory(destination.parent_path()))
        return false;

    const fs::path partial = destination.string() + ".partial";
    std::error_code ec;
    fs::remove(partial, ec);

    info("Downloading " + url);
    const std::string cmd =
        "curl --fail --location --show-error --silent "
        "--retry 5 --retry-delay 2 --connect-timeout 20 "
        "--output " + shell_quote(path_string(partial)) + " " +
        shell_quote(url);

    if (run_shell(cmd) != 0) {
        fs::remove(partial, ec);
        return false;
    }

    fs::rename(partial, destination, ec);
    if (ec) {
        error("Unable to finalize download: " + ec.message());
        fs::remove(partial, ec);
        return false;
    }

    return true;
}

static bool extract_archive(const fs::path& archive,
                            const fs::path& destination) {
    if (!ensure_directory(destination))
        return false;

    const std::string name = lower(archive.filename().string());

    auto ends_with = [](const std::string& value, const std::string& suffix) {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    if (ends_with(name, ".tar.gz") || ends_with(name, ".tgz")) {
        const std::string cmd =
            "tar -xzf " + shell_quote(path_string(archive)) +
            " -C " + shell_quote(path_string(destination));
        return run_shell(cmd) == 0;
    }

    if (ends_with(name, ".tar.xz")) {
        const std::string cmd =
            "tar -xJf " + shell_quote(path_string(archive)) +
            " -C " + shell_quote(path_string(destination));
        return run_shell(cmd) == 0;
    }

    if (ends_with(name, ".zip")) {
        if (!command_exists("unzip")) {
            error("unzip is required to extract " + path_string(archive));
            return false;
        }
        const std::string cmd =
            "unzip -q " + shell_quote(path_string(archive)) +
            " -d " + shell_quote(path_string(destination));
        return run_shell(cmd) == 0;
    }

    error("Unsupported archive format: " + path_string(archive));
    return false;
}

static fs::path find_named_file(const fs::path& root,
                                const std::string& filename,
                                int max_depth = 4) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return {};

    fs::recursive_directory_iterator it(root, ec), end;
    for (; it != end && !ec; it.increment(ec)) {
        if (it.depth() > max_depth) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && it->path().filename() == filename)
            return it->path();
    }
    return {};
}

static bool normalize_extracted_tree(Ecosystem e,
                                     const fs::path& staging,
                                     const fs::path& destination) {
    /*
     * Vendor archives frequently contain one top-level directory. Move the
     * contents of that directory into Nova's stable ecosystem root so NUPM
     * never has to know vendor archive layouts.
     */
    std::error_code ec;
    fs::path actual;

    const auto& spec = registry().at(e);

    if (e == Ecosystem::DotNet) {
        actual = find_named_file(staging,
#ifdef _WIN32
                                 "dotnet.exe"
#else
                                 "dotnet"
#endif
        ).parent_path();
    } else {
        const fs::path expected = staging / spec.manager_relative_path;
        if (executable_file(expected)) actual = staging;
        else {
            const fs::path found = find_named_file(
                staging,
                fs::path(spec.manager_relative_path).filename().string());
            if (!found.empty())
                actual = found.parent_path();
        }
    }

    if (actual.empty()) {
        warning("Could not identify the expected executable in extracted archive.");
        return false;
    }

    /*
     * If the manager is in a vendor top-level directory, its root is normally
     * several parents above bin/<manager>. Walk upward until the archive's
     * staging root or a directory containing bin is reached.
     */
    if (actual != staging) {
        fs::path candidate = actual;
        for (int i = 0; i < 5 && candidate != staging; ++i) {
            if (fs::exists(candidate / "bin", ec) ||
                fs::exists(candidate / "lib", ec) ||
                fs::exists(candidate / "include", ec)) {
                actual = candidate;
                break;
            }
            candidate = candidate.parent_path();
        }
    }

    if (!ensure_directory(destination))
        return false;

    for (const auto& entry : fs::directory_iterator(actual, ec)) {
        if (ec) break;
        const fs::path target = destination / entry.path().filename();
        fs::remove_all(target, ec);
        if (ec) {
            error("Unable to replace " + path_string(target) + ": " + ec.message());
            return false;
        }
        fs::rename(entry.path(), target, ec);
        if (ec) {
            /*
             * Cross-device rename should not normally happen, but a copy
             * fallback makes the bootstrap resilient to unusual mount layouts.
             */
            fs::copy(entry.path(), target,
                     fs::copy_options::recursive |
                     fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error("Unable to install extracted file: " + ec.message());
                return false;
            }
            fs::remove_all(entry.path(), ec);
        }
    }

#ifndef _WIN32
    /*
     * Vendor archives may lose executable bits when copied through some
     * Android/shared-storage paths. Restore them for the bin directory and top-level
     * launchers where possible.
     */
    if (fs::exists(destination / "bin", ec)) {
        for (const auto& entry : fs::directory_iterator(destination / "bin", ec)) {
            if (!ec && entry.is_regular_file(ec))
                ::chmod(entry.path().c_str(), 0755);
        }
    }
#endif

    return manager_ready(e);
}

static bool write_marker(Ecosystem e, const std::string& source) {
    const auto& spec = registry().at(e);
    const fs::path marker = toolchains_root() / spec.key / ".nova-toolchain";

    try {
        std::ofstream out(marker, std::ios::trunc);
        if (!out) return false;
        out << "ecosystem=" << spec.key << '\n';
        out << "version=" << spec.local_version << '\n';
        out << "host_os=" << host_os() << '\n';
        out << "host_arch=" << host_arch() << '\n';
        out << "source=" << source << '\n';
        out << "timestamp="
            << std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count()
            << '\n';
        return true;
    } catch (...) {
        return false;
    }
}

static bool bootstrap_with_archive(Ecosystem e, const std::string& url) {
    const auto& spec = registry().at(e);
    const fs::path destination = toolchains_root() / spec.key;
    const fs::path staging =
        state_root() / ("bootstrap-" + std::string(spec.key));
    const fs::path archive =
        downloads_root() / (std::string(spec.key) + "-" + spec.local_version +
                            "-" + host_os() + "-" + host_arch() +
                            "." + spec.archive_kind);

    std::error_code ec;
    fs::remove_all(staging, ec);

    if (!download_file(url, archive))
        return false;

    const std::string sha = configured_sha256(e);
    if (!verify_sha256(archive, sha)) {
        error("Checksum verification failed for " + std::string(spec.display_name));
        fs::remove(archive, ec);
        return false;
    }

    if (!extract_archive(archive, staging)) {
        fs::remove(archive, ec);
        fs::remove_all(staging, ec);
        return false;
    }

    const fs::path install_staging =
        state_root() / ("install-" + std::string(spec.key));
    fs::remove_all(install_staging, ec);

    if (!normalize_extracted_tree(e, staging, install_staging)) {
        fs::remove(archive, ec);
        fs::remove_all(staging, ec);
        fs::remove_all(install_staging, ec);
        return false;
    }

    fs::remove_all(destination, ec);
    fs::rename(install_staging, destination, ec);
    if (ec) {
        fs::copy(install_staging, destination,
                  fs::copy_options::recursive |
                  fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error("Unable to commit toolchain: " + ec.message());
            return false;
        }
        fs::remove_all(install_staging, ec);
    }

    fs::remove_all(staging, ec);
    write_marker(e, url);
    return manager_ready(e);
}

static bool bootstrap_dotnet() {
    const fs::path destination = toolchains_root() / "dotnet";
    if (!ensure_directory(destination)) return false;

    const std::string url = configured_url(Ecosystem::DotNet);
    if (!url.empty() && url.find("dotnet-install.sh") != std::string::npos) {
        const fs::path script = downloads_root() / "dotnet-install.sh";
        if (!download_file(url, script)) return false;

#ifndef _WIN32
        ::chmod(script.c_str(), 0755);
#endif

        const char* version_env = std::getenv("NOVA_DOTNET_VERSION");
        const std::string version = version_env && *version_env
                                      ? version_env : "latest";

        const std::string cmd =
            "bash " + shell_quote(path_string(script)) +
            " --version " + shell_quote(version) +
            " --install-dir " + shell_quote(path_string(destination)) +
            " --no-path";

        if (run_shell(cmd) != 0) return false;
        return manager_ready(Ecosystem::DotNet);
    }

    return false;
}

static bool bootstrap_rust() {
    const fs::path destination = toolchains_root() / "rust";
    if (!ensure_directory(destination)) return false;

    if (!command_exists("curl")) return false;

    /*
     * rustup supports a completely local installation through RUSTUP_HOME and
     * CARGO_HOME. The installer itself is downloaded, not executed from a
     * global rust installation.
     */
    const fs::path installer = downloads_root() / "rustup-init.sh";
    if (!download_file("https://sh.rustup.rs", installer)) return false;

#ifndef _WIN32
    ::chmod(installer.c_str(), 0755);
#endif

    const fs::path rustup_home = destination / "rustup";
    const fs::path cargo_home = destination / "cargo-home";
    if (!ensure_directory(rustup_home) || !ensure_directory(cargo_home))
        return false;

#ifdef _WIN32
    warning("Rust bootstrap through rustup-init.sh requires a Windows-native installer.");
    return false;
#else
    const std::string cmd =
        "RUSTUP_HOME=" + shell_quote(path_string(rustup_home)) +
        " CARGO_HOME=" + shell_quote(path_string(cargo_home)) +
        " sh " + shell_quote(path_string(installer)) +
        " -y --no-modify-path";

    if (run_shell(cmd) != 0) return false;

    /*
     * Keep the stable cargo/rustc locations predictable.
     */
    const fs::path bin = cargo_home / "bin";
    if (!manager_ready(Ecosystem::Rust)) {
        if (!ensure_directory(destination / "bin")) return false;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(bin, ec)) {
            if (ec) break;
            const fs::path target = destination / "bin" / entry.path().filename();
            fs::copy(entry.path(), target,
                     fs::copy_options::overwrite_existing |
                     fs::copy_options::recursive, ec);
        }
#ifndef _WIN32
        for (const auto& entry : fs::directory_iterator(destination / "bin", ec)) {
            if (!ec && entry.is_regular_file(ec))
                ::chmod(entry.path().c_str(), 0755);
        }
#endif
    }

    write_marker(Ecosystem::Rust, "https://sh.rustup.rs");
    return manager_ready(Ecosystem::Rust);
#endif
}

static bool bootstrap_local_prefix_fallback(Ecosystem e) {
    const auto& spec = registry().at(e);
    const fs::path prefix = toolchains_root() / spec.key;

    /*
     * Termux fallback:
     *
     * `pkg install --root` is not consistently supported across Termux
     * releases/packages. Instead of pretending it is, Nova first attempts
     * Termux's package download facility and extracts package archives into
     * the Nova prefix. This avoids changing the global installed package set.
     */
    if (!is_termux()) return false;

    if (!command_exists("apt")) {
        warning("Termux local fallback requires apt.");
        return false;
    }

    std::string package_name;
    switch (e) {
        case Ecosystem::Python: package_name = "python"; break;
        case Ecosystem::Node: package_name = "nodejs"; break;
        case Ecosystem::Rust: package_name = "rust"; break;
        case Ecosystem::Go: package_name = "golang"; break;
        case Ecosystem::Ruby: package_name = "ruby"; break;
        case Ecosystem::DotNet:
            warning(".NET is not generally available as a native Termux package.");
            return false;
        case Ecosystem::Java: package_name = "openjdk-21"; break;
        case Ecosystem::PHP: package_name = "php"; break;
        case Ecosystem::Cpp: package_name = "cmake"; break;
        case Ecosystem::Git: package_name = "git"; break;
    }

    const fs::path cache = downloads_root() / spec.key;
    if (!ensure_directory(cache) || !ensure_directory(prefix))
        return false;

    /*
     * `apt download` downloads into the chosen directory and does not mutate
     * the installed package database. We then unpack the .deb into the Nova
     * prefix. Dependency closure is not guaranteed by apt download, so this
     * fallback is considered best-effort; production distributions should
     * prefer a pinned Nova mirror.
     */
    const std::string download =
        "cd " + shell_quote(path_string(cache)) +
        " && apt download " + shell_quote(package_name);

    if (run_shell(download) != 0) return false;

    fs::path deb;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(cache, ec)) {
        if (!ec && entry.is_regular_file(ec) &&
            lower(entry.path().extension().string()) == ".deb") {
            deb = entry.path();
            break;
        }
    }

    if (deb.empty()) {
        warning("apt download did not produce a .deb for " + package_name);
        return false;
    }

    if (!command_exists("dpkg-deb")) {
        warning("dpkg-deb is required for the Termux local fallback.");
        return false;
    }

    const std::string extract =
        "dpkg-deb -x " + shell_quote(path_string(deb)) +
        " " + shell_quote(path_string(prefix));

    if (run_shell(extract) != 0) return false;

    /*
     * Termux package binaries normally live under $PREFIX/bin. The extraction
     * mirrors that path below prefix, so PATH injection can use prefix/bin.
     */
    write_marker(e, "termux:apt-download");
    return manager_ready(e);
}

static bool bootstrap_generic(Ecosystem e) {
    const auto& spec = registry().at(e);
    const std::string url = make_download_url(e);

    if (!url.empty()) {
        info("Provisioning " + std::string(spec.display_name) +
             " from pinned vendor archive.");
        if (bootstrap_with_archive(e, url))
            return true;
    }

    if (bootstrap_local_prefix_fallback(e))
        return true;

    error("No safe local bootstrap route succeeded for " +
          std::string(spec.display_name) + ".");
    error("Set NOVA_" + std::string(lower(spec.key)) +
          "_URL and optionally NOVA_" + std::string(lower(spec.key)) +
          "_SHA256 to a pinned, architecture-compatible archive.");
    return false;
}

bool bootstrap_toolchain(Ecosystem ecosystem) {
    std::lock_guard<std::mutex> guard(bootstrap_mutex);

    if (!ensure_directory(toolchains_root()) ||
        !ensure_directory(state_root()) ||
        !ensure_directory(downloads_root()))
        return false;

    if (manager_ready(ecosystem)) {
        return true;
    }

    const auto& spec = registry().at(ecosystem);
    const fs::path lock = state_root() / (std::string(spec.key) + ".lock");

    /*
     * A simple lock directory is portable and atomic on local filesystems.
     * If another Nova process owns it, wait briefly and re-check.
     */
    for (int attempt = 0; attempt < 120; ++attempt) {
        std::error_code ec;
        if (fs::create_directory(lock, ec)) break;

        if (manager_ready(ecosystem)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        if (attempt == 119) {
            error("Timed out waiting for another bootstrap process: " +
                  std::string(spec.key));
            return false;
        }
    }

    struct LockGuard {
        fs::path p;
        ~LockGuard() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } lock_guard{lock};

    if (manager_ready(ecosystem)) return true;

    info("Bootstrapping " + std::string(spec.display_name) +
         " for " + host_os() + "/" + host_arch() + ".");

    bool ok = false;

    if (ecosystem == Ecosystem::Rust)
        ok = bootstrap_rust();
    else if (ecosystem == Ecosystem::DotNet)
        ok = bootstrap_dotnet();
    else
        ok = bootstrap_generic(ecosystem);

    if (ok) {
        success(std::string(spec.display_name) + " is available locally.");
        return true;
    }

    return false;
}

static std::vector<fs::path> all_bin_paths() {
    std::vector<fs::path> paths;
    for (const auto& [e, spec] : registry()) {
        (void)e;
        const fs::path root = toolchains_root() / spec.key;
        const fs::path bin = root / "bin";
        std::error_code ec;
        if (fs::is_directory(bin, ec))
            paths.push_back(bin);

        /*
         * Some portable distributions place the executable at the root.
         */
        if (spec.key == std::string("dotnet"))
            paths.push_back(root);
    }
    return paths;
}

static std::string isolated_path_value() {
    std::ostringstream out;
    bool first = true;

    for (const auto& p : all_bin_paths()) {
        if (!first) out << ':';
        out << path_string(p);
        first = false;
    }

    const char* old_path = std::getenv("PATH");
    if (old_path && *old_path) {
        if (!first) out << ':';
        out << old_path;
    }

    return out.str();
}

static std::string shell_prefix() {
#ifdef _WIN32
    /*
     * Windows support is kept compile-safe. The primary Nova target is
     * Linux/Termux; Windows builds should use a PowerShell/CMD-specific
     * integration layer when enabled.
     */
    return "set \"PATH=" + isolated_path_value() + "\" && ";
#else
    return "PATH=" + shell_quote(isolated_path_value()) + " ";
#endif
}

int run_isolated_command(const std::string& command) {
    if (command.empty()) {
        error("Cannot execute an empty command.");
        return 2;
    }

    const std::string full = shell_prefix() + command;
    info("Executing in Nova toolchain environment.");
    return std::system(full.c_str());
}

int run_isolated_command(Ecosystem ecosystem,
                         const std::string& command) {
    if (!bootstrap_toolchain(ecosystem))
        return 127;

    return run_isolated_command(command);
}

std::string executable(Ecosystem ecosystem, const std::string& name) {
    if (!bootstrap_toolchain(ecosystem))
        return {};

    const fs::path root = toolchains_root() / registry().at(ecosystem).key;

    std::vector<fs::path> candidates = {
        root / "bin" / name,
        root / name
    };

#ifdef _WIN32
    candidates.push_back(root / "bin" / (name + ".exe"));
    candidates.push_back(root / (name + ".exe"));
#endif

    for (const auto& candidate : candidates) {
        if (executable_file(candidate))
            return path_string(candidate);
    }

    return {};
}

bool bootstrap_toolchain(const std::string& ecosystem) {
    const std::string key = lower(ecosystem);
    for (const auto& [e, spec] : registry()) {
        if (key == spec.key)
            return bootstrap_toolchain(e);
    }

    if (key == "pip") return bootstrap_toolchain(Ecosystem::Python);
    if (key == "npm") return bootstrap_toolchain(Ecosystem::Node);
    if (key == "cargo") return bootstrap_toolchain(Ecosystem::Rust);
    if (key == "gem") return bootstrap_toolchain(Ecosystem::Ruby);
    if (key == "nuget") return bootstrap_toolchain(Ecosystem::DotNet);
    if (key == "mvn" || key == "maven") return bootstrap_toolchain(Ecosystem::Java);
    if (key == "composer") return bootstrap_toolchain(Ecosystem::PHP);
    if (key == "cmake") return bootstrap_toolchain(Ecosystem::Cpp);

    error("Unknown ecosystem: " + ecosystem);
    return false;
}

static bool parse_ecosystem(const std::string& value, Ecosystem& out) {
    const std::string key = lower(value);

    if (key == "python" || key == "pip") { out = Ecosystem::Python; return true; }
    if (key == "node" || key == "nodejs" || key == "npm") { out = Ecosystem::Node; return true; }
    if (key == "rust" || key == "cargo") { out = Ecosystem::Rust; return true; }
    if (key == "go" || key == "golang") { out = Ecosystem::Go; return true; }
    if (key == "ruby" || key == "gem") { out = Ecosystem::Ruby; return true; }
    if (key == "dotnet" || key == "nuget" || key == "csharp") { out = Ecosystem::DotNet; return true; }
    if (key == "java" || key == "mvn" || key == "maven") { out = Ecosystem::Java; return true; }
    if (key == "php" || key == "composer") { out = Ecosystem::PHP; return true; }
    if (key == "cpp" || key == "cxx" || key == "c++" || key == "cmake") { out = Ecosystem::Cpp; return true; }
    if (key == "git") { out = Ecosystem::Git; return true; }

    return false;
}

static void print_registry() {
    std::cout << "\nNova local toolchains\n\n";
    for (const auto& [e, spec] : registry()) {
        std::cout << "  " << std::left << std::setw(9) << spec.key
                  << "  " << spec.display_name
                  << "  manager=" << spec.manager_name << '\n';
    }
    std::cout << "\nRoot: " << path_string(toolchains_root()) << "\n";
}

static void print_help(const char* argv0) {
    std::cout
        << "\nNova Omni-Toolchain Bootstrapper\n"
        << "\nUsage:\n"
        << "  " << argv0 << " bootstrap <ecosystem>\n"
        << "  " << argv0 << " run <ecosystem> <command> [args...]\n"
        << "  " << argv0 << " path <ecosystem> <executable>\n"
        << "  " << argv0 << " list\n\n"
        << "Examples:\n"
        << "  " << argv0 << " bootstrap python\n"
        << "  " << argv0 << " run python pip install pandas -t .nova/packages\n"
        << "  " << argv0 << " run node npm install lodash\n"
        << "  " << argv0 << " run rust cargo install ripgrep\n"
        << "  " << argv0 << " run go go install example.com/tool@latest\n"
        << "  " << argv0 << " run ruby gem install rails\n"
        << "  " << argv0 << " run java mvn dependency:get -Dartifact=group:artifact:1.0\n"
        << "  " << argv0 << " run cpp cmake --version\n\n"
        << "Environment overrides:\n"
        << "  NOVA_<ECOSYSTEM>_URL     pinned archive/installer URL\n"
        << "  NOVA_<ECOSYSTEM>_SHA256  expected SHA-256 for archives\n"
        << "  NOVA_DOTNET_VERSION      dotnet version for dotnet-install.sh\n"
        << "  NOVA_ROOT                alternate .nova root\n\n";
}

} // namespace nova::toolchain

#ifndef NOVA_TOOLCHAIN_NO_MAIN

int main(int argc, char** argv) {
    using namespace nova::toolchain;

    std::cout << CYAN
              << "\n╔══════════════════════════════════════════════╗\n"
              << "║       NOVA OMNI-TOOLCHAIN BOOTSTRAPPER      ║\n"
              << "║        LOCAL / ISOLATED TOOLCHAINS           ║\n"
              << "╚══════════════════════════════════════════════╝\n"
              << RESET;

    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    const std::string action = lower(argv[1]);

    if (action == "help" || action == "--help" || action == "-h") {
        print_help(argv[0]);
        return 0;
    }

    if (action == "list") {
        print_registry();
        return 0;
    }

    if (action == "bootstrap") {
        if (argc != 3) {
            error("bootstrap requires an ecosystem.");
            return 2;
        }

        Ecosystem ecosystem;
        if (!parse_ecosystem(argv[2], ecosystem)) {
            error("Unknown ecosystem: " + std::string(argv[2]));
            print_registry();
            return 2;
        }

        return bootstrap_toolchain(ecosystem) ? 0 : 1;
    }

    if (action == "path") {
        if (argc != 4) {
            error("path requires <ecosystem> <executable>.");
            return 2;
        }

        Ecosystem ecosystem;
        if (!parse_ecosystem(argv[2], ecosystem)) {
            error("Unknown ecosystem: " + std::string(argv[2]));
            return 2;
        }

        const std::string path = executable(ecosystem, argv[3]);
        if (path.empty()) return 1;

        std::cout << path << '\n';
        return 0;
    }

    if (action == "run") {
        if (argc < 4) {
            error("run requires <ecosystem> <command> [args...].");
            return 2;
        }

        Ecosystem ecosystem;
        if (!parse_ecosystem(argv[2], ecosystem)) {
            error("Unknown ecosystem: " + std::string(argv[2]));
            return 2;
        }

        /*
         * Preserve the user's command as a single shell string after the
         * ecosystem token. This matches the existing NUPM architecture,
         * which already delegates package-manager command construction to
         * std::system().
         */
        std::ostringstream command;
        for (int i = 3; i < argc; ++i) {
            if (i > 3) command << ' ';
            command << shell_quote(argv[i]);
        }

        return run_isolated_command(ecosystem, command.str());
    }

    error("Unknown action: " + action);
    print_help(argv[0]);
    return 2;
}

#endif
