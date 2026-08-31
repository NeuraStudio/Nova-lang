// ============================================================================
// nupm.cpp — Nova Universal Package Manager
// ----------------------------------------------------------------------------
// A standalone, single-file C++17 CLI tool for the Nova language ecosystem.
//
// Design constraints (Termux / Android, low RAM):
//   - No heavy third-party libraries (no nlohmann/json, no libcurl).
//   - Uses std::filesystem (C++17) for all path/directory work.
//   - Shells out to system CLI tools already present on the user's machine:
//       git, curl, cmake, make, cargo
//   - Config format ("nova.toml"-style) is parsed with a small hand-rolled
//     line/regex parser — no external TOML/JSON library.
//
// Build:
//   clang++ -std=c++17 nupm.cpp -o nupm
//   (or)   g++ -std=c++17 nupm.cpp -o nupm
//
// Commands:
//   nupm init                 Create a new nova.toml in the current directory
//   nupm install <pkg>        Fetch + build + link a dependency
//   nupm install              Fetch + build + link every dependency in nova.toml
//   nupm build                Build all dependencies already present in cache
// ============================================================================

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ============================================================================
// Small utilities
// ============================================================================

namespace util {

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Strip matching leading/trailing quotes (single or double) from a value.
std::string stripQuotes(const std::string& s) {
    std::string t = trim(s);
    if (t.size() >= 2) {
        char front = t.front();
        char back = t.back();
        if ((front == '"' && back == '"') || (front == '\'' && back == '\'')) {
            return t.substr(1, t.size() - 2);
        }
    }
    return t;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Quote a filesystem path (or arbitrary string) for safe use inside a shell
// command string.
std::string quotePath(const fs::path& p) {
    std::string s = p.string();
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Run a shell command, optionally inside a working directory.
// Returns the process exit code (0 == success).
int runCommand(const std::string& cmd, const fs::path& cwd = "") {
    std::string fullCmd = cmd;
    if (!cwd.empty()) {
        // cd into the target directory in a subshell so we never
        // mutate this process's own working directory.
        fullCmd = "cd " + quotePath(cwd) + " && " + cmd;
    }
    std::cout << "  $ " << fullCmd << "\n";
    int rc = std::system(fullCmd.c_str());
    return rc;
}

std::string nowTimestamp() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return std::string(buf);
}

// Derive a filesystem-safe package name from a URL/git path/short name.
std::string deriveName(const std::string& source) {
    std::string s = source;
    // Strip trailing slashes and ".git"
    while (!s.empty() && s.back() == '/') s.pop_back();
    size_t slash = s.find_last_of('/');
    std::string base = (slash == std::string::npos) ? s : s.substr(slash + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".git") == 0) {
        base = base.substr(0, base.size() - 4);
    }
    if (base.empty()) base = "package";
    return base;
}

bool looksLikeUrl(const std::string& s) {
    return startsWith(s, "http://") || startsWith(s, "https://") ||
           startsWith(s, "git@") || startsWith(s, "git://");
}

} // namespace util

// ============================================================================
// Manifest model: nova.toml
//
// Supported shape (deliberately small, TOML-flavored):
//
//   [package]
//   name = "myapp"
//   version = "0.1.0"
//
//   [dependencies]
//   somepkg = "https://github.com/user/somepkg.git"
//   otherpkg = "1.2.0"
// ============================================================================

struct Dependency {
    std::string name;    // logical dependency name
    std::string source;  // URL, git shorthand, or bare version string
};

struct Manifest {
    std::string name = "nova-project";
    std::string version = "0.1.0";
    std::vector<Dependency> dependencies;
};

class ManifestParser {
public:
    // Parses a nova.toml-style file. Throws std::runtime_error on hard
    // failure (missing file); malformed lines are simply skipped with a
    // warning, since this is a lightweight parser by design.
    static Manifest parse(const fs::path& path) {
        std::ifstream in(path);
        if (!in) {
            throw std::runtime_error("cannot open manifest: " + path.string());
        }

        Manifest m;
        std::string line;
        std::string section; // current [section]

        static const std::regex sectionRe(R"(^\[([A-Za-z0-9_\-\.]+)\]$)");
        static const std::regex kvRe(R"(^([A-Za-z0-9_\-\.]+)\s*=\s*(.+)$)");

        while (std::getline(in, line)) {
            std::string trimmed = util::trim(line);
            if (trimmed.empty() || trimmed[0] == '#') continue;

            std::smatch match;
            if (std::regex_match(trimmed, match, sectionRe)) {
                section = match[1].str();
                continue;
            }

            if (std::regex_match(trimmed, match, kvRe)) {
                std::string key = util::trim(match[1].str());
                std::string value = util::stripQuotes(match[2].str());

                if (section == "package") {
                    if (key == "name") m.name = value;
                    else if (key == "version") m.version = value;
                } else if (section == "dependencies") {
                    m.dependencies.push_back(Dependency{key, value});
                }
                // Unknown sections are ignored — forward-compatible by design.
            }
        }
        return m;
    }

    // Serialize a Manifest back to nova.toml text (used by `nupm init`).
    static std::string serialize(const Manifest& m) {
        std::ostringstream out;
        out << "[package]\n";
        out << "name = \"" << m.name << "\"\n";
        out << "version = \"" << m.version << "\"\n";
        out << "\n[dependencies]\n";
        for (const auto& d : m.dependencies) {
            out << d.name << " = \"" << d.source << "\"\n";
        }
        return out.str();
    }
};

// ============================================================================
// Lockfile model: nova.lock
//
// Simple line-based format (no TOML/JSON needed since we own both reader
// and writer):
//
//   name=somepkg
//   source=https://github.com/user/somepkg.git
//   fetched_at=2026-08-27T12:00:00Z
//   ---
// ============================================================================

struct LockEntry {
    std::string name;
    std::string source;
    std::string fetchedAt;
};

class Lockfile {
public:
    static std::vector<LockEntry> load(const fs::path& path) {
        std::vector<LockEntry> entries;
        std::ifstream in(path);
        if (!in) return entries;

        LockEntry current;
        std::string line;
        while (std::getline(in, line)) {
            std::string trimmed = util::trim(line);
            if (trimmed == "---") {
                if (!current.name.empty()) entries.push_back(current);
                current = LockEntry{};
                continue;
            }
            size_t eq = trimmed.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trimmed.substr(0, eq);
            std::string value = trimmed.substr(eq + 1);
            if (key == "name") current.name = value;
            else if (key == "source") current.source = value;
            else if (key == "fetched_at") current.fetchedAt = value;
        }
        if (!current.name.empty()) entries.push_back(current);
        return entries;
    }

    static void save(const fs::path& path, const std::vector<LockEntry>& entries) {
        std::ofstream out(path, std::ios::trunc);
        for (const auto& e : entries) {
            out << "name=" << e.name << "\n";
            out << "source=" << e.source << "\n";
            out << "fetched_at=" << e.fetchedAt << "\n";
            out << "---\n";
        }
    }

    // Upsert: replace an existing entry with the same name, or append.
    static void upsert(std::vector<LockEntry>& entries, const LockEntry& fresh) {
        for (auto& e : entries) {
            if (e.name == fresh.name) {
                e = fresh;
                return;
            }
        }
        entries.push_back(fresh);
    }

    static bool contains(const std::vector<LockEntry>& entries, const std::string& name) {
        return std::any_of(entries.begin(), entries.end(),
                            [&](const LockEntry& e) { return e.name == name; });
    }
};

// ============================================================================
// Workspace paths
// ============================================================================

struct Workspace {
    fs::path projectRoot;      // current working directory (project)
    fs::path novaDir;          // ./.nova
    fs::path localLib;         // ./.nova/lib
    fs::path manifestPath;     // ./nova.toml
    fs::path lockPath;         // ./nova.lock
    fs::path globalCache;      // ~/.nova_cache
    fs::path packageCache;     // ~/.nova_cache/packages

    static Workspace discover() {
        Workspace w;
        w.projectRoot = fs::current_path();
        w.novaDir = w.projectRoot / ".nova";
        w.localLib = w.novaDir / "lib";
        w.manifestPath = w.projectRoot / "nova.toml";
        w.lockPath = w.projectRoot / "nova.lock";

        const char* home = std::getenv("HOME");
        fs::path homeDir = home ? fs::path(home) : fs::path(".");
        w.globalCache = homeDir / ".nova_cache";
        w.packageCache = w.globalCache / "packages";
        return w;
    }

    void ensureDirs() const {
        fs::create_directories(localLib);
        fs::create_directories(packageCache);
    }
};

// ============================================================================
// Fetcher: downloads a package into the global cache
// ============================================================================

class Fetcher {
public:
    explicit Fetcher(const Workspace& ws) : ws(ws) {}

    // Fetches `source` (a git URL, git@ shorthand, or plain tarball URL)
    // into ws.packageCache/<name>. Returns the path to the fetched package
    // directory, or std::nullopt on failure.
    std::optional<fs::path> fetch(const std::string& name, const std::string& source) {
        fs::path dest = ws.packageCache / name;

        if (fs::exists(dest) && !fs::is_empty(dest)) {
            std::cout << "[nupm] using cached package: " << name << "\n";
            return dest;
        }

        fs::create_directories(ws.packageCache);

        bool isGit = util::startsWith(source, "git@") ||
                     util::startsWith(source, "git://") ||
                     source.find(".git") != std::string::npos;

        int rc = -1;
        if (isGit) {
            std::cout << "[nupm] cloning " << source << " -> " << dest.string() << "\n";
            std::string cmd = "git clone --depth 1 " + util::quotePath(source) + " " +
                               util::quotePath(dest);
            rc = util::runCommand(cmd);
        } else if (util::looksLikeUrl(source)) {
            // Treat as a tarball/archive: download then extract.
            fs::create_directories(dest);
            fs::path archive = dest / "download.tar.gz";
            std::cout << "[nupm] downloading " << source << "\n";
            std::string cmd = "curl -L --fail -o " + util::quotePath(archive) + " " +
                               util::quotePath(source);
            rc = util::runCommand(cmd);
            if (rc == 0) {
                std::string extractCmd = "tar -xzf " + util::quotePath(archive) +
                                          " -C " + util::quotePath(dest);
                rc = util::runCommand(extractCmd);
                std::error_code ec;
                fs::remove(archive, ec);
            }
        } else {
            std::cerr << "[nupm] error: don't know how to fetch source '" << source
                      << "' (expected a git URL or http(s) archive)\n";
            return std::nullopt;
        }

        if (rc != 0) {
            std::cerr << "[nupm] error: fetch failed for " << name
                      << " (exit code " << rc << ")\n";
            std::error_code ec;
            fs::remove_all(dest, ec);
            return std::nullopt;
        }

        return dest;
    }

private:
    const Workspace& ws;
};

// ============================================================================
// Builder: detects the native build system in a fetched package and builds
// it, then harvests resulting shared/static libraries into the project's
// local ./.nova/lib workspace.
// ============================================================================

class Builder {
public:
    explicit Builder(const Workspace& ws) : ws(ws) {}

    bool buildAndLink(const fs::path& packageDir) {
        bool didSomething = false;

        if (fs::exists(packageDir / "CMakeLists.txt")) {
            std::cout << "[nupm] detected CMake project in " << packageDir.string() << "\n";
            didSomething |= buildCMake(packageDir);
        }

        if (fs::exists(packageDir / "Cargo.toml")) {
            std::cout << "[nupm] detected Cargo project in " << packageDir.string() << "\n";
            didSomething |= buildCargo(packageDir);
        }

        if (!didSomething) {
            std::cout << "[nupm] no recognized native build system in "
                       << packageDir.string() << " (skipping build step)\n";
        }

        harvestArtifacts(packageDir);
        return true;
    }

private:
    const Workspace& ws;

    bool buildCMake(const fs::path& dir) {
        fs::path buildDir = dir / "build";
        fs::create_directories(buildDir);

        int rc = util::runCommand("cmake -S . -B build", dir);
        if (rc != 0) {
            std::cerr << "[nupm] warning: cmake configure failed in " << dir.string() << "\n";
            return false;
        }
        rc = util::runCommand("cmake --build build -- -j$(nproc 2>/dev/null || echo 2)", dir);
        if (rc != 0) {
            std::cerr << "[nupm] warning: cmake build failed in " << dir.string() << "\n";
            return false;
        }
        return true;
    }

    bool buildCargo(const fs::path& dir) {
        int rc = util::runCommand("cargo build --release", dir);
        if (rc != 0) {
            std::cerr << "[nupm] warning: cargo build failed in " << dir.string() << "\n";
            return false;
        }
        return true;
    }

    // Walk the package directory recursively looking for freshly-built
    // native artifacts (.so, .a, .dylib) and copy them into the project's
    // local .nova/lib folder. We skip common source/vendor dirs to keep
    // this cheap on low-RAM devices.
    void harvestArtifacts(const fs::path& packageDir) {
        static const std::vector<std::string> exts = {".so", ".a", ".dylib"};
        fs::create_directories(ws.localLib);

        std::error_code ec;
        int copied = 0;
        for (auto it = fs::recursive_directory_iterator(
                 packageDir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const fs::directory_entry& entry = *it;

            // Skip vendored source trees / VCS metadata to save time.
            const std::string dirname = entry.path().filename().string();
            if (entry.is_directory() &&
                (dirname == ".git" || dirname == "src" || dirname == "vendor" ||
                 dirname == "node_modules")) {
                it.disable_recursion_pending();
                continue;
            }

            if (!entry.is_regular_file()) continue;

            std::string ext = entry.path().extension().string();
            // Also catch versioned shared objects like libfoo.so.1.2.3
            bool isSharedVersioned = entry.path().string().find(".so.") != std::string::npos;

            if (std::find(exts.begin(), exts.end(), ext) != exts.end() || isSharedVersioned) {
                fs::path target = ws.localLib / entry.path().filename();
                fs::copy_file(entry.path(), target,
                               fs::copy_options::overwrite_existing, ec);
                if (!ec) {
                    std::cout << "[nupm] linked artifact -> " << target.string() << "\n";
                    ++copied;
                } else {
                    ec.clear();
                }
            }
        }

        if (copied == 0) {
            std::cout << "[nupm] no .so/.a/.dylib artifacts found under "
                       << packageDir.string() << "\n";
        }
    }
};

// ============================================================================
// Commands
// ============================================================================

namespace commands {

int init() {
    Workspace ws = Workspace::discover();

    if (fs::exists(ws.manifestPath)) {
        std::cerr << "[nupm] error: nova.toml already exists in this directory\n";
        return 1;
    }

    Manifest m;
    m.name = ws.projectRoot.filename().string();
    if (m.name.empty()) m.name = "nova-project";
    m.version = "0.1.0";

    std::ofstream out(ws.manifestPath);
    out << ManifestParser::serialize(m);
    out.close();

    ws.ensureDirs();

    std::cout << "[nupm] created nova.toml\n";
    std::cout << "[nupm] created " << ws.novaDir.string() << "/\n";
    std::cout << "[nupm] project '" << m.name << "' initialized\n";
    return 0;
}

// Resolves a single dependency: fetch (if not already locked/cached),
// build, link artifacts, and record it in the in-memory lockfile entries.
bool resolveOne(const Workspace& ws, const std::string& name, const std::string& source,
                std::vector<LockEntry>& lockEntries, bool forceRefetch) {
    if (!forceRefetch && Lockfile::contains(lockEntries, name)) {
        std::cout << "[nupm] '" << name << "' already locked — skipping fetch "
                     "(delete nova.lock or pass a fresh version to refetch)\n";
        // Still make sure artifacts are linked into THIS project's .nova/lib,
        // since the lockfile is project-local but the cache is global and
        // a new project may reuse an already-cached package.
        fs::path cached = ws.packageCache / name;
        if (fs::exists(cached)) {
            Builder builder(ws);
            builder.buildAndLink(cached);
        }
        return true;
    }

    Fetcher fetcher(ws);
    auto packageDir = fetcher.fetch(name, source);
    if (!packageDir) {
        return false;
    }

    Builder builder(ws);
    builder.buildAndLink(*packageDir);

    LockEntry entry{name, source, util::nowTimestamp()};
    Lockfile::upsert(lockEntries, entry);
    return true;
}

int install(const std::string& pkgArg) {
    Workspace ws = Workspace::discover();
    ws.ensureDirs();

    std::vector<LockEntry> lockEntries = Lockfile::load(ws.lockPath);

    bool ok = true;

    if (!pkgArg.empty()) {
        // `nupm install <pkg>` — pkgArg may be "name=source" or a bare
        // URL/git path (in which case we derive the name).
        std::string name, source;
        size_t eq = pkgArg.find('=');
        if (eq != std::string::npos) {
            name = pkgArg.substr(0, eq);
            source = pkgArg.substr(eq + 1);
        } else {
            source = pkgArg;
            name = util::deriveName(pkgArg);
        }

        ok = resolveOne(ws, name, source, lockEntries, /*forceRefetch=*/false);

        // Also record it in nova.toml's [dependencies] if it's not there yet,
        // so future `nupm build` / `nupm install` (no args) pick it up.
        if (ok && fs::exists(ws.manifestPath)) {
            Manifest m = ManifestParser::parse(ws.manifestPath);
            bool known = std::any_of(m.dependencies.begin(), m.dependencies.end(),
                                      [&](const Dependency& d) { return d.name == name; });
            if (!known) {
                m.dependencies.push_back(Dependency{name, source});
                std::ofstream out(ws.manifestPath, std::ios::trunc);
                out << ManifestParser::serialize(m);
            }
        }
    } else {
        // `nupm install` with no args — resolve everything in nova.toml.
        if (!fs::exists(ws.manifestPath)) {
            std::cerr << "[nupm] error: no nova.toml found. Run `nupm init` first, "
                         "or pass a package: `nupm install <pkg>`\n";
            return 1;
        }
        Manifest m = ManifestParser::parse(ws.manifestPath);
        if (m.dependencies.empty()) {
            std::cout << "[nupm] no dependencies declared in nova.toml\n";
        }
        for (const auto& dep : m.dependencies) {
            bool result = resolveOne(ws, dep.name, dep.source, lockEntries, false);
            ok = ok && result;
        }
    }

    Lockfile::save(ws.lockPath, lockEntries);
    std::cout << "[nupm] wrote nova.lock\n";

    return ok ? 0 : 1;
}

int build() {
    Workspace ws = Workspace::discover();
    ws.ensureDirs();

    if (!fs::exists(ws.manifestPath)) {
        std::cerr << "[nupm] error: no nova.toml found in this directory\n";
        return 1;
    }

    Manifest m = ManifestParser::parse(ws.manifestPath);
    std::vector<LockEntry> lockEntries = Lockfile::load(ws.lockPath);

    if (m.dependencies.empty()) {
        std::cout << "[nupm] '" << m.name << "' has no dependencies to build\n";
        return 0;
    }

    bool ok = true;
    for (const auto& dep : m.dependencies) {
        fs::path cached = ws.packageCache / dep.name;
        if (!fs::exists(cached)) {
            std::cout << "[nupm] '" << dep.name
                       << "' not yet fetched — fetching now (run `nupm install` "
                          "to persist it in nova.lock)\n";
            ok = resolveOne(ws, dep.name, dep.source, lockEntries, false) && ok;
            continue;
        }
        Builder builder(ws);
        builder.buildAndLink(cached);
        LockEntry entry{dep.name, dep.source, util::nowTimestamp()};
        Lockfile::upsert(lockEntries, entry);
    }

    Lockfile::save(ws.lockPath, lockEntries);
    std::cout << "[nupm] build complete — artifacts in " << ws.localLib.string() << "\n";
    return ok ? 0 : 1;
}

void printUsage() {
    std::cout <<
        "nupm — Nova Universal Package Manager\n\n"
        "Usage:\n"
        "  nupm init                Create a new nova.toml in this directory\n"
        "  nupm install             Fetch + build every dependency in nova.toml\n"
        "  nupm install <pkg>       Fetch + build a single dependency\n"
        "                           <pkg> may be a bare URL/git path, or name=source\n"
        "  nupm build               Build/link dependencies already cached\n";
}

} // namespace commands

// ============================================================================
// Entry point
// ============================================================================

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        commands::printUsage();
        return 1;
    }

    const std::string& cmd = args[0];

    if (cmd == "init") {
        return commands::init();
    }

    if (cmd == "install") {
        std::string pkg = (args.size() >= 2) ? args[1] : "";
        return commands::install(pkg);
    }

    if (cmd == "build") {
        return commands::build();
    }

    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        commands::printUsage();
        return 0;
    }

    std::cerr << "[nupm] unknown command: " << cmd << "\n\n";
    commands::printUsage();
    return 1;
}
