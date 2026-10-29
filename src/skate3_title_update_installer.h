#pragma once

#include <filesystem>
#include <functional>

#include <rex/rex_app.h>

namespace skate3 {

// True when each executable is already TU3, or is the base version with its
// matching, SHA-256-verified TU3 sidecar. Mixed installations are supported.
bool IsTitleUpdateInstalled(const std::filesystem::path& game_root);

// Validate both bundled payloads before writing either into the installed game.
bool StageTitleUpdateFromDirectory(const std::filesystem::path& source_root,
                                  const std::filesystem::path& game_root, std::string& error);
bool TryInstallBundledTitleUpdate(const std::filesystem::path& game_root);

// Stages the title update payloads into game_root from a local source file:
// either the TU STFS package (CON/LIVE/PIRS container) or a raw .xexp payload.
bool StageTitleUpdateFromFile(const std::filesystem::path& source,
                              const std::filesystem::path& game_root, std::string& error);

void ShowTitleUpdateInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                                  std::function<void(rex::PathConfig)> complete);
bool RunTitleUpdateInstallWizardBlocking(rex::ui::WindowedAppContext& app_context,
                                         rex::ui::Window* window,
                                         rex::ui::ImGuiDrawer* drawer,
                                         rex::PathConfig runtime_paths,
                                         rex::PathConfig& installed_paths);

}  // namespace skate3
