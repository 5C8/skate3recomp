// Local player profiles + the boot-time profile picker (see skate3_profiles.h).
// Shared by sk83_na_m and skate3recomp: keep the two copies identical.

#include "skate3_profiles.h"
#include "skate3_splitscreen.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <toml++/toml.hpp>

#include <SDL3/SDL.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <rex/ui/window_win.h>
#endif

namespace skate3::profiles {
namespace {

std::string EnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

std::string OsUserName() {
  std::string user = EnvOrEmpty("USERNAME");
  return user.empty() ? EnvOrEmpty("USER") : user;
}

// The SDK's kernel profile (xam/user_profile.cpp) substitutes these when the
// cvars hold their defaults; the first-run seed below mirrors that exactly so
// a build that ran on the defaults keeps the same identity (and its saves).
constexpr std::string_view kSdkDefaultName = "Player";
constexpr std::string_view kSdkDefaultXuid = "B13E07DFF9AB6772";

uint64_t SdkDefaultXuid() {
  const std::string seed =
      EnvOrEmpty("COMPUTERNAME") + EnvOrEmpty("HOSTNAME") + "|" + OsUserName();
  uint64_t hash = 0xCBF29CE484222325ULL;
  for (char c : seed) {
    hash = (hash ^ static_cast<uint8_t>(c)) * 0x100000001B3ULL;
  }
  return 0x0009000000000000ULL | (hash & 0x0000FFFFFFFFFFFFULL);
}

uint64_t ParseXuid(std::string_view value) {
  std::string text(value);
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.erase(0, 2);
  }
  uint64_t parsed = 0;
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
  if (ec == std::errc() && ptr == text.data() + text.size()) {
    return parsed;
  }
  parsed = 0;
  std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
  return parsed;
}

std::string FormatXuid(uint64_t xuid) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << xuid;
  return stream.str();
}

std::string TomlString(std::string_view value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string MakeId(std::string_view gamertag) {
  std::string id;
  bool previous_dash = false;
  for (char c : gamertag) {
    char out = 0;
    if (c >= 'A' && c <= 'Z') {
      out = static_cast<char>(c - 'A' + 'a');
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out = c;
    } else if (!previous_dash) {
      out = '-';
    }
    if (out) {
      id.push_back(out);
      previous_dash = out == '-';
    }
  }
  while (!id.empty() && id.back() == '-') {
    id.pop_back();
  }
  return id.empty() ? std::string("profile") : id;
}

// 0xB13E prefix: the 58410A1F permission check rejects XUIDs with any of the
// 0x00C0000000000000 bits set ("You do not have permissions..."); this prefix
// keeps them clear.
uint64_t XuidForId(std::string_view id) {
  uint64_t hash = 1469598103934665603ull;
  for (char c : id) {
    hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  }
  return 0xB13E000000000000ull | (hash & 0x00003FFFFFFFFFFFull);
}

bool SameName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool NameTaken(const Store& store, std::string_view name, const Profile* except) {
  for (const Profile& p : store.profiles) {
    if (&p != except && SameName(p.gamertag, name)) {
      return true;
    }
  }
  return false;
}

// First run: the identity the game has been signing in with until now (the
// profile cvars as loaded from the config, after the SDK's own default
// substitution), so existing saves and the server account carry over.
Profile SeedFromCurrentIdentity() {
  Profile p;
  const std::string name =
      (rex::cvar::GetFlagInfo("user_profile_name") != nullptr) ? rex::cvar::Query<std::string>("user_profile_name")
                                              : std::string();
  p.gamertag = (name.empty() || name == kSdkDefaultName) ? DefaultGamertag()
                                                          : SanitizeGamertag(name);
  if (p.gamertag.empty()) {
    p.gamertag = DefaultGamertag();
  }
  const std::string xuid =
      (rex::cvar::GetFlagInfo("user_profile_xuid") != nullptr) ? rex::cvar::Query<std::string>("user_profile_xuid")
                                              : std::string(kSdkDefaultXuid);
  p.xuid = xuid == kSdkDefaultXuid ? SdkDefaultXuid() : ParseXuid(xuid);
  if (p.xuid == 0) {
    p.xuid = SdkDefaultXuid();
  }
  p.id = MakeId(p.gamertag);
  return p;
}

// ---------------------------------------------------------------------------
// Picker dialog
// ---------------------------------------------------------------------------

// Controller state for the picker, as XInput-style button bits. The game's
// input system does not exist yet at boot, so the picker opens its own SDL3
// gamepad session (the same library and PlayStation HIDAPI settings as the
// in-game SDL backend: DualSense / DualShock 4 / Switch Pro / Xbox, USB or
// Bluetooth), with direct XInput as the fallback. The session is closed when
// the picker finishes, before the game's input backend starts. A splitscreen
// window reads only its own controller ("xinput:N" / "sdl:<device path>",
// the same ids as the SDK's input_bound_pad).
struct PadState {
  bool connected = false;
  uint16_t buttons = 0;
  int16_t lx = 0;
  int16_t ly = 0;  // XInput convention: up is positive
};

constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008,
                   kPadStart = 0x0010, kPadBack = 0x0020, kPadLB = 0x0100, kPadRB = 0x0200,
                   kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000;

class PadReader {
 public:
  explicit PadReader(std::string only_pad = {}) : only_pad_(std::move(only_pad)) {
    // "sdl:<path>|serial=<serial>": matched by path, else by serial.
    if (const size_t at = only_pad_.rfind("|serial="); at != std::string::npos) {
      only_serial_ = only_pad_.substr(at + 8);
      only_pad_.resize(at);
    }
    if (only_pad_.starts_with("xinput:")) {
      xinput_slot_ = int(only_pad_.back() - '0');
      return;  // XInput only: no SDL session needed
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
    if (!only_pad_.empty()) {
      // Splitscreen windows read their pad while another window has focus.
      SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    sdl_ok_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!sdl_ok_) {
      REXLOG_WARN("profiles: SDL gamepad init failed ({}); XInput only", SDL_GetError());
    }
  }
  ~PadReader() {
    for (SDL_Gamepad* g : pads_) {
      SDL_CloseGamepad(g);
    }
    if (sdl_ok_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
  }

  PadState Poll() {
    PadState out;
    if (xinput_slot_ >= 0) {
      PollXInput(out, xinput_slot_);
      return out;
    }
    if (sdl_ok_) {
      SDL_PumpEvents();
      SDL_UpdateGamepads();
      if (rescan_-- <= 0) {  // hot-plug: open pads attached while the picker is up
        rescan_ = 30;
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
          for (int i = 0; i < count; ++i) {
            const bool open = std::any_of(pads_.begin(), pads_.end(), [&](SDL_Gamepad* g) {
              return SDL_GetGamepadID(g) == ids[i];
            });
            if (!open) {
              if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) {
                pads_.push_back(g);
                REXLOG_INFO("profiles: controller '{}' ready", SDL_GetGamepadName(g));
              }
            }
          }
          SDL_free(ids);
        }
      }
      static constexpr struct {
        SDL_GamepadButton b;
        uint16_t bit;
      } kMap[] = {
          {SDL_GAMEPAD_BUTTON_DPAD_UP, kPadUp},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDown},
          {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadRight},
          {SDL_GAMEPAD_BUTTON_START, kPadStart},    {SDL_GAMEPAD_BUTTON_BACK, kPadBack},
          {SDL_GAMEPAD_BUTTON_TOUCHPAD, kPadBack},  {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadLB},
          {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadRB},
          {SDL_GAMEPAD_BUTTON_SOUTH, kPadA},        {SDL_GAMEPAD_BUTTON_EAST, kPadB},
          {SDL_GAMEPAD_BUTTON_WEST, kPadX},         {SDL_GAMEPAD_BUTTON_NORTH, kPadY},
      };
      for (auto it = pads_.begin(); it != pads_.end();) {
        SDL_Gamepad* g = *it;
        if (!SDL_GamepadConnected(g)) {
          SDL_CloseGamepad(g);
          it = pads_.erase(it);
          continue;
        }
        if (!only_pad_.empty()) {
          const char* path = SDL_GetGamepadPath(g);
          const char* serial = SDL_GetGamepadSerial(g);
          const bool same_path = path != nullptr && only_pad_ == std::string("sdl:") + path;
          const bool same_serial = !only_serial_.empty() && serial != nullptr &&
                                   only_serial_ == serial;
          if (!same_path && !same_serial) {
            ++it;
            continue;
          }
        }
        out.connected = true;
        for (const auto& m : kMap) {
          if (SDL_GetGamepadButton(g, m.b)) {
            out.buttons |= m.bit;
          }
        }
        const int16_t x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
        const int16_t y = int16_t(std::clamp(-int(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY)),
                                             -32768, 32767));
        if (std::abs(x) > std::abs(out.lx)) out.lx = x;
        if (std::abs(y) > std::abs(out.ly)) out.ly = y;
        ++it;
      }
    }
    if (!out.connected && only_pad_.empty()) {
      PollXInput(out);
    }
    return out;
  }

 private:
  // All slots, or only `only_slot`.
  static void PollXInput(PadState& out, int only_slot = -1) {
#if defined(_WIN32)
    struct XState {
      DWORD packet;
      WORD buttons;
      BYTE lt, rt;
      SHORT lx, ly, rx, ry;
    };
    using GetStateFn = DWORD(WINAPI*)(DWORD, XState*);
    static GetStateFn get_state = []() -> GetStateFn {
      for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
        if (HMODULE m = LoadLibraryA(dll)) {
          if (auto fn = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"))) {
            return fn;
          }
        }
      }
      return nullptr;
    }();
    if (get_state == nullptr) {
      return;
    }
    for (DWORD i = 0; i < 4; ++i) {
      if (only_slot >= 0 && DWORD(only_slot) != i) {
        continue;
      }
      XState s{};
      if (get_state(i, &s) == ERROR_SUCCESS) {
        out.connected = true;
        out.buttons |= s.buttons;
        if (std::abs(s.lx) > std::abs(out.lx)) out.lx = s.lx;
        if (std::abs(s.ly) > std::abs(out.ly)) out.ly = s.ly;
      }
    }
#else
    (void)out;
    (void)only_slot;
#endif
  }

  std::string only_pad_;
  std::string only_serial_;
  int xinput_slot_ = -1;
  bool sdl_ok_ = false;
  int rescan_ = 0;
  std::vector<SDL_Gamepad*> pads_;
};

Profile* FindById(Store& store, std::string_view id) {
  for (Profile& p : store.profiles) {
    if (p.id == id) {
      return &p;
    }
  }
  return nullptr;
}

// Splitscreen windows share one store: edits go to disk at once under the
// session lock, reloading first so no window's edit overwrites another's.
// Without a session, edits stay in memory until the picker finishes.
bool EditStore(splitscreen::Session* session, const std::filesystem::path& path, Store& store,
               const std::function<bool(Store&)>& fn) {
  if (session == nullptr) {
    return fn(store);
  }
  bool ok = false;
  session->WithLock([&] {
    Store disk = Load(path);
    ok = fn(disk);
    if (ok) {
      Save(path, disk);
      session->BumpStoreGeneration();
    }
    store = std::move(disk);
  });
  return ok;
}

class PickerDialog final : public rex::ui::ImGuiDialog {
 public:
  using Done = std::function<void(Store)>;
  PickerDialog(rex::ui::ImGuiDrawer* drawer, Store store, Done done,
               splitscreen::Session* session = nullptr, std::filesystem::path store_path = {})
      : ImGuiDialog(drawer),
        pad_(session                    ? session->pad_id()
             : splitscreen::IsPlayer() ? rex::cvar::Query<std::string>("input_bound_pad")
                                       : std::string()),
        store_(std::move(store)),
        done_(std::move(done)),
        session_(session),
        store_path_(std::move(store_path)) {
    if (session_) {
      store_generation_ = session_->StoreGeneration();
    }
    SelectId(store_.selected);
    if (store_.profiles.empty()) {
      BeginEdit(Mode::kCreate);
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // kWaiting: splitscreen, signed in, waiting for the other players.
  enum class Mode { kList, kCreate, kRename, kDelete, kWaiting };

  void SelectId(std::string_view id) {
    for (size_t i = 0; i < store_.profiles.size(); ++i) {
      if (store_.profiles[i].id == id) {
        sel_ = int(i);
        return;
      }
    }
    sel_ = std::clamp(sel_, 0, EntryCount() - 1);
  }

  std::string SelectedId() const {
    return OnCreateEntry() ? std::string() : store_.profiles[size_t(sel_)].id;
  }

  // Splitscreen: the other player signed in with profile i (0 = free).
  int TakenBy(int i) const {
    return i >= 0 && size_t(i) < taken_.size() ? taken_[size_t(i)] : 0;
  }

  // Splitscreen: pick up profile edits made in the other windows, and who
  // holds which profile.
  void SyncShared() {
    if (!session_) {
      return;
    }
    const uint32_t generation = session_->StoreGeneration();
    if (generation != store_generation_) {
      const std::string id = SelectedId();
      session_->WithLock([&] { store_ = Load(store_path_); });
      store_generation_ = generation;
      SelectId(id);
      if ((mode_ == Mode::kRename || mode_ == Mode::kDelete) &&
          (id.empty() || SelectedId() != id)) {
        mode_ = Mode::kList;  // the profile went away in another window
      }
    }
    taken_.assign(store_.profiles.size(), 0);
    for (size_t i = 0; i < store_.profiles.size(); ++i) {
      taken_[i] = session_->TakenBy(store_.profiles[i].id);
    }
  }

  int EntryCount() const { return int(store_.profiles.size()) + 1; }  // + "Create"
  bool OnCreateEntry() const { return sel_ == int(store_.profiles.size()); }

  void BeginEdit(Mode mode) {
    mode_ = mode;
    error_.clear();
    std::memset(name_buf_, 0, sizeof(name_buf_));
    if (mode == Mode::kRename && !OnCreateEntry()) {
      std::snprintf(name_buf_, sizeof(name_buf_), "%s",
                    store_.profiles[size_t(sel_)].gamertag.c_str());
    } else if (mode == Mode::kCreate && store_.profiles.empty()) {
      std::snprintf(name_buf_, sizeof(name_buf_), "%s", DefaultGamertag().c_str());
    }
    focus_text_ = true;  // names are typed on the keyboard
  }

  void CommitEdit() {
    const std::string name = SanitizeGamertag(name_buf_);
    if (name.empty()) {
      error_ = "Use letters, numbers or spaces.";
      return;
    }
    const std::string editing_id = mode_ == Mode::kRename ? SelectedId() : std::string();
    std::string result_id = editing_id;
    const bool ok = EditStore(session_, store_path_, store_, [&](Store& store) {
      Profile* editing = editing_id.empty() ? nullptr : FindById(store, editing_id);
      if (!editing_id.empty() && editing == nullptr) {
        return false;  // deleted in another splitscreen window meanwhile
      }
      if (NameTaken(store, name, editing)) {
        return false;
      }
      if (editing != nullptr) {
        editing->gamertag = name;  // id and XUID stay: saves remain attached
      } else {
        store.profiles.push_back(MakeProfile(store, name));
        result_id = store.profiles.back().id;
      }
      return true;
    });
    if (session_) {
      store_generation_ = session_->StoreGeneration();
    }
    if (!ok) {
      error_ = "A profile with that name already exists.";
      return;
    }
    SelectId(result_id);
    mode_ = Mode::kList;
  }

  void CancelEdit() {
    if (!store_.profiles.empty()) {
      mode_ = Mode::kList;
    }
  }

  void Delete() {
    if (!OnCreateEntry() && TakenBy(sel_) == 0) {
      const std::string id = SelectedId();
      EditStore(session_, store_path_, store_, [&](Store& store) {
        const auto it = std::find_if(store.profiles.begin(), store.profiles.end(),
                                     [&](const Profile& p) { return p.id == id; });
        if (it == store.profiles.end()) {
          return false;
        }
        store.profiles.erase(it);
        return true;
      });
      if (session_) {
        store_generation_ = session_->StoreGeneration();
      }
      sel_ = std::min(sel_, EntryCount() - 1);
    }
    mode_ = Mode::kList;
  }

  // A profile another splitscreen player holds: say so instead of acting.
  bool RefuseTaken() {
    if (const int other = TakenBy(sel_); other != 0) {
      error_ = "Player " + std::to_string(other) + " is signed in with this profile.";
      return true;
    }
    return false;
  }

  void SignIn() {
    if (OnCreateEntry()) {
      BeginEdit(Mode::kCreate);
      return;
    }
    if (RefuseTaken()) {
      return;
    }
    const Profile& profile = store_.profiles[size_t(sel_)];
    if (session_) {
      if (!session_->Claim(profile.id, profile.gamertag)) {
        error_ = "Another player just signed in with this profile.";
        return;
      }
      chosen_id_ = profile.id;
      chosen_name_ = profile.gamertag;
      error_.clear();
      mode_ = Mode::kWaiting;  // boot together once everyone has signed in
      return;
    }
    Finish(profile.id);
  }

  void Finish(const std::string& id) {
    store_.selected = id;
    finished_ = true;
    if (done_) {
      Done done = std::move(done_);
      done(store_);
    }
    Close();
  }

  PadReader pad_;
  Store store_;
  Done done_;
  splitscreen::Session* session_ = nullptr;
  std::filesystem::path store_path_;
  uint32_t store_generation_ = 0;
  std::vector<int> taken_;  // per profile: the other player holding it
  std::string chosen_id_, chosen_name_;
  Mode mode_ = Mode::kList;
  int sel_ = 0;
  char name_buf_[kMaxGamertag + 1] = {};
  std::string error_;
  bool focus_text_ = false;
  bool finished_ = false;
  bool pad_active_ = false;          // last input came from a controller
  uint16_t prev_buttons_ = 0xFFFF;   // ignore buttons already held at open
  int stick_x_ = 0, stick_y_ = 0;    // stick direction last frame
  float repeat_x_ = 0.0f, repeat_y_ = 0.0f;
};

void PickerDialog::OnDraw(ImGuiIO& io) {
  if (finished_) {
    return;
  }
  SyncShared();
  // ---- input -------------------------------------------------------------
  const PadState pad = pad_.Poll();
  const uint16_t pressed = uint16_t(pad.buttons & ~prev_buttons_);
  prev_buttons_ = pad.buttons;
  // Stick / d-pad directions with auto-repeat (first step on press, then
  // after 0.35 s every 0.09 s while held).
  const auto axis_dir = [](int16_t v) { return v > 16000 ? 1 : (v < -16000 ? -1 : 0); };
  const int sx = axis_dir(pad.lx) +
                 ((pad.buttons & kPadRight) ? 1 : 0) - ((pad.buttons & kPadLeft) ? 1 : 0);
  const int sy = -axis_dir(pad.ly) +
                 ((pad.buttons & kPadDown) ? 1 : 0) - ((pad.buttons & kPadUp) ? 1 : 0);
  const auto step = [&](int dir, int& prev, float& timer) {
    dir = std::clamp(dir, -1, 1);
    int out = 0;
    if (dir != 0 && dir != prev) {
      out = dir;
      timer = 0.35f;
    } else if (dir != 0) {
      timer -= io.DeltaTime;
      if (timer <= 0.0f) {
        out = dir;
        timer = 0.09f;
      }
    }
    prev = dir;
    return out;
  };
  const int pad_dx = step(sx, stick_x_, repeat_x_);
  const int pad_dy = step(sy, stick_y_, repeat_y_);
  if (pressed != 0 || pad_dx != 0 || pad_dy != 0) {
    pad_active_ = true;
  }
  if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
    pad_active_ = false;
  }
  const bool kb_text = io.WantTextInput;
  const auto key = [&](ImGuiKey k) { return !kb_text && ImGui::IsKeyPressed(k, true); };

  switch (mode_) {
    case Mode::kList: {
      int move = pad_dy;
      if (key(ImGuiKey_UpArrow)) move = -1;
      if (key(ImGuiKey_DownArrow)) move = 1;
      if (move != 0) {
        sel_ = (sel_ + move + EntryCount()) % EntryCount();
        error_.clear();
      }
      if ((pressed & (kPadA | kPadStart)) || key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) {
        SignIn();
      } else if (((pressed & kPadX) || key(ImGuiKey_F2)) && !OnCreateEntry()) {
        if (!RefuseTaken()) {
          BeginEdit(Mode::kRename);
        }
      } else if (((pressed & kPadY) || key(ImGuiKey_Delete)) && !OnCreateEntry()) {
        if (!RefuseTaken()) {
          mode_ = Mode::kDelete;
        }
      } else if (((pressed & kPadBack) || key(ImGuiKey_Space)) && session_ == nullptr) {
        store_.auto_sign_in = !store_.auto_sign_in;
      }
      break;
    }
    case Mode::kWaiting:
      if (session_->EveryoneSignedIn()) {
        Finish(chosen_id_);
        return;
      }
      if ((pressed & kPadB) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        session_->Release();
        mode_ = Mode::kList;
      }
      break;
    case Mode::kCreate:
    case Mode::kRename:
      // The name is typed on the keyboard (the text field keeps focus);
      // the controller confirms (A / Start) or cancels (B), as do Enter and
      // Escape.
      if (pressed & (kPadA | kPadStart)) CommitEdit();
      if ((pressed & kPadB) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) CancelEdit();
      break;
    case Mode::kDelete:
      if ((pressed & kPadA) || key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) {
        Delete();
      } else if ((pressed & kPadB) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        mode_ = Mode::kList;
      }
      break;
  }
  if (finished_) {
    return;
  }

  // ---- layout ------------------------------------------------------------
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                    IM_COL32(8, 10, 14, 255));
  const float scale = std::clamp(vp->Size.y / 720.0f, 1.0f, 2.5f);
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(480.0f * scale, 0.0f), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * scale, 18.0f * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 8.0f * scale));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(24, 27, 34, 252));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(70, 76, 90, 255));
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
  ImGui::Begin("##skate3_profile_picker", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);

  const ImU32 kAccent = IM_COL32(110, 200, 90, 255);
  const ImU32 kDim = IM_COL32(150, 156, 168, 255);
  const auto hint = [&](const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
  };
  if (session_) {
    ImGui::PushStyleColor(ImGuiCol_Text, splitscreen::PlayerColor(session_->player()));
    ImGui::Text("Player %d", session_->player());
    ImGui::PopStyleColor();
    ImGui::SameLine();
  }
  ImGui::TextUnformatted(mode_ == Mode::kCreate    ? "Create Profile"
                         : mode_ == Mode::kRename  ? "Rename Profile"
                         : mode_ == Mode::kDelete  ? "Delete Profile"
                         : mode_ == Mode::kWaiting ? "Signed In"
                                                   : "Select Profile");
  ImGui::Separator();

  if (mode_ == Mode::kList) {
    const float row_h = ImGui::GetTextLineHeight() * 1.9f;
    for (int i = 0; i < EntryCount(); ++i) {
      const bool create = i == int(store_.profiles.size());
      const std::string label =
          create ? std::string("+  Create New Profile") : store_.profiles[size_t(i)].gamertag;
      ImGui::PushID(i);
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      // Overlap allowed: the row's own Rename / Delete buttons sit on top.
      ImGui::SetNextItemAllowOverlap();
      const bool clicked = ImGui::InvisibleButton("row", ImVec2(w, row_h));
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem) && !pad_active_) {
        sel_ = i;
      }
      const int taken_by = create ? 0 : TakenBy(i);
      const bool row_buttons = !pad_active_ && !create && i == sel_ && taken_by == 0;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      if (i == sel_) {
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + row_h), IM_COL32(52, 60, 74, 255),
                          4.0f * scale);
        dl->AddRectFilled(p0, ImVec2(p0.x + 4.0f * scale, p0.y + row_h), kAccent);
      }
      const float ty = p0.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;
      dl->AddText(ImVec2(p0.x + 14.0f * scale, ty),
                  create || taken_by != 0 ? kDim : IM_COL32(235, 238, 244, 255), label.c_str());
      if (taken_by != 0) {
        const std::string tag = "Player " + std::to_string(taken_by);
        const float tw = ImGui::CalcTextSize(tag.c_str()).x;
        dl->AddText(ImVec2(p0.x + w - tw - 12.0f * scale, ty), splitscreen::PlayerColor(taken_by),
                    tag.c_str());
      } else if (!create && !row_buttons && session_ == nullptr &&
                 store_.profiles[size_t(i)].id == store_.selected) {
        const char* tag = "last used";
        const float tw = ImGui::CalcTextSize(tag).x;
        dl->AddText(ImVec2(p0.x + w - tw - 12.0f * scale, ty), kDim, tag);
      }
      bool row_action = false;
      if (row_buttons) {
        // Mouse: this profile's own Rename / Delete, right-aligned in the row.
        const ImGuiStyle& st = ImGui::GetStyle();
        const float bw = ImGui::CalcTextSize("Rename").x + ImGui::CalcTextSize("Delete").x +
                         st.FramePadding.x * 4.0f + st.ItemSpacing.x;
        const ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(
            ImVec2(p0.x + w - bw - 10.0f * scale,
                   p0.y + (row_h - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::SmallButton("Rename")) {
          BeginEdit(Mode::kRename);
          row_action = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
          mode_ = Mode::kDelete;
          row_action = true;
        }
        ImGui::SetCursorScreenPos(after);
      }
      ImGui::PopID();
      if (row_action) {
        break;
      }
      if (clicked) {
        sel_ = i;
        SignIn();
        break;
      }
    }
    if (!finished_ && mode_ == Mode::kList) {
      if (!error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 110, 100, 255));
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopStyleColor();
      }
      ImGui::Spacing();
      if (session_ != nullptr) {
        hint("Every player picks their own profile. The game starts in every window once all "
             "players have signed in.");
      } else {
        bool auto_sign_in = store_.auto_sign_in;
        if (ImGui::Checkbox("Sign in automatically", &auto_sign_in)) {
          store_.auto_sign_in = auto_sign_in;
        }
        hint(store_.auto_sign_in ? "The chosen profile signs in by itself on the next launches."
                                 : "This screen will open every time the game starts.");
      }
      ImGui::Spacing();
      if (session_ != nullptr && pad_active_) {
        hint(OnCreateEntry() ? "A  Create" : "A  Sign in    X  Rename    Y  Delete");
      } else if (session_ != nullptr) {
        hint("Click a profile to sign in. Keyboard: Enter  Sign in    F2  Rename    Del  Delete");
      } else if (pad_active_) {
        hint(OnCreateEntry() ? "A  Create        View / Touchpad  Auto sign-in"
                             : "A  Sign in    X  Rename    Y  Delete    View  Auto sign-in");
      } else {
        hint("Click a profile to sign in. Keyboard: Enter  Sign in    F2  Rename    Del  "
             "Delete    Space  Auto sign-in");
      }
    }
  } else if (mode_ == Mode::kCreate || mode_ == Mode::kRename) {
    hint(mode_ == Mode::kCreate
             ? "Enter a gamertag: up to 15 letters, numbers or spaces. It is the name you "
               "play online with."
             : "Enter the new gamertag. Saves stay with this profile.");
    if (focus_text_) {
      ImGui::SetKeyboardFocusHere();
      focus_text_ = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##gamertag", name_buf_, sizeof(name_buf_),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      CommitEdit();
      if (mode_ != Mode::kList) {
        focus_text_ = true;
      }
    }
    if (!error_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 110, 100, 255));
      ImGui::TextUnformatted(error_.c_str());
      ImGui::PopStyleColor();
    }
    if (mode_ != Mode::kList) {
      // Keep typing focus on the field (a click elsewhere must not strand it).
      if (!ImGui::IsItemActive()) {
        focus_text_ = true;
      }
      hint(pad_active_ ? "Type the name on the keyboard.    A / Start  Save    B  Cancel"
                       : "Type the name on the keyboard.    Enter  Save    Esc  Cancel");
      if (!pad_active_) {
        if (ImGui::Button(mode_ == Mode::kCreate ? "Create" : "Save")) CommitEdit();
        if (!store_.profiles.empty()) {
          ImGui::SameLine();
          if (ImGui::Button("Cancel")) CancelEdit();
        }
      }
    }
  } else if (mode_ == Mode::kWaiting) {
    int signed_in = 0, players = 0;
    session_->Count(signed_in, players);
    ImGui::TextWrapped("Signed in as %s.", chosen_name_.c_str());
    hint(("Waiting for the other players: " + std::to_string(signed_in) + " of " +
          std::to_string(players) + " signed in.")
             .c_str());
    hint(pad_active_ ? "B  Choose another profile" : "Esc  Choose another profile");
  } else {  // kDelete
    ImGui::TextWrapped("Delete \"%s\"?", store_.profiles[size_t(sel_)].gamertag.c_str());
    hint("The profile is removed from this list. Its saves stay on disk.");
    hint(pad_active_ ? "A  Delete    B  Cancel" : "Enter  Delete    Esc  Cancel");
    if (!pad_active_) {
      if (ImGui::Button("Delete")) Delete();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) mode_ = Mode::kList;
    }
  }

  ImGui::End();
  ImGui::PopFont();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
}

}  // namespace

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

std::filesystem::path StorePath(const std::filesystem::path& user_data_root) {
  return user_data_root / "profiles" / "profiles.toml";
}

Store Load(const std::filesystem::path& path) {
  Store store;
  if (!std::filesystem::exists(path)) {
    return store;
  }
  try {
    auto table = toml::parse_file(path.string());
    store.selected = table["selected_profile"].value_or(std::string{});
    store.auto_sign_in = table["auto_sign_in"].value_or(true);
    if (auto list = table["profiles"].as_array()) {
      list->for_each([&](toml::table& t) {
        Profile p;
        p.id = t["id"].value_or(std::string{});
        p.gamertag = t["gamertag"].value_or(std::string{});
        p.signed_in = t["signed_in"].value_or(true);
        p.live_signed_in = t["live_signed_in"].value_or(true);
        if (auto xuid = t["xuid"].value<std::string>()) {
          p.xuid = ParseXuid(*xuid);
        } else if (auto xuid_int = t["xuid"].value<int64_t>()) {
          p.xuid = static_cast<uint64_t>(*xuid_int);
        }
        if (!p.id.empty() && !p.gamertag.empty() && p.xuid != 0) {
          store.profiles.push_back(std::move(p));
        }
      });
    }
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("profiles: {} unreadable ({}); starting a new store", path.string(),
                e.description());
    store = {};
  }
  return store;
}

bool Save(const std::filesystem::path& path, const Store& store) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    REXLOG_WARN("profiles: cannot write {}", path.string());
    return false;
  }
  file << "selected_profile = " << TomlString(store.selected) << "\n";
  file << "auto_sign_in = " << (store.auto_sign_in ? "true" : "false") << "\n\n";
  for (const Profile& p : store.profiles) {
    file << "[[profiles]]\n";
    file << "id = " << TomlString(p.id) << "\n";
    file << "gamertag = " << TomlString(p.gamertag) << "\n";
    file << "xuid = " << TomlString(FormatXuid(p.xuid)) << "\n";
    file << "signed_in = " << (p.signed_in ? "true" : "false") << "\n";
    file << "live_signed_in = " << (p.live_signed_in ? "true" : "false") << "\n\n";
  }
  return true;
}

Profile* Selected(Store& store) {
  for (Profile& p : store.profiles) {
    if (p.id == store.selected) {
      return &p;
    }
  }
  return nullptr;
}

const Profile* Selected(const Store& store) {
  return Selected(const_cast<Store&>(store));
}

Profile MakeProfile(const Store& store, const std::string& gamertag) {
  Profile p;
  p.gamertag = gamertag;
  const std::string base = MakeId(gamertag);
  p.id = base;
  for (int n = 2;; ++n) {
    const bool taken = std::any_of(store.profiles.begin(), store.profiles.end(),
                                   [&](const Profile& q) { return q.id == p.id; });
    if (!taken) {
      break;
    }
    p.id = base + "-" + std::to_string(n);
  }
  p.xuid = XuidForId(p.id);
  return p;
}

std::string SanitizeGamertag(const std::string& text) {
  std::string tag;
  for (char c : text) {
    if (tag.size() == kMaxGamertag) {
      break;
    }
    if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') {
      tag.push_back(c);
    }
  }
  while (!tag.empty() && tag.front() == ' ') tag.erase(tag.begin());
  while (!tag.empty() && tag.back() == ' ') tag.pop_back();
  return tag;
}

std::string DefaultGamertag() {
  const std::string tag = SanitizeGamertag(OsUserName());
  return tag.empty() ? std::string(kSdkDefaultName) : tag;
}

void ApplyCvars(const Profile& profile) {
  rex::cvar::SetFlagByName("selected_user_profile", profile.id);
  rex::cvar::SetFlagByName("user_profile_name", profile.gamertag);
  rex::cvar::SetFlagByName("user_profile_xuid", FormatXuid(profile.xuid));
  rex::cvar::SetFlagByName("user_profile_signed_in", profile.signed_in ? "true" : "false");
  rex::cvar::SetFlagByName("user_live_signed_in",
                           profile.signed_in && profile.live_signed_in ? "true" : "false");
}

// ---------------------------------------------------------------------------
// Boot flow
// ---------------------------------------------------------------------------

bool SignInAtBoot(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                  rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& store_path) {
  // Splitscreen: every window picks (no auto sign-in), and the store is shared
  // with the other windows (see EditStore).
  const bool splitscreen_player = splitscreen::IsPlayer();
  splitscreen::Session* session = splitscreen::Session::Get();
  Store store;
  const auto load_or_seed = [&] {
    const bool had_store = std::filesystem::exists(store_path);
    store = Load(store_path);
    if (!had_store) {
      // First run: carry the current identity over as the first profile and
      // sign it in automatically (nothing changes for an existing player).
      Profile seed = SeedFromCurrentIdentity();
      store.selected = seed.id;
      store.profiles.push_back(std::move(seed));
      Save(store_path, store);
      REXLOG_INFO("profiles: created {} with profile '{}'", store_path.string(),
                  store.profiles.front().gamertag);
    }
  };
  if (session) {
    session->WithLock(load_or_seed);
  } else {
    load_or_seed();
  }

  if (!splitscreen_player && store.auto_sign_in && Selected(store) != nullptr) {
    ApplyCvars(*Selected(store));
    REXLOG_INFO("profiles: signed in '{}' automatically", Selected(store)->gamertag);
    return true;
  }
  if (drawer == nullptr) {
    // No UI to ask with: sign in the last (or first) profile.
    if (Selected(store) == nullptr && !store.profiles.empty()) {
      store.selected = store.profiles.front().id;
    }
    if (const Profile* p = Selected(store)) {
      ApplyCvars(*p);
    }
    return true;
  }

  struct Result {
    bool done = false;
    Store store;
  };
  auto result = std::make_shared<Result>();
  new PickerDialog(
      drawer, store,
      [result](Store chosen) {
        result->store = std::move(chosen);
        result->done = true;
      },
      session, store_path);

#if defined(_WIN32)
  HWND hwnd = nullptr;
  if (auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(window)) {
    hwnd = win32_window->hwnd();
  }
#endif
  REXLOG_INFO("profiles: showing the profile picker");
  if (window) {
    window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  }
  while (!result->done && !app_context.HasQuitFromUIThread()) {
    app_context.ExecutePendingFunctionsFromUIThread();
#if defined(_WIN32)
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        app_context.QuitFromUIThread();
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (app_context.HasQuitFromUIThread()) {
      break;
    }
    if (window) {
      window->RequestPaint();
    }
    if (hwnd) {
      RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
#else
    if (window) {
      window->RequestPaint();
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  if (window) {
    window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
  }
  if (!result->done) {
    return false;
  }
  if (session) {
    // Only this window's choice goes to disk; the rest of the store is
    // whatever the windows have saved together.
    session->WithLock([&] {
      Store disk = Load(store_path);
      if (FindById(disk, result->store.selected) != nullptr) {
        disk.selected = result->store.selected;
        Save(store_path, disk);
        session->BumpStoreGeneration();
        result->store = std::move(disk);
      }
    });
  } else {
    Save(store_path, result->store);
  }
  if (const Profile* p = Selected(result->store)) {
    ApplyCvars(*p);
    REXLOG_INFO("profiles: signed in '{}' (auto sign-in {})", p->gamertag,
                result->store.auto_sign_in ? "on" : "off");
  }
  return true;
}

// ---------------------------------------------------------------------------
// Runtime edits
// ---------------------------------------------------------------------------

namespace {

// The profile this process signed in with (ApplyCvars), else the last used.
Profile* SignedIn(Store& store) {
  const std::string id = rex::cvar::Query<std::string>("selected_user_profile");
  if (Profile* p = FindById(store, id)) {
    return p;
  }
  return Selected(store);
}

}  // namespace

std::string SelectedGamertag(const std::filesystem::path& store_path) {
  Store store = Load(store_path);
  const Profile* p = SignedIn(store);
  return p ? p->gamertag : std::string();
}

bool RenameSelected(const std::filesystem::path& store_path, const std::string& gamertag) {
  const std::string name = SanitizeGamertag(gamertag);
  bool ok = false;
  const auto rename = [&] {
    Store store = Load(store_path);
    Profile* p = SignedIn(store);
    if (name.empty() || p == nullptr || NameTaken(store, name, p)) {
      return;
    }
    p->gamertag = name;
    Save(store_path, store);
    ApplyCvars(*p);
    ok = true;
  };
  if (splitscreen::Session* session = splitscreen::Session::Get()) {
    session->WithLock(rename);
  } else {
    rename();
  }
  return ok;
}

bool AutoSignIn(const std::filesystem::path& store_path) {
  return Load(store_path).auto_sign_in;
}

void SetAutoSignIn(const std::filesystem::path& store_path, bool enabled) {
  Store store = Load(store_path);
  store.auto_sign_in = enabled;
  Save(store_path, store);
}

}  // namespace skate3::profiles
