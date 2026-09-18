#pragma once

// Ribbon-facing GlowTile factory. Copy GlowHost + GlowTile + GlowTip into Tanit.
// Do not copy GlowPlay or SvgIcon — bind Tanit icons with SetIconBind.
//
// After integrate, D2D is the default painter. Pass a Frame with an RT; HDC-only
// falls back to GDI+ (PARGB DIB + AlphaBlend).
//
// Theme / font — do not include Tanit theme.cpp or ui_font.cpp from this file.
//   host.ApplyTheme(theme_palette());          // any type with ThemePalette fields
//   host.ApplyFont(ui_font_extra_pt(), dpi);   // appearance.font_size_extra_pt (2 = system)
//
// Metrics / pulse tokens are copies of pmui::ui::RibbonStripLayout + CommandPulse
// (ui_constants.hpp). Keep the numbers in sync when that header changes.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef DrawText
#undef DrawText
#endif

#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>

#include "GlowTile.h"
#include "GlowTip.h"

namespace glowplay {

// Subset of pmui::ThemePalette that the tile needs. Field names match on purpose
// so ApplyTheme(theme_palette()) compiles in Tanit without this header seeing theme.hpp.
struct ThemeSnap {
    bool dark = false;
    COLORREF window_bg = RGB(245, 244, 243); // ui::ContentSurface light #F5F4F3
    COLORREF window_fg = RGB(50, 49, 48);    // make_light()
    COLORREF accent = RGB(0, 120, 212);      // #0078D4
    COLORREF caption_pen = RGB(225, 223, 221);
    COLORREF control_fg = RGB(32, 31, 30);

    static ThemeSnap Light(); // theme.cpp make_light()
    static ThemeSnap Dark();  // theme.cpp make_dark()
};

class GlowHost {
public:
    // Stored appearance.font_size_extra_pt. 2 = system message font (+0 pt).
    static constexpr int kFontSystemExtraPt = 2;

    // pmui::ui::CommandPulse — same cadence on every command surface.
    struct Pulse {
        static constexpr int periodMs = 1400;
        static constexpr int tickMs = 33;
        static constexpr int abortRevealMs = 220;
        static constexpr int errorHoldMs = 6000;
        static constexpr int alphaMin = 80;
        static constexpr int alphaMax = 220;
    };

    // pmui::ui::RibbonStripLayout / IconSize (logical px @ 96).
    struct Strip {
        static constexpr int iconLarge = 24;
        static constexpr int iconSmall = 20;
        static constexpr int iconVisualMax = 28;
        static constexpr int toolbarPadX = 12;
        static constexpr int dropdownArrowW = 14;
        static constexpr int iconOnlyButtonW = 36;
        static constexpr int darkHairline = 56;
        static constexpr int lightHairline = 168;
        static constexpr int sepVertInsetMin = 4;
        static constexpr int sepVertInsetDiv = 4;
    };

    bool preferD2d = true;
    bool glowOn = true;
    bool shadowOn = true;

    GlowTile* tile = nullptr;
    GlowTip* tip = nullptr;

    void Attach(GlowTile& t, GlowTip* tipWnd = nullptr);

    void ApplyDpi(int dpi);
    // extraPtStored = appearance.font_size_extra_pt (0..8). dpi 0 → tile's current DPI.
    void ApplyFont(int extraPtStored, int dpi = 0);
    void ApplyTheme(const ThemeSnap& pal);
    void ApplyTheme(bool dark, COLORREF accent, COLORREF windowFg, COLORREF windowBg,
        COLORREF captionPen = CLR_INVALID);

    // Tanit: host.ApplyTheme(theme_palette());
    template <typename Pal>
    void ApplyTheme(const Pal& pal)
    {
        ThemeSnap t;
        t.dark = pal.dark;
        t.window_bg = pal.window_bg;
        t.window_fg = pal.window_fg;
        t.accent = pal.accent;
        t.caption_pen = pal.caption_pen;
        ApplyTheme(t);
    }

    const ThemeSnap& Theme() const { return theme_; }
    int FontExtraPt() const { return fontExtraPt_; }

    struct Item {
        bool down = false;
        bool selected = false;
        bool running = false;
        bool finish = false;
        bool failed = false;
        bool hot = false;
        bool enabled = true;
        bool dropdown = false;
        float progress = -1.f; // <0 off; else 0..100
        COLORREF accent = 0;   // 0 → theme accent
        const wchar_t* caption = nullptr;
        const wchar_t* tooltip = nullptr;
        const wchar_t* iconStem = nullptr;
        LabelMode labelMode = LabelMode::Inside;
        bool paintCaption = true;
        ULONGLONG pulseOrigin = 0; // 0 + running → host clock
    };

    // CDIS_* from NMTBCUSTOMDRAW without pulling commctrl.
    // CDIS_SELECTED=0x0002 (mouse-down), CDIS_DISABLED=0x0004, CDIS_HOT=0x0040.
    static Item FromToolbarDraw(UINT uItemState, bool checked, bool running, bool finish, bool failed);

    GlowTile::Draw FillDraw(const Item& item) const;

    using IconGdi = void (*)(Gdiplus::Graphics&, const Gdiplus::RectF&, const wchar_t* stem, ToolBtnState,
        const Gdiplus::Color&);
    using IconD2d = void (*)(ID2D1RenderTarget*, ID2D1Factory*, const D2D1_RECT_F&, const wchar_t* stem,
        ToolBtnState, const D2D1_COLOR_F&);

    void SetIconBind(IconGdi gdi, IconD2d d2d);

    struct Frame {
        HDC hdc = nullptr;
        ID2D1RenderTarget* rt = nullptr;
        ID2D1Factory* factory = nullptr;
        IDWriteFactory* dwrite = nullptr;
        IDWriteTextFormat* labelTf = nullptr;
    };

    // D2D when preferD2d && Frame.rt; else GDI+ if hdc is set.
    bool PaintItem(const Frame& f, const RECT& cell, const Item& item);
    bool PaintItem(const Frame& f, const RECT& cell, const GlowTile::Draw& d, bool dropdown = false);

    void PaintSeparator(HDC hdc, const RECT& slot, COLORREF bg = CLR_INVALID) const;
    void PaintSeparator(ID2D1RenderTarget* rt, const RECT& slot) const;
    void PaintChevron(HDC hdc, const RECT& cell, COLORREF color) const;
    void PaintChevron(ID2D1RenderTarget* rt, const RECT& cell, COLORREF color) const;

    int ScalePx(int logical96) const;
    int LargeIconPx() const;
    int CompactIconPx() const;
    int IconOnlyButtonW() const;
    int DropdownArrowW() const;
    COLORREF Hairline() const;
    COLORREF LabelColor() const;

    static float Pulse01(ULONGLONG origin = 0);
    static float MessageFontPx96(int extraPtStored);

private:
    ThemeSnap theme_{};
    int fontExtraPt_ = kFontSystemExtraPt;
    IconGdi iconGdi_ = nullptr;
    IconD2d iconD2d_ = nullptr;

    void SyncTip();
    COLORREF ChevronColor(const GlowTile::Draw& d) const;
};

} // namespace glowplay

namespace pmui {
using glowplay::GlowHost;
using glowplay::ThemeSnap;
}
