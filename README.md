# BedrockMCP
An MCP server implementation for Minecraft (Windows), Inspired by [mcpelauncher-agent](https://github.com/bedrock-mc/mcpelauncher-agent).

## Features
- A DLL injected into `Minecraft.Windows.exe` (GDK build) that serves MCP over HTTP at `127.0.0.1:18080/mcp`.
- Screenshot capture via Windows.Graphics.Capture, falling back to reading the Direct3D 12 back buffer at `Present` where WGC is unavailable (Wine, such as [BedrockOnLinux](https://github.com/Wyze3306/BedrockOnLinux)).
- Mouse input through the game's `MouseDevice::feed` function, which registers clicks that SendInput and PostMessage do not.
- Keyboard input through PostMessage for menus and in-world movement.
- Tools: `screenshot`, `state`, `move`, `click`, `look`, `scroll` (menus only, not hotbar), `key`, `hold_key` (hold a key for a duration), `type`, `chat`, `batch` (several actions in one call for precise timing, e.g. double-tap to fly or holding two keys at once), `wait`, `focus`, `open_uri`, `add_server` (add a server to the Servers tab), `connect_server` (join a server directly, no menu navigation), `ping`.

## Setup
Download `bedrock_mcp.dll`, `bedrock_mcp_injector.exe`, `find_feed_signature.py`, and `config.example.json` from the latest release, and keep them in the same folder. The server reads `config.json` from that folder and creates it with defaults on first run, so copying `config.example.json` to `config.json` is optional (do it to customize port, auth, or keybinds).

Start Minecraft, then run the injector:

```powershell
.\bedrock_mcp_injector.exe
```

Register the server with Claude Code:

```powershell
claude mcp add --transport http bedrock http://127.0.0.1:18080/mcp
```

Other MCP clients take an `mcpServers` entry in their config JSON:

```json
{
  "mcpServers": {
    "bedrock": {
      "type": "http",
      "url": "http://127.0.0.1:18080/mcp"
    }
  }
}
```

If `auth_token` is set in `config.json`, pass it as a Bearer header:

```json
{
  "mcpServers": {
    "bedrock": {
      "type": "http",
      "url": "http://127.0.0.1:18080/mcp",
      "headers": { "Authorization": "Bearer YOUR_TOKEN" }
    }
  }
}
```

## Notes

* Both mouse and keyboard input work without window focus. However, single-player pauses when the window loses focus, so in-world play requires the window to be focused.
* Open a world and move the mouse once before using the mouse tools. `state` will report `feed_ready: true` when ready.
* Re-inject after each game restart.
* After a game update, regenerate `feed_signature` with `find_feed_signature.py` and place it in `config.json` (in the same folder as the DLL and injector).

## Build from source

Requirements: Visual Studio with the "C++ Clang tools for Windows" component and Windows SDK, CMake 4.3+, Ninja, and Git.

From Developer PowerShell for VS:

```powershell
cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo
```

Build outputs are generated in `build\relwithdebinfo\`. Close Minecraft before rebuilding, as the loaded DLL will be locked.

## Credits

* [spyglass](https://github.com/EndstoneMC/spyglass)

Built with [Claude Code](https://claude.ai/code)