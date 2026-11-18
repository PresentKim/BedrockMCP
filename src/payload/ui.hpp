#pragma once

#include <string>

// Legacy JSON UI tooling built on the vendored Conduit client: reload, screen
// diagnostics and the active screen's control tree.
namespace ui {

struct Result {
    bool ok = false;
    std::string text;
};

// Install the Conduit hooks in the background (retried until the game has
// decrypted the code they patch) and start the Ctrl+R reload hotkey.
void init();

Result status();
Result reload();
Result screen();

struct TreeOptions {
    int max_depth = 64;
    int limit = 400;
    bool include_hidden = false;
    std::string filter;
};
Result tree(const TreeOptions& options);

// Control under the client-area point (x, y), in the same pixels as move/click.
Result at(int x, int y);

} // namespace ui
