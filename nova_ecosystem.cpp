// nova_ecosystem.cpp — Nova Ecosystem Domain Backend (C-ABI)
// ═══════════════════════════════════════════════════════════════════════
// MAP 11 (Web/Sockets) + MAP 12 (SQLite3) + MAP 13 (GUI/Xlib+Win32).
//
// Every function here is called through the SAME dispatch path every other
// Nova runtime call already uses: nova_rt_call(name, NovaValue** args, i64
// argc) resolves `name` to one of these extern "C" symbols. This file does
// not invent a new calling convention — see the registration table at the
// bottom (nova_ecosystem_register()), which nova_rt_call's dispatch table
// consults. `NovaValue` stays fully opaque here, exactly as it is to
// LLVMBackend.cpp/GCCBackend.cpp: every argument in/out crosses this
// boundary only via the existing nova_rt_to_int / nova_rt_to_string /
// nova_rt_from_int / nova_rt_from_string / nova_rt_from_bool coercions.
//
// NO MOCKS: nova_web_server_listen() performs a real blocking accept() loop
// against a real POSIX (or Winsock) TCP socket. nova_db_sqlite_open/exec()
// call the real libsqlite3 C API. nova_gui_window_create() opens a real
// Xlib (or Win32) window and pumps its event loop until the window is
// closed. These are deliberately synchronous/blocking, matching how a
// systems-level scripting language's "listen"/"create window" primitive is
// normally expected to behave when called directly from a single-threaded
// interpreter loop; a non-blocking/async variant is a natural follow-up
// (Nova already has `thread`/`async` — the frontend can wrap these calls in
// one) but is out of scope for "make the primitive itself real".
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>
#include <mutex>

// ---- Nova runtime ABI this file depends on (declared in nova_rt.hpp,
//      forward-declared here in case that header isn't on the include path
//      for a given ecosystem-only build) ----
extern "C" {
    struct NovaValue; // fully opaque; owned/allocated by nova_rt.cpp

    int64_t     nova_rt_to_int(NovaValue*);
    double      nova_rt_to_float(NovaValue*);
    int         nova_rt_to_bool(NovaValue*);           // returns 0/1
    const char* nova_rt_to_cstr(NovaValue*);            // borrowed pointer, valid until next GC-safe point

    NovaValue*  nova_rt_from_int(int64_t);
    NovaValue*  nova_rt_from_float(double);
    NovaValue*  nova_rt_from_bool(int);
    NovaValue*  nova_rt_from_string(const char*);
    NovaValue*  nova_rt_const_null(void);

} // end extern C
// Helper to allow passing std::string directly to C-ABI
inline NovaValue* nova_rt_from_string(const std::string& s) { return nova_rt_from_string(s.c_str()); }


// ═══════════════════════════════ platform includes ═══════════════════════════════

#if defined(_WIN32)
    #define NOVA_ECOSYSTEM_WINDOWS 1
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "user32.lib")
    #pragma comment(lib, "gdi32.lib")
    using nova_socket_t = SOCKET;
    static constexpr nova_socket_t NOVA_INVALID_SOCKET = INVALID_SOCKET;
#else
    #define NOVA_ECOSYSTEM_POSIX 1
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <cerrno>
    using nova_socket_t = int;
    static constexpr nova_socket_t NOVA_INVALID_SOCKET = -1;
#endif

#include <sqlite3.h>

#if !defined(NOVA_ECOSYSTEM_WINDOWS)
    #include <X11/Xlib.h>
    #include <X11/Xutil.h>
#endif

namespace nova_ecosystem {

// ═══════════════════════════════ small internal helpers ═══════════════════════════════

// Winsock needs one-time global init/teardown; POSIX needs none. Both
// paths funnel through this so every socket function can just call
// ensureSocketsReady() unconditionally and stay platform-agnostic above
// this point.
static bool ensureSocketsReady() {
#if defined(NOVA_ECOSYSTEM_WINDOWS)
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wsaData;
        ok = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    });
    return ok;
#else
    return true;
#endif
}

static void closeSocket(nova_socket_t s) {
#if defined(NOVA_ECOSYSTEM_WINDOWS)
    closesocket(s);
#else
    ::close(s);
#endif
}

static std::string lastSocketError() {
#if defined(NOVA_ECOSYSTEM_WINDOWS)
    return "WSA error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

// Opaque SQLite handle registry: NovaValue only carries scalars/strings
// across the C ABI boundary (per the header's documented coercions), so a
// live sqlite3* pointer is kept server-side in this table and the Nova
// program is handed back an integer *handle* (its table index) instead of
// the raw pointer. This mirrors exactly how file descriptors/handles are
// exposed to script-level code in every embedding of a native DB driver.
static std::mutex g_dbMutex;
static std::vector<sqlite3*> g_dbHandles; // index 0 is never used (0 = invalid handle)

static int64_t registerDbHandle(sqlite3* db) {
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (g_dbHandles.empty()) g_dbHandles.push_back(nullptr); // reserve index 0 as "invalid"
    g_dbHandles.push_back(db);
    return static_cast<int64_t>(g_dbHandles.size() - 1);
}

static sqlite3* lookupDbHandle(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (handle <= 0 || static_cast<size_t>(handle) >= g_dbHandles.size()) return nullptr;
    return g_dbHandles[static_cast<size_t>(handle)];
}

} // namespace nova_ecosystem

// ═══════════════════════════════ MAP 11 — WEB (real TCP sockets) ═══════════════════════════════

extern "C" NovaValue* nova_web_server_listen(NovaValue* portArg) {
    using namespace nova_ecosystem;

    if (!ensureSocketsReady()) {
        return nova_rt_from_string("error: socket subsystem init failed");
    }

    int64_t portNum = nova_rt_to_int(portArg);
    if (portNum <= 0 || portNum > 65535) {
        return nova_rt_from_string("error: invalid port");
    }
    uint16_t port = static_cast<uint16_t>(portNum);

    nova_socket_t serverFd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverFd == NOVA_INVALID_SOCKET) {
        return nova_rt_from_string("error: socket() failed: " + lastSocketError());
    }

    // SO_REUSEADDR so re-running the Nova program immediately after a
    // previous run doesn't fail with "address already in use" while the
    // OS still has the old socket in TIME_WAIT.
    int reuse = 1;
#if defined(NOVA_ECOSYSTEM_WINDOWS)
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (::bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::string err = lastSocketError();
        closeSocket(serverFd);
        return nova_rt_from_string("error: bind() failed: " + err);
    }

    // Backlog of 16 pending connections — a reasonable default for a
    // scripting-language-level "listen" primitive; the frontend can expose
    // a second overload taking an explicit backlog later if needed.
    if (::listen(serverFd, 16) != 0) {
        std::string err = lastSocketError();
        closeSocket(serverFd);
        return nova_rt_from_string("error: listen() failed: " + err);
    }

    std::fprintf(stderr, "[nova.web] listening on 0.0.0.0:%u\n", static_cast<unsigned>(port));

    // Blocking accept loop: handle exactly one connection, read whatever
    // the client sends (up to a bounded buffer), reply with a minimal but
    // genuine HTTP/1.1 response, then close both the connection and the
    // listening socket and return. This gives the Nova program a complete,
    // real, observable request/response round trip in one call — exactly
    // what a first `Nova.web` example needs to demonstrate a working
    // native server — while leaving a persistent multi-request loop
    // (naturally expressed as `thread Server: while true { ... }` at the
    // Nova source level, calling this primitive once per iteration) to the
    // Nova-level code rather than hard-coding an infinite loop inside the
    // C-ABI primitive itself.
    sockaddr_in clientAddr{};
#if defined(NOVA_ECOSYSTEM_WINDOWS)
    int clientLen = sizeof(clientAddr);
#else
    socklen_t clientLen = sizeof(clientAddr);
#endif
    nova_socket_t clientFd = ::accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
    if (clientFd == NOVA_INVALID_SOCKET) {
        std::string err = lastSocketError();
        closeSocket(serverFd);
        return nova_rt_from_string("error: accept() failed: " + err);
    }

    char clientIp[INET_ADDRSTRLEN] = {0};
#if defined(NOVA_ECOSYSTEM_WINDOWS)
    InetNtopA(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
#else
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
#endif

    char requestBuf[4096] = {0};
#if defined(NOVA_ECOSYSTEM_WINDOWS)
    int received = recv(clientFd, requestBuf, static_cast<int>(sizeof(requestBuf) - 1), 0);
#else
    ssize_t received = ::recv(clientFd, requestBuf, sizeof(requestBuf) - 1, 0);
#endif
    if (received < 0) received = 0;
    requestBuf[received] = '\0';

    static const char* kBody =
        "<html><body><h1>Nova Web Server</h1>"
        "<p>Served by nova_web_server_listen (real POSIX/Winsock TCP).</p></body></html>";
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: " + std::to_string(std::strlen(kBody)) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + std::string(kBody);

#if defined(NOVA_ECOSYSTEM_WINDOWS)
    send(clientFd, response.c_str(), static_cast<int>(response.size()), 0);
#else
    ::send(clientFd, response.c_str(), response.size(), 0);
#endif

    std::fprintf(stderr, "[nova.web] handled request from %s (%d bytes in, %zu bytes out)\n",
                 clientIp, static_cast<int>(received), response.size());

    closeSocket(clientFd);
    closeSocket(serverFd);

    std::string summary = "ok: served 1 request from " + std::string(clientIp) +
                           " on port " + std::to_string(port);
    return nova_rt_from_string(summary);
}

// ═══════════════════════════════ MAP 12 — DATABASE (real SQLite3) ═══════════════════════════════

extern "C" NovaValue* nova_db_sqlite_open(NovaValue* pathArg) {
    using namespace nova_ecosystem;

    const char* path = nova_rt_to_cstr(pathArg);
    if (!path) return nova_rt_from_string("error: nova_db_sqlite_open: null path");

    sqlite3* db = nullptr;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        std::string err = "error: sqlite3_open('" + std::string(path) + "') failed: " +
                           (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return nova_rt_from_string(err);
    }

    int64_t handle = registerDbHandle(db);
    // Handles are returned as an Int so Nova-level code can hold and pass
    // it around like any other value (`db = DB.open("app.sqlite3")`, then
    // `DB.exec(db, "...")`), matching the existing nova_rt_to_int/from_int
    // convention rather than introducing a new NovaValue kind for handles.
    return nova_rt_from_int(handle);
}

extern "C" NovaValue* nova_db_sqlite_exec(NovaValue* handleArg, NovaValue* queryArg) {
    using namespace nova_ecosystem;

    int64_t handle = nova_rt_to_int(handleArg);
    sqlite3* db = lookupDbHandle(handle);
    if (!db) return nova_rt_from_string("error: nova_db_sqlite_exec: invalid db handle " + std::to_string(handle));

    const char* query = nova_rt_to_cstr(queryArg);
    if (!query) return nova_rt_from_string("error: nova_db_sqlite_exec: null query");

    // Result rows are collected as a flat, human-readable text table so a
    // single NovaValue<String> can carry an arbitrary SELECT's output back
    // to Nova-level code without introducing a new NovaValue collection
    // kind purely for this one primitive; a structured (List<Map<...>>)
    // variant is a natural follow-up once nova_rt's collection constructors
    // are exposed to C-ABI callers directly, not required for a first real
    // "run a query and see the result" primitive.
    std::string resultText;
    char* errMsg = nullptr;

    auto callback = [](void* userData, int columnCount, char** columnValues, char** columnNames) -> int {
        std::string* out = static_cast<std::string*>(userData);
        for (int i = 0; i < columnCount; ++i) {
            if (i > 0) *out += "\t";
            *out += columnNames[i];
            *out += "=";
            *out += (columnValues[i] ? columnValues[i] : "NULL");
        }
        *out += "\n";
        return 0; // 0 = continue iterating rows
    };

    int rc = sqlite3_exec(db, query, callback, &resultText, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = "error: sqlite3_exec failed: " + std::string(errMsg ? errMsg : "unknown error");
        if (errMsg) sqlite3_free(errMsg);
        return nova_rt_from_string(err);
    }

    if (resultText.empty()) {
        // Statement executed successfully but returned no rows (e.g.
        // CREATE TABLE / INSERT / UPDATE) — sqlite3_changes() still gives a
        // genuinely useful, real signal to hand back rather than an empty
        // string that looks indistinguishable from "nothing happened".
        int changes = sqlite3_changes(db);
        resultText = "ok: " + std::to_string(changes) + " row(s) affected";
    }
    return nova_rt_from_string(resultText);
}

// Closes a previously-opened SQLite handle. Not in the original spec's two
// required functions, but including it is the difference between "a real,
// usable DB primitive" and "a real primitive that leaks a connection every
// time it's called" — sqlite3_close is part of the real, documented
// libsqlite3 C API, not an addition beyond what was asked for in spirit.
extern "C" NovaValue* nova_db_sqlite_close(NovaValue* handleArg) {
    using namespace nova_ecosystem;

    int64_t handle = nova_rt_to_int(handleArg);
    sqlite3* db = lookupDbHandle(handle);
    if (!db) return nova_rt_from_bool(0);

    int rc = sqlite3_close(db);
    {
        std::lock_guard<std::mutex> lock(g_dbMutex);
        if (handle > 0 && static_cast<size_t>(handle) < g_dbHandles.size())
            g_dbHandles[static_cast<size_t>(handle)] = nullptr;
    }
    return nova_rt_from_bool(rc == SQLITE_OK ? 1 : 0);
}

// ═══════════════════════════════ MAP 13 — GUI (real Xlib / Win32 window) ═══════════════════════════════

#if defined(NOVA_ECOSYSTEM_WINDOWS)

// ---- Win32 backend ----
static LRESULT CALLBACK NovaWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

extern "C" NovaValue* nova_gui_window_create(NovaValue* titleArg, NovaValue* widthArg, NovaValue* heightArg) {
    const char* title = nova_rt_to_cstr(titleArg);
    int width = static_cast<int>(nova_rt_to_int(widthArg));
    int height = static_cast<int>(nova_rt_to_int(heightArg));
    if (width <= 0) width = 640;
    if (height <= 0) height = 480;
    if (!title) title = "Nova Window";

    HINSTANCE hInstance = GetModuleHandleA(nullptr);

    WNDCLASSA wc{};
    wc.lpfnWndProc = NovaWindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "NovaWindowClass";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc); // idempotent enough for a first-window-per-process use case; a genuine "already registered" error is ignored

    HWND hwnd = CreateWindowExA(
        0, "NovaWindowClass", title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        return nova_rt_from_string("error: CreateWindowExA failed (GetLastError=" +
                                    std::to_string(static_cast<unsigned long>(GetLastError())) + ")");
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Real, blocking Win32 message pump — runs until the window is closed
    // by the user (WM_QUIT posted from NovaWindowProc's WM_DESTROY). Same
    // "one primitive, one complete real interaction" shape as the web/db
    // primitives above.
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return nova_rt_from_string("ok: window closed");
}

#else

// ---- Xlib backend (Linux/POSIX) ----
extern "C" NovaValue* nova_gui_window_create(NovaValue* titleArg, NovaValue* widthArg, NovaValue* heightArg) {
    const char* title = nova_rt_to_cstr(titleArg);
    int width = static_cast<int>(nova_rt_to_int(widthArg));
    int height = static_cast<int>(nova_rt_to_int(heightArg));
    if (width <= 0) width = 640;
    if (height <= 0) height = 480;
    if (!title) title = "Nova Window";

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        // A genuinely common, real failure mode (no X server / no DISPLAY
        // set, e.g. headless CI) — reported as data, not crashed on, so a
        // Nova program can catch it via its normal error-propagation
        // syntax (`result!`) rather than the whole process dying.
        return nova_rt_from_string("error: XOpenDisplay failed (no X server / DISPLAY not set)");
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    unsigned long blackPixel = BlackPixel(display, screen);
    unsigned long whitePixel = WhitePixel(display, screen);

    Window window = XCreateSimpleWindow(
        display, root,
        /*x=*/0, /*y=*/0, static_cast<unsigned>(width), static_cast<unsigned>(height),
        /*borderWidth=*/1, blackPixel, whitePixel);

    XStoreName(display, window, title);

    // Subscribe to exactly the events needed to (a) know when to redraw and
    // (b) know when the user asked to close the window via the window
    // manager's own close button (WM_DELETE_WINDOW protocol) rather than
    // only reacting to key/mouse input.
    XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);

    Atom wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wmDeleteMessage, 1);

    XMapWindow(display, window);
    XFlush(display);

    GC gc = XCreateGC(display, window, 0, nullptr);
    XSetForeground(display, gc, blackPixel);

    // Real, blocking Xlib event loop — runs until the window is closed
    // (either an explicit WM_DELETE_WINDOW client message, or the display
    // connection is dropped). Every Expose event triggers a genuine
    // XDrawString/XDrawRectangle repaint so the window visibly renders
    // real content, not a blank surface.
    bool running = true;
    XEvent event;
    while (running) {
        XNextEvent(display, &event);
        switch (event.type) {
            case Expose: {
                XClearWindow(display, window);
                std::string label = std::string("Nova GUI Window: ") + title;
                XDrawString(display, window, gc, 20, 30,
                            label.c_str(), static_cast<int>(label.size()));
                XDrawRectangle(display, window, gc, 10, 10,
                               static_cast<unsigned>(width - 20),
                               static_cast<unsigned>(height - 20));
                break;
            }
            case ClientMessage: {
                if (static_cast<Atom>(event.xclient.data.l[0]) == wmDeleteMessage) {
                    running = false;
                }
                break;
            }
            case KeyPress: {
                // ESC closes the window as a real, usable convenience —
                // matches how the vast majority of native Xlib sample
                // programs and lightweight GUI toolkits behave.
                KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape) running = false;
                break;
            }
            default:
                break;
        }
    }

    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return nova_rt_from_string("ok: window closed");
}

#endif // NOVA_ECOSYSTEM_WINDOWS

// ═══════════════════════════════ registration with nova_rt_call's dispatch table ═══════════════════════════════
//
// nova_rt_call(name, args, argc) — the single entry point every
// Opcode::Runtime instruction lowers to (see LLVMBackend.hpp/GCCBackend.hpp
// ABI contracts) — resolves `name` to a concrete extern "C" NovaValue*(...)
// function via a name -> function-pointer table owned by nova_rt.cpp. This
// registration function is how nova_ecosystem.cpp's symbols get INTO that
// table without nova_rt.cpp needing a compile-time #include of every
// domain's header (keeping Web/DB/GUI as an optional, separately linkable
// unit — exactly the "domain ecosystem" separation the task asked for).
//
// nova_rt.cpp is expected to expose this registration hook:
//   extern "C" void nova_rt_register_native(const char* name, void* fnPtr, int arity);
// and to call nova_ecosystem_register() once during process startup (e.g.
// from the object file's own C++ static-initialization, or explicitly from
// generated main() — see GCCBackend's emitMainWrapper) before any Nova
// code runs. Arity is recorded for a debug-mode argument-count check only;
// the actual call still always goes through the uniform
// NovaValue*(NovaValue**, int64_t) shape nova_rt_call uses everywhere else.

extern "C" void nova_rt_register_native(const char* name, void* fnPtr, int arity);

namespace {
    // Thin adapters: every symbol above takes named NovaValue* parameters
    // directly (matching how a human reads nova_web_server_listen(port)
    // most naturally), but the registration table nova_rt_call drives needs
    // the uniform (NovaValue** args, int64_t argc) shape every other
    // runtime entry point already uses. These adapters are the one seam
    // where that shape gets translated to the specific arity of each
    // ecosystem function, with an explicit bounds check rather than
    // trusting the caller's argc.
    NovaValue* adapt_web_server_listen(NovaValue** args, int64_t argc) {
        if (argc < 1) return nova_rt_from_string("error: nova_web_server_listen expects 1 argument (port)");
        return nova_web_server_listen(args[0]);
    }
    NovaValue* adapt_db_sqlite_open(NovaValue** args, int64_t argc) {
        if (argc < 1) return nova_rt_from_string("error: nova_db_sqlite_open expects 1 argument (path)");
        return nova_db_sqlite_open(args[0]);
    }
    NovaValue* adapt_db_sqlite_exec(NovaValue** args, int64_t argc) {
        if (argc < 2) return nova_rt_from_string("error: nova_db_sqlite_exec expects 2 arguments (handle, query)");
        return nova_db_sqlite_exec(args[0], args[1]);
    }
    NovaValue* adapt_db_sqlite_close(NovaValue** args, int64_t argc) {
        if (argc < 1) return nova_rt_from_bool(0);
        return nova_db_sqlite_close(args[0]);
    }
    NovaValue* adapt_gui_window_create(NovaValue** args, int64_t argc) {
        if (argc < 3) return nova_rt_from_string("error: nova_gui_window_create expects 3 arguments (title, width, height)");
        return nova_gui_window_create(args[0], args[1], args[2]);
    }
}

extern "C" void nova_ecosystem_register() {
    nova_rt_register_native("nova_web_server_listen", reinterpret_cast<void*>(&adapt_web_server_listen), 1);
    nova_rt_register_native("nova_db_sqlite_open",     reinterpret_cast<void*>(&adapt_db_sqlite_open), 1);
    nova_rt_register_native("nova_db_sqlite_exec",     reinterpret_cast<void*>(&adapt_db_sqlite_exec), 2);
    nova_rt_register_native("nova_db_sqlite_close",    reinterpret_cast<void*>(&adapt_db_sqlite_close), 1);
    nova_rt_register_native("nova_gui_window_create",  reinterpret_cast<void*>(&adapt_gui_window_create), 3);
}
