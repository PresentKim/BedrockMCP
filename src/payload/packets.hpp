#pragma once

#include <string>
#include <vector>

// AI tools over the packet capture of spyglass (github.com/EndstoneMC/spyglass), which
// runs in the same game process. They read its live capture files through
// tools/packetlog/packetlog.exe, which also decodes fields with gophertunnel.
namespace packets {

struct Result {
    bool ok = false;
    std::string text;
};

// Arguments are passed through to packetlog.exe as -flag value pairs.
Result run(const std::string& command, const std::vector<std::pair<std::string, std::string>>& flags);

// Named marks: start records the newest packet number, stop returns the packets after it.
Result session_start(const std::string& name);
Result session_stop(const std::string& name, std::vector<std::pair<std::string, std::string>> flags);

} // namespace packets
