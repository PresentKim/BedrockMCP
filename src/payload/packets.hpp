#pragma once

#include <string>

// Logs every game packet the client sends or receives as plaintext bytes
// (varuint header plus payload) to a .bmpl file that tools/packetlog decodes.
// In a local world the in-process server's packets are logged too.
namespace packets {

struct Result {
    bool ok = false;
    std::string text;
};

// Install the hooks in the background once the exact supported game build is confirmed.
void init();

Result start();
Result stop();
Result status();

struct DecodeOptions {
    int skip = 0;
    int limit = 100;
    std::string name;
    std::string dir;
    int max_json = 2000;
    std::string file;
};
Result decode(const DecodeOptions& options);

// Rebuild resource packs carried by ResourcePackChunkData packets into out_dir.
Result extract_packs(const std::string& file, const std::string& out_dir);

} // namespace packets
