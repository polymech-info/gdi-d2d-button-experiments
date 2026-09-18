#pragma once

// Ribbon glow playground (Variation 4). After integrate, GlowHost paints D2D
// by default; the GDI+ row is the fallback (PARGB DIB + AlphaBlend).

#include <windows.h>

#include "GlowTile.h"

namespace glowplay {

constexpr bool FEATURE_GLOWPLAY = true;

// Cadence matches pmui::ui::CommandPulse so a port keeps the same running beat.
constexpr int kPulsePeriodMs = 1400;
constexpr int kPulseTickMs = 33;
constexpr UINT_PTR kPulseTimerId = 0x504D474Cu; // 'PMGL'

GlowTile& Tile(); // mutate Tile().at(Hover).glow / .bg / .frame / .label …
// Theme + app font go through GlowHost (T / Light chip, [ ] extra-pt).
// Draw.tooltip is per-button and can change while shown (GlowTip, not tooltips_class32).

void SetHost(HWND hwnd);
void Shutdown();

// Vertical space the playground wants above `reserveBottom` (filmstrip parked).
int PanelMinHeightPx();

// Paint into the band under the title bar, above `reserveBottom`.
void PaintOverGdi(HDC hdc, int windowW, int windowH, int titleBarPx, int reserveBottom,
    int frameInset);

bool OnMouseMove(HWND hwnd, int clientX, int clientY, int windowW, int windowH, int titleBarPx,
    int reserveBottom, int frameInset);
bool OnLButtonDown(HWND hwnd, int clientX, int clientY, int windowW, int windowH, int titleBarPx,
    int reserveBottom, int frameInset);
bool OnLButtonUp(HWND hwnd, int clientX, int clientY);
bool OnLButtonDblClk(HWND hwnd, int clientX, int clientY, int windowW, int windowH, int titleBarPx,
    int reserveBottom, int frameInset);
void OnMouseLeave(HWND hwnd);
bool OnSetCursor(); // hand over tiles/chips; true if this class set the cursor
bool OnTimer(HWND hwnd, WPARAM timerId);
bool OnKeyDown(HWND hwnd, UINT vk);

} // namespace glowplay
