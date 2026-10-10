#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kGameExe[] = L"Minecraft.Windows.exe";

int fail(const char* what)
{
    std::cerr << what << " failed (error " << GetLastError() << ")\n";
    return 1;
}

DWORD find_process(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry))
    {
        if (_wcsicmp(entry.szExeFile, name) == 0)
        {
            pid = entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snap);
    return pid;
}

bool has_module(DWORD pid, const std::wstring& module_name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    MODULEENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &entry); ok; ok = Module32NextW(snap, &entry))
    {
        if (_wcsicmp(entry.szModule, module_name.c_str()) == 0)
        {
            found = true;
            break;
        }
    }
    CloseHandle(snap);
    return found;
}

// Packaged processes can only load DLLs that ALL APPLICATION PACKAGES may read.
bool grant_app_packages_read(const fs::path& path)
{
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &sid))
        return false;

    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL new_dacl = nullptr;
    bool ok = false;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &old_dacl, nullptr, &sd) == ERROR_SUCCESS)
    {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        if (SetEntriesInAclW(1, &access, old_dacl, &new_dacl) == ERROR_SUCCESS)
        {
            ok = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl,
                                       nullptr) == ERROR_SUCCESS;
        }
    }
    LocalFree(new_dacl);
    LocalFree(sd);
    LocalFree(sid);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    fs::path dll = argc > 1 ? fs::absolute(argv[1]) : fs::path(self).parent_path() / L"bedrock_mcp.dll";
    if (!fs::exists(dll))
    {
        std::wcerr << L"DLL not found: " << dll.wstring() << L"\n";
        return 1;
    }

    DWORD pid = find_process(kGameExe);
    if (pid == 0)
    {
        std::wcerr << kGameExe << L" is not running\n";
        return 1;
    }
    std::wcout << L"Found " << kGameExe << L" (pid " << pid << L")\n";

    if (has_module(pid, dll.filename().wstring()))
    {
        std::wcout << L"Already injected\n";
        return 0;
    }

    if (!grant_app_packages_read(dll))
        std::wcerr << L"Warning: could not grant ALL APPLICATION PACKAGES read on the DLL\n";

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process)
        return fail("OpenProcess");

    const std::wstring dll_path = dll.wstring();
    const size_t bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
        return fail("VirtualAllocEx");
    if (!WriteProcessMemory(process, remote, dll_path.c_str(), bytes, nullptr))
        return fail("WriteProcessMemory");

    auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
    if (!thread)
        return fail("CreateRemoteThread");
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);

    if (!has_module(pid, dll.filename().wstring()))
    {
        std::wcerr << L"LoadLibraryW ran but the DLL is not loaded in the game\n";
        return 1;
    }
    std::wcout << L"Injected " << dll_path << L"\n";
    return 0;
}
