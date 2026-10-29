#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skate3 {

struct LocalProfile {
  std::string id;
  std::string gamertag;
  uint64_t xuid = 0;
  // Tested and ruled out: temporarily defaulting these to false (disabling
  // forced LIVE sign-in at boot) did NOT stop the Team/Clubs boot-time crash
  // chain, so it isn't gated on this state - restored to true, which is
  // needed to avoid "You need to be signed into Xbox LIVE".
  bool signed_in = true;
  // Signed-in profiles are LIVE-signed-in: online play goes to the custom
  // Blaze server (net_host_overrides). With this false, XamUserGetSigninState
  // returned 1 (local) and retail refused online with "You need to be signed
  // into Xbox LIVE". Matches sk83_na_m, which runs on the SDK's LIVE default.
  bool live_signed_in = true;
};

struct LocalProfileStore {
  std::string selected_profile;
  std::vector<LocalProfile> profiles;
};

std::filesystem::path ProfilesFilePath(const std::filesystem::path& user_data_root);
LocalProfileStore LoadProfiles(const std::filesystem::path& profiles_path);
bool SaveProfiles(const std::filesystem::path& profiles_path, const LocalProfileStore& store);
LocalProfile MakeDefaultProfile(std::string gamertag);
LocalProfile* FindSelectedProfile(LocalProfileStore& store);
const LocalProfile* FindSelectedProfile(const LocalProfileStore& store);
void EnsureUsableProfileStore(LocalProfileStore& store, std::string default_gamertag);
void ApplyProfileCvars(const LocalProfile& profile);
void ApplyVideoCvars(int resolution_scale, double refresh_rate);
std::string FormatXuid(uint64_t xuid);

}  // namespace skate3
