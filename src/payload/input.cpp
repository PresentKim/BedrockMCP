#include "input.hpp"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>

#include "feed.hpp"
#include "game_window.hpp"

namespace input {
namespace {

struct Key {
    WORD vk;
    bool extended;
};

std::optional<Key> resolve_key(std::string name)
{
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (name.rfind("vk:", 0) == 0)
    {
        WORD vk = static_cast<WORD>(std::strtol(name.c_str() + 3, nullptr, 0));
        if (vk == 0)
            return std::nullopt;
        return Key{vk, false};
    }

    if (name.size() == 1)
    {
        char c = name[0];
        if (c >= 'a' && c <= 'z')
            return Key{static_cast<WORD>('A' + (c - 'a')), false};
        if (c >= '0' && c <= '9')
            return Key{static_cast<WORD>(c), false};
    }

    if (name.size() >= 2 && name[0] == 'f')
    {
        bool digits = true;
        for (size_t i = 1; i < name.size(); ++i)
            digits = digits && std::isdigit(static_cast<unsigned char>(name[i]));
        if (digits)
        {
            int n = std::atoi(name.c_str() + 1);
            if (n >= 1 && n <= 24)
                return Key{static_cast<WORD>(VK_F1 + (n - 1)), false};
        }
    }

    static const std::unordered_map<std::string, Key> table = {
        {"space", {VK_SPACE, false}},     {"enter", {VK_RETURN, false}},
        {"return", {VK_RETURN, false}},   {"escape", {VK_ESCAPE, false}},
        {"esc", {VK_ESCAPE, false}},      {"tab", {VK_TAB, false}},
        {"backspace", {VK_BACK, false}},  {"shift", {VK_LSHIFT, false}},
        {"ctrl", {VK_LCONTROL, false}},   {"control", {VK_LCONTROL, false}},
        {"alt", {VK_LMENU, false}},       {"lshift", {VK_LSHIFT, false}},
        {"rshift", {VK_RSHIFT, false}},   {"lctrl", {VK_LCONTROL, false}},
        {"rctrl", {VK_RCONTROL, true}},   {"lalt", {VK_LMENU, false}},
        {"ralt", {VK_RMENU, true}},       {"capslock", {VK_CAPITAL, false}},
        {"up", {VK_UP, true}},            {"down", {VK_DOWN, true}},
        {"left", {VK_LEFT, true}},        {"right", {VK_RIGHT, true}},
        {"insert", {VK_INSERT, true}},    {"delete", {VK_DELETE, true}},
        {"del", {VK_DELETE, true}},       {"home", {VK_HOME, true}},
        {"end", {VK_END, true}},          {"pageup", {VK_PRIOR, true}},
        {"pagedown", {VK_NEXT, true}},    {"numpad0", {VK_NUMPAD0, false}},
        {"numpad1", {VK_NUMPAD1, false}}, {"numpad2", {VK_NUMPAD2, false}},
        {"numpad3", {VK_NUMPAD3, false}}, {"numpad4", {VK_NUMPAD4, false}},
        {"numpad5", {VK_NUMPAD5, false}}, {"numpad6", {VK_NUMPAD6, false}},
        {"numpad7", {VK_NUMPAD7, false}}, {"numpad8", {VK_NUMPAD8, false}},
        {"numpad9", {VK_NUMPAD9, false}},
    };
    auto it = table.find(name);
    if (it == table.end())
        return std::nullopt;
    return it->second;
}

// A delivery backend for synthesized input. SendInputMethod drives the OS input
// queue (foreground window only); PostMessageMethod posts straight to the game
// window (no focus needed, but Raw Input paths may ignore it).
class InputMethod {
  public:
    virtual ~InputMethod() = default;
    virtual void key(const Key& key, bool down) = 0;
    virtual void character(wchar_t ch) = 0;
    virtual void button(MouseButton button, bool down) = 0;
    virtual bool move_to(int x, int y) = 0; // client coordinates
    virtual void scroll(int amount) = 0;
};

class SendInputMethod : public InputMethod {
  public:
    void key(const Key& key, bool down) override
    {
        WORD scan = static_cast<WORD>(MapVirtualKeyW(key.vk, MAPVK_VK_TO_VSC));
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (!down)
            in.ki.dwFlags |= KEYEVENTF_KEYUP;
        if (key.extended)
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        SendInput(1, &in, sizeof(in));
    }

    void character(wchar_t ch) override
    {
        for (bool down : {true, false})
        {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wScan = ch;
            in.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
            SendInput(1, &in, sizeof(in));
        }
    }

    void button(MouseButton b, bool down) override
    {
        DWORD flag = 0;
        switch (b)
        {
        case MouseButton::Left:
            flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case MouseButton::Right:
            flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case MouseButton::Middle:
            flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        }
        emit(flag);
    }

    bool move_to(int x, int y) override
    {
        HWND hwnd = find_game_window();
        if (!hwnd)
            return false;
        POINT pt{x, y};
        ClientToScreen(hwnd, &pt);

        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw <= 1 || vh <= 1)
            return false;

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = static_cast<LONG>((pt.x - vx) * 65535.0 / (vw - 1));
        in.mi.dy = static_cast<LONG>((pt.y - vy) * 65535.0 / (vh - 1));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        SendInput(1, &in, sizeof(in));
        return true;
    }

    void scroll(int amount) override
    {
        emit(MOUSEEVENTF_WHEEL, static_cast<DWORD>(amount * WHEEL_DELTA));
    }

    void relative(int dx, int dy)
    {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = dx;
        in.mi.dy = dy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(in));
    }

  private:
    static void emit(DWORD flags, DWORD data = 0)
    {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = flags;
        in.mi.mouseData = data;
        SendInput(1, &in, sizeof(in));
    }
};

class PostMessageMethod : public InputMethod {
  public:
    void key(const Key& key, bool down) override
    {
        HWND hwnd = find_game_window();
        if (!hwnd)
            return;
        PostMessageW(hwnd, down ? WM_KEYDOWN : WM_KEYUP, key.vk, key_lparam(key, down));
    }

    void character(wchar_t ch) override
    {
        HWND hwnd = find_game_window();
        if (hwnd)
            PostMessageW(hwnd, WM_CHAR, ch, 1);
    }

    void button(MouseButton b, bool down) override
    {
        HWND hwnd = find_game_window();
        if (!hwnd)
            return;
        UINT msg = 0;
        switch (b)
        {
        case MouseButton::Left:
            msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
            break;
        case MouseButton::Right:
            msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
            break;
        case MouseButton::Middle:
            msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
            break;
        }
        WPARAM mk = down ? button_mk(b) : 0;
        LPARAM pos = MAKELPARAM(pos_.x, pos_.y);
        PostMessageW(hwnd, WM_MOUSEMOVE, mk, pos);
        PostMessageW(hwnd, msg, mk, pos);
    }

    bool move_to(int x, int y) override
    {
        HWND hwnd = find_game_window();
        if (!hwnd)
            return false;
        pos_ = POINT{x, y};
        PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        return true;
    }

    void scroll(int amount) override
    {
        HWND hwnd = find_game_window();
        if (!hwnd)
            return;
        POINT screen = pos_;
        ClientToScreen(hwnd, &screen);
        PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, amount * WHEEL_DELTA),
                     MAKELPARAM(screen.x, screen.y));
    }

  private:
    static LPARAM key_lparam(const Key& key, bool down)
    {
        WORD scan = static_cast<WORD>(MapVirtualKeyW(key.vk, MAPVK_VK_TO_VSC));
        LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16);
        if (key.extended)
            lp |= (1 << 24);
        if (!down)
            lp |= (1 << 30) | (1 << 31);
        return lp;
    }

    static WPARAM button_mk(MouseButton b)
    {
        switch (b)
        {
        case MouseButton::Left:
            return MK_LBUTTON;
        case MouseButton::Right:
            return MK_RBUTTON;
        case MouseButton::Middle:
            return MK_MBUTTON;
        }
        return 0;
    }

    POINT pos_{0, 0};
};

SendInputMethod g_send;
PostMessageMethod g_post;

// Calls MouseDevice::feed below GameInput. Mouse only; keyboard/scroll delegate to
// PostMessage so they stay focus-free too. When feed is not available (e.g. a game
// update changed the signature, or it is not decrypted yet), mouse falls back to
// SendInput so the tool degrades to focus-required rather than failing outright.
class FeedMethod : public InputMethod {
  public:
    void key(const Key& key, bool down) override { g_post.key(key, down); }
    void character(wchar_t ch) override { g_post.character(ch); }
    void button(MouseButton b, bool down) override
    {
        if (!feed::ready())
        {
            g_send.button(b, down);
            return;
        }
        int id = b == MouseButton::Left ? 1 : b == MouseButton::Right ? 2 : 3;
        feed::button(id, down);
    }
    bool move_to(int x, int y) override
    {
        if (!feed::ready())
            return g_send.move_to(x, y);
        feed::move(x, y);
        return true;
    }
    void scroll(int amount) override { g_post.scroll(amount); }
};

FeedMethod g_feed;
std::atomic<Method> g_default_method{Method::Send};

InputMethod& backend(Method m)
{
    if (m == Method::Post)
        return g_post;
    if (m == Method::Feed)
        return g_feed;
    return g_send;
}

} // namespace

void set_default_method(Method m)
{
    g_default_method.store(m);
}

Method default_method()
{
    return g_default_method.load();
}

void init_feed(const char* signature)
{
    feed::init(signature);
}

bool send_key(const std::string& name, KeyAction action, Method method)
{
    auto key = resolve_key(name);
    if (!key)
        return false;
    InputMethod& b = backend(method);
    if (action == KeyAction::Press || action == KeyAction::Down)
        b.key(*key, true);
    if (action == KeyAction::Press || action == KeyAction::Up)
        b.key(*key, false);
    return true;
}

void type_text(const std::string& utf8, Method method)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 1)
        return;
    std::wstring wide(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), wlen);

    InputMethod& b = backend(method);
    for (wchar_t ch : wide)
        b.character(ch);
}

void mouse_button(MouseButton button, KeyAction action, Method method)
{
    InputMethod& b = backend(method);
    if (action == KeyAction::Press || action == KeyAction::Down)
        b.button(button, true);
    if (action == KeyAction::Press || action == KeyAction::Up)
        b.button(button, false);
}

bool move_to(int x, int y, Method method)
{
    return backend(method).move_to(x, y);
}

void move_relative(int dx, int dy, Method method)
{
    if (method == Method::Feed && feed::ready())
        feed::look(dx, dy);
    else
        g_send.relative(dx, dy);
}

bool feed_ready()
{
    return feed::ready();
}

void scroll(int amount, Method method)
{
    backend(method).scroll(amount);
}

bool focus_game()
{
    HWND hwnd = find_game_window();
    if (!hwnd)
        return false;
    if (GetForegroundWindow() == hwnd)
        return true;
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);

    // A background process (the game, while another window is focused) is normally
    // barred from taking the foreground. Attaching to the current foreground thread's
    // input queue lifts that restriction long enough to raise the game window.
    DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD self_thread = GetCurrentThreadId();
    bool attached = fg_thread && fg_thread != self_thread &&
                    AttachThreadInput(self_thread, fg_thread, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (attached)
        AttachThreadInput(self_thread, fg_thread, FALSE);
    return GetForegroundWindow() == hwnd;
}

} // namespace input
