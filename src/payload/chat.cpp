#include "chat.hpp"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "minecraft/compatibility.hpp"

namespace chat {
namespace {

// Minecraft.Windows.exe 1.26.65.1; docs/porting.md records how these were found.
// The chat screen's Enter handler (ChatScreenController::_sendChatMessage) sends text that starts
// with '/' to the command function with the client instance, and anything else to
// MinecraftScreenModel::sendChatMessage. The screen model tick runs on the UI thread every tick.
struct Target {
    std::uintptr_t rva;
    std::array<std::uint8_t, 16> prologue;
};
constexpr Target kModelTick{0x4d5c2e0, {0x55, 0x56, 0x48, 0x83, 0xec, 0x78, 0x48, 0x8d, 0x6c, 0x24, 0x70, 0x0f, 0x29,
                                        0x75, 0xf0, 0x48}};
constexpr Target kSendChat{0x2680900, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48,
                                       0x81, 0xec, 0x18}};
constexpr Target kRunCommand{0x683060, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48, 0x81, 0xec,
                                        0x80, 0x01, 0x00}};
constexpr std::size_t kModelClientInstance = 0x48;

using TickFn = std::uintptr_t (*)(void*, void*, void*, void*);
using SendChatFn = bool (*)(void*, const std::string*);
using RunCommandFn = bool (*)(const std::string*, void*);

struct NonOwnerClientInstance {
    void* handle;
    void* control;
    void* instance;
};

struct Request {
    std::string text;
    bool done = false;
};

TickFn g_tick = nullptr;
SendChatFn g_send_chat = nullptr;
RunCommandFn g_run_command = nullptr;
std::string g_status = "installing";

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<std::shared_ptr<Request>> g_queue;

void deliver(void* model, Request& request)
{
    if (request.text.rfind('/', 0) == 0)
    {
        NonOwnerClientInstance client{};
        std::memcpy(&client, static_cast<std::uint8_t*>(model) + kModelClientInstance, sizeof(client));
        if (!client.control || !client.instance)
            return;
        _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(client.control) + 8));
        g_run_command(&request.text, &client);
    }
    else
    {
        g_send_chat(model, &request.text);
    }
}

std::uintptr_t on_tick(void* model, void* a, void* b, void* c)
{
    const std::uintptr_t result = g_tick(model, a, b, c);
    std::shared_ptr<Request> request;
    {
        std::lock_guard lock(g_mutex);
        if (g_queue.empty())
            return result;
        request = g_queue.front();
        g_queue.pop_front();
    }
    deliver(model, *request);
    {
        std::lock_guard lock(g_mutex);
        request->done = true;
    }
    g_cv.notify_all();
    return result;
}

void install_loop()
{
    try
    {
        if (conduit::minecraft::requireRunningCompatibility().build != "1.26.65.1")
        {
            g_status = "unsupported game build; direct chat exists only for 1.26.65.1";
            return;
        }
    }
    catch (const std::exception&)
    {
        g_status = "unsupported game build; direct chat exists only for 1.26.65.1";
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (int attempt = 0; attempt < 600; ++attempt)
    {
        const bool ready = conduit::minecraft::matchesMainExecutableCode(kModelTick.rva, kModelTick.prologue) &&
                           conduit::minecraft::matchesMainExecutableCode(kSendChat.rva, kSendChat.prologue) &&
                           conduit::minecraft::matchesMainExecutableCode(kRunCommand.rva, kRunCommand.prologue);
        if (ready)
        {
            g_send_chat = reinterpret_cast<SendChatFn>(base + kSendChat.rva);
            g_run_command = reinterpret_cast<RunCommandFn>(base + kRunCommand.rva);
            MH_STATUS init = MH_Initialize();
            void* target = reinterpret_cast<void*>(base + kModelTick.rva);
            if ((init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) ||
                MH_CreateHook(target, reinterpret_cast<void*>(&on_tick), reinterpret_cast<void**>(&g_tick)) != MH_OK ||
                MH_EnableHook(target) != MH_OK)
            {
                g_status = "failed to hook the screen model tick";
                return;
            }
            g_status = "installed";
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    g_status = "the chat functions never matched their expected bytes";
}

} // namespace

void init()
{
    std::thread(install_loop).detach();
}

Result send(const std::string& text)
{
    if (!g_tick)
        return {false, "direct chat is not available: " + g_status};
    if (text.empty() || text.size() > 512)
        return {false, "text must be 1 to 512 bytes"};
    auto request = std::make_shared<Request>();
    request->text = text;
    std::unique_lock lock(g_mutex);
    g_queue.push_back(request);
    if (!g_cv.wait_for(lock, std::chrono::seconds(5), [&] { return request->done; }))
    {
        auto it = std::find(g_queue.begin(), g_queue.end(), request);
        if (it != g_queue.end())
            g_queue.erase(it);
        return {false, "no screen ticked within 5 seconds (the game is loading, minimized, or between worlds); "
                       "nothing was sent"};
    }
    const bool command = text.rfind('/', 0) == 0;
    return {true, std::string(command ? "command submitted" : "chat submitted") +
                      "; the reply shows in chat (screenshot) and as CommandOutput or Text packets (packet_find)"};
}

} // namespace chat
