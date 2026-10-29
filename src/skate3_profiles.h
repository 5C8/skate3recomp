#pragma once

// Local player profiles, shared by sk83_na_m and skate3recomp (keep the two
// copies identical).
//
// A profile is the identity the game signs in with: a gamertag and a stable
// XUID (saves are stored per XUID, the server logs players in by both). The
// store lives in <user data>/profiles/profiles.toml together with the
// "sign in automatically" switch.
//
// Boot: with auto sign-in on and a profile to sign in, the last profile is
// applied silently; otherwise an Xbox-style picker opens before the game
// starts (select, create, rename, delete, and the auto sign-in toggle). The
// chosen profile is applied through the SDK's profile cvars, which the
// kernel's user profile reads when the runtime is constructed.
//
// Splitscreen (skate3_splitscreen.h): every player window opens the picker
// on its own controller; profiles other players signed in with are taken,
// edits are shared live, and the windows boot once all have signed in.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::ui {
class ImGuiDrawer;
class Window;
class WindowedAppContext;
}  // namespace rex::ui

namespace skate3::profiles {

struct Profile {
  std::string id;        // file-safe key, stable across renames
  std::string gamertag;  // what the game and the server show
  uint64_t xuid = 0;     // stable across renames (saves are keyed by it)
  // Signed-in profiles are LIVE-signed-in: online play goes to the custom
  // server (net_host_overrides); a local-only sign-in makes the game refuse
  // online with "You need to be signed into Xbox LIVE".
  bool signed_in = true;
  bool live_signed_in = true;
};

struct Store {
  std::string selected;     // Profile::id of the last profile signed in
  bool auto_sign_in = true;  // sign the last profile in at boot without asking
  std::vector<Profile> profiles;
};

// Gamertag rules (Xbox): at most 15 characters, letters, digits and spaces.
constexpr size_t kMaxGamertag = 15;

std::filesystem::path StorePath(const std::filesystem::path& user_data_root);
Store Load(const std::filesystem::path& path);
bool Save(const std::filesystem::path& path, const Store& store);

Profile* Selected(Store& store);
const Profile* Selected(const Store& store);
// A new profile with a unique id and its own stable XUID.
Profile MakeProfile(const Store& store, const std::string& gamertag);
std::string SanitizeGamertag(const std::string& text);
// The Windows user name as a gamertag (the SDK's own default).
std::string DefaultGamertag();

// Publishes the profile to the SDK's profile cvars (user_profile_name,
// user_profile_xuid, sign-in state).
void ApplyCvars(const Profile& profile);

// Boot flow. Loads the store (on first run seeding it from the identity the
// game has been using, so existing saves stay attached), shows the picker
// when auto sign-in is off or there is nothing to sign in, and applies the
// chosen profile. Blocks while the picker is open. Returns false if the
// window was closed instead.
bool SignInAtBoot(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                  rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& store_path);

// Runtime edits from the settings menu.
std::string SelectedGamertag(const std::filesystem::path& store_path);
// Renames the signed-in profile (id and XUID unchanged); false if the name is
// empty after sanitizing. Applies the cvars; the running game keeps the old
// name until restart.
bool RenameSelected(const std::filesystem::path& store_path, const std::string& gamertag);
bool AutoSignIn(const std::filesystem::path& store_path);
void SetAutoSignIn(const std::filesystem::path& store_path, bool enabled);

}  // namespace skate3::profiles
