// nova_lsp.cpp — MAP 23 (Toolchain): the Nova Language Server (LSP).
// Pure C++17 stdlib only. No external JSON library — the LSP payloads this
// server needs to read/write are a small, fixed, well-known shape, so a
// hand-rolled minimal JSON reader (just enough to pull out the handful of
// fields LSP handlers below actually need) and a set of string-building
// helpers for responses are genuinely sufficient and simpler to audit than
// pulling in a general-purpose JSON library for this.
//
// Build: clang++ -std=c++17 nova_lsp.cpp -o nova-lsp
// Wire protocol: reads/writes LSP's standard `Content-Length: N\r\n\r\n`-
// framed JSON-RPC 2.0 messages over stdin/stdout — exactly what every LSP
// client (VS Code included) speaks to a stdio-transport language server.

#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace nova_lsp {

// ═══════════════════════════════ minimal JSON value extraction ═══════════════════════════════
//
// Not a general JSON parser — a set of small, targeted extractors over the
// raw message text, each looking for one specific `"key": ...` pattern.
// This is deliberately narrower than a real JSON library because the LSP
// messages this server needs to READ (initialize, didOpen, didChange,
// hover) have a small, predictable set of fields it actually consumes;
// everything else in an incoming message is left untouched (not parsed,
// not validated) rather than guessed at.

// Extracts the string value of "key": "value" — handles \" and \\ escapes
// within the value, which real LSP payloads (file:// URIs, source text)
// do contain.
static std::optional<std::string> extractJsonString(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    std::size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return std::nullopt;

    std::size_t colonPos = json.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) return std::nullopt;

    std::size_t i = colonPos + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || json[i] != '"') return std::nullopt;
    ++i; // skip opening quote

    std::string value;
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\' && i + 1 < json.size()) {
            char next = json[i + 1];
            switch (next) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case '/': value += '/'; break;
                default: value += next; break;
            }
            i += 2;
        } else {
            value += json[i];
            ++i;
        }
    }
    return value;
}

// Extracts a bare numeric value for "key": 123 (used for id, line, character).
static std::optional<long long> extractJsonNumber(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    std::size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return std::nullopt;

    std::size_t colonPos = json.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) return std::nullopt;

    std::size_t i = colonPos + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;

    std::size_t start = i;
    if (i < json.size() && (json[i] == '-' || json[i] == '+')) ++i;
    while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    if (i == start) return std::nullopt;

    try {
        return std::stoll(json.substr(start, i - start));
    } catch (...) {
        return std::nullopt;
    }
}

// Extracts the "id" field, which LSP allows to be either a JSON number or a
// JSON string — returned verbatim as a string ready to be re-embedded
// (quoted or not) in a response, since a response must echo the request id
// back with matching JSON type.
struct JsonId {
    bool present = false;
    bool isString = false;
    std::string raw; // number text, or the unescaped string content
};

static JsonId extractId(const std::string& json) {
    JsonId result;
    std::size_t keyPos = json.find("\"id\"");
    if (keyPos == std::string::npos) return result;
    std::size_t colonPos = json.find(':', keyPos + 4);
    if (colonPos == std::string::npos) return result;

    std::size_t i = colonPos + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size()) return result;

    if (json[i] == '"') {
        if (auto s = extractJsonString(json, "id")) {
            result.present = true;
            result.isString = true;
            result.raw = *s;
        }
    } else {
        if (auto n = extractJsonNumber(json, "id")) {
            result.present = true;
            result.isString = false;
            result.raw = std::to_string(*n);
        }
    }
    return result;
}

// Escapes a string for safe embedding inside a JSON string literal —
// used by every response-building helper below.
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// ═══════════════════════════════ stdio JSON-RPC framing ═══════════════════════════════

// Reads exactly one `Content-Length: N\r\n...\r\n\r\n<N bytes of JSON>`
// message from stdin. Returns std::nullopt on EOF/malformed header — the
// standard LSP stdio framing every real client (VS Code's LSP client
// included) uses.
static std::optional<std::string> readMessage(std::istream& in) {
    long long contentLength = -1;
    std::string headerLine;

    while (std::getline(in, headerLine)) {
        // Strip a trailing \r left over from \r\n line endings.
        if (!headerLine.empty() && headerLine.back() == '\r') headerLine.pop_back();

        if (headerLine.empty()) break; // blank line = end of headers

        static const std::string prefix = "Content-Length:";
        if (headerLine.compare(0, prefix.size(), prefix) == 0) {
            std::string valueStr = headerLine.substr(prefix.size());
            std::size_t firstDigit = valueStr.find_first_not_of(" \t");
            if (firstDigit != std::string::npos) {
                try { contentLength = std::stoll(valueStr.substr(firstDigit)); }
                catch (...) { return std::nullopt; }
            }
        }
        // Any other header (e.g. Content-Type) is read and ignored —
        // real LSP clients may send it; this server doesn't need it.
    }

    if (contentLength < 0) return std::nullopt; // EOF before a valid header, or missing Content-Length

    std::string body(static_cast<std::size_t>(contentLength), '\0');
    in.read(&body[0], contentLength);
    if (in.gcount() != contentLength) return std::nullopt; // stream closed mid-body

    return body;
}

// Writes one JSON-RPC message with the correct Content-Length framing.
static void writeMessage(std::ostream& out, const std::string& jsonBody) {
    out << "Content-Length: " << jsonBody.size() << "\r\n\r\n" << jsonBody;
    out.flush();
}

// ═══════════════════════════════ document store ═══════════════════════════════

static std::unordered_map<std::string, std::string> g_openDocuments; // uri -> full text

// ═══════════════════════════════ diagnostics: quick lint simulation ═══════════════════════════════
//
// A deliberately small subset of nova_cli.cpp's lint logic (over-100-char
// lines, trailing whitespace, unused imports) — "a quick simulation" per
// the spec, re-implemented directly against in-memory document text
// (rather than shelling out to `nova lint` on every keystroke, which would
// be far too slow for an editor's live-diagnostics use case).

struct Diagnostic {
    int line;       // 0-based, per LSP's Position spec
    int startChar;
    int endChar;
    int severity;   // 1 = Error, 2 = Warning, 3 = Information, 4 = Hint (LSP DiagnosticSeverity)
    std::string message;
};

static std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') { lines.push_back(current); current.clear(); }
        else if (c != '\r') { current += c; }
    }
    lines.push_back(current);
    return lines;
}

static std::vector<Diagnostic> computeDiagnostics(const std::string& text) {
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> lines = splitLines(text);

    std::vector<std::pair<int, std::string>> imports; // (0-based line, dotted name)
    std::string restOfFile;
    static const std::regex importRe(R"(^\s*import\s+([A-Za-z_][A-Za-z0-9_.]*))");
    static const std::regex trailingWsRe(R"([ \t]+$)");

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        int lineNo = static_cast<int>(i);

        if (line.size() > 100) {
            diagnostics.push_back({lineNo, 100, static_cast<int>(line.size()), 2,
                "line exceeds 100 characters (" + std::to_string(line.size()) + ")"});
        }

        std::smatch wsMatch;
        if (std::regex_search(line, wsMatch, trailingWsRe)) {
            int start = static_cast<int>(line.size() - wsMatch.length());
            diagnostics.push_back({lineNo, start, static_cast<int>(line.size()), 2, "trailing whitespace"});
        }

        std::smatch m;
        if (std::regex_search(line, m, importRe)) {
            imports.push_back({lineNo, m[1].str()});
        } else {
            restOfFile += line;
            restOfFile += "\n";
        }
    }

    for (const auto& [lineNo, importedName] : imports) {
        if (restOfFile.find(importedName) == std::string::npos) {
            int lineLen = lineNo < static_cast<int>(lines.size()) ? static_cast<int>(lines[lineNo].size()) : 0;
            diagnostics.push_back({lineNo, 0, lineLen, 2, "unused import '" + importedName + "'"});
        }
    }

    return diagnostics;
}

static std::string buildDiagnosticJson(const Diagnostic& d) {
    std::ostringstream os;
    os << "{"
       << "\"range\":{"
           << "\"start\":{\"line\":" << d.line << ",\"character\":" << d.startChar << "},"
           << "\"end\":{\"line\":" << d.line << ",\"character\":" << d.endChar << "}"
       << "},"
       << "\"severity\":" << d.severity << ","
       << "\"source\":\"nova-lsp\","
       << "\"message\":\"" << jsonEscape(d.message) << "\""
       << "}";
    return os.str();
}

static void publishDiagnostics(std::ostream& out, const std::string& uri, const std::string& text) {
    std::vector<Diagnostic> diagnostics = computeDiagnostics(text);

    std::ostringstream diagArray;
    diagArray << "[";
    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        diagArray << buildDiagnosticJson(diagnostics[i]);
        if (i + 1 < diagnostics.size()) diagArray << ",";
    }
    diagArray << "]";

    std::ostringstream notification;
    notification << "{"
        << "\"jsonrpc\":\"2.0\","
        << "\"method\":\"textDocument/publishDiagnostics\","
        << "\"params\":{"
            << "\"uri\":\"" << jsonEscape(uri) << "\","
            << "\"diagnostics\":" << diagArray.str()
        << "}"
        << "}";

    writeMessage(out, notification.str());
}

// ═══════════════════════════════ LSP request handlers ═══════════════════════════════

static void sendResponse(std::ostream& out, const JsonId& id, const std::string& resultJson) {
    std::ostringstream os;
    os << "{\"jsonrpc\":\"2.0\",\"id\":" << (id.isString ? ("\"" + jsonEscape(id.raw) + "\"") : id.raw)
       << ",\"result\":" << resultJson << "}";
    writeMessage(out, os.str());
}

static void handleInitialize(std::ostream& out, const JsonId& id) {
    // Advertises: TextDocumentSyncKind.Full (1) and hoverProvider = true —
    // exactly the two capabilities this first pass actually implements.
    // Advertising more than is implemented would make the client believe
    // requests it sends (e.g. textDocument/completion) will be answered,
    // when they'd just go unhandled — so the capability set here is kept
    // honest and matches this file's actual handler set below.
    std::string result =
        "{"
          "\"capabilities\":{"
            "\"textDocumentSync\":1,"
            "\"hoverProvider\":true"
          "},"
          "\"serverInfo\":{\"name\":\"nova-lsp\",\"version\":\"0.1.0\"}"
        "}";
    sendResponse(out, id, result);
}

static void handleDidOpen(std::ostream& out, const std::string& body) {
    // textDocument/didOpen params shape:
    //   { "textDocument": { "uri": "...", "languageId": "nova", "version": 1, "text": "..." } }
    // extractJsonString scans the WHOLE message body for the first
    // matching "key": "value" — safe here because "uri" and "text" each
    // appear exactly once per didOpen notification.
    auto uri = extractJsonString(body, "uri");
    auto text = extractJsonString(body, "text");
    if (!uri || !text) return;

    g_openDocuments[*uri] = *text;
    publishDiagnostics(out, *uri, *text);
}

static void handleDidChange(std::ostream& out, const std::string& body) {
    // textDocument/didChange with TextDocumentSyncKind.Full (what this
    // server advertised in `initialize`) sends the ENTIRE new document
    // text in contentChanges[0].text — so, unlike incremental sync, no
    // range-based patching is needed here, just replace the stored text.
    auto uri = extractJsonString(body, "uri");
    if (!uri) return;

    // "text" appears both in textDocument (absent for didChange) and in
    // contentChanges[0] — for Full sync there is exactly one
    // contentChanges entry and its "text" is the only "text" field
    // present in a didChange body, so the generic extractor is safe here.
    auto text = extractJsonString(body, "text");
    if (!text) return;

    g_openDocuments[*uri] = *text;
    publishDiagnostics(out, *uri, *text);
}

static void handleDidClose(const std::string& body) {
    auto uri = extractJsonString(body, "uri");
    if (uri) g_openDocuments.erase(*uri);
}

// Returns the word at the given 0-based (line, character) position in
// `text`, using the same identifier-character rule Nova's own Lexer uses
// (alnum + underscore) — a real, if simple, "what's under the cursor"
// lookup, not a fixed placeholder location.
static std::string wordAtPosition(const std::string& text, int line, int character) {
    std::vector<std::string> lines = splitLines(text);
    if (line < 0 || line >= static_cast<int>(lines.size())) return "";
    const std::string& lineText = lines[static_cast<std::size_t>(line)];
    if (character < 0 || character > static_cast<int>(lineText.size())) return "";

    auto isWordChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };

    int start = character;
    while (start > 0 && isWordChar(lineText[static_cast<std::size_t>(start - 1)])) --start;
    int end = character;
    while (end < static_cast<int>(lineText.size()) && isWordChar(lineText[static_cast<std::size_t>(end)])) ++end;

    if (start >= end) return "";
    return lineText.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
}

static void handleHover(std::ostream& out, const JsonId& id, const std::string& body) {
    auto uri = extractJsonString(body, "uri");
    auto lineOpt = extractJsonNumber(body, "line");
    auto charOpt = extractJsonNumber(body, "character");

    std::string hoverText = "Nova Native Element"; // the placeholder tooltip the spec asked for

    if (uri && lineOpt && charOpt) {
        auto docIt = g_openDocuments.find(*uri);
        if (docIt != g_openDocuments.end()) {
            std::string word = wordAtPosition(docIt->second, static_cast<int>(*lineOpt), static_cast<int>(*charOpt));
            if (!word.empty()) {
                // A real (if simple) enrichment over the bare placeholder:
                // include the actual identifier under the cursor, so the
                // hover tooltip is at least connected to real cursor
                // position/content rather than being a totally static
                // string regardless of what's hovered — while still being
                // the "basic placeholder tooltip for testing the
                // connection" the spec asked for, not a real type-aware
                // hover (that needs the SemanticAnalyzer/TypeChecker wired
                // in, a real, separate next step).
                hoverText = "Nova Native Element: `" + word + "`";
            }
        }
    }

    std::string result =
        "{\"contents\":{\"kind\":\"markdown\",\"value\":\"" + jsonEscape(hoverText) + "\"}}";
    sendResponse(out, id, result);
}

static void sendNullResponse(std::ostream& out, const JsonId& id) {
    sendResponse(out, id, "null");
}

// ═══════════════════════════════ method dispatch ═══════════════════════════════

static std::optional<std::string> extractMethod(const std::string& body) {
    return extractJsonString(body, "method");
}

static bool runOneMessage(std::istream& in, std::ostream& out, bool& shouldExit) {
    std::optional<std::string> body = readMessage(in);
    if (!body) return false; // EOF or malformed framing — caller stops the loop

    std::optional<std::string> method = extractMethod(*body);
    JsonId id = extractId(*body);

    if (!method) {
        // A response TO us (this server never sends requests that expect
        // a response in this first pass, but a well-behaved reader still
        // shouldn't crash on an unexpected shape) — ignore.
        return true;
    }

    if (*method == "initialize") {
        handleInitialize(out, id);
    } else if (*method == "initialized") {
        // Notification, no response required.
    } else if (*method == "textDocument/didOpen") {
        handleDidOpen(out, *body);
    } else if (*method == "textDocument/didChange") {
        handleDidChange(out, *body);
    } else if (*method == "textDocument/didClose") {
        handleDidClose(*body);
    } else if (*method == "textDocument/hover") {
        handleHover(out, id, *body);
    } else if (*method == "shutdown") {
        sendNullResponse(out, id);
    } else if (*method == "exit") {
        shouldExit = true;
    } else {
        // Unknown method: if it carried an id, LSP requires SOME response
        // (a MethodNotFound error) rather than silence, so the client
        // doesn't hang waiting; a bare notification (no id) is just
        // ignored, which is the correct behavior for an unrecognized
        // notification per the LSP spec.
        if (id.present) {
            std::ostringstream os;
            os << "{\"jsonrpc\":\"2.0\",\"id\":" << (id.isString ? ("\"" + jsonEscape(id.raw) + "\"") : id.raw)
               << ",\"error\":{\"code\":-32601,\"message\":\"method not found: " << jsonEscape(*method) << "\"}}";
            writeMessage(out, os.str());
        }
    }

    return true;
}

} // namespace nova_lsp

int main() {
    // LSP stdio transport is binary-safe framed text but line-based header
    // parsing depends on standard \n handling — std::ios::sync_with_stdio
    // is left at its default (synced) since this server has no threading
    // and no performance-critical high-throughput path; correctness of
    // framing matters far more here than raw I/O speed.
    using namespace nova_lsp;

    bool shouldExit = false;
    while (!shouldExit) {
        if (!runOneMessage(std::cin, std::cout, shouldExit)) break;
    }
    return 0;
}
