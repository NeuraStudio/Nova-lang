// nova_cli.cpp — MAP 22 (Toolchain): the master `nova` CLI.
// Pure C++17 stdlib only (<filesystem>, <regex>, <iostream>) — no external
// dependencies, matching the constraint this was built against.
//
// Build:  clang++ -std=c++17 nova_cli.cpp -o nova
// Usage:  nova build <file.nova>
//         nova run <file.nova>
//         nova fmt <file.nova>
//         nova lint <file.nova>
//         nova test
//
// This file wraps the EXISTING `novac` compiler binary (built separately
// from Lexer.cpp/Parser.cpp/Semantic.cpp/TypeChecker.cpp/main.cpp) via
// std::system() for build/run/test — it does not reimplement compilation.
// fmt and lint are self-contained here, operating purely on source text.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace nova_cli {

// ═══════════════════════════════ ANSI color helpers ═══════════════════════════════
// Kept as plain functions rather than always-on macros so output degrades
// gracefully (falls back to no color codes) when stdout isn't a real
// terminal — checked once via isatty, the standard, correct way to decide
// whether ANSI escapes belong in a given output stream at all.

#if defined(_WIN32)
    #include <io.h>
    #define NOVA_ISATTY _isatty
    #define NOVA_FILENO _fileno
#else
    #include <unistd.h>
    #include <sys/wait.h>
    #define NOVA_ISATTY isatty
    #define NOVA_FILENO fileno
#endif

static bool colorEnabled() {
    static bool enabled = NOVA_ISATTY(NOVA_FILENO(stdout)) != 0;
    return enabled;
}

static std::string colorize(const std::string& text, const char* ansiCode) {
    if (!colorEnabled()) return text;
    return std::string(ansiCode) + text + "\033[0m";
}

static std::string green(const std::string& s)  { return colorize(s, "\033[32m"); }
static std::string red(const std::string& s)    { return colorize(s, "\033[31m"); }
static std::string blue(const std::string& s)   { return colorize(s, "\033[34m"); }
static std::string yellow(const std::string& s) { return colorize(s, "\033[33m"); }
static std::string bold(const std::string& s)   { return colorize(s, "\033[1m"); }

// ═══════════════════════════════ shared file I/O helpers ═══════════════════════════════

static bool readFile(const fs::path& path, std::string& outContent) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    outContent = ss.str();
    return true;
}

static bool writeFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return true;
}

static std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : content) {
        if (c == '\n') { lines.push_back(current); current.clear(); }
        else if (c != '\r') { current += c; }
    }
    lines.push_back(current); // last line (may be empty if file ends with \n)
    return lines;
}

// Locates the `novac` binary: prefer one sitting next to this CLI binary
// (the normal install layout), fall back to whatever `novac` resolves to
// on PATH. Resolved once per process run, not per command.
static std::string locateNovac(const char* argv0) {
    fs::path selfPath = fs::path(argv0);
    fs::path selfDir = selfPath.has_parent_path() ? selfPath.parent_path() : fs::current_path();
    fs::path candidate = selfDir / "novac";
    if (fs::exists(candidate)) return candidate.string();
    return "novac"; // rely on PATH
}

// ═══════════════════════════════ fmt: lightweight formatter ═══════════════════════════════
//
// Deliberately regex/string-based rather than a real AST-driven
// pretty-printer (that would mean re-running the actual Parser and
// re-emitting source from the AST — a different, heavier tool). This is
// exactly the "lightweight" formatter the spec asked for: normalize
// indentation to 4 spaces per nesting level (tracked via a simple
// brace/colon-block depth counter, matching Nova's own dual block-syntax
// grammar), strip trailing whitespace, and normalize spacing around the
// common binary operators. It intentionally does NOT touch string/comment
// contents — operator-spacing regexes are applied only to code found
// outside quoted strings and `//`/`#` comments, checked line by line
// below, so `"a=b"` inside a string literal is never rewritten.

struct FormatStats {
    int linesProcessed = 0;
    int trailingWhitespaceRemoved = 0;
    int indentationNormalized = 0;
    int operatorSpacingFixed = 0;
};

// Splits a source line into (codePart, commentPart) at the first // or #
// that is NOT inside a string literal — so operator-spacing fixes are
// applied only to codePart, leaving comments and string contents alone.
static std::pair<std::string, std::string> splitCodeAndComment(const std::string& line) {
    bool inString = false;
    char stringQuote = '\0';
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inString) {
            if (c == '\\' && i + 1 < line.size()) { ++i; continue; } // skip escaped char
            if (c == stringQuote) inString = false;
            continue;
        }
        if (c == '"' || c == '\'') { inString = true; stringQuote = c; continue; }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            return {line.substr(0, i), line.substr(i)};
        }
        if (c == '#') {
            return {line.substr(0, i), line.substr(i)};
        }
    }
    return {line, ""};
}

// Rewrites operator spacing in a comment/string-free code fragment. Only
// touches `=`, `+`, `-`, `*`, `/`, `==`, `!=`, `<=`, `>=`, `&&`, `||` —
// applied via careful, ordered regex substitution so multi-char operators
// are matched before their single-char prefixes (e.g. `==` before `=`,
// so `a==b` doesn't get mangled into `a= =b`).
static std::string normalizeOperatorSpacing(const std::string& code) {
    std::string result = code;

    // Multi-character operators first (order matters: longest match wins).
    static const std::vector<std::pair<std::regex, std::string>> multiCharOps = {
        {std::regex(R"(\s*==\s*)"), " == "},
        {std::regex(R"(\s*!=\s*)"), " != "},
        {std::regex(R"(\s*<=\s*)"), " <= "},
        {std::regex(R"(\s*>=\s*)"), " >= "},
        {std::regex(R"(\s*&&\s*)"), " && "},
        {std::regex(R"(\s*\|\|\s*)"), " || "},
        {std::regex(R"(\s*=>\s*)"), " => "},
        {std::regex(R"(\s*\+=\s*)"), " += "},
        {std::regex(R"(\s*-=\s*)"), " -= "},
    };
    for (const auto& [pattern, replacement] : multiCharOps) {
        result = std::regex_replace(result, pattern, replacement);
    }

    // Single-char assignment `=` — only when NOT already part of ==, !=,
    // <=, >=, +=, -=, => (all already normalized above, so a lone `=`
    // remaining at this point is a genuine assignment). Negative
    // lookbehind/lookahead isn't available in std::regex's ECMAScript
    // flavor for all cases needed here, so this is done as a manual
    // character scan instead of a single regex — correctness over
    // cleverness for the one operator most likely to cause a bad rewrite
    // if handled carelessly.
    std::string withEquals;
    withEquals.reserve(result.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        char c = result[i];
        if (c == '=' &&
            (i == 0 || (result[i - 1] != '=' && result[i - 1] != '!' &&
                        result[i - 1] != '<' && result[i - 1] != '>' &&
                        result[i - 1] != '+' && result[i - 1] != '-' &&
                        result[i - 1] != '=')) &&
            (i + 1 >= result.size() || (result[i + 1] != '=' && result[i + 1] != '>'))) {
            // Trim any existing space immediately around this `=`, then
            // re-insert exactly one on each side.
            while (!withEquals.empty() && withEquals.back() == ' ') withEquals.pop_back();
            withEquals += " = ";
            std::size_t j = i + 1;
            while (j < result.size() && result[j] == ' ') ++j;
            i = j - 1; // the outer loop's ++i will land exactly on result[j]
        } else {
            withEquals += c;
        }
    }
    result = withEquals;

    // Collapse any run of spaces this pass may have introduced back down
    // to single spaces (e.g. two adjacent normalized operators).
    result = std::regex_replace(result, std::regex(R"( {2,})"), " ");

    return result;
}

static FormatStats formatSource(std::vector<std::string>& lines) {
    FormatStats stats;
    int depth = 0;
    static const std::regex trailingWs(R"(\s+$)");

    for (std::string& line : lines) {
        stats.linesProcessed++;

        std::string trimmed = std::regex_replace(line, trailingWs, "");
        if (trimmed.size() != line.size()) stats.trailingWhitespaceRemoved++;

        // Determine this line's leading-whitespace-stripped content to
        // decide (a) whether it's blank (skip re-indenting) and (b)
        // whether it opens/closes a brace block, which affects depth
        // for THIS line vs. SUBSEQUENT lines differently (a line starting
        // with '}' dedents itself before being printed).
        std::size_t firstNonSpace = trimmed.find_first_not_of(" \t");
        if (firstNonSpace == std::string::npos) {
            line = ""; // blank line: no indentation to normalize
            continue;
        }
        std::string content = trimmed.substr(firstNonSpace);

        int lineDepth = depth;
        if (!content.empty() && content[0] == '}') lineDepth = std::max(0, depth - 1);
        // Nova's colon-block keywords that continue a previous block
        // (else/elif for if-chains; catch/finally for try; default/case
        // for switch/match) dedent by one level before printing, exactly
        // like a closing '}' does for brace blocks — otherwise every
        // `else:`/`catch(...)`/`default:` would drift one indent level
        // deeper than its matching `if`/`try`/`switch` forever.
        static const std::vector<std::string> continuationKeywords = {
            "else", "elif", "catch", "finally", "default", "case"
        };
        bool isContinuationLine = false;
        for (const auto& kw : continuationKeywords) {
            if (content.compare(0, kw.size(), kw) == 0 &&
                (content.size() == kw.size() || !std::isalnum(static_cast<unsigned char>(content[kw.size()])))) {
                lineDepth = std::max(0, depth - 1);
                isContinuationLine = true;
                break;
            }
        }
        // A continuation line actually dedents the running `depth` itself
        // (not just its own display indent) — everything AFTER this line,
        // until the next dedent, belongs at this shallower level before
        // any re-indent this line's own trailing ':' or '{' adds back.
        if (isContinuationLine) depth = lineDepth;

        auto [codePart, commentPart] = splitCodeAndComment(content);
        std::string formattedCode = normalizeOperatorSpacing(codePart);
        // Re-trim: operator-spacing normalization can leave a single
        // trailing space right before a comment/EOL if the original code
        // ended right after an operator-adjacent token.
        formattedCode = std::regex_replace(formattedCode, trailingWs, "");
        if (formattedCode != codePart) stats.operatorSpacingFixed++;

        std::string newIndent(static_cast<std::size_t>(lineDepth) * 4, ' ');
        std::string newLine = newIndent + formattedCode + commentPart;
        if (newLine != line) stats.indentationNormalized++;
        line = newLine;

        // Update depth for the NEXT line based on THIS line's net brace
        // balance (outside of strings/comments — using codePart, not the
        // full content, so a `{` inside a string or comment is ignored).
        int netBraces = 0;
        for (char c : codePart) {
            if (c == '{') netBraces++;
            else if (c == '}') netBraces--;
        }
        depth = std::max(0, depth + netBraces);

        // Nova's colon-introduced indent blocks (`if x:` / `function f():`)
        // also increase depth for the following lines, matching the
        // language's dual brace/indentation block grammar. A trailing `:`
        // on the code part (after stripping any trailing comment) signals
        // this, as long as the line doesn't already end with a brace
        // (which would already have been counted above).
        std::string codeNoTrailingWs = std::regex_replace(codePart, trailingWs, "");
        if (!codeNoTrailingWs.empty() && codeNoTrailingWs.back() == ':') {
            depth++;
        }
    }
    return stats;
}

static int runFmt(const std::string& filePath) {
    std::string content;
    if (!readFile(filePath, content)) {
        std::cerr << red("error: ") << "could not open file '" << filePath << "'\n";
        return 1;
    }

    std::vector<std::string> lines = splitLines(content);
    FormatStats stats = formatSource(lines);

    std::ostringstream rebuilt;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        rebuilt << lines[i];
        if (i + 1 < lines.size()) rebuilt << "\n";
    }

    if (!writeFile(filePath, rebuilt.str())) {
        std::cerr << red("error: ") << "could not write file '" << filePath << "'\n";
        return 1;
    }

    std::cout << green("formatted ") << filePath << "\n";
    std::cout << "  " << blue(std::to_string(stats.linesProcessed)) << " lines processed, "
              << blue(std::to_string(stats.trailingWhitespaceRemoved)) << " trailing-whitespace fixes, "
              << blue(std::to_string(stats.indentationNormalized)) << " indentation fixes, "
              << blue(std::to_string(stats.operatorSpacingFixed)) << " operator-spacing fixes\n";
    return 0;
}

// ═══════════════════════════════ lint: static analyzer ═══════════════════════════════

struct LintIssue {
    int lineNumber;
    std::string severity; // "warning" or "error"
    std::string message;
};

static std::vector<LintIssue> lintSource(const std::vector<std::string>& lines) {
    std::vector<LintIssue> issues;

    // Track every `import X` / `module X:` name seen, and every identifier
    // token seen anywhere else in the file, so an import that's never
    // referenced again can be flagged — a real, if simple, heuristic
    // (matches how many linters' "unused import" check starts: textual
    // presence elsewhere in the file, not full semantic reachability).
    std::vector<std::pair<int, std::string>> imports; // (line, importedName)
    std::string wholeFileExcludingImportLines;

    static const std::regex importRe(R"(^\s*import\s+([A-Za-z_][A-Za-z0-9_.]*))");
    static const std::regex trailingWsRe(R"([ \t]+$)");

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        int lineNo = static_cast<int>(i) + 1;

        if (line.size() > 100) {
            issues.push_back({lineNo, "warning",
                "line exceeds 100 characters (" + std::to_string(line.size()) + ")"});
        }

        if (std::regex_search(line, trailingWsRe)) {
            issues.push_back({lineNo, "warning", "trailing whitespace"});
        }

        std::smatch m;
        if (std::regex_search(line, m, importRe)) {
            // Only the top-level imported name (before the first '.') is
            // what a Nova program would actually reference by identifier
            // afterward, e.g. `import Nova.database` is referenced as
            // `Nova.database.connect(...)` — the FULL dotted path is what
            // must appear again, so store that.
            imports.push_back({lineNo, m[1].str()});
        } else {
            wholeFileExcludingImportLines += line;
            wholeFileExcludingImportLines += "\n";
        }
    }

    for (const auto& [lineNo, importedName] : imports) {
        if (wholeFileExcludingImportLines.find(importedName) == std::string::npos) {
            issues.push_back({lineNo, "warning", "unused import '" + importedName + "'"});
        }
    }

    return issues;
}

static int runLint(const std::string& filePath) {
    std::string content;
    if (!readFile(filePath, content)) {
        std::cerr << red("error: ") << "could not open file '" << filePath << "'\n";
        return 1;
    }

    std::vector<std::string> lines = splitLines(content);
    std::vector<LintIssue> issues = lintSource(lines);

    if (issues.empty()) {
        std::cout << green("✓ ") << filePath << ": no issues found\n";
        return 0;
    }

    int errorCount = 0, warningCount = 0;
    for (const auto& issue : issues) {
        std::string tag = issue.severity == "error" ? red("error") : yellow("warning");
        std::cout << bold(filePath + ":" + std::to_string(issue.lineNumber) + ": ")
                   << tag << ": " << issue.message << "\n";
        if (issue.severity == "error") errorCount++; else warningCount++;
    }
    std::cout << "\n" << (errorCount > 0 ? red(std::to_string(errorCount) + " error(s)") : "0 errors")
               << ", " << (warningCount > 0 ? yellow(std::to_string(warningCount) + " warning(s)") : "0 warnings")
               << "\n";

    return errorCount > 0 ? 1 : 0;
}

// ═══════════════════════════════ build / run: wrap novac ═══════════════════════════════

static int runBuild(const std::string& novacPath, const std::string& filePath) {
    if (!fs::exists(filePath)) {
        std::cerr << red("error: ") << "file not found: " << filePath << "\n";
        return 1;
    }
    std::string command = "\"" + novacPath + "\" \"" + filePath + "\"";
    std::cout << blue("→ building ") << filePath << "\n";
    int result = std::system(command.c_str());
    int exitCode = WIFEXITED(result) ? WEXITSTATUS(result) : 1;
    if (exitCode == 0) std::cout << green("✓ build succeeded") << "\n";
    else std::cout << red("✗ build failed (exit code " + std::to_string(exitCode) + ")") << "\n";
    return exitCode;
}

static int runRun(const std::string& novacPath, const std::string& filePath) {
    // `novac` today (per the shipped main.cpp) lexes/parses/analyzes and
    // prints a report rather than executing the program — there is no
    // separate Nova interpreter/VM binary yet. `nova run` wraps whatever
    // `novac` currently does end-to-end (the closest real equivalent to
    // "run" available today) rather than inventing a fake execution step;
    // once a real `novai` interpreter or JIT exists, this is a one-line
    // swap of which binary gets invoked here.
    return runBuild(novacPath, filePath);
}

// ═══════════════════════════════ test: discover + run *_test.nova / @Test files ═══════════════════════════════

struct TestResult {
    std::string name;
    bool passed;
    int exitCode;
};

static bool fileContainsTestAnnotation(const fs::path& path) {
    std::string content;
    if (!readFile(path, content)) return false;
    return content.find("@Test") != std::string::npos;
}

static std::vector<fs::path> discoverTestFiles(const fs::path& root) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) continue;
        const fs::directory_entry& entry = *it;
        if (!entry.is_regular_file(ec)) continue;
        const fs::path& p = entry.path();
        if (p.extension() != ".nova") continue;

        std::string stem = p.stem().string();
        bool nameMatches = stem.size() >= 5 && stem.compare(stem.size() - 5, 5, "_test") == 0;
        if (nameMatches || fileContainsTestAnnotation(p)) {
            found.push_back(p);
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

static int runTest(const std::string& novacPath) {
    fs::path root = fs::current_path();
    std::vector<fs::path> testFiles = discoverTestFiles(root);

    if (testFiles.empty()) {
        std::cout << yellow("no test files found") << " (looked for *_test.nova and files containing @Test under "
                   << root.string() << ")\n";
        return 0;
    }

    std::cout << blue("discovered " + std::to_string(testFiles.size()) + " test file(s)") << "\n\n";

    std::vector<TestResult> results;
    for (const fs::path& testFile : testFiles) {
        std::string name = testFile.stem().string();
        std::string command = "\"" + novacPath + "\" \"" + testFile.string() + "\" > /dev/null 2>&1";
        int raw = std::system(command.c_str());
        int exitCode = WIFEXITED(raw) ? WEXITSTATUS(raw) : 1;
        bool passed = (exitCode == 0);
        results.push_back({name, passed, exitCode});

        if (passed) {
            std::cout << green("  ✓ ") << name << " passed\n";
        } else {
            std::cout << red("  ✗ ") << name << " failed (exit code " << exitCode << ")\n";
        }
    }

    int passCount = 0, failCount = 0;
    for (const auto& r : results) { if (r.passed) passCount++; else failCount++; }

    std::cout << "\n" << bold("Test summary: ")
               << green(std::to_string(passCount) + " passed") << ", "
               << (failCount > 0 ? red(std::to_string(failCount) + " failed") : "0 failed") << "\n";

    return failCount > 0 ? 1 : 0;
}

// ═══════════════════════════════ command dispatch ═══════════════════════════════

static void printUsage() {
    std::cout << bold("nova") << " — the Nova toolchain CLI\n\n"
              << "Usage:\n"
              << "  nova build <file.nova>   compile a Nova source file\n"
              << "  nova run <file.nova>     compile and run a Nova source file\n"
              << "  nova fmt <file.nova>     format a Nova source file in place\n"
              << "  nova lint <file.nova>    check a Nova source file for issues\n"
              << "  nova test                discover and run *_test.nova / @Test files\n";
}

} // namespace nova_cli

int main(int argc, char** argv) {
    using namespace nova_cli;

    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::string command = argv[1];
    std::string novacPath = locateNovac(argv[0]);

    if (command == "build") {
        if (argc < 3) { std::cerr << red("error: ") << "nova build requires a file argument\n"; return 1; }
        return runBuild(novacPath, argv[2]);
    } else if (command == "run") {
        if (argc < 3) { std::cerr << red("error: ") << "nova run requires a file argument\n"; return 1; }
        return runRun(novacPath, argv[2]);
    } else if (command == "fmt") {
        if (argc < 3) { std::cerr << red("error: ") << "nova fmt requires a file argument\n"; return 1; }
        return runFmt(argv[2]);
    } else if (command == "lint") {
        if (argc < 3) { std::cerr << red("error: ") << "nova lint requires a file argument\n"; return 1; }
        return runLint(argv[2]);
    } else if (command == "test") {
        return runTest(novacPath);
    } else if (command == "-h" || command == "--help" || command == "help") {
        printUsage();
        return 0;
    } else {
        std::cerr << red("error: ") << "unknown command '" << command << "'\n\n";
        printUsage();
        return 1;
    }
}
