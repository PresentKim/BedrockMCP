#pragma once

#include <optional>
#include <string>

namespace input {

enum class KeyAction { Press, Down, Up };
enum class MouseButton { Left, Right, Middle };

// Send goes through SendInput to the foreground window; Post uses PostMessage to
// the game window directly; Feed calls the game's MouseDevice::feed below
// GameInput. Feed needs no focus and the game cannot ignore it (mouse only;
// keyboard falls back to Post).
enum class Method { Send, Post, Feed };

// Set the default delivery method (from config at startup).
void set_default_method(Method m);
Method default_method();

// Start the Feed backend: locate and hook MouseDevice::feed using the given
// signature (retried in the background).
void init_feed(const char* signature);

// Each input call takes the method to use for that call.
bool send_key(const std::string& name, KeyAction action, Method method);
void type_text(const std::string& utf8, Method method);
void mouse_button(MouseButton button, KeyAction action, Method method);
bool move_to(int x, int y, Method method);

// Relative mouselook. Send uses SendInput, Feed calls feed directly; Post has no
// relative-move message and falls back to Send.
void move_relative(int dx, int dy, Method method);

// True once the Feed backend has located MouseDevice::feed and a device instance.
bool feed_ready();

void scroll(int amount, Method method);

// Bring the game window to the foreground so synthesized input lands on it.
bool focus_game();

} // namespace input
