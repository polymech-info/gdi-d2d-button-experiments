#pragma once

// Owner-drawn tooltip. Replaces tooltips_class32 for ribbon buttons.
// Text is a per-button option (Draw.tooltip) and can change while shown.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace glowplay {

class GlowTile;

class GlowTip {
public:
    float fontPx = 13.f;
    float radius = 8.f;
    float padX = 13.f;
    float padY = 8.f;
    int maxW = 300;
    int delayMs = 420;
    int gapPx = 8;
    const wchar_t* fontFace = L"Segoe UI Variable Text";
    bool enabled = true;

    void Attach(HWND owner);
    void Detach();
    void Hide();
    void SyncFrom(const GlowTile& tile); // face + px + radius from the tile metrics

    // `id` is the hover identity. New id restarts the delay; same id + new text
    // updates immediately if the tip is already up.
    void Track(int id, const RECT& anchorClient, const wchar_t* text, bool light, COLORREF accent);

    void SetText(const wchar_t* text); // runtime change while visible
    bool OnTimer(HWND owner, WPARAM timerId);

private:
    HWND owner_ = nullptr;
    HWND popup_ = nullptr;
    int id_ = -1;
    int pendingId_ = -1;
    bool visible_ = false;
    bool light_ = true;
    COLORREF accent_ = RGB(0, 162, 255);
    RECT anchor_{};
    std::wstring text_;
    std::wstring pending_;

    void EnsurePopup();
    void Present();
    void Place(int tipW, int tipH, POINT& screen);
    void PaintLayered(int w, int h);
};

} // namespace glowplay

namespace pmui {
using glowplay::GlowTip;
}
