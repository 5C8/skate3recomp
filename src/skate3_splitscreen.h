#pragma once

// Splitscreen (launch option --splitscreen), shared by sk83_na_m and
// skate3recomp (keep the two copies identical).
//
// Splitscreen is several copies of the game running at once, one window and
// one controller each, every copy signed in with its own profile, so the
// players meet online like on separate consoles:
//
//  1. The launching process shows the seat screen: a map of the monitors.
//     Every controller moves between them and takes a spot (A). A monitor
//     taken by more than one controller is divided (Y flips side-by-side /
//     top-bottom; four players get quarters). Every other monitor shows its
//     number and the spots taken on it.
//  2. Start launches one game process per player (cvars splitscreen_session /
//     splitscreen_player, input_bound_pad, window_fullscreen_rect, and
//     audio_mute for everyone but player 1), then the launcher quits.
//  3. Each window opens the profile picker on its own controller. The
//     pickers share a session (shared memory + a named mutex): a profile one
//     player signed in with is unavailable to the others, profile edits show
//     up in every window, and all windows boot together once every player
//     has signed in.
//
// Windows only; elsewhere --splitscreen is ignored.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace rex::ui {
class ImGuiDrawer;
class Window;
class WindowedAppContext;
}  // namespace rex::ui

namespace skate3::splitscreen {

constexpr int kMaxPlayers = 8;
constexpr int kMaxPlayersPerMonitor = 4;

// --splitscreen was given and this process is the launcher.
bool IsLauncher();
// This process is one player's window (started by the launcher).
bool IsPlayer();
// 1-based player number of this window; 0 when not a player window.
int PlayerNumber();

// Player colour (ImGui IM_COL32 packing: 0xAABBGGRR), player is 1-based.
uint32_t PlayerColor(int player);

// The splitscreen rectangle this window covers (window_fullscreen_rect), for
// code that sizes output to the display. False when not set.
bool FullscreenRectSize(int32_t& width, int32_t& height);

// Player windows: Windows only lowers the taskbar for windows covering a
// whole monitor, so a window on part of one keeps the taskbar on top of it.
// This keeps the window above the taskbar while any splitscreen window has
// focus (and lets it drop back when another app is in front).
void KeepAboveTaskbar(void* native_window);

// The launcher: blocks on the seat screen, then starts the player windows.
// Returns true when the windows were started. The caller quits either way.
bool RunLauncher(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                 rex::ui::ImGuiDrawer* drawer);

// The players' shared profile state. Get() is null outside a player window or
// when the session could not be opened (the window then signs in on its own).
class Session {
 public:
  static Session* Get();

  int player() const { return player_; }
  // The controller this window reads ("xinput:N" / "sdl:<device path>").
  const std::string& pad_id() const { return pad_id_; }

  // The other live player signed in with `profile_id`, 0 when it is free.
  int TakenBy(std::string_view profile_id);
  // Signs this window in with the profile; false if another player has it.
  bool Claim(std::string_view profile_id, std::string_view gamertag);
  // Back to choosing (the profile is free again).
  void Release();
  // Whether `pid` is one of this session's live player windows.
  bool IsPlayerProcess(uint32_t pid);
  // Players still in the session, and how many of them have signed in.
  void Count(int& signed_in, int& players);
  bool EveryoneSignedIn() {
    int signed_in = 0, players = 0;
    Count(signed_in, players);
    return players > 0 && signed_in == players;
  }

  // The profile store is shared by every window: edits run under the session
  // lock and bump the generation so the other pickers reload.
  void WithLock(const std::function<void()>& fn);
  uint32_t StoreGeneration();
  void BumpStoreGeneration();  // call while holding the lock

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

 private:
  Session() = default;
  struct Impl;
  Impl* impl_ = nullptr;
  int player_ = 0;
  std::string pad_id_;
};

}  // namespace skate3::splitscreen
