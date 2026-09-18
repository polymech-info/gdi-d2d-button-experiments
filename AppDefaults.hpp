// AppDefaults — persists and restores window placement across sessions.
//
// Storage  : HKCU\Software\Polymech\Win32Mini  (binary WINDOWPLACEMENT blob)
// Monitor  : if the saved monitor is no longer attached the window is moved
//            to the primary monitor's work area, keeping its saved size.

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace AppDefaults {

// Restore the last-saved window placement on hwnd.
// Call after CreateWindowExW but before ShowWindow.
// Falls back gracefully when no saved data exists or the target monitor was
// unplugged since last run.
void LoadAndApply(HWND hwnd);

// Persist the current placement of hwnd to the registry.
// Call on WM_WINDOWPOSCHANGED (real moves/resizes) and WM_DESTROY.
// Skips writes while the window is minimised (minimised coords are junk).
void Save(HWND hwnd);

} // namespace AppDefaults
