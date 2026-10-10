#include "game_window.hpp"

namespace {

struct Search {
    DWORD pid;
    HWND best;
    LONG best_area;
};

BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lparam)
{
    auto* s = reinterpret_cast<Search*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s->pid)
        return TRUE;
    if (!IsWindowVisible(hwnd))
        return TRUE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd)
        return TRUE;
    RECT r;
    if (!GetWindowRect(hwnd, &r))
        return TRUE;
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > s->best_area)
    {
        s->best_area = area;
        s->best = hwnd;
    }
    return TRUE;
}

} // namespace

HWND find_game_window()
{
    Search s{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&s));
    return s.best;
}
