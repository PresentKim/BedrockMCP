#pragma once

#include <functional>
#include <string>

namespace capture {

// Draws onto the client-area BGRA pixels before scaling; (0,0) is the client origin.
using Painter = std::function<void(unsigned char* bgra, unsigned pitch, int width, int height)>;

struct Result {
    bool ok;
    std::string png;   // raw PNG bytes when ok
    std::string error; // message when not ok
    int width = 0;         // output image size
    int height = 0;
    int client_width = 0;  // game client size the image maps to
    int client_height = 0;
};

// Capture the game's client area and encode it as PNG, scaled to fit within
// (max_width, max_height) keeping aspect ratio. 0 for both means no scaling.
// The output maps to client coordinates directly: client_x = image_x *
// client_width / width, client_y = image_y * client_height / height.
Result screenshot(int max_width, int max_height, const Painter& paint = {});

} // namespace capture
