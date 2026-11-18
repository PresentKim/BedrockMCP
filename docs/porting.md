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
