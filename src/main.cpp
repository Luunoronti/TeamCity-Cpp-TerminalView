// teamcity_ticker - simple console webhook listener
// Build: see README.md

// --- Windows headers order fix (winsock2 before windows.h) ---
#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  // Prevent <windows.h> from dragging in <winsock.h>
  #ifndef _WINSOCKAPI_
  #define _WINSOCKAPI_
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#endif
// --------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// third-party single headers (vendored)
#include "httplib.h"      // header-only HTTP server
#include "json.hpp"       // header-only JSON
using json = nlohmann::json;

using Clock = std::chrono::steady_clock;
using SysClock = std::chrono::system_clock;

enum class BuildState { Queued, Running, Success, Failure, Canceled, Unknown };

struct BuildCard {
    std::string id;
    std::string number;
    std::string definition;
    std::string issuer;
    BuildState  state = BuildState::Unknown;

    bool has_start = false;
    Clock::time_point start_tp{};        // local start time (for elapsed)

    SysClock::time_point last_update{};
    std::string last_change;
};

struct AppState {
    std::mutex mtx;
    std::deque<BuildCard> cards;                 // newest first
    std::unordered_map<std::string, size_t> idx; // id -> position in cards
    std::unordered_set<std::string> queued;      // build ids currently queued
    size_t max_cards = 20;
};

static std::atomic<bool> g_running{true};

#ifdef _WIN32
static void enable_vt_colors() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(h, mode);
}
#else
static void enable_vt_colors() {}
#endif

// ANSI colors
namespace ansi {
    const char* reset  = "\x1b[0m";
    const char* dim    = "\x1b[2m";
    const char* bold   = "\x1b[1m";
    const char* gray   = "\x1b[90m";
    const char* red    = "\x1b[31m";
    const char* green  = "\x1b[32m";
    const char* yellow = "\x1b[33m";
}

static std::string trim_to(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    if (n <= 1) return "…";
    return s.substr(0, n - 1) + "…";
}

static std::string state_label(BuildState st) {
    switch (st) {
        case BuildState::Queued:   return "QUEUED";
        case BuildState::Running:  return "RUNNING";
        case BuildState::Success:  return "SUCCESS";
        case BuildState::Failure:  return "FAILURE";
        case BuildState::Canceled: return "CANCELED";
        default:                   return "UNKNOWN";
    }
}
static const char* state_color(BuildState st) {
    using namespace ansi;
    switch (st) {
        case BuildState::Running:  return yellow;
        case BuildState::Success:  return green;
        case BuildState::Failure:  return red;
        case BuildState::Canceled: return red;
        case BuildState::Queued:   return gray;
        default:                   return gray;
    }
}

static std::string hhmmss(std::chrono::seconds s) {
    auto h = std::chrono::duration_cast<std::chrono::hours>(s);
    s -= h;
    auto m = std::chrono::duration_cast<std::chrono::minutes>(s);
    s -= m;
    std::ostringstream os;
    os << std::setfill('0') << std::setw(2) << h.count() << ":"
       << std::setw(2) << m.count() << ":" << std::setw(2) << s.count();
    return os.str();
}

static void clear_screen() {
    std::cout << "\x1b[2J\x1b[H"; // clear + home
}

static void render(const AppState& S) {
    using namespace ansi;

    clear_screen();
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(S.mtx));

    size_t running_cnt = 0;
    for (const auto& c : S.cards) if (c.state == BuildState::Running) ++running_cnt;

    std::cout << bold << "TeamCity Webhook Ticker" << reset << "  "
              << dim << "(POST /webhook)" << reset << "\n";
    std::cout << "Queue: " << S.queued.size()
              << "    Running: " << running_cnt
              << "    Showing: " << std::min(S.cards.size(), S.max_cards)
              << "\n\n";

    const size_t max_show = std::min(S.cards.size(), S.max_cards);
    for (size_t i = 0; i < max_show; ++i) {
        const auto& c = S.cards[i];

        // Box
        std::cout << "+---------------------------------\n";

        // Title line: number + definition
        std::string title = trim_to(c.number.empty() ? "(no number)" : c.number, 18)
                          + "  " + trim_to(c.definition, 50);
        std::cout << title << "\n";

        // While running: status + elapsed, colored
        if (c.state == BuildState::Running) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - c.start_tp);
            std::cout << state_color(c.state) << state_label(c.state) << reset
                      << "  (" << hhmmss(elapsed) << ")\n";
        }

        // Always show "by issuer"
        std::cout << "by " << (c.issuer.empty() ? "unknown" : c.issuer) << "\n";

        // You asked to hide status/time for non-running. We still show a faint last change line (not status/time).
        if (c.state != BuildState::Running && !c.last_change.empty()) {
            std::cout << dim << c.last_change << reset << "\n";
        }

        std::cout << "+---------------------------------\n\n";
    }
    std::cout.flush();
}

static void move_to_front(AppState& S, const std::string& id) {
    auto it = S.idx.find(id);
    if (it == S.idx.end()) return;
    size_t pos = it->second;
    if (pos == 0) return;
    BuildCard card = S.cards[pos];
    S.cards.erase(S.cards.begin() + static_cast<long>(pos));
    S.cards.emplace_front(std::move(card));
    // rebuild indices (small N; OK)
    S.idx.clear();
    for (size_t i = 0; i < S.cards.size(); ++i) S.idx[S.cards[i].id] = i;
}

static BuildCard& upsert_card(AppState& S, const std::string& id) {
    auto it = S.idx.find(id);
    if (it != S.idx.end()) {
        move_to_front(S, id);
        return S.cards.front();
    }
    BuildCard fresh;
    fresh.id = id;
    fresh.last_update = SysClock::now();
    S.cards.emplace_front(std::move(fresh));
    S.idx.clear();
    for (size_t i = 0; i < S.cards.size(); ++i) S.idx[S.cards[i].id] = i;

    // Trim
    while (S.cards.size() > S.max_cards) {
        S.idx.erase(S.cards.back().id);
        S.cards.pop_back();
    }
    return S.cards.front();
}

// JSON helpers (best-effort field probing)
static std::optional<std::string> jstr(const json& j, std::initializer_list<const char*> keys) {
    for (auto k : keys) {
        auto it = j.find(k);
        if (it != j.end() && it->is_string()) return it->get<std::string>();
    }
    return std::nullopt;
}
static std::optional<bool> jbool(const json& j, std::initializer_list<const char*> keys) {
    for (auto k : keys) {
        auto it = j.find(k);
        if (it != j.end() && it->is_boolean()) return it->get<bool>();
    }
    return std::nullopt;
}

struct Parsed {
    std::string id;
    std::string number;
    std::string defname;
    std::string issuer;
    BuildState  state = BuildState::Unknown;
    std::string event_hint;
    bool started_now = false;
    bool queued_now  = false;
    bool finished_now = false;
};

static std::string pick_issuer(const json& jbuild) {
    // Try multiple shapes
    if (auto v = jstr(jbuild, {"issuer","user","username","userName"})) return *v;
    if (jbuild.contains("triggered") && jbuild["triggered"].is_object()) {
        auto t = jbuild["triggered"];
        if (auto u = jstr(t, {"user","username","userName","displayName"})) return *u;
        if (t.contains("user") && t["user"].is_object()) {
            if (auto un = jstr(t["user"], {"name","username","userName","login"})) return *un;
        }
    }
    if (jbuild.contains("triggeredBy") && jbuild["triggeredBy"].is_object()) {
        auto t = jbuild["triggeredBy"];
        if (auto u = jstr(t, {"username","userName","name"})) return *u;
    }
    if (jbuild.contains("agent") && jbuild["agent"].is_object()) {
        if (auto a = jstr(jbuild["agent"], {"name"})) return *a;
    }
    return {};
}

static Parsed parse_payload(const json& j) {
    Parsed out;

    // Support both "queuedBuild" and "build" shapes; or flattened
    json jb;
    bool is_queue_obj = false;

    if (j.contains("queuedBuild")) {
        jb = j["queuedBuild"];
        is_queue_obj = true;
        out.event_hint = j.value("event", std::string{});
    } else if (j.contains("build")) {
        jb = j["build"];
        out.event_hint = j.value("event", std::string{});
    } else {
        jb = j; // flat
        out.event_hint = j.value("event", std::string{});
    }

    // id
    if (auto v = jstr(jb, {"id","buildId"})) out.id = *v;
    else if (j.contains("buildId") && j["buildId"].is_string()) out.id = j["buildId"].get<std::string>();
    else out.id = std::to_string(reinterpret_cast<uintptr_t>(&jb)); // fallback unique-ish

    // number
    if (auto v = jstr(jb, {"buildNumber","number"})) out.number = *v;

    // definition name
    if (jb.contains("buildType") && jb["buildType"].is_object()) {
        if (auto v = jstr(jb["buildType"], {"name","id"})) out.defname = *v;
    }
    if (out.defname.empty()) {
        if (auto v = jstr(jb, {"buildTypeName","definition","buildName"})) out.defname = *v;
    }

    // issuer
    out.issuer = pick_issuer(jb);

    // state/event
    std::string state  = jb.value("state", std::string{});
    std::string status = jb.value("status", std::string{});
    bool running = jbool(jb, {"running"}).value_or(false);
    bool canceled = jb.contains("canceledInfo");

    if (is_queue_obj || state == "queued" || out.event_hint == "buildQueued") {
        out.state = BuildState::Queued;
        out.queued_now = true;
        return out;
    }

    if (state == "running" || running || out.event_hint == "buildStarted") {
        out.state = BuildState::Running;
        out.started_now = true;
        return out;
    }

    if (state == "finished" || out.event_hint == "buildFinished" || out.event_hint == "buildInterrupted") {
        out.finished_now = true;
        if (canceled || out.event_hint == "buildInterrupted") out.state = BuildState::Canceled;
        else if (status == "SUCCESS") out.state = BuildState::Success;
        else if (status == "FAILURE" || status == "ERROR") out.state = BuildState::Failure;
        else out.state = BuildState::Unknown;
        return out;
    }

    // Fallback
    if (!status.empty()) {
        if (status == "SUCCESS") out.state = BuildState::Success;
        else if (status == "FAILURE" || status == "ERROR") out.state = BuildState::Failure;
    } else if (running) {
        out.state = BuildState::Running;
    } else {
        out.state = BuildState::Unknown;
    }

    return out;
}

static void on_event(AppState& S, const Parsed& p) {
    std::lock_guard<std::mutex> lock(S.mtx);

    auto& c = upsert_card(S, p.id);
    c.number = p.number.empty() ? c.number : p.number;
    c.definition = p.defname.empty() ? c.definition : p.defname;
    c.issuer = p.issuer.empty() ? c.issuer : p.issuer;
    c.last_update = SysClock::now();

    // Update queue set
    if (p.queued_now) {
        S.queued.insert(p.id);
        c.state = BuildState::Queued;
        c.has_start = false;
        c.last_change = "queued";
    } else if (p.started_now) {
        S.queued.erase(p.id);
        c.state = BuildState::Running;
        c.has_start = true;
        c.start_tp = Clock::now(); // local timer as requested
        c.last_change = "started";
    } else if (p.finished_now) {
        S.queued.erase(p.id);
        c.state = p.state;
        c.has_start = false;
        switch (p.state) {
            case BuildState::Success:  c.last_change = "finished (SUCCESS)"; break;
            case BuildState::Failure:  c.last_change = "finished (FAILURE)"; break;
            case BuildState::Canceled: c.last_change = "canceled"; break;
            default:                   c.last_change = "finished";
        }
    } else {
        // Passive update (e.g., unknown/partial)
        c.state = p.state;
        c.last_change = "updated";
    }
}

static void sigint_handler(int) {
    g_running = false;
}

struct Args {
    std::string bind = "127.0.0.1";
    int port = 9876;
    size_t max_cards = 20;
};

static void print_help() {
    std::cout <<
R"(teamcity_ticker [--bind <ip>] [--port <port>] [--max-cards <N>]
  Defaults: bind=127.0.0.1 port=9876 max-cards=20
)";
}

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](const char* name)->std::string {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << name << "\n"; exit(1); }
            return std::string(argv[++i]);
        };
        if (s == "--bind") a.bind = next("--bind");
        else if (s == "--port") a.port = std::stoi(next("--port"));
        else if (s == "--max-cards") a.max_cards = static_cast<size_t>(std::stoul(next("--max-cards")));
        else if (s == "--help" || s == "-h" || s == "/?") { print_help(); exit(0); }
        else { std::cerr << "Unknown arg: " << s << "\n"; print_help(); exit(1); }
    }
    return a;
}

int main(int argc, char** argv) {
    enable_vt_colors();

    std::signal(SIGINT,  sigint_handler);
#ifdef SIGTERM
    std::signal(SIGTERM, sigint_handler);
#endif

    Args args = parse_args(argc, argv);

    AppState state;
    state.max_cards = args.max_cards;

    // UI refresher: re-render once per second (to update elapsed time)
    std::thread ui([&]{
        while (g_running) {
            render(state);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    // HTTP server
    httplib::Server svr;

    svr.Get("/", [](const httplib::Request&, httplib::Response& res){
        res.set_content("teamcity_ticker: POST JSON to /webhook (see README)", "text/plain");
    });
    svr.Get("/ping", [](const httplib::Request&, httplib::Response& res){
        res.set_content("OK", "text/plain");
    });
    svr.Post("/webhook", [&](const httplib::Request& req, httplib::Response& res){
        try {
            auto j = json::parse(req.body);
            Parsed p = parse_payload(j);
            on_event(state, p);
            res.set_content("ok\n", "text/plain");
        } catch (const std::exception& e) {
            std::ostringstream os; os << "bad request: " << e.what() << "\n";
            res.status = 400;
            res.set_content(os.str(), "text/plain");
        }
    });

    std::cout << "Listening on http://" << args.bind << ":" << args.port << "/webhook\n";
    std::cout << "(Press Ctrl+C to quit)\n";

    // Note: httplib::Server::listen blocks
    bool ok = false;
    try {
        ok = svr.listen(args.bind.c_str(), args.port);
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
    }

    g_running = false;
    if (ui.joinable()) ui.join();

    if (!ok) {
        std::cerr << "Failed to bind. Check IP/port or firewall.\n";
        return 1;
    }
    return 0;
}
