#include "ui.hpp"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/logger.hpp"
#include "experimental/legacy_reload.hpp"
#include "minecraft/compatibility.hpp"
#include "platform/windows/debug_overlay.hpp"

#include "game_window.hpp"

namespace ui {
namespace {

namespace cx = conduit::experimental;
namespace fs = std::filesystem;

std::mutex g_backend_mutex;
std::unique_ptr<conduit::ReloadBackend> g_backend;
std::string g_install_status = "installing";
std::mutex g_reload_mutex;

std::mutex g_job_serial;
std::mutex g_job_mutex;
std::condition_variable g_job_cv;
std::function<void()> g_job;
std::atomic<bool> g_job_pending{false};
bool g_job_done = false;

void on_frame() noexcept
{
    if (!g_job_pending.load(std::memory_order_acquire))
        return;
    std::function<void()> job;
    {
        std::lock_guard lock(g_job_mutex);
        job = std::move(g_job);
        g_job = nullptr;
        g_job_pending.store(false, std::memory_order_release);
    }
    try
    {
        if (job)
            job();
    }
    catch (...)
    {
    }
    {
        std::lock_guard lock(g_job_mutex);
        g_job_done = true;
    }
    g_job_cv.notify_all();
}

bool run_on_ui_thread(std::function<void()> job)
{
    std::lock_guard serial(g_job_serial);
    std::unique_lock lock(g_job_mutex);
    g_job = std::move(job);
    g_job_done = false;
    g_job_pending.store(true, std::memory_order_release);
    if (g_job_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_job_done; }))
        return true;
    if (g_job_pending.load(std::memory_order_acquire))
    {
        g_job = nullptr;
        g_job_pending.store(false, std::memory_order_release);
        return false;
    }
    g_job_cv.wait(lock, [] { return g_job_done; });
    return true;
}

conduit::windows::DebugReloadStats g_overlay_stats;

fs::path data_dir()
{
    fs::path base;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
        base = p;
    CoTaskMemFree(p);
    fs::path dir = base / L"BedrockMCP";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

void overlay_loop()
{
    const fs::path dir = data_dir();
    try
    {
        static conduit::Logger logger(dir / L"overlay.log");
        g_overlay_stats.diagnostics = &logger;
        const fs::path disable = dir / L"DISABLE_OVERLAY";
        conduit::windows::runDebugOverlay(disable.c_str(), g_overlay_stats);
        logger.write("info", "overlay stopped");
    }
    catch (...)
    {
    }
}

std::unique_ptr<conduit::ReloadBackend> with_overlay(std::unique_ptr<conduit::ReloadBackend> backend)
{
    if (const auto* profile = conduit::minecraft::runningCompatibility())
        g_overlay_stats.clientVersion = profile->build;
    g_overlay_stats.screenMetrics = &cx::nativeScreenMetrics();
    g_overlay_stats.inspectHoveredControl = &cx::inspectHoveredControl;
    return conduit::windows::trackDebugReloads(std::move(backend), g_overlay_stats);
}

void set_install_status(std::string text)
{
    std::lock_guard lock(g_backend_mutex);
    g_install_status = std::move(text);
}

void install_loop()
{
    const fs::path log = data_dir() / L"conduit.log";
    for (int attempt = 0; attempt < 600; ++attempt)
    {
        try
        {
            auto backend = with_overlay(cx::makeLegacyReloadBackend(log, true));
            cx::setFrameObserver(&on_frame);
            {
                std::lock_guard lock(g_backend_mutex);
                g_backend = std::move(backend);
                g_install_status = "installed";
            }
            std::thread(overlay_loop).detach();
            return;
        }
        catch (const std::exception& error)
        {
            std::string message = error.what();
            set_install_status(message);
            if (message.find("Unsupported") != std::string::npos ||
                message.find("single-use") != std::string::npos)
                return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void hotkey_loop()
{
    bool was_down = false;
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        HWND game = find_game_window();
        bool down = game && GetForegroundWindow() == game && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                    (GetAsyncKeyState('R') & 0x8000);
        if (down && !was_down)
            reload();
        was_down = down;
    }
}

struct Item {
    int parent = -1;
    int depth = 0;
    std::int64_t layer = 0;
    std::size_t children = 0;
    std::string name;
    std::string type;
    cx::ControlNode node;
};

struct Snapshot {
    bool ok = false;
    std::string error;
    std::vector<Item> items;
    bool truncated = false;
};

constexpr std::size_t kMaxNodes = 20000;

Snapshot collect(bool include_hidden, int max_depth)
{
    Snapshot snap;
    bool ran = run_on_ui_thread([&] {
        const auto root = cx::activeVisualRoot();
        if (!root)
        {
            snap.error = "the active screen is not a legacy JSON UI screen (Ore UI screens have no "
                         "control tree), or the UI thread is between screens";
            return;
        }
        struct Pending {
            std::uintptr_t control;
            int parent;
            int depth;
            std::int64_t parent_layer;
        };
        std::vector<Pending> stack{{root, -1, 0, 0}};
        std::vector<std::uintptr_t> children;
        while (!stack.empty())
        {
            if (snap.items.size() >= kMaxNodes)
            {
                snap.truncated = true;
                break;
            }
            Pending cur = stack.back();
            stack.pop_back();
            Item item;
            if (!cx::readControlNode(cur.control, item.node))
                continue;
            if (!item.node.visible && !include_hidden && cur.parent >= 0)
                continue;
            item.parent = cur.parent;
            item.depth = cur.depth;
            item.layer = cur.parent_layer + item.node.layer;
            item.name = cx::readControlName(cur.control);
            item.type = cx::readControlType(cur.control);
            if (cx::readControlChildren(cur.control, children))
                item.children = children.size();
            int index = static_cast<int>(snap.items.size());
            snap.items.push_back(std::move(item));
            if (cur.depth >= max_depth)
                continue;
            for (auto it = children.rbegin(); it != children.rend(); ++it)
                stack.push_back({*it, index, cur.depth + 1, snap.items.back().layer});
        }
        snap.ok = true;
    });
    if (!ran)
        snap.error = "the game's UI thread did not run a frame within 3 seconds (minimized, loading, "
                     "or the Conduit hooks are not armed yet)";
    return snap;
}

struct Scale {
    double sx = 1, sy = 1;
    int client_w = 0, client_h = 0;
};

Scale scale_of(const Snapshot& snap)
{
    Scale s;
    RECT rc{};
    if (HWND game = find_game_window(); game && GetClientRect(game, &rc))
    {
        s.client_w = rc.right;
        s.client_h = rc.bottom;
    }
    const auto& root = snap.items.front().node;
    if (root.width > 0 && root.height > 0 && s.client_w > 0 && s.client_h > 0)
    {
        s.sx = s.client_w / root.width;
        s.sy = s.client_h / root.height;
    }
    return s;
}

std::string label(const Item& item)
{
    std::string name = item.name.empty() ? "(unnamed)" : item.name;
    if (!item.type.empty() && item.type != "unknown")
        name += " [" + item.type + "]";
    return name;
}

std::string rect_text(const Item& item, const Scale& s)
{
    if (!item.node.positioned)
        return "unpositioned";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%ld,%ld %ldx%ld", std::lround(item.node.x * s.sx),
                  std::lround(item.node.y * s.sy), std::lround(item.node.width * s.sx),
                  std::lround(item.node.height * s.sy));
    return buf;
}

std::string path_of(const Snapshot& snap, int index)
{
    std::vector<int> chain;
    for (int i = index; i >= 0; i = snap.items[i].parent)
        chain.push_back(i);
    std::string out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        if (!out.empty())
            out += " / ";
        const auto& name = snap.items[*it].name;
        out += name.empty() ? "(unnamed)" : name;
    }
    return out;
}

std::string header(const Snapshot& snap, const Scale& s)
{
    const auto& root = snap.items.front().node;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "gui %.0fx%.0f, client %dx%d; rects are client pixels (x,y wxh), the same units as "
                  "move/click\n",
                  root.width, root.height, s.client_w, s.client_h);
    return buf;
}

} // namespace

void init()
{
    std::thread(install_loop).detach();
    std::thread(hotkey_loop).detach();
}

Result status()
{
    std::lock_guard lock(g_backend_mutex);
    return {g_backend != nullptr, "install: " + g_install_status + "; reload: " + cx::legacyReloadState()};
}

Result reload()
{
    std::lock_guard serial(g_reload_mutex);
    conduit::ReloadBackend* backend = nullptr;
    std::string install;
    {
        std::lock_guard lock(g_backend_mutex);
        backend = g_backend.get();
        install = g_install_status;
    }
    if (!backend)
        return {false, "Conduit reload is not available yet: " + install};
    conduit::ReloadRequest request;
    request.explicitRequest = true;
    switch (backend->reload(request))
    {
    case conduit::ReloadResult::Succeeded:
        return {true, "reloaded the JSON UI definitions; check the result with screenshot"};
    case conduit::ReloadResult::Unsupported:
        return {false, "the reload backend is not armed yet (it needs a few frames of the game's UI "
                       "thread); retry in a few seconds. state: " +
                           cx::legacyReloadState()};
    default:
        return {false, "reload failed (a reload may still be running; a timeout disables reload until "
                       "the game restarts). state: " +
                           cx::legacyReloadState()};
    }
}

Result screen()
{
    const auto d = cx::nativeScreenMetrics().diagnostics(std::chrono::steady_clock::now());
    std::string name = d.name.data();
    if (name == "Unavailable")
        return {false, "no legacy JSON UI screen was sampled in the last second (Ore UI screen, the "
                       "game is paused or minimized, or the hooks are not armed). " +
                           status().text};
    auto count = [](const conduit::ControlCount& c) {
        if (c.status == conduit::ControlCountStatus::Unavailable)
            return std::string("unavailable");
        return std::to_string(c.count) + (c.status == conduit::ControlCountStatus::Complete ? "" : " (partial)");
    };
    std::string out = "screen: " + name + "\ncontrols: " + count(d.elements) +
                      "\nbindings: " + count(d.bindings) + "\ncollection: " +
                      std::to_string(d.collectionMicros) + " us\n";
    const auto& f = d.factories;
    if (f.available)
    {
        out += "factories: " + std::to_string(f.instances) + " instances, " + std::to_string(f.liveRoots) +
               " generated roots, " + std::to_string(f.generatedControls) + " generated controls\n";
        for (std::uint32_t i = 0; i < f.detailCount; ++i)
        {
            const auto& row = f.details[i];
            out += "  " + std::string(row.nameAvailable ? row.name.data() : "(unnamed)") + " in " +
                   std::string(row.ownerAvailable ? row.owner.data() : "(unnamed)") + ": " +
                   std::to_string(row.liveRoots) + " roots, " + std::to_string(row.controls) + " controls\n";
        }
        if (f.detailsTruncated)
            out += "  ...\n";
    }
    return {true, out};
}

Result tree(const TreeOptions& options)
{
    Snapshot snap = collect(options.include_hidden, std::clamp(options.max_depth, 0, 256));
    if (!snap.ok)
        return {false, snap.error};
    Scale s = scale_of(snap);
    std::string out = header(snap, s);
    int printed = 0, matched = 0;
    for (std::size_t i = 0; i < snap.items.size(); ++i)
    {
        const Item& item = snap.items[i];
        if (!options.filter.empty() && item.name.find(options.filter) == std::string::npos)
            continue;
        ++matched;
        if (printed >= options.limit)
            continue;
        ++printed;
        std::string line = options.filter.empty() ? std::string(item.depth * 2, ' ') + label(item)
                                                  : path_of(snap, static_cast<int>(i)) +
                                                        (item.type.empty() || item.type == "unknown"
                                                             ? ""
                                                             : " [" + item.type + "]");
        line += "  " + rect_text(item, s) + " layer=" + std::to_string(item.layer);
        if (!item.node.visible)
            line += " hidden";
        if (item.depth >= options.max_depth && item.children)
            line += " +" + std::to_string(item.children) + " children";
        out += line + "\n";
    }
    if (matched > printed)
        out += "... " + std::to_string(matched - printed) + " more (raise limit, or use filter)\n";
    if (snap.truncated)
        out += "... stopped after " + std::to_string(kMaxNodes) + " controls\n";
    out += std::to_string(snap.items.size()) + " controls read";
    return {true, out};
}

Result at(int x, int y)
{
    Snapshot snap = collect(false, 256);
    if (!snap.ok)
        return {false, snap.error};
    Scale s = scale_of(snap);
    const double gx = x / s.sx, gy = y / s.sy;
    int selected = -1;
    for (std::size_t i = 0; i < snap.items.size(); ++i)
    {
        const auto& item = snap.items[i];
        const auto& n = item.node;
        if (!n.visible || !n.positioned || n.width == 0 || n.height == 0)
            continue;
        const double left = std::min<double>(n.x, n.x + n.width), right = std::max<double>(n.x, n.x + n.width);
        const double top = std::min<double>(n.y, n.y + n.height), bottom = std::max<double>(n.y, n.y + n.height);
        if (gx < left || gx >= right || gy < top || gy >= bottom)
            continue;
        if (selected < 0 || item.layer >= snap.items[selected].layer)
            selected = static_cast<int>(i);
    }
    if (selected < 0)
        return {false, "no visible control contains client point " + std::to_string(x) + "," + std::to_string(y)};
    const Item& item = snap.items[selected];
    std::string out = header(snap, s) + "control: " + label(item) + "\nrect: " + rect_text(item, s) +
                      "\nlayer: " + std::to_string(item.layer) + "\npath: " + path_of(snap, selected);
    return {true, out};
}

} // namespace ui
