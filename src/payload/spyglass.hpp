#pragma once

#include <string>

// Controls the overlay of a spyglass DLL loaded in the game (github.com/EndstoneMC/spyglass).
// spyglass shows its overlay as soon as it loads; only F12 hides it.
namespace spyglass_control {

struct Result {
    bool ok = false;
    std::string text;
};

// Hide the overlay once spyglass has loaded, when start_hidden is set.
void init(bool start_hidden);

// visible: -1 reports the state, 0 hides, 1 shows.
Result overlay(int visible);

} // namespace spyglass_control
