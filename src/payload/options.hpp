#pragma once

#include <string>

// Game options: the saved options.txt, and the live OptionRegistry inside the game.
namespace options {

struct Result {
    bool ok = false;
    std::string text;
};

// Hook OptionRegistry::get in the background to capture the live registries (exact build only).
void init();

// Options whose key contains any comma-separated term of filter (all when empty), from options.txt.
Result get(const std::string& filter, int limit);

// The same from the running game: save name, type, current value and range.
Result live(const std::string& filter, int limit);

// Set an option by save name from a value string, the way the Settings screen does, and save.
Result set(const std::string& key, const std::string& value);

} // namespace options
