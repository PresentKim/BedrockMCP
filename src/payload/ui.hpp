#pragma once

#include <functional>
#include <string>

#include "capture.hpp"

// Legacy JSON UI tooling built on the vendored Conduit client: reload, screen
// diagnostics, the active screen's control tree and the F3/F5 overlay.
namespace ui {

struct Result {
    bool ok = false;
    std::string text;
};

// Install the Conduit hooks in the background (retried until the game has
// decrypted the code they patch), start the Ctrl+R reload hotkey, and watch
// resource pack ui/*.json files for automatic reloads.
void init();

Result status();

// Run job on the game's UI thread during the next frame (waits up to 3 s for it to start).
// Needs the Conduit frame hook, so it fails on game builds without a Conduit profile.
bool on_ui_thread(std::function<void()> job);

// With diff, compares the visible control tree before and after the reload.
// Content log lines written during the reload are appended either way.
Result reload(bool diff);

Result screen();

// Waits until the active screen name contains `name` (if given) and does not
// contain `leave` (if given).
Result wait_for_screen(const std::string& name, const std::string& leave, int timeout_ms);

struct TreeOptions {
    int max_depth = 64;
    int limit = 400;
    bool include_hidden = false;
    std::string filter;
};
Result tree(const TreeOptions& options);

// Control under the client-area point (x, y), in the same pixels as move/click.
Result at(int x, int y);

struct Target {
    bool ok = false;
    std::string text;
    int x = 0;
    int y = 0;
};

// Center of the index-th visible control named `name` (exact match first,
// then substring), in client pixels.
Target find(const std::string& name, int index);

struct AnnotateOptions {
    std::string filter;
    std::string type;
    int max_depth = 64;
    int limit = 60;
    int width = 1280;
    int height = 720;
};

struct Annotated {
    capture::Result shot;
    std::string legend;
};

// Screenshot with numbered boxes over the selected controls and a legend.
Annotated annotate(const AnnotateOptions& options);

// panel and inspector: -1 keeps the state, 0 turns off, 1 turns on.
Result overlay(int panel, int inspector);

// enabled: -1 keeps the state, 0 or 1 sets it.
Result auto_reload(int enabled);

Result content_log(int lines, const std::string& filter);

} // namespace ui
