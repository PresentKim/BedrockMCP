#include "ui.hpp"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/logger.hpp"
#include "experimental/legacy_reload.hpp"
#include "minecraft/compatibility.hpp"
#include "platform/windows/debug_overlay.hpp"

#include "config.hpp"
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
std::atomic<bool> g_overlay_running{false};

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
        g_overlay_running.store(false);
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
            g_overlay_running.store(true);
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
            reload(false);
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

std::string display_name(const Item& item)
{
    return item.name.empty() ? "(unnamed)" : item.name;
}

std::vector<std::string> keys_of(const Snapshot& snap)
{
    std::vector<std::string> keys(snap.items.size());
    std::map<std::pair<int, std::string>, int> seen;
    for (std::size_t i = 0; i < snap.items.size(); ++i)
    {
        const Item& item = snap.items[i];
        const std::string name = display_name(item);
        const int ordinal = seen[{item.parent, name}]++;
        keys[i] = (item.parent >= 0 ? keys[item.parent] + " / " : std::string()) + name +
                  (ordinal ? "#" + std::to_string(ordinal) : std::string());
    }
    return keys;
}

std::string diff_text(const Snapshot& before, const Snapshot& after)
{
    constexpr int kMaxLines = 80;
    const Scale sb = scale_of(before), sa = scale_of(after);
    const auto before_keys = keys_of(before), after_keys = keys_of(after);
    std::unordered_map<std::string, std::size_t> old_index;
    for (std::size_t i = 0; i < before_keys.size(); ++i)
        old_index.emplace(before_keys[i], i);
    std::vector<std::string> lines;
    int added = 0, removed = 0, changed = 0;
    for (std::size_t i = 0; i < after_keys.size(); ++i)
    {
        const std::string now = rect_text(after.items[i], sa);
        auto it = old_index.find(after_keys[i]);
        if (it == old_index.end())
        {
            ++added;
            lines.push_back("+ " + after_keys[i] + "  " + now);
            continue;
        }
        const std::string was = rect_text(before.items[it->second], sb);
        old_index.erase(it);
        if (was != now)
        {
            ++changed;
            lines.push_back("~ " + after_keys[i] + "  " + now + " (was " + was + ")");
        }
    }
    for (const auto& [key, index] : old_index)
    {
        ++removed;
        lines.push_back("- " + key + "  " + rect_text(before.items[index], sb));
    }
    std::string out = "visible controls: " + std::to_string(before.items.size()) + " before, " +
                      std::to_string(after.items.size()) + " after; " + std::to_string(changed) +
                      " moved or resized, " + std::to_string(added) + " appeared, " +
                      std::to_string(removed) + " disappeared\n";
    for (int i = 0; i < static_cast<int>(lines.size()) && i < kMaxLines; ++i)
        out += lines[i] + "\n";
    if (static_cast<int>(lines.size()) > kMaxLines)
        out += "... " + std::to_string(lines.size() - kMaxLines) + " more\n";
    return out;
}

struct Box {
    RECT rect;
    int number;
};

void paint_boxes(unsigned char* pixels, unsigned pitch, int width, int height, const std::vector<Box>& boxes)
{
    static const COLORREF kColors[] = {RGB(255, 64, 64),  RGB(64, 224, 64),  RGB(64, 160, 255),
                                       RGB(255, 200, 0),  RGB(255, 64, 255), RGB(0, 230, 230)};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits)
    {
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
        return;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    auto* dib = static_cast<unsigned char*>(bits);
    for (int y = 0; y < height; ++y)
        std::memcpy(dib + y * row_bytes, pixels + static_cast<std::size_t>(y) * pitch, row_bytes);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    HFONT font = CreateFontW(-std::max(14, height / 45), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                             DEFAULT_PITCH, L"Consolas");
    HGDIOBJ old_font = SelectObject(dc, font);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetBkMode(dc, OPAQUE);
    SetTextColor(dc, RGB(0, 0, 0));
    const int pen_width = std::max(2, width / 640);
    for (const Box& box : boxes)
    {
        const COLORREF color = kColors[box.number % std::size(kColors)];
        HPEN pen = CreatePen(PS_SOLID, pen_width, color);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        Rectangle(dc, box.rect.left, box.rect.top, box.rect.right, box.rect.bottom);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        const std::wstring label = std::to_wstring(box.number);
        SetBkColor(dc, color);
        TextOutW(dc, box.rect.left + pen_width, box.rect.top + pen_width, label.c_str(),
                 static_cast<int>(label.size()));
    }
    GdiFlush();
    for (int y = 0; y < height; ++y)
    {
        unsigned char* row = pixels + static_cast<std::size_t>(y) * pitch;
        std::memcpy(row, dib + y * row_bytes, row_bytes);
        for (int x = 0; x < width; ++x)
            row[x * 4 + 3] = 0xFF;
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

fs::path content_log_file()
{
    fs::path base;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)))
        base = p;
    CoTaskMemFree(p);
    std::error_code ec;
    fs::path newest;
    for (const auto& entry : fs::directory_iterator(base / L"Minecraft Bedrock" / L"logs", ec))
    {
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"ContentLog", 0) == 0 && entry.path().extension() == L".txt" &&
            (newest.empty() || name > newest.filename().wstring()))
            newest = entry.path();
    }
    return newest;
}

std::uintmax_t size_of(const fs::path& path)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : size;
}

std::string read_from(const fs::path& path, std::uintmax_t offset)
{
    constexpr std::uintmax_t kMaxBytes = 256 * 1024;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    in.seekg(0, std::ios::end);
    const std::uintmax_t end = static_cast<std::uintmax_t>(in.tellg());
    if (offset > end)
        offset = 0;
    if (end - offset > kMaxBytes)
        offset = end - kMaxBytes;
    std::string bytes(static_cast<std::size_t>(end - offset), '\0');
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

std::vector<std::string> log_lines(const std::string& text, const std::string& filter)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty() && (filter.empty() || line.find(filter) != std::string::npos))
            lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

std::string screen_name()
{
    const std::string name = cx::nativeScreenMetrics().diagnostics(std::chrono::steady_clock::now()).name.data();
    return name == "Unavailable" ? "none (Ore UI screen, loading, or between screens)" : name;
}

std::atomic<bool> g_auto_reload{true};
std::mutex g_watch_mutex;
std::string g_watch_status = "no change seen yet";
std::string g_pending_file;
std::chrono::steady_clock::time_point g_pending_since{};
bool g_pending = false;

std::string utf8(const std::wstring& wide)
{
    if (wide.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

bool is_ui_json(std::wstring path)
{
    for (auto& c : path)
        c = static_cast<wchar_t>(towlower(c));
    const bool json = path.size() > 5 && path.compare(path.size() - 5, 5, L".json") == 0;
    return json && (path.rfind(L"ui\\", 0) == 0 || path.find(L"\\ui\\") != std::wstring::npos);
}

void watch_dir(std::wstring dir)
{
    HANDLE handle = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        std::lock_guard lock(g_watch_mutex);
        g_watch_status = "cannot watch " + utf8(dir);
        return;
    }
    alignas(DWORD) unsigned char buffer[64 * 1024];
    for (;;)
    {
        DWORD bytes = 0;
        if (!ReadDirectoryChangesW(handle, buffer, sizeof(buffer), TRUE,
                                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME, &bytes, nullptr,
                                   nullptr))
            break;
        if (bytes == 0)
            continue;
        for (auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);;
             info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<unsigned char*>(info) +
                                                               info->NextEntryOffset))
        {
            std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            if (is_ui_json(name))
            {
                std::lock_guard lock(g_watch_mutex);
                g_pending = true;
                g_pending_file = utf8(name);
                g_pending_since = std::chrono::steady_clock::now();
            }
            if (!info->NextEntryOffset)
                break;
        }
    }
    CloseHandle(handle);
}

void auto_reload_loop()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::string file;
        {
            std::lock_guard lock(g_watch_mutex);
            if (!g_pending || std::chrono::steady_clock::now() - g_pending_since < std::chrono::milliseconds(400))
                continue;
            g_pending = false;
            file = g_pending_file;
        }
        if (!g_auto_reload.load())
            continue;
        const Result result = reload(false);
        std::lock_guard lock(g_watch_mutex);
        g_watch_status = "reloaded after " + file + " changed: " +
                         result.text.substr(0, result.text.find('\n'));
    }
}

} // namespace

void init()
{
    g_auto_reload.store(config::ui_auto_reload());
    std::thread(install_loop).detach();
    std::thread(hotkey_loop).detach();
    for (const auto& dir : config::ui_watch_dirs())
        std::thread(watch_dir, dir).detach();
    std::thread(auto_reload_loop).detach();
}

Result status()
{
    std::lock_guard lock(g_backend_mutex);
    return {g_backend != nullptr, "install: " + g_install_status + "; reload: " + cx::legacyReloadState()};
}

Result reload_once()
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

Result reload(bool diff)
{
    const fs::path log = content_log_file();
    const std::uintmax_t log_start = log.empty() ? 0 : size_of(log);
    Snapshot before;
    if (diff)
        before = collect(false, 256);
    Result result = reload_once();
    if (!result.ok)
        return result;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (diff)
    {
        Snapshot after = collect(false, 256);
        if (before.ok && after.ok)
            result.text += "\n" + diff_text(before, after);
        else
            result.text += "\nno tree diff: " + (before.ok ? after.error : before.error);
    }
    if (!log.empty())
    {
        const auto lines = log_lines(read_from(log, log_start), {});
        if (!lines.empty())
        {
            result.text += "\ncontent log during reload:\n";
            for (const auto& line : lines)
                result.text += "  " + line + "\n";
        }
    }
    return result;
}

Result wait_for_screen(const std::string& name, const std::string& leave, int timeout_ms)
{
    if (name.empty() && leave.empty())
        return {false, "give 'name' (screen to reach) or 'leave' (screen to move away from)"};
    timeout_ms = std::clamp(timeout_ms, 0, 30000);
    const auto start = std::chrono::steady_clock::now();
    std::string current;
    for (;;)
    {
        current = screen_name();
        const bool matched = (name.empty() || current.find(name) != std::string::npos) &&
                             (leave.empty() || current.find(leave) == std::string::npos);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (matched)
            return {true, "screen: " + current + " (after " + std::to_string(elapsed) + " ms)"};
        if (elapsed >= timeout_ms)
            return {false, "still on screen " + current + " after " + std::to_string(elapsed) +
                               " ms; call again to keep waiting"};
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

Target find(const std::string& name, int index)
{
    Target target;
    if (name.empty())
    {
        target.text = "name is required";
        return target;
    }
    Snapshot snap = collect(false, 256);
    if (!snap.ok)
    {
        target.text = snap.error;
        return target;
    }
    const auto usable = [&](const Item& item) {
        const auto& n = item.node;
        return n.visible && n.positioned && n.width != 0 && n.height != 0;
    };
    std::vector<int> matches;
    for (int pass = 0; pass < 2 && matches.empty(); ++pass)
        for (std::size_t i = 0; i < snap.items.size(); ++i)
        {
            const Item& item = snap.items[i];
            const bool hit = pass == 0 ? item.name == name : item.name.find(name) != std::string::npos;
            if (hit && usable(item))
                matches.push_back(static_cast<int>(i));
        }
    if (matches.empty())
    {
        target.text = "no visible control named or containing '" + name + "'";
        return target;
    }
    if (index < 0 || index >= static_cast<int>(matches.size()))
    {
        target.text = std::to_string(matches.size()) + " matches; index must be 0-" +
                      std::to_string(matches.size() - 1);
        return target;
    }
    const Scale s = scale_of(snap);
    const Item& item = snap.items[matches[index]];
    target.x = static_cast<int>(std::lround((item.node.x + item.node.width / 2) * s.sx));
    target.y = static_cast<int>(std::lround((item.node.y + item.node.height / 2) * s.sy));
    target.ok = true;
    target.text = "clicked " + label(item) + " at " + std::to_string(target.x) + "," + std::to_string(target.y) +
                  " (rect " + rect_text(item, s) + ", match " + std::to_string(index + 1) + " of " +
                  std::to_string(matches.size()) + ")\npath: " + path_of(snap, matches[index]);
    return target;
}

Annotated annotate(const AnnotateOptions& options)
{
    Annotated out;
    Snapshot snap = collect(false, std::clamp(options.max_depth, 0, 256));
    if (!snap.ok)
    {
        out.shot = {false, {}, snap.error};
        return out;
    }
    const Scale s = scale_of(snap);
    std::vector<Box> boxes;
    for (std::size_t i = 1; i < snap.items.size() && static_cast<int>(boxes.size()) < options.limit; ++i)
    {
        const Item& item = snap.items[i];
        const auto& n = item.node;
        if (!n.visible || !n.positioned || n.width == 0 || n.height == 0)
            continue;
        if (!options.filter.empty() && item.name.find(options.filter) == std::string::npos)
            continue;
        if (!options.type.empty() && item.type != options.type)
            continue;
        const double left = std::min<double>(n.x, n.x + n.width), top = std::min<double>(n.y, n.y + n.height);
        RECT rect{static_cast<LONG>(std::lround(left * s.sx)), static_cast<LONG>(std::lround(top * s.sy)),
                  static_cast<LONG>(std::lround((left + std::abs(n.width)) * s.sx)),
                  static_cast<LONG>(std::lround((top + std::abs(n.height)) * s.sy))};
        const int number = static_cast<int>(boxes.size()) + 1;
        boxes.push_back({rect, number});
        std::string path = path_of(snap, static_cast<int>(i));
        std::size_t cut = path.size();
        for (int parts = 0; parts < 3 && cut != std::string::npos && cut > 0; ++parts)
            cut = path.rfind(" / ", cut - 1);
        if (cut != std::string::npos && cut > 0)
            path = "... / " + path.substr(cut + 3);
        out.legend += std::to_string(number) + ": " + path +
                      (item.type.empty() || item.type == "unknown" ? "" : " [" + item.type + "]") + "  " +
                      rect_text(item, s) + "\n";
    }
    if (boxes.empty())
        out.legend = "no visible control matched; nothing drawn\n";
    out.shot = capture::screenshot(options.width, options.height,
                                   [&boxes](unsigned char* bgra, unsigned pitch, int width, int height) {
                                       paint_boxes(bgra, pitch, width, height, boxes);
                                   });
    return out;
}

Result overlay(int panel, int inspector)
{
    if (!g_overlay_running.load())
        return {false, "the overlay is not running (Conduit hooks not installed). " + status().text};
    if (panel >= 0)
        g_overlay_stats.diagnosticsRequest.store(panel ? 1 : 0);
    if (inspector >= 0)
        g_overlay_stats.tooltipRequest.store(inspector ? 1 : 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return {true, std::string("F3 panel: ") + (g_overlay_stats.diagnosticsShown.load() ? "on" : "off") +
                      ", F5 inspector: " + (g_overlay_stats.tooltipShown.load() ? "on" : "off") +
                      " (the inspector follows the real mouse cursor and draws only while the game window "
                      "has focus)"};
}

Result auto_reload(int enabled)
{
    if (enabled >= 0)
        g_auto_reload.store(enabled != 0);
    std::lock_guard lock(g_watch_mutex);
    return {true, std::string("auto reload: ") + (g_auto_reload.load() ? "on" : "off") + "; last: " +
                      g_watch_status};
}

Result content_log(int lines, const std::string& filter)
{
    const fs::path log = content_log_file();
    if (log.empty())
        return {false, "no ContentLog*.txt in %APPDATA%\\Minecraft Bedrock\\logs; the game writes it only "
                       "when its content log file option is on"};
    auto all = log_lines(read_from(log, 0), filter);
    lines = std::clamp(lines, 1, 500);
    const std::size_t first = all.size() > static_cast<std::size_t>(lines) ? all.size() - lines : 0;
    std::string written = "unknown";
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    FILETIME local{};
    SYSTEMTIME time{};
    if (GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &attributes) &&
        FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &time))
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth, time.wDay,
                      time.wHour, time.wMinute, time.wSecond);
        written = buf;
    }
    std::string out = utf8(log.filename().wstring()) + " (last written " + written + " local time; compare "
                      "with the game start, it can be from an earlier session): " + std::to_string(all.size()) +
                      " matching lines\n";
    for (std::size_t i = first; i < all.size(); ++i)
        out += all[i] + "\n";
    return {true, out};
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
