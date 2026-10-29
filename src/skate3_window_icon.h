// Title-bar / taskbar icon from the exe's own MAINICON resource.
//
// The SDK registers its window class with hIcon = MAINICON, which is enough
// for Explorer and Alt-Tab, but the caption stayed blank: the window had no
// icon of its OWN (WM_GETICON returned null for every size) and the title bar
// draws only that. Sending the window its own correctly sized icons fixes it
// (verified live: WM_SETICON on a running window made the caption icon
// appear). Both games start fullscreen, though, and the caption the SDK
// rebuilds on leaving fullscreen dropped the icon again, so the window is
// also subclassed to re-apply it whenever its caption returns.
// Identical copy in sk83_na_m/src and skate3recomp/src.
#pragma once

namespace skate3 {

// No-op when the exe carries no MAINICON (the window keeps its default) and
// on non-Windows platforms.
void ApplyEmbeddedWindowIcon(void* native_window_handle);

}  // namespace skate3
