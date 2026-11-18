# Conduit Client (vendored subset)

Source: https://github.com/Conduit-BE/Client at commit `204ce670371fb92de2cb295b29dfd8984e13cd3e`, MIT licensed (see `LICENSE`).

Only the files needed for the legacy JSON UI reload backend, screen diagnostics and control types are kept. The overlay, launcher, settings and file watcher are not included.

Local changes in `src/experimental/legacy_reload.cpp` and `.hpp`:

- `MH_Initialize` may already have run (the payload's feed hook shares MinHook), and a failed install does not call `MH_Uninitialize`.
- `setFrameObserver` runs a callback on the verified UI thread each natural frame.
- `activeVisualRoot`, `readControlNode`, `readControlChildren`, `readControlName` and `readControlType` read the active screen's control tree from inside that callback. The hover inspector's control offsets moved to namespace scope so both use them.
