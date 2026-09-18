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

#include "GlowTip.h"
#include "GlowTile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

#include <gdiplus.h>

namespace glowplay {
namespace {

constexpr UINT_PTR kTipDelayTimer = 0x54495031u; // 'TIP1'
constexpr wchar_t kTipClass[] = L"PmGlowTip";

BYTE ClampByte(float v)
{
    if (v < 0.f) {
        return 0;
    }
    if (v > 255.f) {
        return 255;
    }
    return static_cast<BYTE>(v);
}

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rc, float rad)
{
    const float rr = (std::max)(0.5f, (std::min)(rad, (std::min)(rc.Width, rc.Height) * 0.5f));
    const float d = rr * 2.f;
    path.AddArc(rc.X, rc.Y, d, d, 180.f, 90.f);
    path.AddArc(rc.GetRight() - d, rc.Y, d, d, 270.f, 90.f);
    path.AddArc(rc.GetRight() - d, rc.GetBottom() - d, d, d, 0.f, 90.f);
    path.AddArc(rc.X, rc.GetBottom() - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

Gdiplus::Color Gp(BYTE a, COLORREF c)
{
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

LRESULT CALLBACK TipWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterTipClass()
{
    static bool once = false;
    if (once) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TipWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTipClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    once = true;
}

SIZE MeasureTip(const wchar_t* face, float px, const wchar_t* text, int maxW)
{
    (void)face;
    SIZE sz{0, 0};
    if (!text || !text[0]) {
        return sz;
    }
    const Gdiplus::RectF bounds = GlowTile::MeasureUiTextGdi(text, px, static_cast<float>(maxW));
    sz.cx = (std::max)(1, static_cast<int>(std::ceil(bounds.Width)));
    sz.cy = (std::max)(1, static_cast<int>(std::ceil(bounds.Height)));
    return sz;
}

} // namespace

void GlowTip::SyncFrom(const GlowTile& tile)
{
    fontFace = GlowTile::UiFontFace();
    fontPx = tile.metrics.labelPx;
    radius = tile.metrics.radius;
}

void GlowTip::Attach(HWND owner)
{
    owner_ = owner;
}

void GlowTip::Detach()
{
    Hide();
    if (popup_) {
        DestroyWindow(popup_);
        popup_ = nullptr;
    }
    owner_ = nullptr;
}

void GlowTip::Hide()
{
    if (owner_) {
        KillTimer(owner_, kTipDelayTimer);
    }
    pendingId_ = -1;
    pending_.clear();
    visible_ = false;
    if (popup_) {
        ShowWindow(popup_, SW_HIDE);
    }
}

void GlowTip::EnsurePopup()
{
    if (popup_) {
        return;
    }
    RegisterTipClass();
    popup_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kTipClass,
        L"", WS_POPUP, 0, 0, 8, 8, owner_, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void GlowTip::Place(int tipW, int tipH, POINT& screen)
{
    RECT anchor = anchor_;
    if (owner_) {
        POINT tl{anchor.left, anchor.top};
        POINT br{anchor.right, anchor.bottom};
        ClientToScreen(owner_, &tl);
        ClientToScreen(owner_, &br);
        anchor = {tl.x, tl.y, br.x, br.y};
    }
    screen.x = (anchor.left + anchor.right - tipW) / 2;
    screen.y = anchor.bottom + gapPx;

    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        const RECT& wa = mi.rcWork;
        if (screen.y + tipH > wa.bottom - 4) {
            screen.y = anchor.top - gapPx - tipH;
        }
        if (screen.x < wa.left + 4) {
            screen.x = wa.left + 4;
        }
        if (screen.x + tipW > wa.right - 4) {
            screen.x = wa.right - 4 - tipW;
        }
        if (screen.y < wa.top + 4) {
            screen.y = wa.top + 4;
        }
    }
}

void GlowTip::PaintLayered(int w, int h)
{
    if (!popup_ || w < 4 || h < 4) {
        return;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    if (!mem) {
        return;
    }
    HBITMAP hb = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) {
        if (hb) {
            DeleteObject(hb);
        }
        DeleteDC(mem);
        return;
    }
    std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    HGDIOBJ old = SelectObject(mem, hb);
    {
        Gdiplus::Bitmap wrap(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&wrap);
        g.SetPageUnit(Gdiplus::UnitPixel);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

        const float shadow = 8.f;
        // Integer card so the 1px hairline sits on the fill (AA against the card, not air).
        const float cardX = std::floor(shadow) + 0.5f;
        const float cardY = std::floor(shadow * 0.55f) + 0.5f;
        const float cardW = std::floor(static_cast<float>(w) - shadow * 2.f);
        const float cardH = std::floor(static_cast<float>(h) - shadow * 1.55f);
        const Gdiplus::RectF card(cardX, cardY, cardW, cardH);
        for (int i = 4; i >= 1; --i) {
            const float t = static_cast<float>(i) / 4.f;
            const float ex = t * 5.f;
            const float ey = t * 4.f;
            Gdiplus::GraphicsPath sh;
            AddRoundRect(sh,
                Gdiplus::RectF(card.X - ex * 0.2f, card.Y + t * 3.f, card.Width + ex * 0.4f, card.Height + ey),
                radius + t * 2.f);
            Gdiplus::SolidBrush br(Gdiplus::Color(ClampByte(22.f * (1.f - t) * (1.f - t)), 0, 0, 0));
            g.FillPath(&br, &sh);
        }

        const COLORREF bg = light_ ? RGB(250, 251, 252) : RGB(32, 36, 42);
        const COLORREF fg = light_ ? RGB(32, 36, 42) : RGB(236, 240, 246);
        const COLORREF hair = light_ ? RGB(198, 202, 208) : RGB(78, 84, 92);
        Gdiplus::GraphicsPath body;
        AddRoundRect(body, card, radius);
        Gdiplus::SolidBrush fill(Gp(252, bg));
        g.FillPath(&fill, &body);

        Gdiplus::RectF rim = card;
        rim.Inflate(-0.5f, -0.5f);
        Gdiplus::GraphicsPath rimPath;
        AddRoundRect(rimPath, rim, (std::max)(0.5f, radius - 0.5f));
        Gdiplus::Pen hairPen(Gp(light_ ? 180 : 210, hair), 1.f);
        hairPen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&hairPen, &rimPath);
        Gdiplus::Pen accentPen(Gp(light_ ? 56 : 90, accent_), 1.f);
        accentPen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&accentPen, &rimPath);

        const Gdiplus::RectF tr(card.X + padX, card.Y + padY, card.Width - padX * 2.f, card.Height - padY * 2.f);
        GlowTile::FillUiTextGdi(g, text_.c_str(), fontPx, tr, Gp(240, fg), Gdiplus::StringAlignmentCenter,
            Gdiplus::StringAlignmentCenter, false);
    }

    POINT src{0, 0};
    SIZE size{w, h};
    POINT dst{};
    Place(w, h, dst);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(popup_, nullptr, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(hb);
    DeleteDC(mem);
}

void GlowTip::Present()
{
    if (!enabled || text_.empty() || !owner_) {
        Hide();
        return;
    }
    EnsurePopup();
    if (!popup_) {
        return;
    }
    const SIZE textSz = MeasureTip(fontFace, fontPx, text_.c_str(), maxW);
    const int shadow = 16;
    const int w = textSz.cx + static_cast<int>(padX * 2.f) + shadow;
    const int h = textSz.cy + static_cast<int>(padY * 2.f) + shadow;
    PaintLayered(w, h);
    SetWindowPos(popup_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
}

void GlowTip::Track(int id, const RECT& anchorClient, const wchar_t* text, bool light, COLORREF accent)
{
    if (!enabled || id < 0 || !text || !text[0]) {
        Hide();
        id_ = -1;
        return;
    }
    light_ = light;
    accent_ = accent;
    anchor_ = anchorClient;
    const bool same = (id == id_ && visible_);
    if (same) {
        if (text_ != text) {
            text_ = text;
            Present();
        } else {
            POINT dst{};
            const SIZE textSz = MeasureTip(fontFace, fontPx, text_.c_str(), maxW);
            const int shadow = 16;
            Place(textSz.cx + static_cast<int>(padX * 2.f) + shadow,
                textSz.cy + static_cast<int>(padY * 2.f) + shadow, dst);
            if (popup_) {
                SetWindowPos(popup_, HWND_TOP, dst.x, dst.y, 0, 0,
                    SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
            }
        }
        return;
    }
    if (id == pendingId_ && pending_ == text) {
        return;
    }
    id_ = -1;
    visible_ = false;
    if (popup_) {
        ShowWindow(popup_, SW_HIDE);
    }
    pendingId_ = id;
    pending_ = text;
    text_ = text;
    if (owner_) {
        SetTimer(owner_, kTipDelayTimer, static_cast<UINT>((std::max)(1, delayMs)), nullptr);
    }
}

void GlowTip::SetText(const wchar_t* text)
{
    if (!text || !text[0]) {
        return;
    }
    if (pendingId_ >= 0) {
        pending_ = text;
    }
    if (!visible_ || text_ == text) {
        return;
    }
    text_ = text;
    Present();
}

bool GlowTip::OnTimer(HWND owner, WPARAM timerId)
{
    if (timerId != kTipDelayTimer) {
        return false;
    }
    KillTimer(owner, kTipDelayTimer);
    if (!enabled || pendingId_ < 0 || pending_.empty()) {
        return false;
    }
    id_ = pendingId_;
    text_ = pending_;
    pendingId_ = -1;
    Present();
    return false;
}

} // namespace glowplay
