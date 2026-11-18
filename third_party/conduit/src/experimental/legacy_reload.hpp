#pragma once
#include "core/reload_manager.hpp"
#include "core/hover_inspection.hpp"
#include "core/screen_metrics.hpp"
#include "experimental/legacy_reload_completion.hpp"
#include <memory>
#include <filesystem>
#include <string>
#include <vector>
namespace conduit::experimental {
std::unique_ptr<ReloadBackend> makeLegacyReloadBackend(const std::filesystem::path& diagnosticLog, bool automaticFrame = false);
bool armFromObservedThread(unsigned processId, unsigned threadId);
bool retireLegacyReloadHooks() noexcept;
void disableLegacyReload() noexcept;
std::string legacyReloadState();
// Resident copied diagnostics; no game pointer escapes the native callback.
ScreenMetrics& nativeScreenMetrics() noexcept;
bool isActiveTitleView(void* view, void* scene) noexcept;
bool isActiveWorldView(void* view, void* scene) noexcept;
bool inspectHoveredControl(void* view, void* scene, float x, float y, HoveredControl& result) noexcept;

// Runs on the verified UI thread once per natural frame after the backend is armed.
using FrameObserver = void (*)() noexcept;
void setFrameObserver(FrameObserver observer) noexcept;

struct ControlNode {
    float x = 0, y = 0, width = 0, height = 0;
    std::int32_t layer = 0;
    bool visible = false, positioned = false;
};
// The functions below are valid only inside the frame observer.
std::uintptr_t activeVisualRoot() noexcept;
bool readControlNode(std::uintptr_t control, ControlNode& node) noexcept;
bool readControlChildren(std::uintptr_t control, std::vector<std::uintptr_t>& children);
std::string readControlName(std::uintptr_t control);
std::string readControlType(std::uintptr_t control);
}
