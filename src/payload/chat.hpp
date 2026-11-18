#pragma once

#include <string>

// Sends chat messages and runs commands through the functions the chat screen calls on
// Enter, without opening the chat UI. Exact build 1.26.65.1.
namespace chat {

struct Result {
    bool ok = false;
    std::string text;
};

// Hook the screen model tick in the background; queued messages go out from it.
void init();

// Text starting with '/' runs as a command, anything else is sent as chat.
Result send(const std::string& text);

} // namespace chat
