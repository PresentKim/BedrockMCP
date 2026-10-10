#include "feed.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <MinHook.h>

#include "sigscan.hpp"

namespace feed {
namespace {

// void MouseDevice::feed(this, buttonId, buttonData, x, y, dx, dy, forceMotionless)
using FeedFn = void (*)(void*, int, int, int, int, int, int, int);

FeedFn g_original = nullptr;
std::atomic<bool> g_hooked{false};
std::atomic<void*> g_self{nullptr};
std::atomic<int> g_x{0};
std::atomic<int> g_y{0};
std::string g_signature;

void detour(void* self, int a, int b, int c, int d, int e, int f, int g)
{
    g_self.store(self);
    g_original(self, a, b, c, d, e, f, g);
}

bool try_hook()
{
    uint8_t* addr = find_pattern("Minecraft.Windows.exe", g_signature.c_str());
    if (!addr)
        return false;
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (MH_CreateHook(addr, reinterpret_cast<void*>(&detour),
                      reinterpret_cast<void**>(&g_original)) != MH_OK)
        return false;
    if (MH_EnableHook(addr) != MH_OK)
        return false;
    g_hooked.store(true);
    return true;
}

void* resolve_self()
{
    return g_self.load();
}

} // namespace

void init(const char* signature)
{
    g_signature = signature ? signature : "";
    if (g_signature.empty())
        return;
    // The game lazily decrypts MouseDevice::feed when it first runs, so the
    // signature is absent at startup. Retry in the background until it appears,
    // then hook it; the hook captures the MouseDevice instance on the next call.
    std::thread([] {
        for (int i = 0; i < 1200 && !g_hooked.load(); ++i)
        {
            if (try_hook())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
}

bool ready()
{
    return g_hooked.load() && g_original != nullptr && resolve_self() != nullptr;
}

void move(int x, int y)
{
    void* self = resolve_self();
    if (!g_original || !self)
        return;
    g_x.store(x);
    g_y.store(y);
    g_original(self, 0, 0, x, y, 0, 0, 0);
}

void look(int dx, int dy)
{
    void* self = resolve_self();
    if (!g_original || !self)
        return;
    g_original(self, 0, 0, g_x.load(), g_y.load(), dx, dy, 0);
}

void button(int id, bool down)
{
    void* self = resolve_self();
    if (!g_original || !self)
        return;
    g_original(self, id, down ? 1 : 0, g_x.load(), g_y.load(), 0, 0, 0);
}

} // namespace feed
