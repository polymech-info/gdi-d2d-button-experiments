// AppDefaults.cpp — window placement persistence.
// See AppDefaults.hpp for the public contract.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#pragma comment(lib, "advapi32.lib")

#include "AppDefaults.hpp"

namespace AppDefaults {

namespace {

// Registry key path (no trailing backslash).
static constexpr wchar_t kRegKey[]   = L"Software\\Polymech\\Win32Mini";
static constexpr wchar_t kRegValue[] = L"WindowPlacement";

// Minimum visible strip that must land on a real monitor when we decide
// whether the saved position is still "reachable".
constexpr int kMinVisiblePx = 64;

// -----------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------

// Returns a monitor rect that covers the given rect, or primary if none.
// *workArea receives the work area (sans taskbar) of that monitor.
HMONITOR MonitorForRect(const RECT& r, RECT* workArea)
{
    HMONITOR hm = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!hm) {
        hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(hm, &mi)) {
        *workArea = mi.rcWork;
    } else {
        // Last-resort fallback — single-monitor desktop rect
        workArea->left = workArea->top = 0;
        workArea->right  = GetSystemMetrics(SM_CXSCREEN);
        workArea->bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    return hm;
}

// Clamp rect so it fits on the given work area and has at least
// kMinVisiblePx of its top-left corner visible.
void ConstrainToWorkArea(RECT& r, const RECT& wa)
{
    const int rw = r.right - r.left;
    const int rh = r.bottom - r.top;
    const int waw = wa.right - wa.left;
    const int wah = wa.bottom - wa.top;

    // Don't grow the window beyond the work area, but don't shrink below
    // the minimum either (that's enforced elsewhere by WM_GETMINMAXINFO).
    const int w = (rw > waw) ? waw : rw;
    const int h = (rh > wah) ? wah : rh;

    // Clamp position so top-left stays inside the work area.
    int x = r.left;
    int y = r.top;
    if (x + kMinVisiblePx > wa.right)  { x = wa.right  - kMinVisiblePx; }
    if (y + kMinVisiblePx > wa.bottom) { y = wa.bottom - kMinVisiblePx; }
    if (x < wa.left) { x = wa.left; }
    if (y < wa.top)  { y = wa.top;  }

    r.left   = x;
    r.top    = y;
    r.right  = x + w;
    r.bottom = y + h;
}

// Center a rect of given size inside a work area.
RECT CenteredRect(int w, int h, const RECT& wa)
{
    const int waw = wa.right - wa.left;
    const int wah = wa.bottom - wa.top;
    const int x = wa.left + (waw - w) / 2;
    const int y = wa.top  + (wah - h) / 2;
    return { x, y, x + w, y + h };
}

bool ReadPlacement(WINDOWPLACEMENT& wp)
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &hk) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD size = sizeof(wp);
    const bool ok =
        RegQueryValueExW(hk, kRegValue, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&wp), &size) == ERROR_SUCCESS
        && type == REG_BINARY
        && size == sizeof(WINDOWPLACEMENT);

    RegCloseKey(hk);
    return ok;
}

void WritePlacement(const WINDOWPLACEMENT& wp)
{
    HKEY hk = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, &disp)
        != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(hk, kRegValue, 0, REG_BINARY,
                   reinterpret_cast<const BYTE*>(&wp), sizeof(wp));
    RegCloseKey(hk);
}

} // anonymous namespace

// -----------------------------------------------------------------
// Public API
// -----------------------------------------------------------------

void LoadAndApply(HWND hwnd)
{
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);

    if (!ReadPlacement(wp)) {
        // No saved data — leave the window wherever CreateWindowEx placed it.
        return;
    }

    // Validate blob length (future-proofing against struct layout changes).
    if (wp.length != sizeof(WINDOWPLACEMENT)) {
        return;
    }

    // --- Monitor-aware fixup ---
    // rcNormalPosition is in "workspace coordinates" (virtual screen).
    // Check whether any monitor still overlaps this rect.
    HMONITOR savedMon = MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL);
    RECT wa{};
    if (!savedMon) {
        // The monitor this window lived on is gone.  Find the primary and
        // relocate the window there, preserving its size.
        HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(primary, &mi);
        wa = mi.rcWork;

        const int w = wp.rcNormalPosition.right  - wp.rcNormalPosition.left;
        const int h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        wp.rcNormalPosition = CenteredRect(w, h, wa);
        // Do not restore as maximised on a different monitor.
        if (wp.showCmd == SW_SHOWMAXIMIZED) {
            wp.showCmd = SW_SHOWNORMAL;
        }
    } else {
        // Monitor exists — still clamp in case the work area shrank
        // (e.g. taskbar moved, resolution changed, DPI scale changed).
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(savedMon, &mi);
        wa = mi.rcWork;
        ConstrainToWorkArea(wp.rcNormalPosition, wa);
    }

    // Never restore as minimised on startup.
    if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE) {
        wp.showCmd = SW_SHOWNORMAL;
    }

    // Apply.  SetWindowPlacement will also set the show state; the caller
    // must NOT call ShowWindow with SW_SHOW / SW_SHOWNORMAL afterwards, or
    // it will fight with whatever the initial show_cmd was.
    // We let the caller still call ShowWindow with the original show_cmd —
    // that call will win for the first show if the saved cmd is SW_SHOWNORMAL,
    // and WINDOWPLACEMENT already stores the maximised bit separately so
    // maximised restores correctly either way.
    wp.flags = 0; // clear WPF_RESTORETOMAXIMIZED; we manage showCmd ourselves
    SetWindowPlacement(hwnd, &wp);
}

void Save(HWND hwnd)
{
    // Never save the minimised position — it's an off-screen parking spot.
    if (IsIconic(hwnd)) {
        return;
    }

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) {
        return;
    }

    // rcNormalPosition already holds the restored rect even when maximised.
    WritePlacement(wp);
}

} // namespace AppDefaults
