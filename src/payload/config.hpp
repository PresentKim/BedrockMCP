#pragma once

#include <string>

namespace config {

// Load config.json from %APPDATA%\BedrockMCP, writing defaults if it is absent.
void load();

// Resolve an action name (e.g. "chat", "inventory") to the configured key name.
// Unknown names are returned unchanged, so raw key names pass through.
std::string key_for(const std::string& action);

// HTTP server bind port (default 18080).
int port();

// Optional bearer token required on MCP requests ("" = no auth).
std::string auth_token();

// Default input delivery: "sendinput", "postmessage" or "feed".
std::string input_mode();

// Byte-pattern signature for MouseDevice::feed (version-specific; editable so a
// game update can be handled without recompiling).
std::string feed_signature();

} // namespace config
