# Porting to a new game build

Most of BedrockMCP works on any build, but a few parts depend on addresses or layouts of one exact `Minecraft.Windows.exe` or of one spyglass release. This page lists them and how each was found, so the next build can be done the same way.

Keep a copy of each build's executable outside `%LOCALAPPDATA%` (for example `C:\Users\<you>\re-inputs\mcbe-win-<version>\`), because the game's own folder is protected and REA refuses ReFS drives. Read the file version from the PE version resource; the profiles below select on it.

| Part | Where it lives | What changes per build |
| --- | --- | --- |
| Mouse injection | `config.json` `feed_signature`, `src/payload/feed.cpp` | The byte pattern of `MouseDevice::feed` |
| JSON UI tools and F3/F5 overlay | `third_party/conduit/src/minecraft/versions/bedrock_*.hpp`, `compatibility.cpp` | RVAs of the reload, frame, screen and renderer functions, tables and masks |
| spyglass packet capture | `spyglass/spyglass-<ver>-<client>.dll` | Which spyglass DLL supports the build |
| spyglass overlay hide | `src/payload/spyglass.cpp` `kBuilds` | Offsets inside one spyglass DLL build |
| Packet decoding | `tools/packetlog/go.mod` (gophertunnel), `capture.go` | Protocol version, spyglass capture format |
| Live options (`options_get` live, `options_set`) | `src/payload/options.cpp` `k126651` | `OptionRegistry::get` RVA, registry and Option layouts |
| Direct chat (`send_chat`) | `src/payload/chat.cpp` | Screen model tick, chat send and command RVAs, model offsets |

## Live options

The game keeps every option in an `OptionRegistry`, an array of `unique_ptr<Option>` at `registry+0x10` indexed by option id (`0x339` entries in 1.26.65.1; `isValidOption` and `_registerOption` both compare against that count). There is no map by save name, so the payload walks the array and compares each option's save name.

How the 1.26.65.1 layout was found:

1. Save names such as `gfx_viewdistance` are copied inline with `movups` in the registration function, so `lea` searches miss them; search RIP-relative references to the string data instead. They lead to the registration function (`0x5b0c2c0`) and to `_registerOption` (`0x5af8ba0`), which stores into the array.
2. `OptionRegistry::get(out, id)` (`0x5af9150`) is a short function in the registry vtable at slots `+0x08` and `+0xd8`: `push rsi; sub rsp,20h; mov rsi,rdx; mov edx,r8d; call ...; mov [rsi],rax`. It returns through a hidden out pointer. The payload hooks it to capture the registry objects, because no global holds them; it picks the one whose path string at `registry+0x1a08` is the active `options.txt` (the game writes it with `/` separators).
3. The Option classes have one vtable per type; find them from the `_purecall` slots of the base vtable. Slot `+0x20` is load from a `std::string const&` (it only reads and copies the string, so a DLL-owned string is safe).
4. `Option+0x8` is the Impl: `+0x188` save name (`std::string`), `+0x1d4` type (0 bool, 2 float, 4 string, 5 int, 6 enum, 8 string list; 1 and 3 are per-input-mode and have an empty `+0x20`), `+0x238` override source (a shared option from another profile; follow it), `+0x260` a save-request functor whose second word (`+0x268`) is the owning registry.
5. Values live in the Option itself: bool `+0x10`; float value `+0x18`, min `+0x10`, max `+0x14`; int and enum value `+0x18`, max `+0x10`, min `+0x14`; string `+0x10`.
6. To save, call the registry vtable `+0x118` (`elevateSaveRequestMode`, argument 1) and `+0xa8` (`saveIfNeeded`), from the UI thread.

The string loader neither clamps nor converts units: `gfx_viewdistance` is 46 chunks live and 736 blocks in `options.txt`, and loading `"736"` stored 736 chunks. `options_set` therefore checks numbers against the option's own range before loading. Keybinds are not options; they are `KeyboardRemappingLayout` objects at `registry+0x1ab8`.

## Direct chat and commands

The chat screen's Enter button (`button.send`) calls `ChatScreenController::_sendChatMessage` (`0x4ff5e00`), which branches on a leading `/`:

- Chat: `MinecraftScreenModel::sendChatMessage(model, std::string const&)` (`0x2680900`). It copies the text into a `TextPacket` and sends it.
- Command: `0x683060(std::string const& command, NotNullNonOwnerPtr<IClientInstance>)`, keeping the `/`. The pointer argument is passed by value (a 0x18-byte `{handle, control, instance}` the callee releases), so the payload copies `model+0x48..0x58` and increments the control block's use count at `+8` first, as the game's own two call sites do.

Both must run on the UI thread with a live screen model. The payload hooks the screen model tick (`0x4d5c2e0`, `rcx` is the `ClientInstanceScreenModel`; `HudScreenController::tick` calls it every tick) and sends one queued message after the original returns. To find these again: start from the `button.send` name hash (`0x9c96df27`) handler, or from `ChatScreenController` code that compares the text with `"/cls"` (the dev-console check at controller `+0xdb3`); the command function then leads to `MinecraftCommands` and the `CommandRequestPacket` constructor (`0x4198950`, packet id `0x4d`).

## MouseDevice::feed

`feed` is the seven-argument `MouseDevice::feed(this, buttonId, buttonData, x, y, dx, dy, forceMotionlessPointer)`. Its prologue pushes eight registers, spills the four register arguments and reads the three stack arguments at `rsp+0xb0`, `0xb8`, `0xc0` and `0xc8` (`movzx ... word [rsp+0xc0]` and so on). That run of bytes, starting at the function's first byte, is the signature in `config.json`; regenerate it with `find_feed_signature.py`.

- The game decrypts `feed` lazily, so the payload retries the scan every 500 ms instead of failing at startup.
- The `MouseDevice` instance is static. Every caller loads it with `lea rcx, [rip+disp]` within 0x40 bytes before `call feed`; `find_static_instance` takes the address most callers use. If that vote stops working (an inlined caller, a different register), fall back to the MinHook detour in the same file, which captures `this` from the first real mouse event.
- spyglass hooks `feed` too and replaces its first bytes with a jump. The payload therefore does not hook `feed` once it has the static instance, and `find_patched_feed` scans for the signature without its first 16 bytes, accepting a match that starts with `E9` or `FF 25`. spyglass documents its own recipe for `feed` in `src/spyglass/signature.h` upstream; reuse it to confirm the function.

## Conduit profile (JSON UI tools, F3/F5 overlay)

Each supported build has a profile in `third_party/conduit/src/minecraft/versions/` and an entry in `compatibility.cpp` keyed on the exact PE file version. A build without a profile loads no Conduit hooks; the rest of the payload still works.

Upstream Conduit (`docs/reverse-engineering/1.26.6x-compatibility-profile.md` in Conduit-BE/Client) records each port. The method that carried 1.26.60.5 to 1.26.62.1, 1.26.63.1 and 1.26.65.1:

1. Take the previous profile's functions and bound each one with the PE unwind table (`.pdata`).
2. Disassemble both builds with Capstone and compare instruction bytes after masking RIP-relative displacements and relative call and branch operands. When a function matches over its full length, its layout is unchanged and only the address moved.
3. Match vtables in `.rdata` by resolving several function pointers at the same slot offsets in both images. From 1.26.63.1 to 1.26.65.1 the matched vtables moved by exactly `0x1000` and kept their slots.
4. Resolve the scene-name getter through the scene stack table slot `+0x128`.
5. Find the factory and binding component masks through the RIP-relative references from their component constructors.
6. Record the return addresses the hooks check (the frame caller and the primary screen-name caller) from the call sites.

Before hooking, Conduit checks each entry's prologue in committed executable memory (`matchesMainExecutableCode`), so a wrong address fails closed. After porting, check `%LOCALAPPDATA%\BedrockMCP\conduit.log` for `automatic_backend_armed`, call `ui_screen` on the title screen and `ui_reload` after a visible pack edit.

The F3/F5 renderer RVAs (`viewRenderRva`, `flushTextRva`, `drawTextRva`, `fillRva`, `setClipRva`, `textWidthRva`, the context and font vtables) are in the same profile and are found the same way.

## spyglass

spyglass ships one DLL per client line and loads only into a client whose version is at least the one it was built for (`verify_client` compares major.minor.patch). It depends on a single pattern, `MinecraftPackets::createPacket`, whose head bounds-checks the packet id and dispatches through a jump table (recipe in upstream `src/spyglass/signature.h`).

To check whether an existing spyglass DLL works on a new build before a spyglass release supports it:

1. Find `createPacket` in the new executable from the string `readNoHeader failed! packetId: {}`: it has one `lea` reference inside a small error helper, whose single caller is the `NetworkSystem` packet loop, and `createPacket` is the callee that starts with `cmp edx, <max id>` and a jump table.
2. Compare its first 48 bytes with the release pattern in `signature.h` (`56 48 83 EC 20 48 89 CE 81 FA ? 01 00 00 77 ? 89 D0 48 8D 0D ...` for release clients). For 1.26.65.1 it matched the 1.26.60 pattern byte for byte (`createPacket` at RVA `0x23764d0`, max id `0x169`), and `spyglass-0.4.0-1.26.60.dll` worked unchanged.
3. Inject it through the relay and look for a `spyglass_*.cap` in `%TEMP%` growing while you play, and for an empty error window in the F12 overlay.

Keep the DLL under its release file name and outside `%LOCALAPPDATA%`: a renamed copy, and a copy under `%LOCALAPPDATA%\BedrockMCP`, both failed to load into the game.

### Overlay hide offsets

`src/payload/spyglass.cpp` writes `spyglass::View::visible_` through `View::getInstance()`. Both come from the PDB in the spyglass release zip. For a new spyglass release, add a row to `kBuilds`:

```powershell
$pdb = "spyglass\spyglass-<ver>-<client>.pdb"
$util = "C:\Program Files (x86)\LLVM\bin\llvm-pdbutil.exe"
& $util dump -publics $pdb | Select-String "getInstance@View@spyglass" -Context 0,1
& $util dump -types $pdb | Select-String '`visible_`'
& $util dump -section-headers $pdb | Select-String "virtual address" | Select -First 1
```

- `getInstance` is printed as `addr = 0001:<offset>`; its RVA is the `.text` virtual address (`0x1000`) plus that offset. For v0.4.0 it is `0x87810`.
- `spyglass::decode_fields(Packet&, int)` (search the publics for `decode_fields@spyglass`; `0x91f10` in v0.4.0) is hooked to return a null JSON, because the 1.26.60 build crashed 1.26.65.1 inside it (`entt::meta_type::from_void`) while a server sent its resource pack list. Find the crashing function from the Windows Application log (faulting module and offset) with `llvm-symbolizer --obj=<dll> --relative-address <offset>`; it reads the PDB next to the DLL.
- The `visible_` member line gives its offset in `View` (3128 for v0.4.0).
- Take the PE `TimeDateStamp` and `SizeOfImage` of the DLL and the first 10 bytes at the `getInstance` RVA; the payload only writes when all three match.

## Packet decoding

`tools/packetlog` decodes spyglass capture bodies with gophertunnel. When the game's protocol number changes (`bedrock-protocol-docs` releases list it; 1.26.60 to 1.26.65 is 2225), update `github.com/sandertv/gophertunnel` in `go.mod` to a release whose `minecraft/protocol/info.go` has that `CurrentProtocol`, then `go test ./...`. Packets gophertunnel does not know show `gophertunnel_error` or `gophertunnel_unread`; the client's own decode result from spyglass is unaffected.

`capture.go` reads spyglass capture format version 2 (`SPYGCAP\0`, `.cap` blobs and 32-byte `.index` entries, from upstream `src/spyglass/overlay/store.cpp`). If a spyglass release changes `kVersion` there, the reader refuses the file with a message naming both versions; update the entry layout from `store.h`.

## Packet hooks without spyglass

Commit `1dedf03` hooked the plaintext packet path directly, before spyglass replaced it. If spyglass ever stops supporting a build, these 1.26.65.1 functions are the places to start (all checked against LeviLamina 26.60.8 and Endstone headers):

- `NetworkSystem::send` (`0x2360650`) writes the varuint header (`id | recipient << 10 | sender << 12`) into the stream at `this+0x190`, calls the packet's vtable `+0x28` (`writeWithSerializationMode`), then calls `NetworkSystem::_sendInternal` (`0x23830c0`, arguments `NetworkIdentifier const&, Packet const&, std::string const& data`), which every sent packet passes through, in local worlds for both the client and the in-process server.
- `NetworkSystem::_sortAndPacketizeEvents` (`0x237dc90`, found from its own name string) receives one packet at a time through `connection->mPeer (+0x118)` vtable `+0x40`, which is `PacketTraceNetworkPeer::_receivePacket` (`0x226b2b0`, the outermost peer for both local and remote connections; `0` means data). The receive buffer is `NetworkSystem+0x150`.
- `LoopbackPacketSender::sendToServer` (`0x2360d40`, vtable `0x14f1395d0`) is only used by the client, which tells the client `NetworkSystem` from the server one in a local world.
- Packet vtable: `+0x08` getId, `+0x10` getName (returns the class name string; packet classes have no RTTI, so find a packet's vtable from `lea` references to the function that returns its name), `+0x28` writeWithSerializationMode, `+0x48` read with reflection context (`NetworkSystem+0x230`).
