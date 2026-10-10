#pragma once

#include <windows.h>

// Resolve the main top-level window owned by the current (game) process.
// Returns nullptr if none is visible yet.
HWND find_game_window();
