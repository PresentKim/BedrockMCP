#include "options.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <vector>

#include <MinHook.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include "config.hpp"
#include "minecraft/compatibility.hpp"
#include "ui.hpp"

namespace options {
namespace {

// OptionRegistry layout in Minecraft.Windows.exe 1.26.65.1; docs/porting.md records how it was found.
struct Layout {
    std::uintptr_t get_rva;
    std::array<std::uint8_t, 16> get_prologue;
    std::size_t options;
    int option_count;
    std::size_t path;
    std::size_t vt_save_if_needed;
    std::size_t vt_elevate_save;
    std::size_t option_impl;
    std::size_t vt_load_string;
    std::size_t impl_tag;
    std::size_t impl_type;
    std::size_t impl_override;
    std::size_t impl_owner_registry;
};

constexpr Layout k126651{0x5af9150,
                         {0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xd6, 0x44, 0x89, 0xc2, 0xe8, 0xb0, 0xfd, 0xff, 0xff},
                         0x10,
                         0x339,
                         0x1a08,
                         0xa8,
                         0x118,
                         0x8,
                         0x20,
                         0x188,
                         0x1d4,
                         0x238,
                         0x268};

enum OptionType { Bool = 0, InputModeBool = 1, Float = 2, InputModeFloat = 3, String = 4, Int = 5, Enum = 6,
                  StringList = 8 };

using GetFn = void* (*)(void*, void*, int);
using LoadStringFn = void (*)(void*, const std::string*);
using RegistryVoidFn = void (*)(void*);
using ElevateFn = void (*)(void*, int);

const Layout* g_layout = nullptr;
GetFn g_get = nullptr;
std::mutex g_registries_mutex;
std::set<std::uintptr_t> g_registries;
std::string g_status = "installing";

void* on_get(void* registry, void* out, int id)
{
    {
        std::lock_guard lock(g_registries_mutex);
        g_registries.insert(reinterpret_cast<std::uintptr_t>(registry));
    }
    return g_get(registry, out, id);
}

template <typename T> T at(const void* base, std::size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
    return value;
}

std::string game_string(const void* base, std::size_t offset)
{
    const auto* s = static_cast<const std::uint8_t*>(base) + offset;
    const auto size = at<std::size_t>(s, 0x10);
    const auto capacity = at<std::size_t>(s, 0x18);
    const char* data = capacity > 15 ? at<const char*>(s, 0) : reinterpret_cast<const char*>(s);
    if (size > 4096)
        return {};
    return std::string(data, size);
}

void* vfunc(void* object, std::size_t slot)
{
    return at<void**>(object, 0)[slot / sizeof(void*)];
}

std::string lower(std::string s);

void* user_registry(std::string& error)
{
    std::set<std::uintptr_t> registries;
    {
        std::lock_guard lock(g_registries_mutex);
        registries = g_registries;
    }
    if (registries.empty())
    {
        error = "no OptionRegistry captured yet (hooks: " + g_status +
                "); the game reads an option within a few frames of any screen change";
        return nullptr;
    }
    const auto wanted_u8 = config::game_options_path().u8string();
    const std::string wanted = lower(std::string(wanted_u8.begin(), wanted_u8.end()));
    void* fallback = nullptr;
    for (auto r : registries)
    {
        void* registry = reinterpret_cast<void*>(r);
        std::string path = lower(game_string(registry, g_layout->path));
        std::replace(path.begin(), path.end(), '/', '\\');
        if (!wanted.empty() && path == wanted)
            return registry;
        if (path.find("\\users\\") != std::string::npos && path.find("\\shared\\") == std::string::npos)
            fallback = registry;
    }
    if (fallback)
        return fallback;
    error = "captured " + std::to_string(registries.size()) + " OptionRegistry objects but none uses " + wanted +
            "; their paths:";
    for (auto r : registries)
        error += " [" + game_string(reinterpret_cast<void*>(r), g_layout->path) + "]";
    return nullptr;
}

void* option_at(void* registry, int id)
{
    return at<void*>(registry, g_layout->options + static_cast<std::size_t>(id) * sizeof(void*));
}

void* impl_of(void* option)
{
    return at<void*>(option, g_layout->option_impl);
}

std::string type_name(int type)
{
    switch (type)
    {
    case Bool: return "bool";
    case InputModeBool: return "bool per input mode";
    case Float: return "float";
    case InputModeFloat: return "float per input mode";
    case String: return "string";
    case Int: return "int";
    case Enum: return "enum";
    case StringList: return "string list";
    }
    return "type " + std::to_string(type);
}

std::string value_text(void* option, int type)
{
    char buf[128];
    switch (type)
    {
    case Bool:
        return at<std::uint8_t>(option, 0x10) ? "1" : "0";
    case Float:
        std::snprintf(buf, sizeof(buf), "%g (range %g to %g)", at<float>(option, 0x18), at<float>(option, 0x10),
                      at<float>(option, 0x14));
        return buf;
    case Int:
    case Enum:
        std::snprintf(buf, sizeof(buf), "%d (range %d to %d)", at<int>(option, 0x18), at<int>(option, 0x14),
                      at<int>(option, 0x10));
        return buf;
    case String:
        return game_string(option, 0x10);
    case StringList: {
        const auto begin = at<std::uintptr_t>(option, 0x10), end = at<std::uintptr_t>(option, 0x18);
        return std::to_string((end - begin) / 0x20) + " entries";
    }
    }
    return "(not shown)";
}

// The string loader does not clamp or convert units (gfx_viewdistance takes chunks, not the
// blocks options.txt stores), so numbers are checked against the option's own range first.
bool value_in_range(void* option, int type, const std::string& value, std::string& error)
{
    char* end = nullptr;
    if (type == Bool)
    {
        if (value == "0" || value == "1" || value == "true" || value == "false")
            return true;
        error = "bool options take 0 or 1";
        return false;
    }
    if (type == Int || type == Enum)
    {
        const long long v = std::strtoll(value.c_str(), &end, 10);
        const int low = at<int>(option, 0x14), high = at<int>(option, 0x10);
        if (end == value.c_str() || *end != '\0' || v < low || v > high)
        {
            error = "value must be an integer from " + std::to_string(low) + " to " + std::to_string(high) +
                    ", in the units options_get shows";
            return false;
        }
        return true;
    }
    if (type == Float)
    {
        const double v = std::strtod(value.c_str(), &end);
        const float low = at<float>(option, 0x10), high = at<float>(option, 0x14);
        if (end == value.c_str() || *end != '\0' || v < low || v > high)
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "value must be a number from %g to %g", low, high);
            error = buf;
            return false;
        }
        return true;
    }
    return true;
}

void install_loop()
{
    try
    {
        if (conduit::minecraft::requireRunningCompatibility().build != "1.26.65.1")
        {
            g_status = "unsupported game build; live options exist only for 1.26.65.1";
            return;
        }
    }
    catch (const std::exception&)
    {
        g_status = "unsupported game build; live options exist only for 1.26.65.1";
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (int attempt = 0; attempt < 600; ++attempt)
    {
        if (conduit::minecraft::matchesMainExecutableCode(k126651.get_rva, k126651.get_prologue))
        {
            MH_STATUS init = MH_Initialize();
            void* target = reinterpret_cast<void*>(base + k126651.get_rva);
            if ((init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) ||
                MH_CreateHook(target, reinterpret_cast<void*>(&on_get), reinterpret_cast<void**>(&g_get)) != MH_OK ||
                MH_EnableHook(target) != MH_OK)
            {
                g_status = "failed to hook OptionRegistry::get";
                return;
            }
            g_layout = &k126651;
            g_status = "installed";
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    g_status = "OptionRegistry::get never matched its expected bytes";
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> terms(const std::string& filter)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= filter.size())
    {
        size_t end = filter.find(',', start);
        if (end == std::string::npos)
            end = filter.size();
        std::string term = filter.substr(start, end - start);
        term.erase(0, term.find_first_not_of(' '));
        term.erase(term.find_last_not_of(' ') + 1);
        if (!term.empty())
            out.push_back(lower(term));
        start = end + 1;
    }
    return out;
}

std::string written_at(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    FILETIME local{};
    SYSTEMTIME t{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        !FileTimeToLocalFileTime(&data.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &t))
        return "unknown";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond);
    return buf;
}

} // namespace

Result get(const std::string& filter, int limit)
{
    const auto path = config::game_options_path();
    std::ifstream in(path);
    if (path.empty() || !in)
        return {false, "options.txt not found; set options_path in config.json"};
    const auto wanted = terms(filter);
    limit = std::clamp(limit, 1, 2000);
    std::string out;
    int total = 0, shown = 0;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = lower(line.substr(0, colon));
        if (!wanted.empty() &&
            std::none_of(wanted.begin(), wanted.end(), [&](const std::string& t) { return key.find(t) != std::string::npos; }))
            continue;
        ++total;
        if (shown >= limit)
            continue;
        ++shown;
        out += line + "\n";
    }
    const auto utf8 = path.u8string();
    std::string head = std::string(utf8.begin(), utf8.end()) + " (last written " + written_at(path) +
                       " local time by the game; it holds the values the game last saved): " + std::to_string(total) +
                       " matching options\n";
    if (total > shown)
        out += "... " + std::to_string(total - shown) + " more; raise limit or narrow filter\n";
    return {true, head + out};
}

void init()
{
    std::thread(install_loop).detach();
}

Result live(const std::string& filter, int limit)
{
    if (!g_layout)
        return {false, "live options are not available: " + g_status};
    const auto wanted = terms(filter);
    limit = std::clamp(limit, 1, 2000);
    std::string error, out;
    int total = 0, shown = 0;
    const bool ran = ui::on_ui_thread([&] {
        void* registry = user_registry(error);
        if (!registry)
            return;
        for (int id = 0; id < g_layout->option_count; ++id)
        {
            void* option = option_at(registry, id);
            if (!option)
                continue;
            void* impl = impl_of(option);
            if (!impl)
                continue;
            const std::string tag = game_string(impl, g_layout->impl_tag);
            if (tag.empty())
                continue;
            const std::string key = lower(tag);
            if (!wanted.empty() && std::none_of(wanted.begin(), wanted.end(),
                                                [&](const std::string& t) { return key.find(t) != std::string::npos; }))
                continue;
            ++total;
            if (shown >= limit)
                continue;
            ++shown;
            void* source = at<void*>(impl, g_layout->impl_override);
            void* shown_option = source ? source : option;
            const int type = at<int>(impl_of(shown_option), g_layout->impl_type);
            out += tag + " = " + value_text(shown_option, type) + " [" + type_name(type) + (source ? ", shared" : "") +
                   "]\n";
        }
    });
    if (!ran)
        return {false, "the game's UI thread did not run a frame within 3 seconds"};
    if (!error.empty())
        return {false, error};
    std::string head = std::to_string(total) + " matching options in the running game\n";
    if (total > shown)
        out += "... " + std::to_string(total - shown) + " more; raise limit or narrow filter\n";
    return {true, head + out};
}

Result set(const std::string& key, const std::string& value)
{
    if (!g_layout)
        return {false, "live options are not available: " + g_status};
    if (key.empty() || value.size() > 4096)
        return {false, "key is required and value must be at most 4096 bytes"};
    std::string error, before, after;
    bool found = false;
    const bool ran = ui::on_ui_thread([&] {
        void* registry = user_registry(error);
        if (!registry)
            return;
        for (int id = 0; id < g_layout->option_count && !found; ++id)
        {
            void* option = option_at(registry, id);
            if (!option || !impl_of(option) || game_string(impl_of(option), g_layout->impl_tag) != key)
                continue;
            found = true;
            if (void* source = at<void*>(impl_of(option), g_layout->impl_override))
                option = source;
            void* impl = impl_of(option);
            const int type = at<int>(impl, g_layout->impl_type);
            if (type == InputModeBool || type == InputModeFloat)
            {
                error = key + " is a per-input-mode option, which has no string setter";
                return;
            }
            before = value_text(option, type);
            if (!value_in_range(option, type, value, error))
                return;
            const std::string text = value;
            reinterpret_cast<LoadStringFn>(vfunc(option, g_layout->vt_load_string))(option, &text);
            after = value_text(option, type);
            void* owner = at<void*>(impl, g_layout->impl_owner_registry);
            void* save_registry = owner ? owner : registry;
            reinterpret_cast<ElevateFn>(vfunc(save_registry, g_layout->vt_elevate_save))(save_registry, 1);
            reinterpret_cast<RegistryVoidFn>(vfunc(save_registry, g_layout->vt_save_if_needed))(save_registry);
        }
    });
    if (!ran)
        return {false, "the game's UI thread did not run a frame within 3 seconds"};
    if (!error.empty())
        return {false, error};
    if (!found)
        return {false, "no option with save name '" + key + "'; list them with options_get live=true"};
    if (before == after)
        return {true, key + " unchanged: " + after + " (value rejected, out of range, or already set)"};
    return {true, key + ": " + before + " changed to " + after + "; save requested"};
}

} // namespace options
