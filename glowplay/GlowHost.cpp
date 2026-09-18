#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>

#ifdef DrawText
#undef DrawText
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "GlowHost.h"

#include <algorithm>
#include <cmath>

#include <gdiplus.h>

#ifndef CDIS_SELECTED
#define CDIS_SELECTED 0x0002U
#endif
#ifndef CDIS_DISABLED
#define CDIS_DISABLED 0x0004U
#endif
#ifndef CDIS_HOT
#define CDIS_HOT 0x0040U
#endif

#ifndef SPI_GETNONCLIENTMETRICS
#define SPI_GETNONCLIENTMETRICS 0x0029
#endif

namespace glowplay {
namespace {

D2D1_COLOR_F D2(COLORREF c, float a = 1.f)
{
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}

int ClampInt(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

} // namespace

ThemeSnap ThemeSnap::Light()
{
    ThemeSnap t;
    t.dark = false;
    t.window_bg = RGB(245, 244, 243);
    t.window_fg = RGB(50, 49, 48);
    t.accent = RGB(0, 120, 212);
    t.caption_pen = RGB(225, 223, 221);
    t.control_fg = RGB(32, 31, 30);
    return t;
}

ThemeSnap ThemeSnap::Dark()
{
    ThemeSnap t;
    t.dark = true;
    t.window_bg = RGB(35, 35, 35);
    t.window_fg = RGB(225, 225, 225);
    t.accent = RGB(64, 156, 230);
    t.caption_pen = RGB(60, 60, 60);
    t.control_fg = RGB(232, 232, 232);
    return t;
}

void GlowHost::Attach(GlowTile& t, GlowTip* tipWnd)
{
    tile = &t;
    tip = tipWnd;
    if (theme_.window_fg == 0 && theme_.window_bg == 0) {
        theme_ = ThemeSnap::Light();
    }
    SyncTip();
}

void GlowHost::ApplyDpi(int dpi)
{
    if (!tile) {
        return;
    }
    tile->ApplyDpi(dpi);
    SyncTip();
}

float GlowHost::MessageFontPx96(int extraPtStored)
{
    const int extra = ClampInt(extraPtStored, 0, 8);
    const int applied = extra - kFontSystemExtraPt;
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    int base = 12;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, 96)
        || SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        const int h = ncm.lfMessageFont.lfHeight;
        base = h < 0 ? -h : h;
        if (base < 8) {
            base = 12;
        }
    }
    const int px = base + MulDiv(applied, 96, 72);
    return static_cast<float>(ClampInt(px, 9, 22));
}

void GlowHost::ApplyFont(int extraPtStored, int dpi)
{
    fontExtraPt_ = ClampInt(extraPtStored, 0, 8);
    if (!tile) {
        return;
    }
    const int applied = fontExtraPt_ - kFontSystemExtraPt;
    tile->metrics.labelPxBase = MessageFontPx96(fontExtraPt_);
    tile->metrics.labelHBase = 18.f + static_cast<float>(applied);
    if (tile->metrics.labelHBase < 16.f) {
        tile->metrics.labelHBase = 16.f;
    }
    const int useDpi = dpi > 0 ? dpi : tile->Dpi();
    tile->ApplyDpi(useDpi);
    SyncTip();
}

void GlowHost::ApplyTheme(const ThemeSnap& pal)
{
    theme_ = pal;
    if (!tile) {
        return;
    }
    tile->colors.label.light = pal.dark ? RGB(32, 36, 42) : pal.window_fg;
    tile->colors.label.dark = pal.dark ? pal.window_fg : RGB(236, 240, 246);
    for (int i = 0; i < kToolBtnStateCount; ++i) {
        tile->at(static_cast<ToolBtnState>(i)).label = tile->colors.label;
    }
}

void GlowHost::ApplyTheme(bool dark, COLORREF accent, COLORREF windowFg, COLORREF windowBg, COLORREF captionPen)
{
    ThemeSnap t = dark ? ThemeSnap::Dark() : ThemeSnap::Light();
    t.dark = dark;
    t.accent = accent;
    t.window_fg = windowFg;
    t.window_bg = windowBg;
    if (captionPen != CLR_INVALID) {
        t.caption_pen = captionPen;
    }
    ApplyTheme(t);
}

GlowHost::Item GlowHost::FromToolbarDraw(UINT uItemState, bool checked, bool running, bool finish, bool failed)
{
    Item it;
    it.down = (uItemState & CDIS_SELECTED) != 0;
    it.enabled = (uItemState & CDIS_DISABLED) == 0;
    it.hot = (uItemState & CDIS_HOT) != 0;
    it.selected = checked;
    it.running = running;
    it.finish = finish;
    it.failed = failed;
    return it;
}

GlowTile::Draw GlowHost::FillDraw(const Item& item) const
{
    GlowTile::Draw d;
    d.state = DutyState(item.down, item.selected, item.running, item.finish, item.failed);
    d.hot = item.hot && item.enabled;
    d.enabled = item.enabled;
    d.light = !theme_.dark;
    if (item.finish && !item.failed) {
        d.accent = kFinishAccent;
    } else {
        d.accent = item.accent != 0 ? item.accent : theme_.accent;
    }
    d.pulse = item.running ? Pulse01(item.pulseOrigin) : 0.5f;
    d.progress = item.progress;
    d.caption = item.caption;
    d.tooltip = item.tooltip;
    d.iconStem = item.iconStem;
    d.labelMode = item.labelMode;
    d.paintCaption = item.paintCaption;
    d.glowOn = glowOn;
    d.shadowOn = shadowOn;
    d.iconGdi = iconGdi_;
    d.iconD2d = iconD2d_;
    return d;
}

void GlowHost::SetIconBind(IconGdi gdi, IconD2d d2d)
{
    iconGdi_ = gdi;
    iconD2d_ = d2d;
}

bool GlowHost::PaintItem(const Frame& f, const RECT& cell, const Item& item)
{
    return PaintItem(f, cell, FillDraw(item), item.dropdown);
}

bool GlowHost::PaintItem(const Frame& f, const RECT& cell, const GlowTile::Draw& d, bool dropdown)
{
    if (!tile) {
        return false;
    }
    bool ok = false;
    if (preferD2d && f.rt) {
        tile->PaintD2d(f.rt, f.factory, f.dwrite, f.labelTf, cell, d);
        if (dropdown) {
            PaintChevron(f.rt, cell, ChevronColor(d));
        }
        ok = true;
    } else if (f.hdc) {
        ok = tile->PaintGdi(f.hdc, cell, d);
        if (dropdown && f.hdc) {
            PaintChevron(f.hdc, cell, ChevronColor(d));
        }
    }
    return ok;
}

void GlowHost::PaintSeparator(HDC hdc, const RECT& slot, COLORREF bg) const
{
    if (!hdc) {
        return;
    }
    if (bg != CLR_INVALID) {
        if (HBRUSH br = CreateSolidBrush(bg)) {
            FillRect(hdc, &slot, br);
            DeleteObject(br);
        }
    }
    const COLORREF line = Hairline();
    HPEN pen = CreatePen(PS_SOLID, 1, line);
    if (!pen) {
        return;
    }
    const HGDIOBJ old = SelectObject(hdc, pen);
    const int h = slot.bottom - slot.top;
    const int inset = (std::max)(Strip::sepVertInsetMin, h / Strip::sepVertInsetDiv);
    const int x = slot.left + (slot.right - slot.left) / 2;
    MoveToEx(hdc, x, slot.top + inset, nullptr);
    LineTo(hdc, x, slot.bottom - inset);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

void GlowHost::PaintSeparator(ID2D1RenderTarget* rt, const RECT& slot) const
{
    if (!rt) {
        return;
    }
    ID2D1SolidColorBrush* br = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(D2(Hairline()), &br)) || !br) {
        return;
    }
    const int h = slot.bottom - slot.top;
    const int inset = (std::max)(Strip::sepVertInsetMin, h / Strip::sepVertInsetDiv);
    const float x = static_cast<float>(slot.left + slot.right) * 0.5f;
    rt->DrawLine(D2D1::Point2F(x, static_cast<float>(slot.top + inset)),
        D2D1::Point2F(x, static_cast<float>(slot.bottom - inset)), br, 1.f);
    br->Release();
}

void GlowHost::PaintChevron(HDC hdc, const RECT& cell, COLORREF color) const
{
    if (!hdc) {
        return;
    }
    const int cx = cell.right - ScalePx(5);
    const int cy = (cell.top + cell.bottom) / 2;
    const int hw = (std::max)(ScalePx(3), ScalePx(Strip::dropdownArrowW) / 4);
    const int hh = (std::max)(ScalePx(2), ScalePx(3));
    const POINT pts[3] = {{cx - hw, cy - 1}, {cx + hw, cy - 1}, {cx, cy + hh}};
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) {
        return;
    }
    const HGDIOBJ oldBrush = SelectObject(hdc, brush);
    const HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, pts, 3);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);
}

void GlowHost::PaintChevron(ID2D1RenderTarget* rt, const RECT& cell, COLORREF color) const
{
    if (!rt) {
        return;
    }
    ID2D1Factory* factory = nullptr;
    rt->GetFactory(&factory);
    if (!factory) {
        return;
    }
    ID2D1PathGeometry* geo = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    const int cx = cell.right - ScalePx(5);
    const int cy = (cell.top + cell.bottom) / 2;
    const int hw = (std::max)(ScalePx(3), ScalePx(Strip::dropdownArrowW) / 4);
    const int hh = (std::max)(ScalePx(2), ScalePx(3));
    if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) {
        factory->Release();
        return;
    }
    if (FAILED(geo->Open(&sink)) || !sink) {
        geo->Release();
        factory->Release();
        return;
    }
    sink->BeginFigure(D2D1::Point2F(static_cast<float>(cx - hw), static_cast<float>(cy - 1)),
        D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(static_cast<float>(cx + hw), static_cast<float>(cy - 1)));
    sink->AddLine(D2D1::Point2F(static_cast<float>(cx), static_cast<float>(cy + hh)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
    if (SUCCEEDED(rt->CreateSolidColorBrush(D2(color), &br)) && br) {
        rt->FillGeometry(geo, br);
        br->Release();
    }
    geo->Release();
    factory->Release();
}

int GlowHost::ScalePx(int logical96) const
{
    const int dpi = tile ? tile->Dpi() : 96;
    return MulDiv(logical96, dpi, 96);
}

int GlowHost::LargeIconPx() const
{
    return tile ? tile->metrics.iconPx : Strip::iconVisualMax;
}

int GlowHost::CompactIconPx() const
{
    return ScalePx(Strip::iconSmall);
}

int GlowHost::IconOnlyButtonW() const
{
    return ScalePx(Strip::iconOnlyButtonW);
}

int GlowHost::DropdownArrowW() const
{
    return ScalePx(Strip::dropdownArrowW);
}

COLORREF GlowHost::Hairline() const
{
    const int g = theme_.dark ? (Strip::darkHairline + 44) : Strip::lightHairline;
    return RGB(g, g, g);
}

COLORREF GlowHost::LabelColor() const
{
    return theme_.window_fg;
}

float GlowHost::Pulse01(ULONGLONG origin)
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG t0 = origin != 0 ? origin : now;
    const float t = static_cast<float>((now - t0) % static_cast<ULONGLONG>(Pulse::periodMs))
        / static_cast<float>(Pulse::periodMs);
    return 0.5f + 0.5f * std::sin(t * 6.2831853f);
}

COLORREF GlowHost::ChevronColor(const GlowTile::Draw& d) const
{
    if (!tile) {
        return theme_.window_fg;
    }
    const TileFx fx = tile->Resolve(d);
    return fx.labelRgb;
}

void GlowHost::SyncTip()
{
    if (tip && tile) {
        tip->SyncFrom(*tile);
    }
}

} // namespace glowplay
