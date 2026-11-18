#include "packets.hpp"

#include <windows.h>

#include <shlobj.h>

#include <MinHook.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "minecraft/compatibility.hpp"

namespace packets {
namespace {

namespace fs = std::filesystem;

// Minecraft.Windows.exe 1.26.65.1, checked against LeviLamina 26.60.8 and Endstone headers:
// NetworkSystem::_sendInternal(NetworkIdentifier const&, Packet const&, std::string const& data),
// PacketTraceNetworkPeer::_receivePacket(std::string& out, time_point const&) returning DataStatus,
// LoopbackPacketSender::sendToServer(Packet&). NetworkSystem passes its mReceiveBuffer at +0x150.
struct Target {
    std::uintptr_t rva;
    std::array<std::uint8_t, 12> prologue;
};
constexpr Target kSendInternal{0x23830c0, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x53}};
constexpr Target kReceive{0x226b2b0, {0x56, 0x57, 0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0xd7, 0x48, 0x89, 0xce}};
constexpr Target kSendToServer{0x2360d40, {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x81}};
constexpr std::uintptr_t kReceiveBufferOffset = 0x150;
constexpr std::size_t kNetworkIdentifierSize = 0xb0;
constexpr int kDataStatusHasData = 0;

constexpr std::uint8_t kSent = 0, kReceived = 1;
constexpr std::uint8_t kSideUnknown = 0, kSideClient = 1, kSideServer = 2;

using SendInternalFn = void (*)(void*, const void*, const void*, const void*);
using ReceiveFn = int (*)(void*, void*, const void*);
using SendToServerFn = std::uintptr_t (*)(void*, void*);

SendInternalFn g_send_internal = nullptr;
ReceiveFn g_receive = nullptr;
SendToServerFn g_send_to_server = nullptr;

std::atomic<bool> g_installed{false};
std::string g_install_status = "installing";
std::atomic<bool> g_logging{false};

std::mutex g_buffer_mutex;
std::vector<char> g_buffer;
std::uint64_t g_records = 0, g_bytes = 0, g_dropped = 0;
fs::path g_file, g_last_file;

std::mutex g_side_mutex;
std::set<std::uintptr_t> g_client_systems;
thread_local bool t_in_send_to_server = false;

constexpr std::size_t kMaxBuffered = 64u << 20;

std::string_view msvc_string(const void* str)
{
    auto* base = static_cast<const std::uint8_t*>(str);
    const std::size_t size = *reinterpret_cast<const std::size_t*>(base + 0x10);
    const std::size_t capacity = *reinterpret_cast<const std::size_t*>(base + 0x18);
    const char* data = capacity > 15 ? *reinterpret_cast<const char* const*>(base)
                                     : reinterpret_cast<const char*>(base);
    return {data, size};
}

std::uint8_t side_of(std::uintptr_t system)
{
    std::lock_guard lock(g_side_mutex);
    if (g_client_systems.count(system))
        return kSideClient;
    return g_client_systems.empty() ? kSideUnknown : kSideServer;
}

std::uint64_t fnv1a(const void* data, std::size_t size)
{
    std::uint64_t h = 1469598103934665603ull;
    auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

void append(std::uint8_t dir, std::uint8_t side, std::uint64_t conn, std::string_view data)
{
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    char fixed[24]{};
    std::memcpy(fixed, &micros, 8);
    fixed[8] = static_cast<char>(dir);
    fixed[9] = static_cast<char>(side);
    std::memcpy(fixed + 12, &conn, 8);
    const auto size = static_cast<std::uint32_t>(data.size());
    std::memcpy(fixed + 20, &size, 4);
    std::lock_guard lock(g_buffer_mutex);
    if (!g_logging.load() || g_buffer.size() + sizeof(fixed) + data.size() > kMaxBuffered)
    {
        if (g_logging.load())
            ++g_dropped;
        return;
    }
    g_buffer.insert(g_buffer.end(), fixed, fixed + sizeof(fixed));
    g_buffer.insert(g_buffer.end(), data.begin(), data.end());
    ++g_records;
    g_bytes += data.size();
}

void on_send_internal(void* system, const void* id, const void* packet, const void* data)
{
    if (g_logging.load())
    {
        const auto key = reinterpret_cast<std::uintptr_t>(system);
        if (t_in_send_to_server)
        {
            std::lock_guard lock(g_side_mutex);
            g_client_systems.insert(key);
        }
        append(kSent, side_of(key), fnv1a(id, kNetworkIdentifierSize), msvc_string(data));
    }
    g_send_internal(system, id, packet, data);
}

int on_receive(void* peer, void* out, const void* time)
{
    const int status = g_receive(peer, out, time);
    if (status == kDataStatusHasData && g_logging.load())
    {
        const auto system = reinterpret_cast<std::uintptr_t>(out) - kReceiveBufferOffset;
        append(kReceived, side_of(system), reinterpret_cast<std::uintptr_t>(peer), msvc_string(out));
    }
    return status;
}

std::uintptr_t on_send_to_server(void* sender, void* packet)
{
    const bool outer = t_in_send_to_server;
    t_in_send_to_server = true;
    const std::uintptr_t result = g_send_to_server(sender, packet);
    t_in_send_to_server = outer;
    return result;
}

void set_status(std::string text)
{
    std::lock_guard lock(g_buffer_mutex);
    g_install_status = std::move(text);
}

bool prologue_matches(std::uintptr_t, const Target& target)
{
    return conduit::minecraft::matchesMainExecutableCode(target.rva, target.prologue);
}

void install_loop()
{
    std::string build = "unknown";
    try
    {
        build = std::string(conduit::minecraft::requireRunningCompatibility().build);
    }
    catch (const std::exception&)
    {
    }
    if (build != "1.26.65.1")
    {
        set_status("unsupported game build " + build + "; packet hooks exist only for 1.26.65.1");
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (int attempt = 0; attempt < 600; ++attempt)
    {
        if (prologue_matches(base, kSendInternal) && prologue_matches(base, kReceive) &&
            prologue_matches(base, kSendToServer))
        {
            MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                set_status("MH_Initialize failed");
                return;
            }
            const auto hook = [&](const Target& t, void* detour, void** original) {
                void* target = reinterpret_cast<void*>(base + t.rva);
                return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
            };
            if (!hook(kSendToServer, reinterpret_cast<void*>(&on_send_to_server),
                      reinterpret_cast<void**>(&g_send_to_server)) ||
                !hook(kSendInternal, reinterpret_cast<void*>(&on_send_internal),
                      reinterpret_cast<void**>(&g_send_internal)) ||
                !hook(kReceive, reinterpret_cast<void*>(&on_receive), reinterpret_cast<void**>(&g_receive)))
            {
                set_status("failed to hook the packet functions");
                return;
            }
            g_installed.store(true);
            set_status("installed");
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    set_status("the packet functions never matched their expected bytes");
}

void writer_loop()
{
    std::vector<char> pending;
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        fs::path file;
        {
            std::lock_guard lock(g_buffer_mutex);
            pending.swap(g_buffer);
            file = g_file;
        }
        if (pending.empty() || file.empty())
        {
            pending.clear();
            continue;
        }
        std::ofstream out(file, std::ios::binary | std::ios::app);
        out.write(pending.data(), static_cast<std::streamsize>(pending.size()));
        pending.clear();
    }
}

fs::path data_dir()
{
    fs::path base;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
        base = p;
    CoTaskMemFree(p);
    return base / L"BedrockMCP";
}

fs::path dll_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&dll_dir), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    return fs::path(path).parent_path();
}

fs::path decoder_path()
{
    std::error_code ec;
    for (const fs::path& candidate :
         {dll_dir() / L"packetlog.exe", dll_dir() / L".." / L".." / L"tools" / L"packetlog" / L"packetlog.exe"})
        if (fs::exists(candidate, ec))
            return fs::weakly_canonical(candidate, ec);
    return {};
}

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size);
    return out;
}

std::string narrow(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring quote(const std::wstring& arg)
{
    std::wstring out = L"\"";
    for (wchar_t c : arg)
    {
        if (c == L'"')
            out += L'\\';
        out += c;
    }
    return out + L"\"";
}

Result run_decoder(const std::vector<std::wstring>& args)
{
    const fs::path exe = decoder_path();
    if (exe.empty())
        return {false, "packetlog.exe not found next to the DLL or in tools\\packetlog; build it with "
                       "'go build' in tools\\packetlog"};
    std::wstring command = quote(exe.wstring());
    for (const auto& arg : args)
        command += L" " + quote(arg);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0))
        return {false, "CreatePipe failed"};
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = write;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi))
    {
        CloseHandle(read);
        CloseHandle(write);
        return {false, "could not start " + narrow(exe.wstring())};
    }
    CloseHandle(write);
    std::string output;
    char chunk[4096];
    DWORD got = 0;
    constexpr std::size_t kMaxOutput = 400 * 1024;
    while (ReadFile(read, chunk, sizeof(chunk), &got, nullptr) && got)
        if (output.size() < kMaxOutput)
            output.append(chunk, got);
    CloseHandle(read);
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (output.size() >= kMaxOutput)
        output += "\n... output truncated; narrow it with skip, limit or name";
    return {code == 0, output.empty() ? "(no output)" : output};
}

fs::path log_to_read(const std::string& file)
{
    if (!file.empty())
        return fs::u8path(file);
    std::lock_guard lock(g_buffer_mutex);
    return g_file.empty() ? g_last_file : g_file;
}

} // namespace

void init()
{
    std::thread(install_loop).detach();
    std::thread(writer_loop).detach();
}

Result start()
{
    if (!g_installed.load())
        return status();
    const fs::path dir = data_dir() / L"packets";
    std::error_code ec;
    fs::create_directories(dir, ec);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"%04u%02u%02u-%02u%02u%02u.bmpl", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    const fs::path file = dir / name;
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        const std::uint32_t version = 1;
        out.write("BMPL", 4);
        out.write(reinterpret_cast<const char*>(&version), 4);
        if (!out)
            return {false, "cannot create " + narrow(file.wstring())};
    }
    {
        std::lock_guard lock(g_buffer_mutex);
        g_buffer.clear();
        g_records = g_bytes = g_dropped = 0;
        g_file = file;
    }
    g_logging.store(true);
    return {true, "logging packets to " + narrow(file.wstring())};
}

Result stop()
{
    g_logging.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    std::lock_guard lock(g_buffer_mutex);
    if (g_file.empty())
        return {false, "packet logging was not running"};
    g_last_file = g_file;
    g_file.clear();
    return {true, "stopped; " + std::to_string(g_records) + " packets, " + std::to_string(g_bytes) + " bytes in " +
                      narrow(g_last_file.wstring())};
}

Result status()
{
    std::lock_guard lock(g_buffer_mutex);
    std::string text = "hooks: " + g_install_status + "; logging: " + (g_logging.load() ? "on" : "off");
    const fs::path file = g_file.empty() ? g_last_file : g_file;
    if (!file.empty())
        text += "; file: " + narrow(file.wstring()) + "; packets: " + std::to_string(g_records) +
                ", bytes: " + std::to_string(g_bytes) + ", dropped: " + std::to_string(g_dropped);
    return {g_installed.load(), text};
}

Result decode(const DecodeOptions& options)
{
    const fs::path file = log_to_read(options.file);
    if (file.empty())
        return {false, "no packet log yet; call packet_log with action start"};
    std::vector<std::wstring> args{L"decode",
                                   L"-file",
                                   file.wstring(),
                                   L"-skip",
                                   std::to_wstring(options.skip),
                                   L"-limit",
                                   std::to_wstring(options.limit),
                                   L"-max-json",
                                   std::to_wstring(options.max_json)};
    if (!options.name.empty())
        args.insert(args.end(), {L"-name", widen(options.name)});
    if (!options.dir.empty())
        args.insert(args.end(), {L"-dir", widen(options.dir)});
    return run_decoder(args);
}

Result extract_packs(const std::string& file, const std::string& out_dir)
{
    const fs::path log = log_to_read(file);
    if (log.empty())
        return {false, "no packet log yet; call packet_log with action start"};
    const fs::path out = out_dir.empty() ? data_dir() / L"packs" / log.stem() : fs::u8path(out_dir);
    return run_decoder({L"packs", L"-file", log.wstring(), L"-out", out.wstring()});
}

} // namespace packets
