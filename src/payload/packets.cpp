#include "packets.hpp"

#include <windows.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace packets {
namespace {

namespace fs = std::filesystem;

std::mutex g_sessions_mutex;
std::map<std::string, unsigned long long> g_sessions;

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

} // namespace

Result run(const std::string& command, const std::vector<std::pair<std::string, std::string>>& flags)
{
    const fs::path exe = decoder_path();
    if (exe.empty())
        return {false, "packetlog.exe not found next to the DLL or in tools\\packetlog; build it with "
                       "'go build' in tools\\packetlog"};
    std::wstring line = quote(exe.wstring()) + L" " + widen(command);
    for (const auto& [flag, value] : flags)
        line += L" " + quote(L"-" + widen(flag) + L"=" + widen(value));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0))
        return {false, "CreatePipe failed"};
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
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
        output += "\n... output truncated; narrow it with a filter, from/to or limit";
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return {code == 0, output.empty() ? "(no output)" : output};
}

Result session_start(const std::string& name)
{
    Result count = run("count", {});
    if (!count.ok)
        return count;
    unsigned long long mark = 0;
    try
    {
        mark = std::stoull(count.text);
    }
    catch (const std::exception&)
    {
        return {false, "unexpected packet count: " + count.text};
    }
    std::lock_guard lock(g_sessions_mutex);
    g_sessions[name] = mark;
    return {true, "session '" + name + "' started after packet #" + std::to_string(mark)};
}

Result session_stop(const std::string& name, std::vector<std::pair<std::string, std::string>> flags)
{
    unsigned long long mark = 0;
    {
        std::lock_guard lock(g_sessions_mutex);
        auto it = g_sessions.find(name);
        if (it == g_sessions.end())
            return {false, "no session named '" + name + "'; call packet_session with action start first"};
        mark = it->second;
        g_sessions.erase(it);
    }
    Result count = run("count", {});
    if (!count.ok)
        return count;
    flags.emplace_back("from", std::to_string(mark + 1));
    flags.emplace_back("to", count.text);
    Result found = run("find", flags);
    found.text = "session '" + name + "' covered packets #" + std::to_string(mark + 1) + " to #" + count.text +
                 "\n" + found.text;
    return found;
}

} // namespace packets
