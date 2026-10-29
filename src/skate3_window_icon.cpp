// See skate3_window_icon.h.

#include "skate3_window_icon.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace skate3 {
namespace {

// Process-lifetime: the window takes no ownership of WM_SETICON icons.
HICON g_big_icon = nullptr;
HICON g_small_icon = nullptr;
WNDPROC g_prev_proc = nullptr;
UINT g_refresh_msg = 0;

// The caption only picks an icon up when it CHANGES: re-sending the handle
// the window already holds is ignored (verified live), so clear it first.
void SetIcons(HWND hwnd) {
  SendMessageW(hwnd, WM_SETICON, ICON_SMALL, 0);
  SendMessageW(hwnd, WM_SETICON, ICON_BIG, 0);
  if (g_big_icon != nullptr) {
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_big_icon));
  }
  if (g_small_icon != nullptr) {
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_small_icon));
  }
}

// Both games start fullscreen, where the SDK strips WS_OVERLAPPEDWINDOW;
// leaving fullscreen reinstates it with SetWindowLong, and the rebuilt
// caption came back without the icon the window still held. Re-apply it
// whenever the caption returns -- posted, so it runs after the style change
// (and the SDK's resize handling around it) has finished.
LRESULT CALLBACK IconSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_STYLECHANGED && wparam == WPARAM(GWL_STYLE)) {
    const auto* ss = reinterpret_cast<const STYLESTRUCT*>(lparam);
    if ((ss->styleNew & WS_CAPTION) == WS_CAPTION &&
        (ss->styleOld & WS_CAPTION) != WS_CAPTION) {
      PostMessageW(hwnd, g_refresh_msg, 0, 0);
    }
  } else if (g_refresh_msg != 0 && msg == g_refresh_msg) {
    SetIcons(hwnd);
    return 0;
  }
  return CallWindowProcW(g_prev_proc, hwnd, msg, wparam, lparam);
}

}  // namespace

void ApplyEmbeddedWindowIcon(void* native_window_handle) {
  HWND hwnd = static_cast<HWND>(native_window_handle);
  if (hwnd == nullptr) {
    return;
  }
  if (g_big_icon == nullptr && g_small_icon == nullptr) {
    HINSTANCE exe = GetModuleHandleW(nullptr);
    g_big_icon = static_cast<HICON>(
        LoadImageW(exe, L"MAINICON", IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                   GetSystemMetrics(SM_CYICON), 0));
    g_small_icon = static_cast<HICON>(
        LoadImageW(exe, L"MAINICON", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON), 0));
    if (g_big_icon == nullptr && g_small_icon == nullptr) {
      return;  // no MAINICON in this exe
    }
  }
  SetIcons(hwnd);
  if (g_prev_proc == nullptr) {
    g_refresh_msg = RegisterWindowMessageW(L"skate3.window_icon.refresh");
    g_prev_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&IconSubclassProc)));
  }
}

}  // namespace skate3

#else

namespace skate3 {
void ApplyEmbeddedWindowIcon(void*) {}
}  // namespace skate3

#endif
