"""Recover the MouseDevice::feed signature for the current Minecraft Bedrock build.

Run this after a game update when mouse (feed) stops working. It attaches to the
running Minecraft.Windows.exe, checks whether the configured signature still
matches, and if not, relocates feed with a wildcarded pattern and prints a fresh
signature to paste into %APPDATA%\\BedrockMCP\\config.json ("feed_signature").

IMPORTANT, two conditions, or the scan finds nothing:
  1. Be IN A WORLD and move the mouse once first - the game decrypts
     MouseDevice::feed only on its first use, so its code page is otherwise blank.
  2. Run on a game WITHOUT bedrock_mcp.dll hooking feed. Our own MinHook rewrites
     feed's prologue bytes, which hides the signature. If state reports
     feed_ready:true, the DLL already hooked it - in that case the signature is
     fine and you do not need this tool. This tool is for when a game update broke
     the signature (feed_ready stays false): then nothing is hooked and the
     prologue is intact for the relaxed pattern to relocate.

Requires frida. Run with uv (no install needed):
    uv run --with frida python tools/find_feed_signature.py
or with pip:
    pip install frida && python tools/find_feed_signature.py
"""

import json
import os
import sys

try:
    import frida
except ImportError:
    sys.exit("frida not installed. Run: uv run --with frida python tools/find_feed_signature.py")

PROCESS = "Minecraft.Windows.exe"

# The 60-byte feed prologue. The wildcarded variant tolerates a changed stack
# frame (the sub-rsp immediate and the four stack-arg displacements) so it can
# relocate feed across minor version changes.
EXACT_DEFAULT = (
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 44 89 CF 44 89 C3 89 D5 "
    "48 89 CE 44 0F B7 A4 24 C0 00 00 00 44 0F B7 AC 24 B8 00 00 00 44 0F B7 "
    "BC 24 B0 00 00 00 0F B6 84 24 C8 00 00 00"
)
RELAXED = (
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC ?? 44 89 CF 44 89 C3 89 D5 "
    "48 89 CE 44 0F B7 A4 24 ?? ?? 00 00 44 0F B7 AC 24 ?? ?? 00 00 44 0F B7 "
    "BC 24 ?? ?? 00 00 0F B6 84 24 ?? ?? 00 00"
)

JS = r"""
rpc.exports = {
  scan: function (pattern) {
    const m = Process.getModuleByName('Minecraft.Windows.exe');
    const out = [];
    const ranges = Process.enumerateRanges('r-x').filter(
      (r) => r.base.compare(m.base) >= 0 && r.base.compare(m.base.add(m.size)) < 0
    );
    for (const r of ranges) {
      try { for (const f of Memory.scanSync(r.base, r.size, pattern)) out.push('0x' + f.address.sub(m.base).toString(16)); } catch (e) {}
    }
    return out;
  },
  dump: function (rva, n) {
    const m = Process.getModuleByName('Minecraft.Windows.exe');
    const bytes = m.base.add(parseInt(rva, 16)).readByteArray(n);
    return Array.from(new Uint8Array(bytes));
  },
};
"""


def config_path():
    appdata = os.environ.get("APPDATA", "")
    return os.path.join(appdata, "BedrockMCP", "config.json")


def load_configured_signature():
    try:
        with open(config_path(), encoding="utf-8") as f:
            sig = json.load(f).get("feed_signature", "")
            if sig:
                return sig
    except OSError:
        pass
    return EXACT_DEFAULT


def to_sig(byte_list):
    return " ".join("%02X" % b for b in byte_list)


def main():
    device = frida.get_local_device()
    procs = [p for p in device.enumerate_processes() if p.name == PROCESS]
    if not procs:
        sys.exit(f"{PROCESS} is not running.")
    session = device.attach(procs[0].pid)
    script = session.create_script(JS)
    script.load()
    api = script.exports_sync if hasattr(script, "exports_sync") else script.exports

    configured = load_configured_signature()
    exact_hits = api.scan(configured)
    print(f"Configured signature matches: {len(exact_hits)} {exact_hits}")

    if len(exact_hits) == 1:
        print("OK - the configured feed_signature is still current. No update needed.")
        print(f"feed RVA: {exact_hits[0]}")
        session.detach()
        return

    print("\nConfigured signature did not match uniquely. Trying the relaxed pattern...")
    relaxed_hits = api.scan(RELAXED)
    print(f"Relaxed pattern matches: {len(relaxed_hits)} {relaxed_hits}")

    if len(relaxed_hits) == 1:
        rva = relaxed_hits[0]
        fresh = to_sig(api.dump(rva, 60))
        print(f"\nFound feed at RVA {rva}. Paste this into config.json \"feed_signature\":\n")
        print(fresh)
        print("\n(If a later update shifts bytes again, the robust wildcarded pattern also works:)")
        print(RELAXED)
    elif len(relaxed_hits) == 0:
        print("\nNothing found. Either:")
        print("  - bedrock_mcp.dll already hooked feed (state feed_ready:true) - then the")
        print("    signature is fine and you don't need this tool; or")
        print("  - feed is not decrypted yet - be IN A WORLD, move the mouse once, and on a")
        print("    game instance WITHOUT the DLL injected, then run this again.")
    else:
        print("\nMultiple matches - the pattern is no longer unique; widen it by hand using the")
        print("RVAs above (compare the functions in a disassembler).")

    session.detach()


if __name__ == "__main__":
    main()
