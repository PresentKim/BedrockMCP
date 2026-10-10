#pragma once

// Direct injection into Minecraft's internal mouse pipeline by calling
// MouseDevice::feed below GameInput. Works without window focus.
namespace feed {

// Start locating MouseDevice::feed using the given byte pattern (retried in the
// background until the game decrypts it) and hook it to capture the MouseDevice
// instance. The game lazily decrypts the function, so this returns immediately.
void init(const char* signature);

// True once the MouseDevice instance is known (captured from a real call or a
// validated fallback), so injection can proceed.
bool ready();

// Absolute cursor motion to client coordinates (menus).
void move(int x, int y);

// Relative motion for in-world camera look.
void look(int dx, int dy);

// Mouse button: id 1=left, 2=right, 3=middle; down true=press, false=release.
void button(int id, bool down);

} // namespace feed
