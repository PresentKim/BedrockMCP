#include "config.hpp"

#include <windows.h>

#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace config {
namespace {

using Json = nlohmann::json;

std::unordered_map<std::string, int> g_options_keys;    // Minecraft action -> Windows VK
std::unordered_map<std::string, std::string> g_overrides; // action -> key name (manual)
int g_port = 18080;
std::string g_auth_token;
std::string g_input_mode = "feed";

// MouseDevice::feed prologue signature (unique on 1.26.40-26.52).
const char* kDefaultFeedSig =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 44 89 CF 44 89 C3 89 D5 "
    "48 89 CE 44 0F B7 A4 24 C0 00 00 00 44 0F B7 AC 24 B8 00 00 00 44 0F B7 "
    "BC 24 B0 00 00 00 0F B6 84 24 C8 00 00 00";
std::string g_feed_signature = kDefaultFeedSig;
bool g_ui_auto_reload = true;
bool g_spyglass_start_hidden = true;
std::vector<std::wstring> g_ui_watch_dirs;

// Vanilla Bedrock defaults, used only when options.txt cannot be read.
const std::unordered_map<std::string, std::string>& fallback()
{
    static const std::unordered_map<std::string, std::string> f = {
        {"chat", "t"},       {"command", "/"},  {"inventory", "e"}, {"forward", "w"},
        {"back", "s"},       {"left", "a"},     {"right", "d"},     {"jump", "space"},
        {"sneak", "shift"},  {"sprint", "ctrl"}, {"drop", "q"},     {"pause", "escape"},
    };
    return f;
}

fs::path appdata()
{
    PWSTR p = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)))
        base = p;
    CoTaskMemFree(p);
    return base;
}

// Directory of this DLL (where the injector and config.json sit).
fs::path dll_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&dll_dir), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    return fs::path(path).parent_path();
}

fs::path config_path()
{
    return dll_dir() / L"config.json";
}

// Pick the most recently modified options.txt across all Bedrock user profiles.
fs::path auto_options_path()
{
    fs::path users = appdata() / L"Minecraft Bedrock" / L"Users";
    std::error_code ec;
    fs::path best;
    fs::file_time_type best_time{};
    if (!fs::is_directory(users, ec))
        return {};
    for (const auto& user : fs::directory_iterator(users, ec))
    {
        fs::path opt = user.path() / L"games" / L"com.mojang" / L"minecraftpe" / L"options.txt";
        if (!fs::exists(opt, ec))
            continue;
        auto t = fs::last_write_time(opt, ec);
        if (best.empty() || t > best_time)
        {
            best = opt;
            best_time = t;
        }
    }
    return best;
}

void parse_options(const fs::path& path)
{
    g_options_keys.clear();
    std::ifstream in(path);
    if (!in)
        return;
    const std::string prefix = "keyboard_type_0_key.";
    std::string line;
    while (std::getline(in, line))
    {
        if (line.rfind(prefix, 0) != 0)
            continue;
        auto colon = line.find(':', prefix.size());
        if (colon == std::string::npos)
            continue;
        std::string action = line.substr(prefix.size(), colon - prefix.size());
        std::string value = line.substr(colon + 1);
        // A binding can list several codes ("84,13"); the first is the primary key.
        int vk = std::atoi(value.c_str());
        if (vk > 0)
            g_options_keys[action] = vk;
    }
}

} // namespace

void load()
{
    Json j{{"port", 18080},
           {"auth_token", ""},
           {"input_mode", "feed"},
           {"feed_signature", kDefaultFeedSig},
           {"options_path", ""},
           {"keybind_overrides", Json::object()},
           {"ui_auto_reload", true},
           {"ui_watch_dirs", Json::array()},
           {"spyglass_start_hidden", true}};

    fs::path path = config_path();
    std::error_code ec;
    if (fs::exists(path, ec))
    {
        try
        {
            std::ifstream in(path);
            j.merge_patch(Json::parse(in, nullptr, true, true));
        }
        catch (...)
        {
        }
    }
    else
    {
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path);
        out << j.dump(2);
    }

    g_port = j.value("port", 18080);
    g_auth_token = j.value("auth_token", std::string());
    g_input_mode = j.value("input_mode", std::string("feed"));
    g_feed_signature = j.value("feed_signature", std::string(kDefaultFeedSig));
    if (g_feed_signature.empty())
        g_feed_signature = kDefaultFeedSig;
    g_overrides.clear();
    if (j["keybind_overrides"].is_object())
        for (auto& [action, key] : j["keybind_overrides"].items())
            if (key.is_string())
                g_overrides[action] = key.get<std::string>();

    g_ui_auto_reload = j.value("ui_auto_reload", true);
    g_spyglass_start_hidden = j.value("spyglass_start_hidden", true);
    g_ui_watch_dirs.clear();
    if (j["ui_watch_dirs"].is_array())
        for (const auto& dir : j["ui_watch_dirs"])
            if (dir.is_string())
                g_ui_watch_dirs.push_back(fs::u8path(dir.get<std::string>()).wstring());

    std::string configured = j.value("options_path", std::string());
    fs::path options = configured.empty() ? auto_options_path() : fs::path(configured);
    if (!options.empty())
        parse_options(options);
}

std::string key_for(const std::string& action)
{
    auto ov = g_overrides.find(action);
    if (ov != g_overrides.end())
        return ov->second;

    auto opt = g_options_keys.find(action);
    if (opt != g_options_keys.end())
        return "vk:" + std::to_string(opt->second);

    auto fb = fallback().find(action);
    if (fb != fallback().end())
        return fb->second;

    return action;
}

int port()
{
    return g_port;
}

std::string auth_token()
{
    return g_auth_token;
}

std::string input_mode()
{
    return g_input_mode;
}

std::string feed_signature()
{
    return g_feed_signature;
}

bool ui_auto_reload()
{
    return g_ui_auto_reload;
}

bool spyglass_start_hidden()
{
    return g_spyglass_start_hidden;
}

std::vector<std::wstring> ui_watch_dirs()
{
    if (!g_ui_watch_dirs.empty())
        return g_ui_watch_dirs;
    return {(appdata() / L"Minecraft Bedrock" / L"Users").wstring()};
}

} // namespace config
