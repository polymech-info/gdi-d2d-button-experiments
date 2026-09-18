// Glow playground — GDI+ bloom on a 32bpp PARGB DIB, then AlphaBlend onto the host DC.
// Cell metrics copy RibbonNextTuning (OwnRibbonLayout.cpp): 64x58 labeled, 28 px icon, r=8.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
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

#include <algorithm>
#include <cmath>
#include <cwchar>

#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>

#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#include "GlowPlay.h"
#include "GlowHost.h"
#include "GlowTip.h"
#include "SvgIcon.h"
#include "../SandboxLook.hpp"

namespace glowplay {
namespace {

GlowTile g_tile;
GlowTip g_tip;
GlowHost g_glow;
int g_fontExtra = GlowHost::kFontSystemExtraPt; // appearance.font_size_extra_pt (2 = system)

HWND g_host = nullptr;
int g_hover = -1;
int g_pressing = -1;
int g_liveDuty = 0; // 0 normal, 1 running, 2 finish, 3 failed — hover/press stay transient
ULONGLONG g_pulseOrigin = 0;
ULONGLONG g_progressOrigin = 0;
constexpr int kProgressMs = 4800;
bool g_glowOn = true;
int g_scale = 2; // 1 = product ribbon, 2 = playground 2×
LabelMode g_labelMode = LabelMode::Inside;

ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dwrite = nullptr;
IDWriteTextFormat* g_tfLabel = nullptr;
float g_tfLabelPx = 0.f;
IDWriteTextFormat* g_tfNote = nullptr;
ID2D1DCRenderTarget* g_rt = nullptr;
int g_rtW = 0;
int g_rtH = 0;

template <typename T>
void ReleaseCom(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

constexpr int kPad = 14;
constexpr int kWellRadius = 10;
constexpr int kChipH = 22;
constexpr int kChipGap = 8;

// Labeled ribbon command — 2x RibbonNextTuning (playground only).
constexpr int kRibbonCellW = 128;
constexpr int kRibbonCellH = 116;
constexpr int kRibbonIcon = 56;
constexpr int kRibbonGap = 20;
constexpr int kRibbonGlow = 16;
constexpr float kRibbonRadius = 16.f;
constexpr float kLabelPx = 16.f;

// Tab-bar / mini-group — TabBarChromeLayout + icon_only_button_w.
constexpr int kCompactCellW = 36;
constexpr int kCompactCellH = 28;
constexpr int kCompactIcon = 20;
constexpr int kCompactGap = 10;
constexpr int kCompactGlow = 8;
constexpr float kCompactRadius = 5.f;

constexpr int kSampleCount = 6;
constexpr int kLongCount = 5;
constexpr int kToggleCount = 5;     // Light Glass Shadow Glow Tip
constexpr int kLabelModeCount = 3;  // Off In Below
constexpr int kScaleCount = 2;      // 1× 2×
constexpr int kChipCount = kToggleCount + kLabelModeCount + kScaleCount;

enum Hit : int {
    HitNone = -1,
    HitLive = 0,
    HitLiveD2d = 8,
    HitSample0 = 1,
    HitChip0 = 10,
    HitScale0 = 20,
    HitLabel0 = 30,
    HitLong0 = 40,
    HitD2d0 = 50,
    HitLongD2d0 = 60,
};

struct Sample {
    ToolBtnState pin;
    COLORREF accent;
    const wchar_t* label;
    const wchar_t* icon;
    const wchar_t* tip;
};

const Sample kSamples[kSampleCount] = {
    {ToolBtnState::Hover, RGB(14, 165, 233), L"Preview", L"photo", L"Open the current file in the viewer"},
    {ToolBtnState::Pressing, RGB(0, 162, 255), L"Region", L"screenshot", L"Capture a rectangular region of the screen"},
    {ToolBtnState::Pressed, RGB(34, 197, 94), L"Console", L"player-play", L"Show the command console"},
    {ToolBtnState::Running, RGB(6, 182, 212), L"Chat", L"message-circle", L"Ask the assistant about this file"},
    {ToolBtnState::Finish, kFinishAccent, L"Done", L"circle-check", L"Last export finished"},
    {ToolBtnState::Failed, RGB(220, 68, 68), L"Failed", L"x", L"Last command failed — click to retry"},
};

const Sample kLongs[kLongCount] = {
    {ToolBtnState::Idle, RGB(99, 102, 241), L"Voice Recorder", L"microphone",
        L"Record from the default microphone"},
    {ToolBtnState::Hover, RGB(14, 165, 233), L"Screen Capture", L"camera", L"Capture the full display"},
    {ToolBtnState::Pressed, RGB(168, 85, 247), L"Batch Convert", L"transform", L"Convert the selected files"},
    {ToolBtnState::Running, RGB(6, 182, 212), L"Background Export Queue", L"player-play",
        L"Jobs stay queued until the encoder is free"},
    {ToolBtnState::Failed, RGB(220, 68, 68), L"Connection Timed Out", L"x",
        L"The last export lost its connection"},
};

constexpr COLORREF kLiveAccent = RGB(0, 162, 255);

struct Layout {
    RECT well{};
    RECT live{};
    RECT liveD2d{};
    RECT samples[kSampleCount]{};
    RECT d2d[kSampleCount]{};
    RECT longs[kLongCount]{};
    RECT longsD2d[kLongCount]{};
    RECT chips[kChipCount]{};
    RECT labelGroup{};
    RECT scaleGroup{};
    bool showLive = false;
    bool showLiveD2d = false;
    bool showSamples = false;
    bool showD2d = false;
    bool showLongs = false;
    bool showLongsD2d = false;
    bool showChips = false;
};

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

// Stroke lives at the corners and dies at the mid-edge (glass rim, not a closed box).
void DrawCornerFadeFrameGdi(Gdiplus::Graphics& g, const Gdiplus::RectF& cell, float radius, BYTE ar, BYTE ag,
    BYTE ab, BYTE peakA, float strokeW)
{
    if (peakA == 0 || cell.Width < 8.f || cell.Height < 8.f) {
        return;
    }
    const float x0 = cell.X;
    const float y0 = cell.Y;
    const float x1 = cell.GetRight();
    const float y1 = cell.GetBottom();
    const float r = (std::max)(1.f, (std::min)(radius, (std::min)(cell.Width, cell.Height) * 0.5f));
    const float midX = (x0 + x1) * 0.5f;
    const float midY = (y0 + y1) * 0.5f;
    // Short taper from the corner — a full mid-edge fade reads as smear.
    const float runX = (std::min)(18.f, (std::max)(6.f, (midX - (x0 + r)) * 0.38f));
    const float runY = (std::min)(18.f, (std::max)(6.f, (midY - (y0 + r)) * 0.38f));
    const Gdiplus::Color on(peakA, ar, ag, ab);
    const Gdiplus::Color off(0, ar, ag, ab);

    auto fade = [&](float ax, float ay, float bx, float by) {
        if (std::fabs(ax - bx) + std::fabs(ay - by) < 1.5f) {
            return;
        }
        Gdiplus::LinearGradientBrush br(Gdiplus::PointF(ax, ay), Gdiplus::PointF(bx, by), on, off);
        br.SetWrapMode(Gdiplus::WrapModeClamp);
        Gdiplus::Pen pen(&br, strokeW);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapFlat);
        g.DrawLine(&pen, ax, ay, bx, by);
    };

    fade(x0 + r, y0, x0 + r + runX, y0);
    fade(x1 - r, y0, x1 - r - runX, y0);
    fade(x0 + r, y1, x0 + r + runX, y1);
    fade(x1 - r, y1, x1 - r - runX, y1);
    fade(x0, y0 + r, x0, y0 + r + runY);
    fade(x0, y1 - r, x0, y1 - r - runY);
    fade(x1, y0 + r, x1, y0 + r + runY);
    fade(x1, y1 - r, x1, y1 - r - runY);

    Gdiplus::Pen corner(on, strokeW);
    corner.SetLineJoin(Gdiplus::LineJoinRound);
    corner.SetStartCap(Gdiplus::LineCapRound);
    corner.SetEndCap(Gdiplus::LineCapRound);
    const float d = r * 2.f;
    g.DrawArc(&corner, x0, y0, d, d, 180.f, 90.f);
    g.DrawArc(&corner, x1 - d, y0, d, d, 270.f, 90.f);
    g.DrawArc(&corner, x1 - d, y1 - d, d, d, 0.f, 90.f);
    g.DrawArc(&corner, x0, y1 - d, d, d, 90.f, 90.f);
}

float Pulse01()
{
    if (g_pulseOrigin == 0) {
        g_pulseOrigin = GetTickCount64();
    }
    const float t = static_cast<float>((GetTickCount64() - g_pulseOrigin) % kPulsePeriodMs)
        / static_cast<float>(kPulsePeriodMs);
    return 0.5f + 0.5f * std::sin(t * 6.2831853f);
}

COLORREF LiveAccent()
{
    if (g_liveDuty == 3) {
        return RGB(220, 68, 68);
    }
    if (g_liveDuty == 2) {
        return kFinishAccent;
    }
    if (g_liveDuty == 1) {
        return RGB(6, 182, 212);
    }
    return kLiveAccent;
}

const wchar_t* LiveCaption(ToolBtnState state)
{
    switch (state) {
    case ToolBtnState::Running:
        return L"Running";
    case ToolBtnState::Finish:
        return L"Done";
    case ToolBtnState::Failed:
        return L"Failed";
    case ToolBtnState::Pressing:
        return L"Press";
    case ToolBtnState::Hover:
        return L"Hover";
    default:
        return L"Region";
    }
}

ToolBtnState LiveDuty()
{
    if (g_liveDuty == 3) {
        return ToolBtnState::Failed;
    }
    if (g_liveDuty == 2) {
        return ToolBtnState::Finish;
    }
    if (g_liveDuty == 1) {
        return ToolBtnState::Running;
    }
    return ToolBtnState::Idle;
}

ToolBtnState OverState(ToolBtnState duty, bool hot, bool down)
{
    if (down) {
        return ToolBtnState::Pressing;
    }
    if (hot) {
        return ToolBtnState::Hover;
    }
    return duty;
}

void SetLiveDuty(int duty)
{
    g_liveDuty = duty;
    g_progressOrigin = (duty == 1) ? GetTickCount64() : 0;
}

void CycleLiveDuty()
{
    if (g_liveDuty >= 3) {
        SetLiveDuty(0);
        return;
    }
    SetLiveDuty(g_liveDuty + 1);
}

float LiveProgress()
{
    if (g_liveDuty != 1) {
        return -1.f;
    }
    if (g_progressOrigin == 0) {
        g_progressOrigin = GetTickCount64();
    }
    const float t = static_cast<float>(GetTickCount64() - g_progressOrigin) / static_cast<float>(kProgressMs);
    if (t >= 1.f) {
        return 100.f;
    }
    if (t <= 0.f) {
        return 0.f;
    }
    return t * 100.f;
}

void AdvanceLiveRun()
{
    if (g_liveDuty == 1 && LiveProgress() >= 100.f) {
        SetLiveDuty(2);
    }
}

void SyncTipType()
{
    g_tip.SyncFrom(g_tile);
}

void SyncHostTheme()
{
    g_glow.ApplyTheme(Sandbox().light ? ThemeSnap::Light() : ThemeSnap::Dark());
}

void SyncHostFlags()
{
    g_glow.glowOn = g_glowOn;
    g_glow.shadowOn = Sandbox().shadow;
}

void ApplyPlayScale(int scale)
{
    const int s = scale >= 2 ? 2 : 1;
    if (g_scale == s) {
        return;
    }
    g_scale = s;
    g_tile.ApplyScale(g_scale);
    g_glow.ApplyFont(g_fontExtra, g_tile.Dpi());
    SyncTipType();
    ReleaseCom(g_tfLabel);
}

void NudgeLabelPx(float delta)
{
    const int next = g_fontExtra + (delta < 0.f ? -1 : 1);
    if (next < 0 || next > 8 || next == g_fontExtra) {
        return;
    }
    g_fontExtra = next;
    g_glow.ApplyFont(g_fontExtra, g_tile.Dpi());
    SyncTipType();
    ReleaseCom(g_tfLabel);
}

int CaptionOffset()
{
    return g_tile.metrics.glowPad + static_cast<int>(g_tile.metrics.labelBelowGap);
}

int RowNoteLift()
{
    return 22;
}

int CaptionBand()
{
    const int header = RowNoteLift();
    if (g_labelMode == LabelMode::Below) {
        return CaptionOffset() + 18 + 16 + header;
    }
    return g_tile.metrics.glowPad + 10 + header;
}

int ChipWidth(int i)
{
    if (i >= kToggleCount + kLabelModeCount) {
        return 36;
    }
    if (i == 4) {
        return 40;
    }
    if (i >= kToggleCount) {
        if (i == kToggleCount) {
            return 40;
        }
        if (i == kToggleCount + 1) {
            return 36;
        }
        return 50;
    }
    return 58;
}

int ChipGapAfter(int i)
{
    if (i + 1 == kToggleCount || i + 1 == kToggleCount + kLabelModeCount) {
        return 14;
    }
    return kChipGap;
}

void CycleLabelMode()
{
    switch (g_labelMode) {
    case LabelMode::None:
        g_labelMode = LabelMode::Inside;
        break;
    case LabelMode::Inside:
        g_labelMode = LabelMode::Below;
        break;
    default:
        g_labelMode = LabelMode::None;
        break;
    }
}

const wchar_t* LiveIconStem()
{
    if (g_liveDuty == 3) {
        return L"x";
    }
    if (g_liveDuty == 2) {
        return L"circle-check";
    }
    if (g_liveDuty == 1) {
        return L"message-circle";
    }
    return L"screenshot";
}

void DrawFakeIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& box, ToolBtnState state, const Gdiplus::Color& color);
void DrawFakeIconD2d(ID2D1RenderTarget* rt, ID2D1Factory* factory, const D2D1_RECT_F& box, ToolBtnState state,
    const D2D1_COLOR_F& color);
void DrawRibbonIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& box, const wchar_t* stem, ToolBtnState state,
    const Gdiplus::Color& color);
void DrawRibbonIconD2d(ID2D1RenderTarget* rt, ID2D1Factory* factory, const D2D1_RECT_F& box, const wchar_t* stem,
    ToolBtnState state, const D2D1_COLOR_F& color);
void DrawRunningArcD2d(ID2D1RenderTarget* rt, ID2D1Factory* factory, const D2D1_RECT_F& box,
    ID2D1SolidColorBrush* br);

GlowTile::Draw MakeDraw(ToolBtnState st, COLORREF accent, float pulse, const wchar_t* cap, bool light, bool hot,
    const wchar_t* stem, float progress = -1.f, const wchar_t* tip = nullptr)
{
    GlowTile::Draw d;
    d.state = st;
    d.accent = accent;
    d.pulse = pulse;
    d.caption = cap;
    d.tooltip = tip;
    d.labelMode = g_labelMode;
    d.iconStem = stem;
    d.light = light;
    d.hot = hot;
    d.progress = progress;
    d.glowOn = g_glowOn;
    d.shadowOn = Sandbox().shadow;
    d.iconGdi = DrawRibbonIcon;
    d.iconD2d = DrawRibbonIconD2d;
    return d;
}

const wchar_t* LiveTooltip()
{
    static wchar_t buf[96];
    if (g_liveDuty == 3) {
        return L"Last run failed";
    }
    if (g_liveDuty == 2) {
        return L"Export finished";
    }
    if (g_liveDuty == 1) {
        (void)swprintf_s(buf, L"Export running — %.0f%%", LiveProgress());
        return buf;
    }
    return L"Capture a rectangular region of the screen";
}

void DrawRibbonIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& box, const wchar_t* stem, ToolBtnState state,
    const Gdiplus::Color& color)
{
    if (SvgIconBlitGdi(g, box, stem, color.GetR(), color.GetG(), color.GetB(), color.GetA())) {
        return;
    }
    DrawFakeIcon(g, box, state, color);
}

void DrawRibbonIconD2d(ID2D1RenderTarget* rt, ID2D1Factory* factory, const D2D1_RECT_F& box, const wchar_t* stem,
    ToolBtnState state, const D2D1_COLOR_F& color)
{
    if (SvgIconBlitD2d(rt, box, stem, static_cast<BYTE>(color.r * 255.f + 0.5f),
            static_cast<BYTE>(color.g * 255.f + 0.5f), static_cast<BYTE>(color.b * 255.f + 0.5f), color.a)) {
        return;
    }
    DrawFakeIconD2d(rt, factory, box, state, color);
}

void BindPlayIcons()
{
    g_glow.SetIconBind(DrawRibbonIcon, DrawRibbonIconD2d);
}

void DrawFakeIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& box, ToolBtnState state, const Gdiplus::Color& color)
{
    const float sw = (std::max)(1.6f, 1.7f * (box.Width / 28.f));
    Gdiplus::Pen pen(color, sw);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::SolidBrush brush(color);

    const float x = box.X;
    const float y = box.Y;
    const float w = box.Width;
    const float h = box.Height;

    switch (state) {
    case ToolBtnState::Idle: {
        Gdiplus::GraphicsPath frame;
        AddRoundRect(frame, box, 3.f);
        g.DrawPath(&pen, &frame);
        g.DrawLine(&pen, x + w * 0.38f, y + 1.5f, x + w * 0.38f, y + h - 1.5f);
        break;
    }
    case ToolBtnState::Hover: {
        Gdiplus::GraphicsPath frame;
        AddRoundRect(frame, box, 3.f);
        g.DrawPath(&pen, &frame);
        Gdiplus::PointF mtn[3] = {
            {x + 2.f, y + h - 2.f},
            {x + w * 0.42f, y + h * 0.42f},
            {x + w - 2.f, y + h - 2.f},
        };
        g.DrawLines(&pen, mtn, 3);
        g.FillEllipse(&brush, x + w * 0.62f, y + 3.f, 5.f, 5.f);
        break;
    }
    case ToolBtnState::Pressing: {
        Gdiplus::Pen dash(color, (std::max)(1.5f, sw * 0.94f));
        dash.SetDashStyle(Gdiplus::DashStyleDash);
        Gdiplus::GraphicsPath frame;
        AddRoundRect(frame, Gdiplus::RectF(x + 1.f, y + 1.f, w - 2.f, h - 2.f), 4.f);
        g.DrawPath(&dash, &frame);
        break;
    }
    case ToolBtnState::Pressed: {
        Gdiplus::PointF tri[3] = {
            {x + w * 0.28f, y + 2.f},
            {x + w * 0.28f, y + h - 2.f},
            {x + w * 0.86f, y + h * 0.5f},
        };
        g.FillPolygon(&brush, tri, 3);
        break;
    }
    case ToolBtnState::Running: {
        Gdiplus::RectF arc(x + 1.5f, y + 1.5f, w - 3.f, h - 3.f);
        g.DrawArc(&pen, arc, 210.f, 240.f);
        break;
    }
    case ToolBtnState::Finish: {
        g.DrawLine(&pen, x + 3.f, y + h * 0.52f, x + w * 0.42f, y + h - 4.f);
        g.DrawLine(&pen, x + w * 0.42f, y + h - 4.f, x + w - 3.f, y + 4.f);
        break;
    }
    case ToolBtnState::Failed: {
        g.DrawLine(&pen, x + 3.f, y + 3.f, x + w - 3.f, y + h - 3.f);
        g.DrawLine(&pen, x + w - 3.f, y + 3.f, x + 3.f, y + h - 3.f);
        break;
    }
    }
}



void DrawFakeIconD2d(ID2D1RenderTarget* rt, ID2D1Factory* factory, const D2D1_RECT_F& box, ToolBtnState state,
    const D2D1_COLOR_F& color)
{
    ID2D1SolidColorBrush* br = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(color, &br)) || !br) {
        return;
    }
    const float x = box.left;
    const float y = box.top;
    const float w = box.right - box.left;
    const float h = box.bottom - box.top;
    const float sw = (std::max)(1.6f, 1.7f * (w / 28.f));
    switch (state) {
    case ToolBtnState::Idle:
        rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3.f, 3.f), br, sw);
        rt->DrawLine(D2D1::Point2F(x + w * 0.38f, y + 1.5f), D2D1::Point2F(x + w * 0.38f, y + h - 1.5f), br, sw);
        break;
    case ToolBtnState::Hover:
        rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3.f, 3.f), br, sw);
        rt->DrawLine(D2D1::Point2F(x + 2.f, y + h - 2.f), D2D1::Point2F(x + w * 0.42f, y + h * 0.42f), br, sw);
        rt->DrawLine(D2D1::Point2F(x + w * 0.42f, y + h * 0.42f), D2D1::Point2F(x + w - 2.f, y + h - 2.f), br, sw);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + w * 0.62f + 2.5f, y + 5.5f), 2.5f, 2.5f), br);
        break;
    case ToolBtnState::Pressing: {
        ID2D1StrokeStyle* dash = nullptr;
        D2D1_STROKE_STYLE_PROPERTIES sp{};
        sp.dashStyle = D2D1_DASH_STYLE_DASH;
        if (factory && SUCCEEDED(factory->CreateStrokeStyle(sp, nullptr, 0, &dash))) {
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(x + 1.f, y + 1.f, box.right - 1.f, box.bottom - 1.f), 4.f, 4.f), br,
                sw, dash);
            dash->Release();
        }
        break;
    }
    case ToolBtnState::Pressed: {
        ID2D1PathGeometry* geo = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (factory && SUCCEEDED(factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(x + w * 0.28f, y + 2.f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(x + w * 0.28f, y + h - 2.f));
            sink->AddLine(D2D1::Point2F(x + w * 0.86f, y + h * 0.5f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            rt->FillGeometry(geo, br);
        }
        if (geo) {
            geo->Release();
        }
        break;
    }
    case ToolBtnState::Running: {
        DrawRunningArcD2d(rt, factory, box, br);
        break;
    }
    case ToolBtnState::Finish:
        rt->DrawLine(D2D1::Point2F(x + 3.f, y + h * 0.52f), D2D1::Point2F(x + w * 0.42f, y + h - 4.f), br, sw);
        rt->DrawLine(D2D1::Point2F(x + w * 0.42f, y + h - 4.f), D2D1::Point2F(x + w - 3.f, y + 4.f), br, sw);
        break;
    case ToolBtnState::Failed:
        rt->DrawLine(D2D1::Point2F(x + 3.f, y + 3.f), D2D1::Point2F(box.right - 3.f, box.bottom - 3.f), br, sw);
        rt->DrawLine(D2D1::Point2F(box.right - 3.f, y + 3.f), D2D1::Point2F(x + 3.f, box.bottom - 3.f), br, sw);
        break;
    }
    br->Release();
}

void DrawRunningArcD2d(ID2D1RenderTarget* rt, ID2D1Factory* factory, const D2D1_RECT_F& box,
    ID2D1SolidColorBrush* br)
{
    ID2D1PathGeometry* geo = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (!factory || FAILED(factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) {
        if (geo) {
            geo->Release();
        }
        return;
    }
    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const float rx = (box.right - box.left) * 0.5f - 1.5f;
    const float ry = (box.bottom - box.top) * 0.5f - 1.5f;
    const float a0 = 210.f * 3.14159265f / 180.f;
    const float a1 = (210.f + 240.f) * 3.14159265f / 180.f;
    sink->BeginFigure(D2D1::Point2F(cx + rx * std::cos(a0), cy + ry * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + rx * std::cos(a1), cy + ry * std::sin(a1)),
        D2D1::SizeF(rx, ry), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    const float sw = (std::max)(1.6f, 1.7f * ((box.right - box.left) / 28.f));
    rt->DrawGeometry(geo, br, sw);
    geo->Release();
}

GlowHost::Frame D2dFrame(HDC hdc)
{
    GlowHost::Frame f;
    f.hdc = hdc;
    f.rt = g_rt;
    f.factory = g_d2d;
    f.dwrite = g_dwrite;
    f.labelTf = g_tfLabel;
    return f;
}

void PaintD2dButton(ID2D1RenderTarget* rt, const RECT& cell, ToolBtnState state, COLORREF accent, float pulse,
    const wchar_t* caption, bool light, bool hot, const wchar_t* stem, float progress = -1.f,
    const wchar_t* tip = nullptr)
{
    (void)rt;
    g_glow.PaintItem(D2dFrame(nullptr), cell,
        MakeDraw(state, accent, pulse, caption, light, hot, stem, progress, tip));
}

bool EnsureD2d(int w, int h)
{
    if (w < 8 || h < 8) {
        return false;
    }
    if (!g_d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) {
        return false;
    }
    if (!g_dwrite) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&g_dwrite)))) {
            return false;
        }
    }
    const float wantPx = g_tile.metrics.labelPx;
    if (g_tfLabel && std::fabs(g_tfLabelPx - wantPx) > 0.01f) {
        ReleaseCom(g_tfLabel);
        g_tfLabelPx = 0.f;
    }
    if (!g_tfLabel) {
        // RT is 96 DPI so DIP == GDI+ UnitPixel.
        if (FAILED(g_dwrite->CreateTextFormat(GlowTile::UiFontFace(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, wantPx, L"en-us", &g_tfLabel))
            && FAILED(g_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, wantPx, L"en-us", &g_tfLabel))) {
            return false;
        }
        g_tfLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_tfLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_tfLabel->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        {
            IDWriteInlineObject* ellipsis = nullptr;
            if (SUCCEEDED(g_dwrite->CreateEllipsisTrimmingSign(g_tfLabel, &ellipsis)) && ellipsis) {
                DWRITE_TRIMMING trim{};
                trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                g_tfLabel->SetTrimming(&trim, ellipsis);
                ellipsis->Release();
            }
        }
        g_tfLabelPx = wantPx;
    }
    if (!g_tfNote) {
        if (FAILED(g_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.f, L"en-us", &g_tfNote))) {
            return false;
        }
        g_tfNote->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_tfNote->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (g_rt && (g_rtW != w || g_rtH != h)) {
        ReleaseCom(g_rt);
    }
    if (!g_rt) {
        D2D1_RENDER_TARGET_PROPERTIES rtp{};
        rtp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        rtp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rtp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        rtp.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;
        rtp.dpiX = 96.f;
        rtp.dpiY = 96.f;
        if (FAILED(g_d2d->CreateDCRenderTarget(&rtp, &g_rt)) || !g_rt) {
            return false;
        }
        g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        g_rtW = w;
        g_rtH = h;
    }
    return true;
}

void ReleaseD2d()
{
    GlowTile::FlushPaintCaches();
    ReleaseCom(g_rt);
    ReleaseCom(g_tfNote);
    ReleaseCom(g_tfLabel);
    g_tfLabelPx = 0.f;
    ReleaseCom(g_dwrite);
    ReleaseCom(g_d2d);
    g_rtW = 0;
    g_rtH = 0;
}

void PaintD2dRow(HDC hdc, int windowW, int windowH, const Layout& L, float pulse, bool light)
{
    if ((!L.showD2d && !L.showLiveD2d) || !EnsureD2d(windowW, windowH) || !g_rt) {
        return;
    }
    RECT bind{0, 0, windowW, windowH};
    if (FAILED(g_rt->BindDC(hdc, &bind))) {
        return;
    }
    g_rt->BeginDraw();
    if (L.showLiveD2d) {
        const bool hot = (g_hover == HitLiveD2d);
        const bool down = (g_pressing == HitLiveD2d);
        const ToolBtnState duty = LiveDuty();
        const ToolBtnState st = OverState(duty, hot, down);
        PaintD2dButton(g_rt, L.liveD2d, st, LiveAccent(), pulse, LiveCaption(duty), light, hot, LiveIconStem(),
            LiveProgress(), LiveTooltip());
        if (g_tfNote) {
            ID2D1SolidColorBrush* note = nullptr;
            const D2D1_COLOR_F nc = light ? D2D1::ColorF(90 / 255.f, 98 / 255.f, 112 / 255.f, 170 / 255.f)
                                          : D2D1::ColorF(132 / 255.f, 148 / 255.f, 168 / 255.f, 150 / 255.f);
            if (SUCCEEDED(g_rt->CreateSolidColorBrush(nc, &note))) {
                const D2D1_RECT_F nr = D2D1::RectF(static_cast<float>(L.liveD2d.left),
                    static_cast<float>(L.liveD2d.top) - static_cast<float>(RowNoteLift()),
                    static_cast<float>(L.liveD2d.right), static_cast<float>(L.liveD2d.top) - 4.f);
                const wchar_t* msg = L"D2D live";
                g_rt->DrawText(msg, static_cast<UINT32>(wcslen(msg)), g_tfNote, nr, note);
                note->Release();
            }
        }
    }
    if (L.showD2d && g_tfNote) {
        ID2D1SolidColorBrush* note = nullptr;
        const D2D1_COLOR_F nc = light ? D2D1::ColorF(90 / 255.f, 98 / 255.f, 112 / 255.f, 170 / 255.f)
                                      : D2D1::ColorF(132 / 255.f, 148 / 255.f, 168 / 255.f, 150 / 255.f);
        if (SUCCEEDED(g_rt->CreateSolidColorBrush(nc, &note))) {
            const D2D1_RECT_F nr = D2D1::RectF(static_cast<float>(L.d2d[0].left),
                static_cast<float>(L.d2d[0].top) - static_cast<float>(RowNoteLift()),
                static_cast<float>(L.d2d[0].left) + 420.f, static_cast<float>(L.d2d[0].top) - 4.f);
            const wchar_t* msg = L"D2D + DirectWrite  (default painter)";
            g_rt->DrawText(msg, static_cast<UINT32>(wcslen(msg)), g_tfNote, nr, note);
            note->Release();
        }
    }
    if (L.showD2d) {
        for (int i = 0; i < kSampleCount; ++i) {
            const int hit = HitD2d0 + i;
            const bool hot = (g_hover == hit);
            const bool down = (g_pressing == hit);
            PaintD2dButton(g_rt, L.d2d[i], OverState(kSamples[i].pin, hot, down), kSamples[i].accent, pulse,
                kSamples[i].label, light, hot, kSamples[i].icon, -1.f, kSamples[i].tip);
        }
    }
    if (L.showLongsD2d) {
        if (g_tfNote) {
            ID2D1SolidColorBrush* note = nullptr;
            const D2D1_COLOR_F nc = light ? D2D1::ColorF(90 / 255.f, 98 / 255.f, 112 / 255.f, 170 / 255.f)
                                          : D2D1::ColorF(132 / 255.f, 148 / 255.f, 168 / 255.f, 150 / 255.f);
            if (SUCCEEDED(g_rt->CreateSolidColorBrush(nc, &note))) {
                const D2D1_RECT_F nr = D2D1::RectF(static_cast<float>(L.longsD2d[0].left),
                    static_cast<float>(L.longsD2d[0].top) - static_cast<float>(RowNoteLift()),
                    static_cast<float>(L.longsD2d[0].left) + 420.f, static_cast<float>(L.longsD2d[0].top) - 4.f);
                const wchar_t* msg = L"D2D  long labels  (ellipsis)";
                g_rt->DrawText(msg, static_cast<UINT32>(wcslen(msg)), g_tfNote, nr, note);
                note->Release();
            }
        }
        for (int i = 0; i < kLongCount; ++i) {
            const int hit = HitLongD2d0 + i;
            const bool hot = (g_hover == hit);
            const bool down = (g_pressing == hit);
            PaintD2dButton(g_rt, L.longsD2d[i], OverState(kLongs[i].pin, hot, down), kLongs[i].accent, pulse,
                kLongs[i].label, light, hot, kLongs[i].icon, -1.f, kLongs[i].tip);
        }
    }
    (void)g_rt->EndDraw();
}

void EnsurePulseTimer(HWND hwnd)
{
    if (hwnd) {
        SetTimer(hwnd, kPulseTimerId, kPulseTickMs, nullptr);
    }
}

void PlaceRow(RECT* out, const Sample* items, int n, int x, int y, int cellH, int gap)
{
    for (int i = 0; i < n; ++i) {
        const int w = g_tile.LabeledWidth(items[i].label);
        out[i] = {x, y, x + w, y + cellH};
        x += w + gap;
    }
}

int LabeledRowWidth(const Sample* items, int n, int gap)
{
    int w = 0;
    for (int i = 0; i < n; ++i) {
        w += g_tile.LabeledWidth(items[i].label);
        if (i + 1 < n) {
            w += gap;
        }
    }
    return w;
}

int ChipsStripWidth()
{
    int w = 0;
    for (int i = 0; i < kChipCount; ++i) {
        w += ChipWidth(i);
        if (i + 1 < kChipCount) {
            w += ChipGapAfter(i);
        }
    }
    return w;
}

Layout ComputeLayout(int windowW, int windowH, int titleBarPx, int reserveBottom, int frameInset)
{
    Layout L{};
    const int top = frameInset + titleBarPx + 8;
    const int bottom = windowH - frameInset - reserveBottom - 8;
    (void)bottom; // well still uses this; rows park from the top so shadow cannot hide D2D
    const int left = frameInset + kPad;
    const int right = windowW - frameInset - kPad;
    if (right - left < 160 || bottom - top < 80) {
        return L;
    }

    L.well = {left, top, right, bottom};

    const int innerL = left + 14;
    const int innerR = right - 14;
    int y = top + 34;

    const int chipsW = ChipsStripWidth();
    if (innerR - innerL > chipsW + 8) {
        L.showChips = true;
        const int chipTop = top + 8;
        int x = innerR - chipsW;
        for (int i = 0; i < kChipCount; ++i) {
            const int cw = ChipWidth(i);
            L.chips[i] = {x, chipTop, x + cw, chipTop + kChipH};
            x += cw + ChipGapAfter(i);
        }
        L.labelGroup = {L.chips[kToggleCount].left - 5, chipTop - 3,
            L.chips[kToggleCount + kLabelModeCount - 1].right + 5, chipTop + kChipH + 3};
        L.scaleGroup = {L.chips[kToggleCount + kLabelModeCount].left - 5, chipTop - 3,
            L.chips[kChipCount - 1].right + 5, chipTop + kChipH + 3};
        if (innerR - innerL <= chipsW + 180) {
            y = top + 40;
        }
    }

    const int cellW = g_tile.metrics.cellW;
    const int cellH = g_tile.metrics.cellH;
    const int rowAdvance = cellH + CaptionBand();

    // Rows are parked from the top. Shadow/gutter must not hide D2D.
    L.showLive = true;
    L.live = {innerL, y, innerL + cellW, y + cellH};
    const int d2dL = innerL + cellW + kRibbonGap;
    if (innerR - d2dL >= cellW) {
        L.showLiveD2d = true;
        L.liveD2d = {d2dL, y, d2dL + cellW, y + cellH};
    }
    y += rowAdvance;

    const int rowW = (std::max)(LabeledRowWidth(kSamples, kSampleCount, kRibbonGap),
        LabeledRowWidth(kLongs, kLongCount, kRibbonGap));
    if (innerR - innerL >= rowW) {
        L.showSamples = true;
        PlaceRow(L.samples, kSamples, kSampleCount, innerL, y, cellH, kRibbonGap);
        y += rowAdvance;

        L.showLongs = true;
        PlaceRow(L.longs, kLongs, kLongCount, innerL, y, cellH, kRibbonGap);
        y += rowAdvance;

        L.showD2d = true;
        PlaceRow(L.d2d, kSamples, kSampleCount, innerL, y + 4, cellH, kRibbonGap);
        y += rowAdvance;

        L.showLongsD2d = true;
        PlaceRow(L.longsD2d, kLongs, kLongCount, innerL, y + 4, cellH, kRibbonGap);
    }
    return L;
}

int HitTest(const Layout& L, int x, int y)
{
    const POINT p{x, y};
    if (L.showChips) {
        for (int i = 0; i < kLabelModeCount; ++i) {
            if (PtInRect(&L.chips[kToggleCount + i], p)) {
                return HitLabel0 + i;
            }
        }
        for (int i = 0; i < kScaleCount; ++i) {
            if (PtInRect(&L.chips[kToggleCount + kLabelModeCount + i], p)) {
                return HitScale0 + i;
            }
        }
        for (int i = 0; i < kToggleCount; ++i) {
            if (PtInRect(&L.chips[i], p)) {
                return HitChip0 + i;
            }
        }
    }
    if (L.showLive && PtInRect(&L.live, p)) {
        return HitLive;
    }
    if (L.showLiveD2d && PtInRect(&L.liveD2d, p)) {
        return HitLiveD2d;
    }
    if (L.showSamples) {
        for (int i = 0; i < kSampleCount; ++i) {
            if (PtInRect(&L.samples[i], p)) {
                return HitSample0 + i;
            }
        }
    }
    if (L.showLongs) {
        for (int i = 0; i < kLongCount; ++i) {
            if (PtInRect(&L.longs[i], p)) {
                return HitLong0 + i;
            }
        }
    }
    if (L.showD2d) {
        for (int i = 0; i < kSampleCount; ++i) {
            if (PtInRect(&L.d2d[i], p)) {
                return HitD2d0 + i;
            }
        }
    }
    if (L.showLongsD2d) {
        for (int i = 0; i < kLongCount; ++i) {
            if (PtInRect(&L.longsD2d[i], p)) {
                return HitLongD2d0 + i;
            }
        }
    }
    return HitNone;
}

bool HitTip(int hit, const Layout& L, RECT& rc, COLORREF& accent, const wchar_t*& text)
{
    if (hit == HitLive && L.showLive) {
        rc = L.live;
        accent = LiveAccent();
        text = LiveTooltip();
        return true;
    }
    if (hit == HitLiveD2d && L.showLiveD2d) {
        rc = L.liveD2d;
        accent = LiveAccent();
        text = LiveTooltip();
        return true;
    }
    if (hit >= HitSample0 && hit < HitSample0 + kSampleCount && L.showSamples) {
        const int i = hit - HitSample0;
        rc = L.samples[i];
        accent = kSamples[i].accent;
        text = kSamples[i].tip;
        return true;
    }
    if (hit >= HitLong0 && hit < HitLong0 + kLongCount && L.showLongs) {
        const int i = hit - HitLong0;
        rc = L.longs[i];
        accent = kLongs[i].accent;
        text = kLongs[i].tip;
        return true;
    }
    if (hit >= HitD2d0 && hit < HitD2d0 + kSampleCount && L.showD2d) {
        const int i = hit - HitD2d0;
        rc = L.d2d[i];
        accent = kSamples[i].accent;
        text = kSamples[i].tip;
        return true;
    }
    if (hit >= HitLongD2d0 && hit < HitLongD2d0 + kLongCount && L.showLongsD2d) {
        const int i = hit - HitLongD2d0;
        rc = L.longsD2d[i];
        accent = kLongs[i].accent;
        text = kLongs[i].tip;
        return true;
    }
    return false;
}

void SyncTip(const Layout& L)
{
    RECT rc{};
    COLORREF accent = kLiveAccent;
    const wchar_t* text = nullptr;
    if (!HitTip(g_hover, L, rc, accent, text)) {
        g_tip.Hide();
        return;
    }
    g_tip.Track(g_hover, rc, text, Sandbox().light, accent);
}

void DrawChip(Gdiplus::Graphics& g, const Gdiplus::Font& font, const RECT& rc, const wchar_t* label,
    bool on, bool light)
{
    const Gdiplus::RectF box(static_cast<float>(rc.left), static_cast<float>(rc.top),
        static_cast<float>(rc.right - rc.left), static_cast<float>(rc.bottom - rc.top));
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, box, 7.f);
    if (on) {
        Gdiplus::SolidBrush fill(light ? Gdiplus::Color(220, 0, 122, 204) : Gdiplus::Color(200, 0, 162, 255));
        g.FillPath(&fill, &path);
    } else {
        Gdiplus::SolidBrush fill(light ? Gdiplus::Color(28, 20, 24, 32) : Gdiplus::Color(40, 255, 255, 255));
        g.FillPath(&fill, &path);
    }
    Gdiplus::Pen edge(light ? Gdiplus::Color(50, 20, 28, 40) : Gdiplus::Color(50, 200, 210, 220), 1.f);
    g.DrawPath(&edge, &path);
    Gdiplus::SolidBrush text(on ? Gdiplus::Color(240, 255, 255, 255)
                                : (light ? Gdiplus::Color(200, 40, 46, 56) : Gdiplus::Color(200, 210, 216, 226)));
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(label, -1, &font, box, &fmt, &text);
}

} // namespace

GlowTile& Tile()
{
    return g_tile;
}

void SetHost(HWND hwnd)
{
    g_host = hwnd;
    g_glow.Attach(g_tile, &g_tip);
    BindPlayIcons();
    g_tile.ApplyScale(g_scale);
    g_glow.ApplyFont(g_fontExtra, g_tile.Dpi());
    SyncHostTheme();
    SyncTipType();
    ReleaseCom(g_tfLabel);
    g_tfLabelPx = 0.f;
    (void)SvgIconInit();
    g_tip.Attach(hwnd);
    EnsurePulseTimer(hwnd);
}

void Shutdown()
{
    if (g_host) {
        KillTimer(g_host, kPulseTimerId);
    }
    g_tip.Detach();
    g_host = nullptr;
    g_hover = -1;
    g_pressing = -1;
    g_liveDuty = 0;
    g_progressOrigin = 0;
    ReleaseD2d();
    SvgIconShutdown();
}

int PanelMinHeightPx()
{
    const int h = g_tile.metrics.cellH;
    const int band = CaptionBand();
    return 40 + (h + band) * 5 + 28;
}

void PaintOverGdi(HDC hdc, int windowW, int windowH, int titleBarPx, int reserveBottom, int frameInset)
{
    if (!hdc || windowW < 80 || windowH < 80) {
        return;
    }
    const Layout L = ComputeLayout(windowW, windowH, titleBarPx, reserveBottom, frameInset);
    if (!L.showLive && !L.showLiveD2d && !L.showSamples) {
        return;
    }

    SyncHostFlags();
    const bool light = Sandbox().light;
    GlowHost::Frame gdiFrame;
    gdiFrame.hdc = hdc;
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    {
        const Gdiplus::RectF well(static_cast<float>(L.well.left), static_cast<float>(L.well.top),
            static_cast<float>(L.well.right - L.well.left),
            static_cast<float>(L.well.bottom - L.well.top));
        Gdiplus::GraphicsPath path;
        AddRoundRect(path, well, static_cast<float>(kWellRadius));
        Gdiplus::SolidBrush fill(light ? Gdiplus::Color(235, 255, 255, 255) : Gdiplus::Color(210, 10, 12, 18));
        g.FillPath(&fill, &path);
        if (light) {
            DrawCornerFadeFrameGdi(g, well, static_cast<float>(kWellRadius), 180, 186, 196, 90, 1.05f);
        } else {
            DrawCornerFadeFrameGdi(g, well, static_cast<float>(kWellRadius), 72, 210, 230, 110, 1.05f);
        }
    }

    Gdiplus::Font fontTitle(L"Segoe UI", 12.f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fontBody(L"Segoe UI", 11.f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush titleBr(light ? Gdiplus::Color(230, 28, 32, 40) : Gdiplus::Color(230, 220, 226, 236));
    Gdiplus::SolidBrush hintBr(light ? Gdiplus::Color(170, 90, 98, 112) : Gdiplus::Color(150, 132, 148, 168));

    const Gdiplus::RectF titleRc(static_cast<float>(L.well.left) + 14.f,
        static_cast<float>(L.well.top) + 8.f,
        static_cast<float>(L.well.right - L.well.left) - static_cast<float>(ChipsStripWidth()) - 28.f, 18.f);
    g.DrawString(L"Ribbon glow", -1, &fontTitle, titleRc, nullptr, &titleBr);

    if (L.showChips) {
        auto fillGroup = [&](const RECT& rc) {
            Gdiplus::GraphicsPath well;
            AddRoundRect(well,
                Gdiplus::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                    static_cast<float>(rc.right - rc.left), static_cast<float>(rc.bottom - rc.top)),
                9.f);
            Gdiplus::SolidBrush wellFill(light ? Gdiplus::Color(36, 20, 24, 32) : Gdiplus::Color(40, 255, 255, 255));
            g.FillPath(&wellFill, &well);
        };
        fillGroup(L.labelGroup);
        fillGroup(L.scaleGroup);
        DrawChip(g, fontBody, L.chips[0], Sandbox().light ? L"Light" : L"Dark", true, light);
        DrawChip(g, fontBody, L.chips[1], L"Glass", Sandbox().glass, light);
        DrawChip(g, fontBody, L.chips[2], L"Shadow", Sandbox().shadow, light);
        DrawChip(g, fontBody, L.chips[3], L"Glow", g_glowOn, light);
        DrawChip(g, fontBody, L.chips[4], L"Tip", g_tip.enabled, light);
        DrawChip(g, fontBody, L.chips[5], L"Off", g_labelMode == LabelMode::None, light);
        DrawChip(g, fontBody, L.chips[6], L"In", g_labelMode == LabelMode::Inside, light);
        DrawChip(g, fontBody, L.chips[7], L"Below", g_labelMode == LabelMode::Below, light);
        DrawChip(g, fontBody, L.chips[8], L"1×", g_scale == 1, light);
        DrawChip(g, fontBody, L.chips[9], L"2×", g_scale == 2, light);
    }

    const float pulse = Pulse01();

    if (L.showLive) {
        const bool hot = (g_hover == HitLive);
        const bool down = (g_pressing == HitLive);
        const ToolBtnState duty = LiveDuty();
        const ToolBtnState st = OverState(duty, hot, down);
        g_glow.PaintItem(gdiFrame, L.live,
            MakeDraw(st, LiveAccent(), pulse, LiveCaption(duty), light, hot, LiveIconStem(), LiveProgress(),
                LiveTooltip()));
        g.DrawString(L"GDI+ live", -1, &fontBody,
            Gdiplus::PointF(static_cast<float>(L.live.left),
                static_cast<float>(L.live.top - RowNoteLift())),
            &hintBr);
        const float nx = static_cast<float>((L.showLiveD2d ? L.liveD2d.right : L.live.right) + 16);
        const float ny = static_cast<float>(L.live.top) + 10.f;
        g.DrawString(L"Live  ·  all states", -1, &fontTitle, Gdiplus::PointF(nx, ny), &titleBr);
        g.DrawString(L"double-click starts a run — progress once, then finish", -1, &fontBody,
            Gdiplus::PointF(nx, ny + 22.f), &hintBr);
        g.DrawString(L"running  ·  progress 0–100  (call site)", -1, &fontBody, Gdiplus::PointF(nx, ny + 40.f),
            &hintBr);
        wchar_t fontHint[48]{};
        (void)swprintf_s(fontHint, L"[  ]  font  extra %d  %.0f px", g_fontExtra, g_tile.metrics.labelPx);
        g.DrawString(fontHint, -1, &fontBody, Gdiplus::PointF(nx, ny + 58.f), &hintBr);
    }

    if (L.showSamples) {
        for (int i = 0; i < kSampleCount; ++i) {
            const int hit = HitSample0 + i;
            const bool hot = (g_hover == hit);
            const bool down = (g_pressing == hit);
            g_glow.PaintItem(gdiFrame, L.samples[i],
                MakeDraw(OverState(kSamples[i].pin, hot, down), kSamples[i].accent, pulse, kSamples[i].label, light,
                    hot, kSamples[i].icon, -1.f, kSamples[i].tip));
        }
    }

    if (L.showSamples) {
        const Gdiplus::RectF note(static_cast<float>(L.samples[0].left),
            static_cast<float>(L.samples[0].top - RowNoteLift()), 280.f, 16.f);
        g.DrawString(L"GDI+  (fallback painter)", -1, &fontBody, note, nullptr, &hintBr);
    }

    if (L.showLongs) {
        const Gdiplus::RectF note(static_cast<float>(L.longs[0].left),
            static_cast<float>(L.longs[0].top - RowNoteLift()), 360.f, 16.f);
        g.DrawString(L"GDI+  long labels  (ellipsis)", -1, &fontBody, note, nullptr, &hintBr);
        for (int i = 0; i < kLongCount; ++i) {
            const int hit = HitLong0 + i;
            const bool hot = (g_hover == hit);
            const bool down = (g_pressing == hit);
            g_glow.PaintItem(gdiFrame, L.longs[i],
                MakeDraw(OverState(kLongs[i].pin, hot, down), kLongs[i].accent, pulse, kLongs[i].label, light, hot,
                    kLongs[i].icon, -1.f, kLongs[i].tip));
        }
    }

    PaintD2dRow(hdc, windowW, windowH, L, pulse, light);
}

bool OnMouseMove(HWND hwnd, int clientX, int clientY, int windowW, int windowH, int titleBarPx,
    int reserveBottom, int frameInset)
{
    (void)hwnd;
    const Layout L = ComputeLayout(windowW, windowH, titleBarPx, reserveBottom, frameInset);
    const int hit = HitTest(L, clientX, clientY);
    const bool changed = (hit != g_hover);
    g_hover = hit;
    SyncTip(L);
    return changed;
}

bool OnLButtonDown(HWND hwnd, int clientX, int clientY, int windowW, int windowH, int titleBarPx,
    int reserveBottom, int frameInset)
{
    const Layout L = ComputeLayout(windowW, windowH, titleBarPx, reserveBottom, frameInset);
    const int hit = HitTest(L, clientX, clientY);
    if (hit < 0) {
        return false;
    }
    if (hit >= HitScale0 && hit < HitScale0 + kScaleCount) {
        ApplyPlayScale(hit == HitScale0 ? 1 : 2);
        g_tip.Hide();
        return true;
    }
    if (hit >= HitLabel0 && hit < HitLabel0 + kLabelModeCount) {
        static const LabelMode kModes[] = {LabelMode::None, LabelMode::Inside, LabelMode::Below};
        g_labelMode = kModes[hit - HitLabel0];
        g_tip.Hide();
        return true;
    }
    if (hit >= HitChip0 && hit < HitChip0 + kToggleCount) {
        const int chip = hit - HitChip0;
        if (chip == 0) {
            Sandbox().light = !Sandbox().light;
            SyncHostTheme();
        } else if (chip == 1) {
            Sandbox().glass = !Sandbox().glass;
        } else if (chip == 2) {
            Sandbox().shadow = !Sandbox().shadow;
        } else if (chip == 3) {
            g_glowOn = !g_glowOn;
        } else {
            g_tip.enabled = !g_tip.enabled;
        }
        g_tip.Hide();
        return true;
    }
    g_tip.Hide();
    g_pressing = hit;
    SetCapture(hwnd);
    return true;
}

bool OnLButtonUp(HWND hwnd, int clientX, int clientY)
{
    (void)clientX;
    (void)clientY;
    const bool had = g_pressing >= 0;
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    g_pressing = -1;
    return had;
}

bool OnLButtonDblClk(HWND hwnd, int clientX, int clientY, int windowW, int windowH, int titleBarPx,
    int reserveBottom, int frameInset)
{
    const Layout L = ComputeLayout(windowW, windowH, titleBarPx, reserveBottom, frameInset);
    const int hit = HitTest(L, clientX, clientY);
    if (hit != HitLive && hit != HitLiveD2d) {
        return false;
    }
    CycleLiveDuty();
    g_pressing = hit;
    g_hover = hit;
    SyncTip(L);
    SetCapture(hwnd);
    return true;
}

void OnMouseLeave(HWND hwnd)
{
    (void)hwnd;
    g_hover = -1;
    g_tip.Hide();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

bool OnSetCursor()
{
    SetCursor(LoadCursorW(nullptr, g_hover >= 0 ? IDC_HAND : IDC_ARROW));
    return true;
}

bool OnTimer(HWND hwnd, WPARAM timerId)
{
    if (g_tip.OnTimer(hwnd, timerId)) {
        return true;
    }
    if (timerId == kPulseTimerId) {
        AdvanceLiveRun();
        if (g_tip.enabled && (g_hover == HitLive || g_hover == HitLiveD2d)
            && (g_liveDuty == 1 || g_liveDuty == 2)) {
            g_tip.SetText(LiveTooltip());
        }
        return true;
    }
    return false;
}

bool OnKeyDown(HWND hwnd, UINT vk)
{
    (void)hwnd;
    if (vk == 'T') {
        Sandbox().light = !Sandbox().light;
        SyncHostTheme();
        return true;
    }
    if (vk == 'G') {
        Sandbox().glass = !Sandbox().glass;
        return true;
    }
    if (vk == 'S') {
        Sandbox().shadow = !Sandbox().shadow;
        return true;
    }
    if (vk == 'O') {
        g_glowOn = !g_glowOn;
        return true;
    }
    if (vk == 'I') {
        g_tip.enabled = !g_tip.enabled;
        if (!g_tip.enabled) {
            g_tip.Hide();
        }
        return true;
    }
    if (vk == 'L') {
        CycleLabelMode();
        return true;
    }
    if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) {
        ApplyPlayScale(1);
        return true;
    }
    if (vk == VK_OEM_PLUS || vk == VK_ADD) {
        ApplyPlayScale(2);
        return true;
    }
    if (vk == VK_OEM_4) { // [
        NudgeLabelPx(-1.f);
        return true;
    }
    if (vk == VK_OEM_6) { // ]
        NudgeLabelPx(1.f);
        return true;
    }
    if (vk >= '1' && vk <= '4') {
        SetLiveDuty(static_cast<int>(vk - '1'));
        if (g_hover == HitLive || g_hover == HitLiveD2d) {
            g_tip.SetText(LiveTooltip());
        }
        return true;
    }
    return false;
}

} // namespace glowplay
