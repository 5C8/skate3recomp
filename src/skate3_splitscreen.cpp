// Splitscreen launcher, seat screen and player session (see skate3_splitscreen.h).
// Shared by sk83_na_m and skate3recomp: keep the two copies identical.

#include "skate3_splitscreen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <SDL3/SDL.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <rex/ui/window_win.h>
#endif

REXCVAR_DEFINE_BOOL(splitscreen, false, "Splitscreen",
                    "Launch in splitscreen: every controller picks a monitor (or part of one), "
                    "then each player gets their own game window and profile");
REXCVAR_DEFINE_STRING(splitscreen_session, "", "Splitscreen",
                      "Set by the splitscreen launcher: the session a player window belongs to");
REXCVAR_DEFINE_INT32(splitscreen_player, 0, "Splitscreen",
                     "Set by the splitscreen launcher: this window's player number (1-based)")
    .range(0, skate3::splitscreen::kMaxPlayers);

namespace skate3::splitscreen {

bool IsPlayer() {
#if defined(_WIN32)
  return !REXCVAR_GET(splitscreen_session).empty() && REXCVAR_GET(splitscreen_player) > 0;
#else
  return false;
#endif
}

bool IsLauncher() {
#if defined(_WIN32)
  return REXCVAR_GET(splitscreen) && !IsPlayer();
#else
  return false;
#endif
}

int PlayerNumber() {
  return IsPlayer() ? REXCVAR_GET(splitscreen_player) : 0;
}

uint32_t PlayerColor(int player) {
  static constexpr uint32_t kColors[kMaxPlayers] = {
      IM_COL32(110, 200, 90, 255),  IM_COL32(80, 150, 240, 255),  IM_COL32(240, 150, 60, 255),
      IM_COL32(230, 90, 160, 255),  IM_COL32(160, 110, 230, 255), IM_COL32(230, 210, 70, 255),
      IM_COL32(70, 200, 210, 255),  IM_COL32(225, 80, 80, 255),
  };
  return player >= 1 && player <= kMaxPlayers ? kColors[player - 1] : IM_COL32(150, 156, 168, 255);
}

bool FullscreenRectSize(int32_t& width, int32_t& height) {
  const std::string spec = rex::cvar::Query<std::string>("window_fullscreen_rect");
  int x = 0, y = 0, w = 0, h = 0;
  if (spec.empty() || std::sscanf(spec.c_str(), "%d , %d , %d , %d", &x, &y, &w, &h) != 4 ||
      w <= 0 || h <= 0) {
    return false;
  }
  width = w;
  height = h;
  return true;
}

#if defined(_WIN32)

namespace {

// ---------------------------------------------------------------------------
// Shared session block
// ---------------------------------------------------------------------------

constexpr uint32_t kMagic = 0x5333534B;  // "KSS3"
constexpr uint32_t kVersion = 1;

enum SlotState : uint32_t {
  kSlotStarting = 0,  // launched, picker not open yet
  kSlotChoosing = 1,  // picker open
  kSlotSignedIn = 2,  // signed in (and from then on: playing)
  kSlotLeft = 3,      // gone (window closed or never started)
};

struct SharedSlot {
  uint32_t state;
  uint32_t pid;
  char profile_id[64];
  char gamertag[32];
};

struct SharedBlock {
  uint32_t magic;
  uint32_t version;
  uint32_t player_count;
  uint32_t store_generation;
  SharedSlot slots[kMaxPlayers];
};

std::wstring Widen(std::string_view text) {
  std::wstring out;
  if (!text.empty()) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    out.resize(size_t(n));
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
  }
  return out;
}

std::wstring MappingName(std::string_view session) {
  return L"Local\\skate3_splitscreen_" + Widen(session) + L"_mem";
}
std::wstring MutexName(std::string_view session) {
  return L"Local\\skate3_splitscreen_" + Widen(session) + L"_lock";
}

void CopyField(char* dst, size_t size, std::string_view src) {
  const size_t n = std::min(size - 1, src.size());
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

class MutexLock {
 public:
  explicit MutexLock(HANDLE mutex) : mutex_(mutex) {
    // An abandoned mutex (a window crashed holding it) is still ours now.
    const DWORD r = mutex_ ? WaitForSingleObject(mutex_, 5000) : WAIT_FAILED;
    held_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
  }
  ~MutexLock() {
    if (held_) {
      ReleaseMutex(mutex_);
    }
  }

 private:
  HANDLE mutex_;
  bool held_ = false;
};

}  // namespace

struct Session::Impl {
  HANDLE mapping = nullptr;
  HANDLE mutex = nullptr;
  SharedBlock* block = nullptr;
  std::array<HANDLE, kMaxPlayers> procs{};
  std::array<uint32_t, kMaxPlayers> proc_pids{};

  // Whether slot i's window still exists (call under the lock).
  bool Alive(int i) {
    const SharedSlot& slot = block->slots[i];
    if (slot.state == kSlotLeft) {
      return false;
    }
    if (slot.pid == 0) {
      return true;  // not started yet
    }
    if (slot.pid == GetCurrentProcessId()) {
      return true;
    }
    if (proc_pids[size_t(i)] != slot.pid) {
      if (procs[size_t(i)]) {
        CloseHandle(procs[size_t(i)]);
      }
      procs[size_t(i)] = OpenProcess(SYNCHRONIZE, FALSE, slot.pid);
      proc_pids[size_t(i)] = slot.pid;
    }
    return procs[size_t(i)] && WaitForSingleObject(procs[size_t(i)], 0) == WAIT_TIMEOUT;
  }
};

Session* Session::Get() {
  static Session* session = []() -> Session* {
    if (!IsPlayer()) {
      return nullptr;
    }
    const std::string name = REXCVAR_GET(splitscreen_session);
    const int player = REXCVAR_GET(splitscreen_player);
    auto impl = std::make_unique<Impl>();
    impl->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MappingName(name).c_str());
    impl->mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, MutexName(name).c_str());
    if (impl->mapping) {
      impl->block = static_cast<SharedBlock*>(
          MapViewOfFile(impl->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
    }
    if (!impl->block || !impl->mutex || impl->block->magic != kMagic ||
        impl->block->version != kVersion || player < 1 ||
        player > int(impl->block->player_count)) {
      REXLOG_WARN("splitscreen: session '{}' unavailable; player {} signs in on its own", name,
                  player);
      return nullptr;
    }
    auto* s = new Session();
    s->impl_ = impl.release();
    s->player_ = player;
    s->pad_id_ = rex::cvar::Query<std::string>("input_bound_pad");
    {
      MutexLock lock(s->impl_->mutex);
      SharedSlot& slot = s->impl_->block->slots[player - 1];
      slot.pid = GetCurrentProcessId();
      slot.state = kSlotChoosing;
      slot.profile_id[0] = '\0';
      slot.gamertag[0] = '\0';
    }
    REXLOG_INFO("splitscreen: player {} of {} joined session '{}' (controller {})", player,
                s->impl_->block->player_count, name, s->pad_id_);
    return s;
  }();
  return session;
}

int Session::TakenBy(std::string_view profile_id) {
  MutexLock lock(impl_->mutex);
  for (int i = 0; i < int(impl_->block->player_count); ++i) {
    const SharedSlot& slot = impl_->block->slots[i];
    if (i + 1 != player_ && slot.state == kSlotSignedIn && profile_id == slot.profile_id &&
        impl_->Alive(i)) {
      return i + 1;
    }
  }
  return 0;
}

bool Session::Claim(std::string_view profile_id, std::string_view gamertag) {
  MutexLock lock(impl_->mutex);
  for (int i = 0; i < int(impl_->block->player_count); ++i) {
    const SharedSlot& slot = impl_->block->slots[i];
    if (i + 1 != player_ && slot.state == kSlotSignedIn && profile_id == slot.profile_id &&
        impl_->Alive(i)) {
      return false;
    }
  }
  SharedSlot& mine = impl_->block->slots[player_ - 1];
  CopyField(mine.profile_id, sizeof(mine.profile_id), profile_id);
  CopyField(mine.gamertag, sizeof(mine.gamertag), gamertag);
  mine.state = kSlotSignedIn;
  return true;
}

void Session::Release() {
  MutexLock lock(impl_->mutex);
  SharedSlot& mine = impl_->block->slots[player_ - 1];
  mine.state = kSlotChoosing;
  mine.profile_id[0] = '\0';
  mine.gamertag[0] = '\0';
}

bool Session::IsPlayerProcess(uint32_t pid) {
  MutexLock lock(impl_->mutex);
  for (int i = 0; i < int(impl_->block->player_count); ++i) {
    if (impl_->block->slots[i].pid == pid && impl_->Alive(i)) {
      return true;
    }
  }
  return false;
}

void Session::Count(int& signed_in, int& players) {
  MutexLock lock(impl_->mutex);
  signed_in = 0;
  players = 0;
  for (int i = 0; i < int(impl_->block->player_count); ++i) {
    if (!impl_->Alive(i)) {
      continue;
    }
    ++players;
    if (impl_->block->slots[i].state == kSlotSignedIn) {
      ++signed_in;
    }
  }
}

void Session::WithLock(const std::function<void()>& fn) {
  MutexLock lock(impl_->mutex);
  fn();
}

uint32_t Session::StoreGeneration() {
  MutexLock lock(impl_->mutex);
  return impl_->block->store_generation;
}

void Session::BumpStoreGeneration() {
  ++impl_->block->store_generation;
}

namespace {

// ---------------------------------------------------------------------------
// Controllers, named the way the game's input backends will see them
// ---------------------------------------------------------------------------

constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008,
                   kPadStart = 0x0010, kPadBack = 0x0020, kPadA = 0x1000, kPadB = 0x2000,
                   kPadY = 0x8000;

struct PadInfo {
  std::string id;  // input_bound_pad value
  std::string name;
  uint16_t buttons = 0;  // XInput bit layout
  int16_t lx = 0;
  int16_t ly = 0;  // up is positive
};

// Mirrors the game's input_backend choice: "auto" = XInput for Xbox pads and
// SDL for the rest, so every id here is one the game's drivers resolve.
class PadHub {
 public:
  PadHub() {
    const std::string backend = rex::cvar::Query<std::string>("input_backend");
    use_xinput_ = backend != "sdl";
    use_sdl_ = backend != "xinput";
    skip_xbox_in_sdl_ = use_xinput_;
    if (use_sdl_) {
      // The same hints as the game's SDL driver: the device paths match.
      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
      SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
      SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
      sdl_ok_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
      if (!sdl_ok_) {
        REXLOG_WARN("splitscreen: SDL gamepad init failed ({})", SDL_GetError());
      }
    }
    if (use_xinput_) {
      for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
        if (HMODULE m = LoadLibraryA(dll)) {
          if (auto fn = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"))) {
            xinput_get_state_ = fn;
            break;
          }
        }
      }
    }
  }
  ~PadHub() {
    for (SDL_Gamepad* g : pads_) {
      SDL_CloseGamepad(g);
    }
    if (sdl_ok_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
  }

  std::vector<PadInfo> Poll() {
    std::vector<PadInfo> out;
    if (xinput_get_state_) {
      for (DWORD i = 0; i < 4; ++i) {
        XState s{};
        if (xinput_get_state_(i, &s) == ERROR_SUCCESS) {
          PadInfo pad;
          pad.id = "xinput:" + std::to_string(i);
          pad.name = "Xbox controller " + std::to_string(i + 1);
          pad.buttons = s.buttons;
          pad.lx = s.lx;
          pad.ly = s.ly;
          out.push_back(std::move(pad));
        }
      }
    }
    if (sdl_ok_) {
      SDL_PumpEvents();
      SDL_UpdateGamepads();
      if (rescan_-- <= 0) {
        rescan_ = 30;
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
          for (int i = 0; i < count; ++i) {
            const bool open = std::any_of(pads_.begin(), pads_.end(), [&](SDL_Gamepad* g) {
              return SDL_GetGamepadID(g) == ids[i];
            });
            if (open || std::find(ignored_.begin(), ignored_.end(), ids[i]) != ignored_.end()) {
              continue;
            }
            SDL_Gamepad* g = SDL_OpenGamepad(ids[i]);
            if (!g) {
              continue;
            }
            const SDL_GamepadType type = SDL_GetGamepadType(g);
            const char* path = SDL_GetGamepadPath(g);
            if ((skip_xbox_in_sdl_ &&
                 (type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE)) ||
                path == nullptr || *path == '\0') {
              // Xbox pads are XInput's in "auto"; a pad without a device path
              // cannot be named to the game window.
              if (path == nullptr || *path == '\0') {
                REXLOG_WARN("splitscreen: controller '{}' has no device path; skipped",
                            SDL_GetGamepadName(g));
              }
              ignored_.push_back(ids[i]);
              SDL_CloseGamepad(g);
              continue;
            }
            pads_.push_back(g);
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
          {SDL_GAMEPAD_BUTTON_SOUTH, kPadA},        {SDL_GAMEPAD_BUTTON_EAST, kPadB},
          {SDL_GAMEPAD_BUTTON_NORTH, kPadY},
      };
      for (auto it = pads_.begin(); it != pads_.end();) {
        SDL_Gamepad* g = *it;
        if (!SDL_GamepadConnected(g)) {
          SDL_CloseGamepad(g);
          it = pads_.erase(it);
          continue;
        }
        PadInfo pad;
        pad.id = std::string("sdl:") + SDL_GetGamepadPath(g);
        // The serial lets the game window find the pad again if it returns
        // under another device path (other USB port, USB <-> Bluetooth).
        if (const char* serial = SDL_GetGamepadSerial(g); serial && *serial) {
          pad.id += std::string("|serial=") + serial;
        }
        const char* name = SDL_GetGamepadName(g);
        pad.name = name ? name : "Controller";
        for (const auto& m : kMap) {
          if (SDL_GetGamepadButton(g, m.b)) {
            pad.buttons |= m.bit;
          }
        }
        pad.lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
        pad.ly = int16_t(
            std::clamp(-int(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY)), -32768, 32767));
        out.push_back(std::move(pad));
        ++it;
      }
    }
    return out;
  }

 private:
  struct XState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
  };
  using GetStateFn = DWORD(WINAPI*)(DWORD, XState*);

  bool use_xinput_ = true;
  bool use_sdl_ = true;
  bool skip_xbox_in_sdl_ = true;
  bool sdl_ok_ = false;
  GetStateFn xinput_get_state_ = nullptr;
  int rescan_ = 0;
  std::vector<SDL_Gamepad*> pads_;
  std::vector<SDL_JoystickID> ignored_;
};

// ---------------------------------------------------------------------------
// Monitors and spots
// ---------------------------------------------------------------------------

struct Monitor {
  RECT rect{};
  bool primary = false;
};

enum class Split { kSideBySide, kTopBottom };

BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM data) {
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(handle, &info)) {
    reinterpret_cast<std::vector<Monitor>*>(data)->push_back(
        Monitor{info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
  }
  return TRUE;
}

// Numbered left to right (then top to bottom), the way they stand.
std::vector<Monitor> EnumerateMonitors() {
  std::vector<Monitor> monitors;
  EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
  std::sort(monitors.begin(), monitors.end(), [](const Monitor& a, const Monitor& b) {
    return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
  });
  return monitors;
}

// The spots of a monitor shared by `count` players, in join order. Two
// players get halves; three get one half plus two quarters; four, quarters.
std::vector<RECT> SpotRects(const RECT& m, int count, Split split) {
  const LONG mx = m.left + (m.right - m.left) / 2;
  const LONG my = m.top + (m.bottom - m.top) / 2;
  const bool side = split == Split::kSideBySide;
  switch (count) {
    case 1:
      return {m};
    case 2:
      return side ? std::vector<RECT>{{m.left, m.top, mx, m.bottom}, {mx, m.top, m.right, m.bottom}}
                  : std::vector<RECT>{{m.left, m.top, m.right, my}, {m.left, my, m.right, m.bottom}};
    case 3:
      return side ? std::vector<RECT>{{m.left, m.top, mx, m.bottom},
                                      {mx, m.top, m.right, my},
                                      {mx, my, m.right, m.bottom}}
                  : std::vector<RECT>{{m.left, m.top, m.right, my},
                                      {m.left, my, mx, m.bottom},
                                      {mx, my, m.right, m.bottom}};
    default:
      return {{m.left, m.top, mx, my},
              {mx, m.top, m.right, my},
              {m.left, my, mx, m.bottom},
              {mx, my, m.right, m.bottom}};
  }
}

// The nearest monitor in direction (dx, dy) from monitor `from`, or `from`.
int NeighborMonitor(const std::vector<Monitor>& monitors, int from, int dx, int dy) {
  const auto center = [](const RECT& r) {
    return std::pair<double, double>((r.left + r.right) * 0.5, (r.top + r.bottom) * 0.5);
  };
  const auto [fx, fy] = center(monitors[size_t(from)].rect);
  int best = from;
  double best_score = 0.0;
  for (int i = 0; i < int(monitors.size()); ++i) {
    if (i == from) {
      continue;
    }
    const auto [cx, cy] = center(monitors[size_t(i)].rect);
    const double along = (cx - fx) * dx + (cy - fy) * dy;
    if (along <= 0.0) {
      continue;
    }
    const double across = std::abs((cx - fx) * dy) + std::abs((cy - fy) * dx);
    const double score = along + 2.0 * across;
    if (best == from || score < best_score) {
      best = i;
      best_score = score;
    }
  }
  return best;
}

// One player's window, as the launcher will start it.
struct PlannedPlayer {
  int player = 0;  // 1-based
  std::string pad_id;
  std::string pad_name;
  int monitor = 0;
  RECT rect{};
};

// ---------------------------------------------------------------------------
// Monitor identification (every monitor but the seat screen's own)
// ---------------------------------------------------------------------------

COLORREF ToColorRef(uint32_t imgui_color) {
  return RGB(imgui_color & 0xFF, (imgui_color >> 8) & 0xFF, (imgui_color >> 16) & 0xFF);
}

class IdentifyWindow {
 public:
  IdentifyWindow(int number, const RECT& rect) : number_(number), rect_(rect) {
    static const wchar_t* kClass = L"Skate3SplitscreenIdentify";
    static bool registered = false;
    if (!registered) {
      WNDCLASSW wc{};
      wc.lpfnWndProc = &IdentifyWindow::WndProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = kClass;
      wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
      registered = RegisterClassW(&wc) != 0;
    }
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass,
                            L"Skate 3 splitscreen", WS_POPUP, rect.left, rect.top,
                            rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (hwnd_) {
      ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
  }
  ~IdentifyWindow() {
    if (hwnd_) {
      DestroyWindow(hwnd_);
    }
  }

  struct Spot {
    RECT rect;  // desktop coordinates
    int player;
    std::string label;
  };
  void SetSpots(std::vector<Spot> spots) {
    spots_ = std::move(spots);
    if (hwnd_) {
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
  }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<IdentifyWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
      case WM_ERASEBKGND:
        return 1;
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
      case WM_PAINT:
        if (self) {
          self->Paint(hwnd);
          return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  static void DrawCentered(HDC dc, const RECT& r, const std::wstring& text, int height,
                           COLORREF color, int weight) {
    HFONT font = CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    RECT copy = r;
    DrawTextW(dc, text.c_str(), int(text.size()), &copy,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(font);
  }

  void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC screen = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    const int w = client.right, h = client.bottom;
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    SetBkMode(dc, TRANSPARENT);

    HBRUSH bg = CreateSolidBrush(RGB(8, 10, 14));
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    if (spots_.empty()) {
      DrawCentered(dc, client, std::to_wstring(number_), h / 3, RGB(235, 238, 244), FW_BOLD);
      RECT below{0, h * 2 / 3, w, h * 2 / 3 + h / 12};
      DrawCentered(dc, below, L"Monitor " + std::to_wstring(number_) + L"  ·  free",
                   h / 24, RGB(150, 156, 168), FW_NORMAL);
    }
    for (const Spot& spot : spots_) {
      RECT r{spot.rect.left - rect_.left, spot.rect.top - rect_.top, spot.rect.right - rect_.left,
             spot.rect.bottom - rect_.top};
      InflateRect(&r, -6, -6);
      const COLORREF color = ToColorRef(PlayerColor(spot.player));
      HBRUSH fill = CreateSolidBrush(RGB(GetRValue(color) / 4, GetGValue(color) / 4,
                                         GetBValue(color) / 4));
      FillRect(dc, &r, fill);
      DeleteObject(fill);
      HBRUSH edge = CreateSolidBrush(color);
      FrameRect(dc, &r, edge);
      RECT inner = r;
      InflateRect(&inner, -1, -1);
      FrameRect(dc, &inner, edge);
      DeleteObject(edge);
      const int rh = r.bottom - r.top;
      RECT top{r.left, r.top, r.right, r.top + rh * 3 / 5};
      DrawCentered(dc, top, L"P" + std::to_wstring(spot.player), rh / 3, color, FW_BOLD);
      RECT bottom{r.left + 12, r.top + rh * 3 / 5, r.right - 12, r.top + rh * 3 / 5 + rh / 8};
      DrawCentered(dc, bottom, Widen(spot.label), std::max(14, rh / 18), RGB(235, 238, 244),
                   FW_NORMAL);
    }
    RECT corner{24, 16, w, 16 + h / 18};
    HFONT font = CreateFontW(h / 28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextColor(dc, RGB(150, 156, 168));
    const std::wstring tag = L"Monitor " + std::to_wstring(number_);
    DrawTextW(dc, tag.c_str(), int(tag.size()), &corner, DT_LEFT | DT_TOP | DT_SINGLELINE);
    SelectObject(dc, old_font);
    DeleteObject(font);

    BitBlt(screen, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
  }

  HWND hwnd_ = nullptr;
  int number_;
  RECT rect_;
  std::vector<Spot> spots_;
};

// ---------------------------------------------------------------------------
// The seat screen
// ---------------------------------------------------------------------------

class SeatDialog final : public rex::ui::ImGuiDialog {
 public:
  using Done = std::function<void(bool launch, std::vector<PlannedPlayer> players)>;

  SeatDialog(rex::ui::ImGuiDrawer* drawer, HWND own_window, Done done)
      : ImGuiDialog(drawer), done_(std::move(done)) {
    monitors_ = EnumerateMonitors();
    if (monitors_.empty()) {
      monitors_.push_back(Monitor{RECT{0, 0, 1920, 1080}, true});
    }
    splits_.assign(monitors_.size(), Split::kSideBySide);
    // The seat screen's own monitor: where the cursors start, and the one
    // that needs no identification overlay.
    HMONITOR own = own_window ? MonitorFromWindow(own_window, MONITOR_DEFAULTTOPRIMARY)
                              : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(own, &info)) {
      for (int i = 0; i < int(monitors_.size()); ++i) {
        if (EqualRect(&monitors_[size_t(i)].rect, &info.rcMonitor)) {
          home_ = i;
        }
      }
    }
    for (int i = 0; i < int(monitors_.size()); ++i) {
      identify_.push_back(i == home_ ? nullptr
                                     : std::make_unique<IdentifyWindow>(
                                           i + 1, monitors_[size_t(i)].rect));
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct Seat {
    std::string pad_id;
    std::string name;
    int monitor = 0;
    bool joined = false;
    uint64_t join_order = 0;
    uint16_t prev_buttons = 0xFFFF;  // ignore what is held when it appears
    int prev_dx = 0, prev_dy = 0;
  };

  int CountOn(int monitor) const {
    return int(std::count_if(seats_.begin(), seats_.end(), [&](const Seat& s) {
      return s.joined && s.monitor == monitor;
    }));
  }

  // Joined seats in join order = player order.
  std::vector<const Seat*> Players() const {
    std::vector<const Seat*> players;
    for (const Seat& s : seats_) {
      if (s.joined) {
        players.push_back(&s);
      }
    }
    std::sort(players.begin(), players.end(),
              [](const Seat* a, const Seat* b) { return a->join_order < b->join_order; });
    return players;
  }

  std::vector<PlannedPlayer> Plan() const {
    std::vector<PlannedPlayer> plan;
    const auto players = Players();
    for (int m = 0; m < int(monitors_.size()); ++m) {
      std::vector<std::pair<int, const Seat*>> here;
      for (int p = 0; p < int(players.size()); ++p) {
        if (players[size_t(p)]->monitor == m) {
          here.emplace_back(p + 1, players[size_t(p)]);
        }
      }
      const auto rects = SpotRects(monitors_[size_t(m)].rect, int(here.size()), splits_[size_t(m)]);
      for (size_t i = 0; i < here.size(); ++i) {
        plan.push_back(PlannedPlayer{here[i].first, here[i].second->pad_id, here[i].second->name,
                                     m, rects[i]});
      }
    }
    std::sort(plan.begin(), plan.end(),
              [](const PlannedPlayer& a, const PlannedPlayer& b) { return a.player < b.player; });
    return plan;
  }

  void Notice(std::string text) {
    notice_ = std::move(text);
    notice_time_ = 3.0f;
  }

  void Finish(bool launch) {
    if (finished_) {
      return;
    }
    finished_ = true;
    identify_.clear();
    if (done_) {
      Done done = std::move(done_);
      done(launch, launch ? Plan() : std::vector<PlannedPlayer>{});
    }
    Close();
  }

  void UpdateIdentify(const std::vector<PlannedPlayer>& plan) {
    std::string signature;
    for (const auto& p : plan) {
      signature += std::to_string(p.player) + ":" + std::to_string(p.monitor) + ":" +
                   std::to_string(p.rect.left) + "," + std::to_string(p.rect.top) + "," +
                   std::to_string(p.rect.right) + "," + std::to_string(p.rect.bottom) + ";";
    }
    if (signature == identify_signature_) {
      return;
    }
    identify_signature_ = signature;
    for (int m = 0; m < int(identify_.size()); ++m) {
      if (!identify_[size_t(m)]) {
        continue;
      }
      std::vector<IdentifyWindow::Spot> spots;
      for (const auto& p : plan) {
        if (p.monitor == m) {
          spots.push_back({p.rect, p.player, p.pad_name});
        }
      }
      identify_[size_t(m)]->SetSpots(std::move(spots));
    }
  }

  PadHub hub_;
  std::vector<Monitor> monitors_;
  std::vector<Split> splits_;
  std::vector<std::unique_ptr<IdentifyWindow>> identify_;
  std::string identify_signature_ = "-";
  std::vector<Seat> seats_;
  int home_ = 0;
  uint64_t join_counter_ = 0;
  std::string notice_;
  float notice_time_ = 0.0f;
  bool finished_ = false;
  Done done_;
};

void SeatDialog::OnDraw(ImGuiIO& io) {
  if (finished_) {
    return;
  }
  // ---- controllers ---------------------------------------------------------
  const std::vector<PadInfo> pads = hub_.Poll();
  seats_.erase(std::remove_if(seats_.begin(), seats_.end(),
                              [&](const Seat& s) {
                                return std::none_of(pads.begin(), pads.end(), [&](const PadInfo& p) {
                                  return p.id == s.pad_id;
                                });
                              }),
               seats_.end());
  bool launch = false;
  for (const PadInfo& pad : pads) {
    auto it = std::find_if(seats_.begin(), seats_.end(),
                           [&](const Seat& s) { return s.pad_id == pad.id; });
    if (it == seats_.end()) {
      Seat seat;
      seat.pad_id = pad.id;
      seat.name = pad.name;
      seat.monitor = home_;
      seats_.push_back(std::move(seat));
      it = seats_.end() - 1;
      REXLOG_INFO("splitscreen: controller '{}' ({})", pad.name, pad.id);
    }
    Seat& seat = *it;
    const uint16_t pressed = uint16_t(pad.buttons & ~seat.prev_buttons);
    seat.prev_buttons = pad.buttons;
    const auto axis_dir = [](int16_t v) { return v > 16000 ? 1 : (v < -16000 ? -1 : 0); };
    const int dx = std::clamp(axis_dir(pad.lx) + ((pad.buttons & kPadRight) ? 1 : 0) -
                                  ((pad.buttons & kPadLeft) ? 1 : 0),
                              -1, 1);
    const int dy = std::clamp(-axis_dir(pad.ly) + ((pad.buttons & kPadDown) ? 1 : 0) -
                                  ((pad.buttons & kPadUp) ? 1 : 0),
                              -1, 1);
    const bool moved = (dx != 0 && dx != seat.prev_dx) || (dy != 0 && dy != seat.prev_dy);
    const int move_dx = dx != seat.prev_dx ? dx : 0;
    const int move_dy = dy != seat.prev_dy ? dy : 0;
    seat.prev_dx = dx;
    seat.prev_dy = dy;
    if (moved && (move_dx != 0 || move_dy != 0)) {
      const int target = NeighborMonitor(monitors_, seat.monitor, move_dx, move_dy);
      if (target != seat.monitor) {
        if (seat.joined && CountOn(target) >= kMaxPlayersPerMonitor) {
          Notice("Monitor " + std::to_string(target + 1) + " is full.");
        } else {
          seat.monitor = target;
        }
      }
    }
    if (pressed & kPadA) {
      if (!seat.joined) {
        if (CountOn(seat.monitor) >= kMaxPlayersPerMonitor) {
          Notice("Monitor " + std::to_string(seat.monitor + 1) + " is full.");
        } else if (Players().size() >= size_t(kMaxPlayers)) {
          Notice("Splitscreen is limited to " + std::to_string(kMaxPlayers) + " players.");
        } else {
          seat.joined = true;
          seat.join_order = ++join_counter_;
        }
      }
    } else if (pressed & kPadB) {
      seat.joined = false;
    } else if ((pressed & kPadY) && seat.joined) {
      Split& split = splits_[size_t(seat.monitor)];
      split = split == Split::kSideBySide ? Split::kTopBottom : Split::kSideBySide;
    } else if (pressed & kPadStart) {
      if (seat.joined) {
        launch = true;
      } else {
        Notice("Press A to take a spot first.");
      }
    }
  }
  const auto players = Players();
  if (!io.WantTextInput) {
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) &&
        !players.empty()) {
      launch = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      Finish(false);
      return;
    }
  }
  if (launch && !players.empty()) {
    Finish(true);
    return;
  }
  notice_time_ = std::max(0.0f, notice_time_ - io.DeltaTime);

  const std::vector<PlannedPlayer> plan = Plan();
  UpdateIdentify(plan);

  // ---- layout --------------------------------------------------------------
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                    IM_COL32(8, 10, 14, 255));
  const float scale = std::clamp(vp->Size.y / 720.0f, 1.0f, 2.5f);
  const ImU32 kText = IM_COL32(235, 238, 244, 255);
  const ImU32 kDim = IM_COL32(150, 156, 168, 255);
  ImFont* font = ImGui::GetFont();
  const float base = ImGui::GetStyle().FontSizeBase * scale;
  const auto text_size = [&](float size, const std::string& text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
  };
  const auto text_centered = [&](ImDrawList* dl, float size, ImVec2 center, ImU32 color,
                                 const std::string& text) {
    const ImVec2 ts = text_size(size, text);
    dl->AddText(font, size, ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), color,
                text.c_str());
  };

  ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36.0f * scale, 28.0f * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 8.0f * scale));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushFont(nullptr, base);
  ImGui::Begin("##skate3_splitscreen_seats", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  ImGui::PushFont(nullptr, base * 1.6f);
  ImGui::TextUnformatted("Splitscreen");
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextWrapped(
      "Every controller picks a screen. Controllers on the same monitor share it. Each player "
      "then gets their own game window and profile.");
  ImGui::PopStyleColor();
  ImGui::Spacing();

  // Monitor map, scaled from the desktop layout.
  RECT bounds = monitors_.front().rect;
  for (const Monitor& m : monitors_) {
    UnionRect(&bounds, &bounds, &m.rect);
  }
  const float map_w = ImGui::GetContentRegionAvail().x;
  const float map_h = vp->Size.y * 0.48f;
  const float bw = float(bounds.right - bounds.left), bh = float(bounds.bottom - bounds.top);
  const float k = std::min(map_w / bw, map_h / bh);
  const ImVec2 map_pos = ImGui::GetCursorScreenPos();
  const ImVec2 origin(map_pos.x + (map_w - bw * k) * 0.5f, map_pos.y + (map_h - bh * k) * 0.5f);
  const auto to_map = [&](const RECT& r) {
    return std::pair<ImVec2, ImVec2>(
        ImVec2(origin.x + (r.left - bounds.left) * k, origin.y + (r.top - bounds.top) * k),
        ImVec2(origin.x + (r.right - bounds.left) * k, origin.y + (r.bottom - bounds.top) * k));
  };
  const float gap = 6.0f * scale;
  for (int m = 0; m < int(monitors_.size()); ++m) {
    auto [p0, p1] = to_map(monitors_[size_t(m)].rect);
    p0 = ImVec2(p0.x + gap, p0.y + gap);
    p1 = ImVec2(p1.x - gap, p1.y - gap);
    const bool hovered = std::any_of(seats_.begin(), seats_.end(), [&](const Seat& s) {
      return !s.joined && s.monitor == m;
    });
    dl->AddRectFilled(p0, p1, IM_COL32(24, 27, 34, 255), 6.0f * scale);
    dl->AddRect(p0, p1, hovered ? IM_COL32(200, 205, 215, 255) : IM_COL32(70, 76, 90, 255),
                6.0f * scale, 0, hovered ? 3.0f * scale : 1.5f * scale);
    const float mh = p1.y - p0.y;
    bool any_player = false;
    for (const PlannedPlayer& p : plan) {
      if (p.monitor != m) {
        continue;
      }
      any_player = true;
      auto [s0, s1] = to_map(p.rect);
      s0 = ImVec2(s0.x + gap * 1.6f, s0.y + gap * 1.6f);
      s1 = ImVec2(s1.x - gap * 1.6f, s1.y - gap * 1.6f);
      const ImU32 color = PlayerColor(p.player);
      dl->AddRectFilled(s0, s1, (color & 0x00FFFFFF) | 0x50000000, 4.0f * scale);
      dl->AddRect(s0, s1, color, 4.0f * scale, 0, 2.0f * scale);
      const float sh = s1.y - s0.y;
      const ImVec2 c((s0.x + s1.x) * 0.5f, (s0.y + s1.y) * 0.5f);
      text_centered(dl, std::clamp(sh * 0.32f, base, base * 3.0f), ImVec2(c.x, c.y - sh * 0.08f),
                    color, "P" + std::to_string(p.player));
      dl->PushClipRect(s0, s1, true);
      text_centered(dl, base * 0.85f, ImVec2(c.x, c.y + sh * 0.22f), kText, p.pad_name);
      dl->PopClipRect();
    }
    if (!any_player) {
      text_centered(dl, std::clamp(mh * 0.35f, base, base * 4.0f),
                    ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), IM_COL32(70, 76, 90, 255),
                    std::to_string(m + 1));
    }
    const RECT& r = monitors_[size_t(m)].rect;
    std::string tag = std::to_string(m + 1) + "   " + std::to_string(r.right - r.left) + "x" +
                      std::to_string(r.bottom - r.top);
    if (m == home_) {
      tag += "   this screen";
    }
    dl->AddText(font, base * 0.8f, ImVec2(p0.x + 8.0f * scale, p0.y + 5.0f * scale), kDim,
                tag.c_str());
    const int count = CountOn(m);
    if (count == 2 || count == 3) {
      const std::string flip = "Y  flip split";
      const ImVec2 ts = text_size(base * 0.75f, flip);
      dl->AddText(font, base * 0.75f, ImVec2(p1.x - ts.x - 8.0f * scale, p0.y + 5.0f * scale),
                  kDim, flip.c_str());
    }
    // Controllers still choosing, stacked along the bottom of the monitor.
    float chip_y = p1.y - 8.0f * scale;
    for (const Seat& s : seats_) {
      if (s.joined || s.monitor != m) {
        continue;
      }
      const std::string label = s.name + "   A  join";
      const ImVec2 ts = text_size(base * 0.8f, label);
      const ImVec2 c0(p0.x + 8.0f * scale, chip_y - ts.y - 8.0f * scale);
      const ImVec2 c1(std::min(p1.x - 8.0f * scale, c0.x + ts.x + 16.0f * scale), chip_y);
      dl->AddRectFilled(c0, c1, IM_COL32(52, 60, 74, 245), 4.0f * scale);
      dl->AddRect(c0, c1, IM_COL32(200, 205, 215, 255), 4.0f * scale, 0, 1.5f * scale);
      dl->PushClipRect(c0, c1, true);
      dl->AddText(font, base * 0.8f, ImVec2(c0.x + 8.0f * scale, c0.y + 4.0f * scale), kText,
                  label.c_str());
      dl->PopClipRect();
      chip_y = c0.y - 4.0f * scale;
    }
  }
  ImGui::Dummy(ImVec2(map_w, map_h));
  ImGui::Spacing();

  // Controller list.
  if (seats_.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted("Connect a controller to begin.");
    ImGui::PopStyleColor();
  }
  for (const Seat& s : seats_) {
    int player = 0;
    for (const PlannedPlayer& p : plan) {
      if (p.pad_id == s.pad_id) {
        player = p.player;
      }
    }
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    const float lh = ImGui::GetTextLineHeight();
    dl->AddRectFilled(cp, ImVec2(cp.x + lh, cp.y + lh), PlayerColor(player), 3.0f * scale);
    ImGui::Dummy(ImVec2(lh, lh));
    ImGui::SameLine();
    std::string status;
    if (player > 0) {
      status = "Player " + std::to_string(player) + "  -  monitor " + std::to_string(s.monitor + 1);
      if (player == 1) {
        status += "  (game audio)";
      }
    } else {
      status = "not playing  -  A  take a spot on monitor " + std::to_string(s.monitor + 1);
    }
    ImGui::Text("%s", s.name.c_str());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, player > 0 ? kText : kDim);
    ImGui::Text("   %s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextWrapped(
      "Left stick / D-pad  Move between monitors    A  Take a spot    B  Leave    Y  Flip the "
      "split    Start  Play        Keyboard:  Enter  Play    Esc  Quit");
  ImGui::PopStyleColor();
  if (notice_time_ > 0.0f) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 180, 90, 255));
    ImGui::TextUnformatted(notice_.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Spacing();
  ImGui::BeginDisabled(players.empty());
  const std::string play = players.empty()
                               ? std::string("Play")
                               : "Play  (" + std::to_string(players.size()) +
                                     (players.size() == 1 ? " player)" : " players)");
  const bool play_clicked = ImGui::Button(play.c_str());
  ImGui::EndDisabled();
  ImGui::SameLine();
  const bool quit_clicked = ImGui::Button("Quit");

  ImGui::End();
  ImGui::PopFont();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);

  if (quit_clicked) {
    Finish(false);
  } else if (play_clicked && !players.empty()) {
    Finish(true);
  }
}

// ---------------------------------------------------------------------------
// Starting the player windows
// ---------------------------------------------------------------------------

// MSVC command-line quoting (CommandLineToArgvW's inverse).
std::wstring QuoteArg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring out = L"\"";
  for (auto it = arg.begin();; ++it) {
    size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
    } else {
      out.append(backslashes, L'\\');
    }
    out.push_back(*it);
  }
  out.push_back(L'"');
  return out;
}

// This launch's own arguments, minus the ones each player window gets its
// own value for (CLI11 rejects a repeated option).
std::vector<std::wstring> ForwardedArgs() {
  static const wchar_t* kOwnedFlags[] = {L"splitscreen", L"fullscreen", L"audio_mute"};
  static const wchar_t* kOwnedOptions[] = {L"splitscreen_session", L"splitscreen_player",
                                           L"input_bound_pad", L"window_fullscreen_rect",
                                           L"monitor"};
  std::vector<std::wstring> out;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return out;
  }
  for (int i = 1; i < argc; ++i) {
    std::wstring arg = argv[i];
    std::wstring name = arg;
    if (name.rfind(L"--", 0) == 0) {
      name.erase(0, 2);
    } else {
      out.push_back(arg);
      continue;
    }
    const size_t eq = name.find(L'=');
    const bool has_value = eq != std::wstring::npos;
    if (has_value) {
      name.resize(eq);
    }
    if (name.rfind(L"no-", 0) == 0) {
      name.erase(0, 3);
    }
    bool owned = false;
    for (const wchar_t* flag : kOwnedFlags) {
      owned |= name == flag;
    }
    bool owned_option = false;
    for (const wchar_t* option : kOwnedOptions) {
      owned_option |= name == option;
    }
    if (owned_option && !has_value) {
      ++i;  // "--option value": skip the value too
    }
    if (!owned && !owned_option) {
      out.push_back(arg);
    }
  }
  LocalFree(argv);
  return out;
}

void PumpUntil(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
               const std::function<bool()>& done) {
  HWND hwnd = nullptr;
  if (auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(window)) {
    hwnd = win32_window->hwnd();
  }
  while (!done() && !app_context.HasQuitFromUIThread()) {
    app_context.ExecutePendingFunctionsFromUIThread();
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
    if (window && hwnd && IsWindowVisible(hwnd)) {
      window->RequestPaint();
      RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

bool StartPlayers(const std::vector<PlannedPlayer>& plan) {
  const std::string session = std::to_string(GetCurrentProcessId()) + "_" +
                              std::to_string(GetTickCount64());
  HANDLE mutex = CreateMutexW(nullptr, FALSE, MutexName(session).c_str());
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(SharedBlock), MappingName(session).c_str());
  auto* block = mapping ? static_cast<SharedBlock*>(
                              MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)))
                        : nullptr;
  if (!mutex || !block) {
    REXLOG_ERROR("splitscreen: could not create the session ({})", GetLastError());
    return false;
  }
  std::memset(block, 0, sizeof(SharedBlock));
  block->magic = kMagic;
  block->version = kVersion;
  block->player_count = uint32_t(plan.size());

  wchar_t exe[MAX_PATH * 4] = {};
  GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
  std::wstring forwarded;
  for (const std::wstring& arg : ForwardedArgs()) {
    forwarded += L" " + QuoteArg(arg);
  }

  std::vector<HANDLE> processes(plan.size(), nullptr);
  for (size_t i = 0; i < plan.size(); ++i) {
    const PlannedPlayer& p = plan[i];
    const std::string rect = std::to_string(p.rect.left) + "," + std::to_string(p.rect.top) + "," +
                             std::to_string(p.rect.right - p.rect.left) + "," +
                             std::to_string(p.rect.bottom - p.rect.top);
    std::wstring cmd = QuoteArg(exe) + forwarded;
    cmd += L" " + QuoteArg(L"--splitscreen_session=" + Widen(session));
    cmd += L" " + QuoteArg(L"--splitscreen_player=" + std::to_wstring(p.player));
    cmd += L" " + QuoteArg(L"--input_bound_pad=" + Widen(p.pad_id));
    cmd += L" " + QuoteArg(L"--window_fullscreen_rect=" + Widen(rect));
    cmd += L" --fullscreen";
    if (p.player != 1) {
      cmd += L" --audio_mute";  // one game's audio, not N copies of it
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');
    if (!CreateProcessW(exe, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, nullptr, &si, &pi)) {
      REXLOG_ERROR("splitscreen: starting player {} failed ({})", p.player, GetLastError());
      MutexLock lock(mutex);
      block->slots[i].state = kSlotLeft;
      continue;
    }
    {
      MutexLock lock(mutex);
      block->slots[i].pid = pi.dwProcessId;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    processes[i] = pi.hProcess;
    REXLOG_INFO("splitscreen: player {} -> pid {} ({}, desktop {})", p.player, pi.dwProcessId,
                p.pad_id, rect);
  }

  // The session lives as long as someone holds it open: stay until every
  // window has opened it (or is gone), then leave the game to them.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  for (;;) {
    bool waiting = false;
    {
      MutexLock lock(mutex);
      for (size_t i = 0; i < plan.size(); ++i) {
        SharedSlot& slot = block->slots[i];
        if (slot.state != kSlotStarting) {
          continue;
        }
        if (!processes[i] || WaitForSingleObject(processes[i], 0) == WAIT_OBJECT_0) {
          slot.state = kSlotLeft;
          continue;
        }
        waiting = true;
      }
    }
    if (!waiting || std::chrono::steady_clock::now() > deadline) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  for (HANDLE h : processes) {
    if (h) {
      CloseHandle(h);
    }
  }
  UnmapViewOfFile(block);
  CloseHandle(mapping);
  CloseHandle(mutex);
  return true;
}

}  // namespace

void KeepAboveTaskbar(void* native_window) {
  HWND hwnd = static_cast<HWND>(native_window);
  if (!IsPlayer() || hwnd == nullptr) {
    return;
  }
  std::thread([hwnd] {
    Session* session = Session::Get();
    const DWORD own_pid = GetCurrentProcessId();
    while (IsWindow(hwnd)) {
      HWND foreground = GetForegroundWindow();
      DWORD foreground_pid = 0;
      if (foreground) {
        GetWindowThreadProcessId(foreground, &foreground_pid);
      }
      const bool playing = foreground_pid == own_pid ||
                           (foreground_pid != 0 && session && session->IsPlayerProcess(foreground_pid));
      const bool topmost = (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
      // Async: the window belongs to the UI thread, which must never wait
      // on this one.
      constexpr UINT kFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                              SWP_ASYNCWINDOWPOS;
      if (playing) {
        // Re-raise when anything (the taskbar, after the Start menu) covers
        // an edge of the window.
        RECT r{};
        GetWindowRect(hwnd, &r);
        const POINT edges[] = {{(r.left + r.right) / 2, r.bottom - 2},
                               {(r.left + r.right) / 2, r.top + 1},
                               {r.left + 1, (r.top + r.bottom) / 2},
                               {r.right - 2, (r.top + r.bottom) / 2}};
        bool covered = false;
        for (const POINT& pt : edges) {
          HWND at = WindowFromPoint(pt);
          covered |= at != nullptr && GetAncestor(at, GA_ROOT) != hwnd;
        }
        if (!topmost || covered) {
          SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, kFlags);
        }
      } else if (topmost) {
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, kFlags);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }).detach();
}

bool RunLauncher(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                 rex::ui::ImGuiDrawer* drawer) {
  if (drawer == nullptr) {
    REXLOG_WARN("splitscreen: no UI to show the seat screen on");
    return false;
  }
  HWND hwnd = nullptr;
  if (auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(window)) {
    hwnd = win32_window->hwnd();
  }
  struct Result {
    bool done = false;
    bool launch = false;
    std::vector<PlannedPlayer> plan;
  };
  auto result = std::make_shared<Result>();
  new SeatDialog(drawer, hwnd, [result](bool launch, std::vector<PlannedPlayer> plan) {
    result->launch = launch;
    result->plan = std::move(plan);
    result->done = true;
  });
  REXLOG_INFO("splitscreen: showing the seat screen");
  if (window) {
    window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  }
  PumpUntil(app_context, window, [&] { return result->done; });
  if (!result->done || !result->launch || result->plan.empty()) {
    REXLOG_INFO("splitscreen: seat screen closed without starting");
    return false;
  }
  // Out of the way of the player windows while they start.
  if (hwnd) {
    ShowWindow(hwnd, SW_HIDE);
  }
  return StartPlayers(result->plan);
}

#else  // !_WIN32

struct Session::Impl {};
Session* Session::Get() {
  return nullptr;
}
int Session::TakenBy(std::string_view) {
  return 0;
}
bool Session::Claim(std::string_view, std::string_view) {
  return true;
}
void Session::Release() {}
bool Session::IsPlayerProcess(uint32_t) {
  return false;
}
void KeepAboveTaskbar(void*) {}
void Session::Count(int& signed_in, int& players) {
  signed_in = players = 0;
}
void Session::WithLock(const std::function<void()>& fn) {
  fn();
}
uint32_t Session::StoreGeneration() {
  return 0;
}
void Session::BumpStoreGeneration() {}

bool RunLauncher(rex::ui::WindowedAppContext&, rex::ui::Window*, rex::ui::ImGuiDrawer*) {
  return false;
}

#endif  // _WIN32

}  // namespace skate3::splitscreen
