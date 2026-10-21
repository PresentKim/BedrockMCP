#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swapchain_capture {

struct Frame {
    std::vector<uint8_t> bgra;
    unsigned width = 0;
    unsigned height = 0;
};

// Read the back buffer of the next frame the game presents through a Direct3D 12
// swapchain. Installs the hooks on first use and blocks until a frame arrives.
bool grab(Frame& out, std::string& error);

} // namespace swapchain_capture
