#pragma once

// Portable ribbon-tile chrome. Copy GlowHost + GlowTile + GlowTip into Tanit; keep GlowPlay here.
// Tanit entry is GlowHost (D2D default). This class is the painter GlowHost drives.
//
//   host.ApplyDpi(GetDpiForWindow(hwnd));
//   host.ApplyTheme(theme_palette());
//   host.ApplyFont(ui_font_extra_pt(), dpi);
//   host.PaintItem(frame, cell, host.FromToolbarDraw(...));
//
// GDI custom-draw is physical pixels + ApplyDpi. D2D: keep the RT at 96 DPI or
// UNIT_MODE_PIXELS — do not ApplyDpi *and* SetDpi.
// Icon drawing is a callback. Hosts pass `iconStem` (Tabler filled name, no .svg).

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

namespace glowplay {

enum class ToolBtnState {
    Idle,
    Hover,
    Pressing,
    Pressed,
    Running,
    Finish, // after Running — pale card, green icon + hairline
    Failed,
};

enum class LabelMode {
    None,
    Inside, // product ribbon — default
    Below,
};

constexpr COLORREF kFinishAccent = RGB(34, 197, 94); // success green after Running

constexpr int kToolBtnStateCount = 7;

inline int StateIndex(ToolBtnState s)
{
    return static_cast<int>(s);
}

// Map toolbar flags → duty. Pointer-over is `Draw.hot`, not a state.
inline ToolBtnState DutyState(bool down, bool selected, bool running, bool finish, bool failed)
{
    if (down) {
        return ToolBtnState::Pressing;
    }
    if (failed) {
        return ToolBtnState::Failed;
    }
    if (finish) {
        return ToolBtnState::Finish;
    }
    if (running) {
        return ToolBtnState::Running;
    }
    if (selected) {
        return ToolBtnState::Pressed;
    }
    return ToolBtnState::Idle;
}

// Light / dark pair. Resolve with theme.
struct Tone {
    BYTE light = 0;
    BYTE dark = 0;

    BYTE At(bool isLight) const
    {
        return isLight ? light : dark;
    }
};

struct BloomLayer {
    float expand = 0.f;
    BYTE alpha = 0;
    float width = 1.f;
};

struct ToneF {
    float light = 1.f;
    float dark = 1.f;

    float At(bool isLight) const
    {
        return isLight ? light : dark;
    }
};

// RGB that can follow the button accent or a fixed light/dark pair.
struct ColorSlot {
    bool fromAccent = true;
    COLORREF light = 0;
    COLORREF dark = 0;

    COLORREF At(bool isLight, COLORREF accent) const
    {
        if (fromAccent) {
            return accent;
        }
        return isLight ? light : dark;
    }
};

struct RgbPair {
    COLORREF light = RGB(255, 255, 255);
    COLORREF dark = RGB(255, 255, 255);

    COLORREF At(bool isLight) const
    {
        return isLight ? light : dark;
    }
};

// One duty state's recipe. Pulse terms are added: value + pulse * pulseTerm (pulse in 0..1).
// Pigments default to the tile palette (seeded in ResetToDefaults); override per state.
struct StateCfg {
    bool bloom = false; // glow
    float bloomScale = 0.f;
    float bloomScalePulse = 0.f;
    float bloomAlpha = 1.f;
    float bloomAlphaPulse = 0.f;
    ColorSlot glow{}; // bloom pigment

    bool tint = false; // bg wash
    Tone tintA{};
    BYTE tintAPulse = 0;
    ColorSlot bg{};

    bool edge = false; // frame
    Tone edgeA{};
    BYTE edgeAPulse = 0;
    float edgeWidth = 1.f;
    ColorSlot frame{};
    ColorSlot bevelLo{};
    RgbPair bevelHi{RGB(255, 255, 255), RGB(236, 248, 255)};
    float edgeTowardWhite = 0.333f;
    float bevelLoGainLight = 0.5f;
    float bevelLoGainDark = 1.f;

    bool glass = false;
    Tone hiA{};
    BYTE hiAPulse = 0;
    Tone loA{};
    RgbPair glassRgb{RGB(255, 255, 255), RGB(236, 246, 255)};

    bool shadow = false;
    Tone shadowA{};
    ColorSlot shade{};

    RgbPair label{RGB(32, 36, 42), RGB(236, 240, 246)};
    Tone labelA{220, 235};
    float labelPx = 0.f; // 0 → metrics.labelPx

    Tone iconA{255, 255};
    ToneF iconGain{0.78f, 0.58f};
    float iconGainPulse = 0.f;

    bool pressNudge = false;
    bool darken = false;
    float darkenGain = 0.55f;
    bool shadowEven = false; // omnidirectional halo (neumorphic); else light-source drop

    // Optional LTR fill when Draw.progress is 0..100. Behind icon + label.
    bool progressFill = false;
    ColorSlot progressFrom{}; // start (left); default = accent
    ColorSlot progressTo{};   // end (leading edge); default = accent lifted
    float progressBlend = 0.4f;
};

// Pointer-over always paints the Hover recipe (same on every duty). Pressing wins.
struct HotOverlay {
    bool enabled = true;
};

struct TileMetrics {
    int cellW = 64;    // RibbonNextTuning::labeled_min_w
    int cellWMax = 84; // RibbonNextTuning::labeled_max_w — grow for long captions
    int cellH = 58;
    int iconPx = 28;
    int glowPad = 3; // bloom reach (~3px); drop uses its own blur/offset
    int labelPadX = 12; // RibbonStripLayout::toolbar_pad_x (both sides when sizing)
    float radius = 8.f;
    float labelPxBase = 13.f; // painted size is labelPxBase * (dpi/96)
    float labelPx = 13.f;
    float iconTop = 5.f;
    float labelGap = 3.f;
    float labelHBase = 18.f; // ApplyFont grows this with extra-pt
    float labelH = 18.f;
    float labelBelowGap = 12.f;
    float labelInsetX = 5.f; // content_inset_x + label_inset_x
    float glassHeight = 0.40f;
    float shadowDx = 1.5f; // product px; equal dx/dy = 45° (light from above-left)
    float shadowDy = 1.5f;
    float shadowBlur = 3.f; // gaussian radius (3-pass box)
    const wchar_t* fontFace = L"Segoe UI Variable Text";
};

// Shared seed copied onto every state by ApplyPalette / ResetToDefaults.
struct TileColors {
    RgbPair label{RGB(32, 36, 42), RGB(236, 240, 246)};
    Tone labelA{220, 235};

    RgbPair glass{RGB(255, 255, 255), RGB(236, 246, 255)};
    RgbPair bevelHi{RGB(255, 255, 255), RGB(236, 248, 255)};

    ColorSlot glow{};
    ColorSlot shade{};
    ColorSlot bg{};
    ColorSlot frame{};
    ColorSlot bevelLo{};

    float edgeTowardWhite = 0.333f;
    float bevelLoGainLight = 0.5f;
    float bevelLoGainDark = 1.f;

    Tone idleIconA{230, 200};
    BYTE activeIconA = 255;
};

// Resolved paint snapshot (after theme / pulse / glow / shadow / hot).
struct TileFx {
    bool bloom = false;
    bool tint = false;
    bool edge = false;
    bool glass = false;
    bool shadow = false;
    bool shadowEven = false;
    float shadowScale = 1.f;
    bool pressNudge = false;
    bool darken = false;
    float bloomScale = 0.f;
    float bloomAlpha = 1.f;
    float edgeWidth = 1.f;
    float darkenGain = 0.55f;
    float iconGain = 1.f;
    float labelPx = 0.f;
    BYTE tintA = 0;
    BYTE edgeA = 0;
    BYTE hiA = 0;
    BYTE loA = 0;
    BYTE shadowA = 0;
    BYTE labelA = 0;
    BYTE iconA = 0;
    COLORREF bloomRgb = 0;
    COLORREF shadowRgb = 0;
    COLORREF tintRgb = 0;
    COLORREF edgeRgb = 0;
    COLORREF glassRgb = 0;
    COLORREF bevelHiRgb = 0;
    COLORREF bevelLoRgb = 0;
    COLORREF labelRgb = 0;
    bool progress = false;
    float progress01 = 0.f;
    BYTE progressA = 0;
    COLORREF progressFromRgb = 0;
    COLORREF progressToRgb = 0;
};

class GlowTile {
public:
    GlowTile();

    void ResetToDefaults();
    void ApplyPalette(); // copy `colors` onto every state (keeps on/off + alphas)
    void ApplyDpi(int dpi); // 96 = product ribbon; scales every metric from 96-dpi bases
    void ApplyScale(int scale); // playground: 1 → ApplyDpi(96), 2 → ApplyDpi(192)
    void ApplyProductRibbonMetrics(); // ApplyDpi(96)
    int Dpi() const { return dpi_; }

    // Bloom + gaussian drop paint outside the cell. Custom-draw hosts: invalidate this.
    int ChromeBleed() const { return metrics.glowPad + 2; }
    RECT PaintBounds(const RECT& cell, LabelMode mode, bool hasCaption) const;

    StateCfg& at(ToolBtnState s);
    const StateCfg& at(ToolBtnState s) const;

    TileMetrics metrics;
    TileColors colors;
    HotOverlay hot;
    BloomLayer bloomLayers[8]{};
    int bloomLayerCount = 4;

    struct Draw {
        ToolBtnState state = ToolBtnState::Idle;
        COLORREF accent = RGB(0, 162, 255);
        float pulse = 0.5f;
        const wchar_t* caption = nullptr;
        const wchar_t* tooltip = nullptr; // runtime-mutable; null = no tip
        const wchar_t* iconStem = nullptr; // Tabler `icons/filled/<stem>.svg`
        LabelMode labelMode = LabelMode::Inside;
        bool light = true;
        bool hot = false;
        float progress = -1.f; // <0 off; else 0..100, call site updates
        bool glowOn = true;
        bool shadowOn = true;
        bool enabled = true; // false → no bloom/drop, dim icon + label
        bool paintCaption = true; // false → chrome + icon only (host owns DrawText)
        void (*iconGdi)(Gdiplus::Graphics&, const Gdiplus::RectF&, const wchar_t* stem, ToolBtnState,
            const Gdiplus::Color&) = nullptr;
        void (*iconD2d)(ID2D1RenderTarget*, ID2D1Factory*, const D2D1_RECT_F&, const wchar_t* stem,
            ToolBtnState, const D2D1_COLOR_F&) = nullptr;
    };

    TileFx Resolve(const Draw& d) const;

    // Segoe UI Variable Text → Segoe UI. Used by GDI+ path text + D2D + tooltip.
    static const wchar_t* UiFontFace();
    static void FillUiTextGdi(Gdiplus::Graphics& g, const wchar_t* text, float px, const Gdiplus::RectF& rc,
        const Gdiplus::Color& color, Gdiplus::StringAlignment alignH, Gdiplus::StringAlignment alignV,
        bool ellipsis);
    static Gdiplus::RectF MeasureUiTextGdi(const wchar_t* text, float px, float maxW);

    // Same clamp as DesiredToolbarButtonWidth: [cellW, cellWMax] from measured caption.
    int LabeledWidth(const wchar_t* caption) const;

    bool PaintGdi(HDC dest, const RECT& cell, const Draw& d) const;
    void PaintD2d(ID2D1RenderTarget* rt, ID2D1Factory* factory, IDWriteFactory* dwrite, IDWriteTextFormat* labelTf,
        const RECT& cell, const Draw& d) const;

    static void FlushPaintCaches(); // scratch DIB + D2D drop bitmaps (call on RT/DPI teardown)

private:
    StateCfg states_[kToolBtnStateCount]{};
    int dpi_ = 96;

    void PaintChromeGdi(Gdiplus::Graphics& g, const Gdiplus::RectF& cell, const Draw& d, const TileFx& fx) const;
    void PaintContentGdi(Gdiplus::Graphics& g, const Gdiplus::RectF& cell, const Draw& d, const TileFx& fx) const;
};

} // namespace glowplay

// Tanit alias — copy these headers as-is and use pmui::GlowTile.
namespace pmui {
using glowplay::BloomLayer;
using glowplay::ColorSlot;
using glowplay::DutyState;
using glowplay::GlowTile;
using glowplay::HotOverlay;
using glowplay::LabelMode;
using glowplay::RgbPair;
using glowplay::StateCfg;
using glowplay::TileColors;
using glowplay::TileFx;
using glowplay::TileMetrics;
using glowplay::Tone;
using glowplay::ToneF;
using glowplay::ToolBtnState;
}
