#include "spyglass.hpp"

#include <windows.h>

#include <psapi.h>

#include <MinHook.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <thread>

namespace spyglass_control {
namespace {

// Offsets from the PDB shipped with the spyglass v0.4.0 release (spyglass-0.4.0-1.26.60.pdb):
// spyglass::View::getInstance is public symbol 0001:550928, View::visible_ is a bool at
// offset 3128, and spyglass::decode_fields(Packet&, int) is 0001:593680. A DLL with another
// build stamp is left alone.
struct Build {
    std::uint32_t time_stamp;
    std::uint32_t image_size;
    std::uintptr_t get_instance_rva;
    std::array<std::uint8_t, 10> get_instance_prologue;
    std::size_t visible_offset;
    std::uintptr_t decode_fields_rva;
    std::array<std::uint8_t, 10> decode_fields_prologue;
};

constexpr Build kBuilds[] = {
    {0x6af39e24, 0x203000, 0x87810, {0x55, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8d, 0x6c, 0x24, 0x30}, 3128, 0x91f10,
     {0x55, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x81, 0xec, 0x70}},
};

using GetInstanceFn = void* (*)();
using DecodeFieldsFn = void* (*)(void*, void*, int);
DecodeFieldsFn g_decode_fields = nullptr;

// The 1.26.60 spyglass build reads the game's reflection data to decode some packets as they
// arrive, and on 1.26.65.1 that crashed the game (access violation in decode_fields, entt
// meta_type::from_void) while joining a server that sends resource packs. Returning a null
// nlohmann::json (16 zero bytes: type null, empty value) keeps the bytes captured without it.
void* skip_decode_fields(void* result, void*, int)
{
    std::memset(result, 0, 16);
    return result;
}

void disable_eager_decoding(HMODULE module, const Build& build)
{
    auto* target = reinterpret_cast<std::uint8_t*>(module) + build.decode_fields_rva;
    if (std::memcmp(target, build.decode_fields_prologue.data(), build.decode_fields_prologue.size()) != 0)
        return;
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        return;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&skip_decode_fields),
                      reinterpret_cast<void**>(&g_decode_fields)) == MH_OK)
        MH_EnableHook(target);
}

struct Located {
    HMODULE module = nullptr;
    std::wstring name;
    const Build* build = nullptr;
};

Located locate()
{
    Located found;
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return found;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i)
    {
        wchar_t name[MAX_PATH] = {};
        if (!GetModuleBaseNameW(GetCurrentProcess(), modules[i], name, MAX_PATH))
            continue;
        std::wstring lower = name;
        for (auto& c : lower)
            c = static_cast<wchar_t>(towlower(c));
        if (lower.rfind(L"spyglass-", 0) != 0)
            continue;
        found.module = modules[i];
        found.name = name;
        auto* base = reinterpret_cast<const std::uint8_t*>(modules[i]);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        for (const Build& build : kBuilds)
            if (nt->FileHeader.TimeDateStamp == build.time_stamp &&
                nt->OptionalHeader.SizeOfImage == build.image_size &&
                std::memcmp(base + build.get_instance_rva, build.get_instance_prologue.data(),
                            build.get_instance_prologue.size()) == 0)
                found.build = &build;
        return found;
    }
    return found;
}

bool* visible_flag(const Located& found)
{
    auto get_instance =
        reinterpret_cast<GetInstanceFn>(reinterpret_cast<std::uintptr_t>(found.module) + found.build->get_instance_rva);
    return reinterpret_cast<bool*>(static_cast<std::uint8_t*>(get_instance()) + found.build->visible_offset);
}

std::string narrow(const std::wstring& s)
{
    std::string out;
    for (wchar_t c : s)
        out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

} // namespace

void init(bool start_hidden)
{
    std::thread([start_hidden] {
        for (int i = 0; i < 12000; ++i)
        {
            Located found = locate();
            if (found.module)
            {
                if (!found.build)
                    return;
                disable_eager_decoding(found.module, *found.build);
                if (!start_hidden)
                    return;
                std::this_thread::sleep_for(std::chrono::seconds(3));
                *visible_flag(found) = false;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
}

Result overlay(int visible)
{
    Located found = locate();
    if (!found.module)
        return {false, "spyglass is not loaded in the game"};
    if (!found.build)
        return {false, narrow(found.name) + " is a spyglass build this payload has no offsets for; use F12 in game"};
    bool* flag = visible_flag(found);
    if (visible >= 0)
        *flag = visible != 0;
    return {true, std::string("spyglass overlay: ") + (*flag ? "shown" : "hidden") +
                      " (F12 in game toggles it too)"};
}

} // namespace spyglass_control
