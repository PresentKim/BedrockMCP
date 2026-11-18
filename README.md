# BedrockMCP
An MCP server implementation for Minecraft (Windows), Inspired by [mcpelauncher-agent](https://github.com/bedrock-mc/mcpelauncher-agent).

## Features
- A DLL injected into `Minecraft.Windows.exe` (GDK build) that serves MCP over HTTP at `127.0.0.1:18080/mcp`.
- Screenshot capture via Windows.Graphics.Capture, falling back to reading the Direct3D 12 back buffer at `Present` where WGC is unavailable (Wine, such as [BedrockOnLinux](https://github.com/Wyze3306/BedrockOnLinux)).
- Mouse input through the game's `MouseDevice::feed` function, which registers clicks that SendInput and PostMessage do not.
- Keyboard input through PostMessage for menus and in-world movement.
- Legacy JSON UI tooling from [Conduit Client](https://github.com/Conduit-BE/Client) (exact builds 1.26.60.5, 1.26.62.1, 1.26.63.1 and 1.26.65.1): reload the UI definitions (`ui_reload`, or Ctrl+R in game), read the active screen's control, binding and factory counts (`ui_screen`), its control tree with rects in client pixels (`ui_tree`), and the control at a point (`ui_at`). Ore UI screens have no control tree. In game, F3 toggles Conduit's diagnostics panel (screen name, control, binding and factory counts, FPS) and F5 toggles the hover inspector (name, type, rect and parents of the control under the cursor); `ui_overlay` toggles both from MCP.
- JSON UI helpers on top of the control tree: `ui_click` (click a control by name), `wait_for_screen`, `ui_screenshot` (numbered boxes over controls plus a legend), a before and after diff in `ui_reload`, automatic reload when a `ui/*.json` file changes (`ui_auto_reload`; `ui_auto_reload` and `ui_watch_dirs` in `config.json`), and `content_log` (the game's newest `ContentLog*.txt`, whose new lines are also appended to `ui_reload`).
- Packet tools over [spyglass](https://github.com/EndstoneMC/spyglass), which captures every packet inside the client (F12 overlay): `packet_info`, `packet_find` (filters by name, id, direction, client decode failures, packet number range), `packet_get` (fields, client error with call stack, hex), `packet_wait` (block until a matching packet arrives), `packet_session` (packets between a start and a stop call), `packet_failures` and `packet_packs` (rebuild resource packs a server sent). They read spyglass's live capture in `%TEMP%\spyglass_*.cap` with `tools/packetlog/packetlog.exe`, which decodes fields with gophertunnel; build it with `go build` in that folder. A capture includes the login packet with your identity tokens, so do not share it. spyglass opens its overlay as soon as it loads; the payload hides it a few seconds later (set `spyglass_start_hidden` to `false` in `config.json` to keep it), and `spyglass_overlay` shows or hides it. This uses offsets from the PDB of the spyglass v0.4.0 release and does nothing on another spyglass build, where F12 still works.
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

### Always-on relay

`tools/relay.mjs` (Node 18+) keeps an MCP endpoint up at `127.0.0.1:18081/mcp` whether or not the game is running. It launches Minecraft when a tool is called while the game is closed, injects the payload whenever a new game process appears, and forwards tool calls once the payload answers. Until then, a call waits up to 20 seconds and returns an error that says what is happening and when to call again. It also adds a `game_status` tool.

```powershell
node tools\relay.mjs
```

Point the client at `http://127.0.0.1:18081/mcp` instead of port 18080. After injecting the payload, the relay also injects the last `spyglass-*.dll` (by name) in the repository's `spyglass\` folder (git-ignored) when there is one (set `BEDROCK_SPYGLASS_DLL` to use another file). Copy the DLL from a spyglass release that matches the game there, for example `spyglass-0.4.0-1.26.60.dll` for 1.26.60 to 1.26.65. The game failed to load the DLL from a folder under `%LOCALAPPDATA%`, so keep it outside AppData. The relay looks for the injector in `build\relwithdebinfo\`; set `BEDROCK_MCP_INJECTOR` to use another one. It does not forward `auth_token`. Logs and the cached tool list are in `%LOCALAPPDATA%\BedrockMCP\`.

## Notes

* Both mouse and keyboard input work without window focus. However, single-player pauses when the window loses focus, so in-world play requires the window to be focused.
* The mouse tools need `feed_ready: true` in `state`. It turns true without user input once the game has decrypted `MouseDevice::feed`; the payload then finds the static `MouseDevice` its callers pass.
* The Conduit hooks log to `%LOCALAPPDATA%\BedrockMCP\conduit.log`, and the overlay to `overlay.log` (factory rows while F3 is open). The overlay polls F3 and F5 every 15 ms, so synthetic key presses must be held briefly; the `key` tool's PostMessage presses do not reach it. Control types in `ui_tree` are known only for controls created after injection; `ui_reload` rebuilds them.
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

* [Conduit Client](https://github.com/Conduit-BE/Client) (MIT), vendored in `third_party/conduit`
* [spyglass](https://github.com/EndstoneMC/spyglass) (MIT), injected alongside the payload; `tools/packetlog` reads its capture format

Built with [Claude Code](https://claude.ai/code)