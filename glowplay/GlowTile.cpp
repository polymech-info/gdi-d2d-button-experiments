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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <vector>

#include "GlowTile.h"

namespace glowplay {
namespace {

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

D2D1_COLOR_F D2Color(COLORREF c, float a)
{
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}

Gdiplus::Color Gp(BYTE a, COLORREF c)
{
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

void PaintBloomGdi(Gdiplus::Graphics& g, const Gdiplus::RectF& cell, float radius, float maxExpand,
    float bloomAlpha, COLORREF rgb)
{
    const float reach = (std::min)(maxExpand, 3.f);
    if (reach < 0.35f || bloomAlpha < 1.f / 255.f) {
        return;
    }
    const int steps = (std::max)(4, static_cast<int>(std::lround(reach * 2.5f)));
    for (int i = steps; i >= 1; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float expand = reach * t;
        const float a = bloomAlpha * 0.22f * std::exp(-t * t * 2.4f);
        if (a < 1.f / 255.f) {
            continue;
        }
        const Gdiplus::RectF rc(cell.X - expand, cell.Y - expand, cell.Width + expand * 2.f,
            cell.Height + expand * 2.f);
        Gdiplus::GraphicsPath path;
        AddRoundRect(path, rc, radius + expand);
        Gdiplus::Pen pen(Gp(ClampByte(a * 255.f), rgb), 1.1f);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&pen, &path);
    }
}

void PaintBloomD2d(ID2D1RenderTarget* rt, const D2D1_RECT_F& rc, float radius, float maxExpand,
    float bloomAlpha, COLORREF rgb)
{
    const float reach = (std::min)(maxExpand, 3.f);
    if (!rt || reach < 0.35f || bloomAlpha < 1.f / 255.f) {
        return;
    }
    const int steps = (std::max)(4, static_cast<int>(std::lround(reach * 2.5f)));
    for (int i = steps; i >= 1; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float expand = reach * t;
        const float a = bloomAlpha * 0.22f * std::exp(-t * t * 2.4f);
        if (a < 1.f / 255.f) {
            continue;
        }
        ID2D1SolidColorBrush* br = nullptr;
        if (FAILED(rt->CreateSolidColorBrush(D2Color(rgb, a), &br)) || !br) {
            continue;
        }
        const D2D1_RECT_F halo = D2D1::RectF(rc.left - expand, rc.top - expand, rc.right + expand,
            rc.bottom + expand);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(halo, radius + expand, radius + expand), br, 1.1f);
        br->Release();
    }
}

int ClampIndex(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void BoxBlurH8(const BYTE* src, BYTE* dst, int w, int h, int radius)
{
    const int span = radius * 2 + 1;
    const int half = span / 2;
    for (int y = 0; y < h; ++y) {
        const BYTE* row = src + static_cast<size_t>(y) * w;
        BYTE* out = dst + static_cast<size_t>(y) * w;
        int s = 0;
        for (int i = -radius; i <= radius; ++i) {
            s += row[ClampIndex(i, 0, w - 1)];
        }
        for (int x = 0; x < w; ++x) {
            out[x] = static_cast<BYTE>((s + half) / span);
            s += row[ClampIndex(x + radius + 1, 0, w - 1)] - row[ClampIndex(x - radius, 0, w - 1)];
        }
    }
}

void BoxBlurV8(const BYTE* src, BYTE* dst, int w, int h, int radius)
{
    const int span = radius * 2 + 1;
    const int half = span / 2;
    for (int x = 0; x < w; ++x) {
        int s = 0;
        for (int i = -radius; i <= radius; ++i) {
            s += src[static_cast<size_t>(ClampIndex(i, 0, h - 1)) * w + x];
        }
        for (int y = 0; y < h; ++y) {
            dst[static_cast<size_t>(y) * w + x] = static_cast<BYTE>((s + half) / span);
            s += src[static_cast<size_t>(ClampIndex(y + radius + 1, 0, h - 1)) * w + x]
                - src[static_cast<size_t>(ClampIndex(y - radius, 0, h - 1)) * w + x];
        }
    }
}

void GaussApproxA8(BYTE* bits, int w, int h, int radius)
{
    if (!bits || w < 1 || h < 1) {
        return;
    }
    const int r = (std::max)(1, radius);
    std::vector<BYTE> tmp(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int pass = 0; pass < 3; ++pass) {
        BoxBlurH8(bits, tmp.data(), w, h, r);
        BoxBlurV8(tmp.data(), bits, w, h, r);
    }
}

struct DropMask {
    std::vector<BYTE> a8;
    int w = 0;
    int h = 0;
    int pad = 0;
};

struct DropKey {
    int cw = 0;
    int ch = 0;
    int radius = 0;
    int dx = 0;
    int dy = 0;
    int blur = 0;

    bool operator==(const DropKey& o) const
    {
        return cw == o.cw && ch == o.ch && radius == o.radius && dx == o.dx && dy == o.dy && blur == o.blur;
    }
};

int Quant4(float v)
{
    return static_cast<int>(std::lround(v * 4.f));
}

DropKey MakeDropKey(float cellW, float cellH, float radius, float dx, float dy, float blurPx)
{
    DropKey k;
    k.cw = (std::max)(1, static_cast<int>(std::ceil(cellW)));
    k.ch = (std::max)(1, static_cast<int>(std::ceil(cellH)));
    k.radius = Quant4(radius);
    k.dx = Quant4(dx);
    k.dy = Quant4(dy);
    k.blur = Quant4(blurPx);
    return k;
}

constexpr int kDropCacheCap = 32;
DropKey g_dropKeys[kDropCacheCap]{};
DropMask g_dropMasks[kDropCacheCap]{};
int g_dropCacheN = 0;
std::vector<BYTE> g_tintPargb;

struct D2dDropSlot {
    ID2D1RenderTarget* rt = nullptr;
    DropKey key{};
    COLORREF rgb = 0;
    BYTE alpha = 0;
    ID2D1Bitmap* bmp = nullptr;
};

constexpr int kD2dDropCap = 8;
D2dDropSlot g_d2dDrops[kD2dDropCap]{};

struct ScratchDib {
    HDC mem = nullptr;
    HBITMAP hb = nullptr;
    void* bits = nullptr;
    int capW = 0;
    int capH = 0;
};

ScratchDib g_scratch;

void ReleaseScratch()
{
    if (g_scratch.mem && g_scratch.hb) {
        SelectObject(g_scratch.mem, GetStockObject(NULL_PEN));
    }
    if (g_scratch.hb) {
        DeleteObject(g_scratch.hb);
        g_scratch.hb = nullptr;
    }
    if (g_scratch.mem) {
        DeleteDC(g_scratch.mem);
        g_scratch.mem = nullptr;
    }
    g_scratch.bits = nullptr;
    g_scratch.capW = 0;
    g_scratch.capH = 0;
}

bool EnsureScratch(int w, int h)
{
    if (w < 1 || h < 1) {
        return false;
    }
    if (g_scratch.hb && w <= g_scratch.capW && h <= g_scratch.capH && g_scratch.mem && g_scratch.bits) {
        std::memset(g_scratch.bits, 0, static_cast<size_t>(g_scratch.capW) * static_cast<size_t>(g_scratch.capH) * 4);
        return true;
    }
    ReleaseScratch();
    g_scratch.capW = (std::max)(w, 64);
    g_scratch.capH = (std::max)(h, 64);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_scratch.capW;
    bmi.bmiHeader.biHeight = -g_scratch.capH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    g_scratch.mem = CreateCompatibleDC(nullptr);
    if (!g_scratch.mem) {
        return false;
    }
    g_scratch.hb = CreateDIBSection(g_scratch.mem, &bmi, DIB_RGB_COLORS, &g_scratch.bits, nullptr, 0);
    if (!g_scratch.hb || !g_scratch.bits) {
        ReleaseScratch();
        return false;
    }
    SelectObject(g_scratch.mem, g_scratch.hb);
    std::memset(g_scratch.bits, 0, static_cast<size_t>(g_scratch.capW) * static_cast<size_t>(g_scratch.capH) * 4);
    return true;
}

void ReleaseD2dDrops()
{
    for (int i = 0; i < kD2dDropCap; ++i) {
        if (g_d2dDrops[i].bmp) {
            g_d2dDrops[i].bmp->Release();
            g_d2dDrops[i].bmp = nullptr;
        }
        g_d2dDrops[i] = {};
    }
}

void FlushAllPaintCaches()
{
    ReleaseScratch();
    ReleaseD2dDrops();
    g_dropCacheN = 0;
    for (int i = 0; i < kDropCacheCap; ++i) {
        g_dropMasks[i] = {};
        g_dropKeys[i] = {};
    }
    g_tintPargb.clear();
}

bool RasterDropMask(float cellW, float cellH, float radius, float dx, float dy, float blurPx, DropMask& out)
{
    if (cellW < 2.f || cellH < 2.f) {
        return false;
    }
    const int r = (std::max)(1, static_cast<int>(std::lround(blurPx)));
    const int pad = r + static_cast<int>(std::lround((std::max)(std::fabs(dx), std::fabs(dy)))) + 2;
    const int cw = (std::max)(1, static_cast<int>(std::ceil(cellW)));
    const int ch = (std::max)(1, static_cast<int>(std::ceil(cellH)));
    out.w = cw + pad * 2;
    out.h = ch + pad * 2;
    out.pad = pad;
    std::vector<BYTE> pargb(static_cast<size_t>(out.w) * static_cast<size_t>(out.h) * 4, 0);
    {
        Gdiplus::Bitmap wrap(out.w, out.h, out.w * 4, PixelFormat32bppPARGB, pargb.data());
        Gdiplus::Graphics sg(&wrap);
        if (sg.GetLastStatus() != Gdiplus::Ok) {
            return false;
        }
        sg.SetPageUnit(Gdiplus::UnitPixel);
        sg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        sg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        sg.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        Gdiplus::GraphicsPath path;
        AddRoundRect(path, Gdiplus::RectF(static_cast<float>(pad) + dx, static_cast<float>(pad) + dy, cellW, cellH),
            radius);
        Gdiplus::SolidBrush br(Gdiplus::Color(255, 255, 255, 255));
        sg.FillPath(&br, &path);
    }
    out.a8.assign(static_cast<size_t>(out.w) * static_cast<size_t>(out.h), 0);
    const size_t n = static_cast<size_t>(out.w) * static_cast<size_t>(out.h);
    for (size_t i = 0; i < n; ++i) {
        out.a8[i] = pargb[i * 4 + 3];
    }
    GaussApproxA8(out.a8.data(), out.w, out.h, r);
    return true;
}

const DropMask* GetDropMask(const Gdiplus::RectF& cell, float radius, float dx, float dy, float blurPx, float& destX,
    float& destY)
{
    const DropKey key = MakeDropKey(cell.Width, cell.Height, radius, dx, dy, blurPx);
    for (int i = 0; i < g_dropCacheN; ++i) {
        if (g_dropKeys[i] == key && !g_dropMasks[i].a8.empty()) {
            destX = cell.X - static_cast<float>(g_dropMasks[i].pad);
            destY = cell.Y - static_cast<float>(g_dropMasks[i].pad);
            return &g_dropMasks[i];
        }
    }
    const int slot = (g_dropCacheN < kDropCacheCap) ? g_dropCacheN++ : (kDropCacheCap - 1);
    if (!RasterDropMask(cell.Width, cell.Height, radius, dx, dy, blurPx, g_dropMasks[slot])) {
        return nullptr;
    }
    g_dropKeys[slot] = key;
    destX = cell.X - static_cast<float>(g_dropMasks[slot].pad);
    destY = cell.Y - static_cast<float>(g_dropMasks[slot].pad);
    return &g_dropMasks[slot];
}

void TintMask(const DropMask& mask, BYTE alpha, COLORREF rgb, std::vector<BYTE>& pargb)
{
    const size_t n = static_cast<size_t>(mask.w) * static_cast<size_t>(mask.h);
    pargb.resize(n * 4);
    const int pr = GetRValue(rgb);
    const int pg = GetGValue(rgb);
    const int pb = GetBValue(rgb);
    for (size_t i = 0; i < n; ++i) {
        const unsigned a = (static_cast<unsigned>(mask.a8[i]) * static_cast<unsigned>(alpha) + 127u) / 255u;
        pargb[i * 4 + 0] = static_cast<BYTE>((pb * a + 127u) / 255u);
        pargb[i * 4 + 1] = static_cast<BYTE>((pg * a + 127u) / 255u);
        pargb[i * 4 + 2] = static_cast<BYTE>((pr * a + 127u) / 255u);
        pargb[i * 4 + 3] = static_cast<BYTE>(a);
    }
}

void DrawDropGdi(Gdiplus::Graphics& g, const Gdiplus::RectF& cell, float radius, float dx, float dy, float blurPx,
    BYTE alpha, COLORREF rgb)
{
    float destX = 0.f;
    float destY = 0.f;
    const DropMask* mask = GetDropMask(cell, radius, dx, dy, blurPx, destX, destY);
    if (!mask || alpha < 2) {
        return;
    }
    TintMask(*mask, alpha, rgb, g_tintPargb);
    Gdiplus::Bitmap wrap(mask->w, mask->h, mask->w * 4, PixelFormat32bppPARGB, g_tintPargb.data());
    const Gdiplus::InterpolationMode prev = g.GetInterpolationMode();
    g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    g.DrawImage(&wrap, destX, destY, static_cast<Gdiplus::REAL>(mask->w), static_cast<Gdiplus::REAL>(mask->h));
    g.SetInterpolationMode(prev);
}

void DrawDropD2d(ID2D1RenderTarget* rt, const Gdiplus::RectF& cell, float radius, float dx, float dy, float blurPx,
    BYTE alpha, COLORREF rgb)
{
    if (!rt || alpha < 2) {
        return;
    }
    for (int i = 0; i < kD2dDropCap; ++i) {
        if (g_d2dDrops[i].bmp && g_d2dDrops[i].rt != rt) {
            g_d2dDrops[i].bmp->Release();
            g_d2dDrops[i] = {};
        }
    }
    float destX = 0.f;
    float destY = 0.f;
    const DropMask* mask = GetDropMask(cell, radius, dx, dy, blurPx, destX, destY);
    if (!mask) {
        return;
    }
    const DropKey key = MakeDropKey(cell.Width, cell.Height, radius, dx, dy, blurPx);
    for (int i = 0; i < kD2dDropCap; ++i) {
        D2dDropSlot& s = g_d2dDrops[i];
        if (s.bmp && s.rt == rt && s.key == key && s.rgb == rgb && s.alpha == alpha) {
            rt->DrawBitmap(s.bmp,
                D2D1::RectF(destX, destY, destX + static_cast<float>(mask->w), destY + static_cast<float>(mask->h)),
                1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            return;
        }
    }
    TintMask(*mask, alpha, rgb, g_tintPargb);
    D2D1_BITMAP_PROPERTIES props{};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96.f;
    props.dpiY = 96.f;
    ID2D1Bitmap* bmp = nullptr;
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(mask->w), static_cast<UINT32>(mask->h)),
            g_tintPargb.data(), static_cast<UINT32>(mask->w * 4), props, &bmp))
        || !bmp) {
        return;
    }
    int slot = 0;
    for (int i = 0; i < kD2dDropCap; ++i) {
        if (!g_d2dDrops[i].bmp) {
            slot = i;
            break;
        }
        slot = i;
    }
    if (g_d2dDrops[slot].bmp) {
        g_d2dDrops[slot].bmp->Release();
    }
    g_d2dDrops[slot] = {rt, key, rgb, alpha, bmp};
    rt->DrawBitmap(bmp,
        D2D1::RectF(destX, destY, destX + static_cast<float>(mask->w), destY + static_cast<float>(mask->h)), 1.f,
        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

class ClipBleed {
public:
    ClipBleed(HDC dc, const RECT& bleed)
        : dc_(dc)
        , saved_(nullptr)
        , had_(-1)
    {
        if (!dc_) {
            return;
        }
        saved_ = CreateRectRgn(0, 0, 0, 0);
        had_ = GetClipRgn(dc_, saved_);
        HRGN extra = CreateRectRgnIndirect(&bleed);
        if (!extra) {
            return;
        }
        if (had_ == 1) {
            ExtSelectClipRgn(dc_, extra, RGN_OR);
        } else {
            RECT box{};
            if (GetClipBox(dc_, &box) == ERROR) {
                SelectClipRgn(dc_, extra);
            } else {
                HRGN boxRgn = CreateRectRgnIndirect(&box);
                HRGN uni = CreateRectRgn(0, 0, 0, 0);
                if (boxRgn && uni) {
                    CombineRgn(uni, boxRgn, extra, RGN_OR);
                    SelectClipRgn(dc_, uni);
                } else {
                    SelectClipRgn(dc_, extra);
                }
                if (boxRgn) {
                    DeleteObject(boxRgn);
                }
                if (uni) {
                    DeleteObject(uni);
                }
            }
        }
        DeleteObject(extra);
    }

    ~ClipBleed()
    {
        if (!dc_) {
            return;
        }
        if (had_ == 1 && saved_) {
            SelectClipRgn(dc_, saved_);
        }
        if (saved_) {
            DeleteObject(saved_);
        }
    }

    ClipBleed(const ClipBleed&) = delete;
    ClipBleed& operator=(const ClipBleed&) = delete;

private:
    HDC dc_;
    HRGN saved_;
    int had_;
};
COLORREF MixTowardWhite(COLORREF c, float t)
{
    const float k = (std::min)(1.f, (std::max)(0.f, t));
    const float u = 1.f - k;
    return RGB(ClampByte(GetRValue(c) * u + 255.f * k), ClampByte(GetGValue(c) * u + 255.f * k),
        ClampByte(GetBValue(c) * u + 255.f * k));
}

COLORREF ScaleRgb(COLORREF c, float gain)
{
    return RGB(ClampByte(GetRValue(c) * gain), ClampByte(GetGValue(c) * gain), ClampByte(GetBValue(c) * gain));
}

void DrawCornerArcD2d(ID2D1Factory* factory, ID2D1RenderTarget* rt, D2D1_POINT_2F start, D2D1_POINT_2F end,
    float radius, ID2D1SolidColorBrush* br, float strokeW)
{
    if (!factory || !br) {
        return;
    }
    ID2D1PathGeometry* geo = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) {
        if (geo) {
            geo->Release();
        }
        return;
    }
    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(radius, radius), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
        D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    rt->DrawGeometry(geo, br, strokeW);
    geo->Release();
}

float IconDrawPx(const TileMetrics& m, LabelMode mode, float cellW, float cellH)
{
    const float base = static_cast<float>(m.iconPx);
    if (mode != LabelMode::Below) {
        return base;
    }
    // Caption is outside the chrome — use the vacated label band.
    const float want = base + m.labelGap + m.labelH;
    const float maxSq = (std::min)(cellW, cellH) - m.iconTop * 2.f;
    return std::floor((std::max)(base, (std::min)(want, maxSq)));
}

const wchar_t* FirstUiFace()
{
    static const wchar_t* picked = nullptr;
    if (picked) {
        return picked;
    }
    static const wchar_t* kTry[] = {L"Segoe UI Variable Text", L"Segoe UI Variable", L"Segoe UI"};
    for (const wchar_t* face : kTry) {
        Gdiplus::FontFamily fam(face);
        if (fam.GetLastStatus() == Gdiplus::Ok && fam.IsStyleAvailable(Gdiplus::FontStyleRegular)) {
            picked = face;
            return picked;
        }
    }
    return L"Segoe UI";
}

int MeasureCaptionExtent(const wchar_t* face, float px, const wchar_t* caption)
{
    if (!caption || !caption[0] || px < 0.5f) {
        return 0;
    }
    const Gdiplus::RectF bounds = GlowTile::MeasureUiTextGdi(caption, px, 4000.f);
    (void)face;
    return (std::max)(0, static_cast<int>(std::ceil(bounds.Width)));
}

} // namespace

const wchar_t* GlowTile::UiFontFace()
{
    return FirstUiFace();
}

void GlowTile::FillUiTextGdi(Gdiplus::Graphics& g, const wchar_t* text, float px, const Gdiplus::RectF& rc,
    const Gdiplus::Color& color, Gdiplus::StringAlignment alignH, Gdiplus::StringAlignment alignV, bool ellipsis)
{
    if (!text || !text[0] || px < 0.5f || rc.Width < 1.f || rc.Height < 1.f) {
        return;
    }
    Gdiplus::FontFamily family(FirstUiFace());
    if (family.GetLastStatus() != Gdiplus::Ok || !family.IsStyleAvailable(Gdiplus::FontStyleRegular)) {
        Gdiplus::Font font(FirstUiFace(), px, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush br(color);
        Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
        fmt.SetAlignment(alignH);
        fmt.SetLineAlignment(alignV);
        fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        if (ellipsis) {
            fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        }
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.DrawString(text, -1, &font, rc, &fmt, &br);
        return;
    }
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetAlignment(alignH);
    fmt.SetLineAlignment(alignV);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip);
    if (ellipsis) {
        fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    }
    Gdiplus::GraphicsPath path;
    path.AddString(text, -1, &family, Gdiplus::FontStyleRegular, px, rc, &fmt);
    const Gdiplus::SmoothingMode prevSmooth = g.GetSmoothingMode();
    const Gdiplus::PixelOffsetMode prevPx = g.GetPixelOffsetMode();
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush br(color);
    g.FillPath(&br, &path);
    g.SetSmoothingMode(prevSmooth);
    g.SetPixelOffsetMode(prevPx);
}

Gdiplus::RectF GlowTile::MeasureUiTextGdi(const wchar_t* text, float px, float maxW)
{
    Gdiplus::RectF bounds(0.f, 0.f, 0.f, 0.f);
    if (!text || !text[0] || px < 0.5f) {
        return bounds;
    }
    Gdiplus::FontFamily family(FirstUiFace());
    if (family.GetLastStatus() != Gdiplus::Ok) {
        return bounds;
    }
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip);
    Gdiplus::GraphicsPath path;
    const Gdiplus::RectF layout(0.f, 0.f, (std::max)(8.f, maxW), 800.f);
    path.AddString(text, -1, &family, Gdiplus::FontStyleRegular, px, layout, &fmt);
    path.GetBounds(&bounds);
    if (bounds.Width < 0.f) {
        bounds.Width = 0.f;
    }
    if (bounds.Height < 0.f) {
        bounds.Height = 0.f;
    }
    return bounds;
}

GlowTile::GlowTile()
{
    ResetToDefaults();
}

void GlowTile::ApplyPalette()
{
    for (int i = 0; i < kToolBtnStateCount; ++i) {
        StateCfg& s = states_[i];
        s.glow = colors.glow;
        s.bg = colors.bg;
        s.frame = colors.frame;
        s.shade = colors.shade;
        s.bevelLo = colors.bevelLo;
        s.bevelHi = colors.bevelHi;
        s.glassRgb = colors.glass;
        s.edgeTowardWhite = colors.edgeTowardWhite;
        s.bevelLoGainLight = colors.bevelLoGainLight;
        s.bevelLoGainDark = colors.bevelLoGainDark;
        s.label = colors.label;
        s.labelA = colors.labelA;
        s.iconA = {colors.activeIconA, colors.activeIconA};
    }
    at(ToolBtnState::Idle).iconA = colors.idleIconA;

    // Finish keeps a pale card. Stamping accent bg here made a solid green fill
    // (icon same green → invisible). Theme switch must not clobber this.
    StateCfg& fin = at(ToolBtnState::Finish);
    fin.bg.fromAccent = false;
    fin.bg.light = RGB(255, 255, 255);
    fin.bg.dark = RGB(46, 50, 56);
    fin.shade.fromAccent = false;
    fin.shade.light = RGB(72, 76, 82);
    fin.shade.dark = RGB(0, 0, 0);
    fin.frame.fromAccent = true;
    fin.edgeTowardWhite = 0.08f;
}

void GlowTile::ResetToDefaults()
{
    metrics = TileMetrics{};
    colors = TileColors{};
    hot = HotOverlay{};
    dpi_ = 96;

    bloomLayerCount = 4;
    bloomLayers[0] = {3.f, 10, 1.1f};
    bloomLayers[1] = {2.f, 16, 1.1f};
    bloomLayers[2] = {1.2f, 22, 1.1f};
    bloomLayers[3] = {0.5f, 28, 1.f};
    for (int i = 4; i < 8; ++i) {
        bloomLayers[i] = {};
    }

    for (int i = 0; i < kToolBtnStateCount; ++i) {
        states_[i] = StateCfg{};
    }
    ApplyPalette();

    {
        StateCfg& s = at(ToolBtnState::Idle);
        s.iconGain = {0.78f, 0.58f};
    }
    {
        StateCfg& s = at(ToolBtnState::Hover);
        s.bloom = true;
        s.bloomScale = 0.75f;
        s.bloomAlpha = 1.f;
        s.tint = true;
        s.tintA = {8, 14};
        s.edge = true;
        s.edgeA = {92, 150};
        s.glass = true;
        s.hiA = {40, 28};
        s.loA = {22, 36};
        s.shadow = true;
        s.shadowA = {16, 22};
        s.iconGain = {1.f, 1.f};
    }
    {
        StateCfg& s = at(ToolBtnState::Pressing);
        s.bloom = true;
        s.bloomScale = 0.28f;
        s.bloomAlpha = 0.45f;
        s.tint = true;
        s.tintA = {22, 30};
        s.darken = true;
        s.edge = true;
        s.edgeA = {70, 110};
        s.loA = {28, 40};
        s.pressNudge = true;
        s.iconGain = {0.86f, 0.86f};
    }
    {
        StateCfg& s = at(ToolBtnState::Pressed);
        s.bloom = true;
        s.bloomScale = 1.2f;
        s.bloomAlpha = 1.15f;
        s.tint = true;
        s.tintA = {18, 26};
        s.edge = true;
        s.edgeA = {150, 200};
        s.glass = true;
        s.hiA = {58, 42};
        s.loA = {32, 48};
        s.shadow = true;
        s.shadowA = {20, 28};
        s.shadowEven = false;
        s.iconGain = {1.f, 1.f};
    }
    {
        StateCfg& s = at(ToolBtnState::Running);
        s.bloom = true;
        s.bloomScale = 0.75f;
        s.bloomScalePulse = 0.4f;
        s.bloomAlpha = 0.9f;
        s.bloomAlphaPulse = 0.25f;
        s.tint = true;
        s.tintA = {10, 10};
        s.tintAPulse = 10;
        s.edge = true;
        s.edgeA = {100, 150};
        s.edgeAPulse = 40;
        s.glass = true;
        s.hiA = {28, 28};
        s.hiAPulse = 18;
        s.loA = {24, 24};
        s.iconGain = {0.92f, 0.92f};
        s.iconGainPulse = 0.08f;
        s.progressFill = true;
        s.progressBlend = 0.4f;
    }
    {
        StateCfg& s = at(ToolBtnState::Finish);
        s.bloom = false;
        s.glass = false;
        s.tint = true;
        s.tintA = {236, 210};
        s.bg.fromAccent = false;
        s.bg.light = RGB(255, 255, 255);
        s.bg.dark = RGB(46, 50, 56);
        s.shadow = true;
        s.shadowEven = false;
        s.shadowA = {30, 42};
        s.shade.fromAccent = false;
        s.shade.light = RGB(72, 76, 82);
        s.shade.dark = RGB(0, 0, 0);
        s.edge = true;
        s.edgeWidth = 1.f;
        s.edgeA = {140, 180};
        s.frame.fromAccent = true;
        s.edgeTowardWhite = 0.08f;
        s.iconGain = {1.f, 1.f};
    }
    {
        StateCfg& s = at(ToolBtnState::Failed);
        s.bloom = true;
        s.bloomScale = 1.05f;
        s.bloomAlpha = 1.05f;
        s.tint = true;
        s.tintA = {16, 24};
        s.edge = true;
        s.edgeA = {140, 190};
        s.glass = true;
        s.hiA = {36, 28};
        s.loA = {30, 30};
        s.iconGain = {1.f, 1.f};
    }
}

void GlowTile::FlushPaintCaches()
{
    FlushAllPaintCaches();
}

void GlowTile::ApplyDpi(int dpi)
{
    const int d = dpi < 48 ? 96 : dpi;
    const bool dpiChanged = (d != dpi_);
    dpi_ = d;
    const float f = static_cast<float>(d) / 96.f;
    metrics.cellW = static_cast<int>(std::lround(64.f * f));
    metrics.cellWMax = static_cast<int>(std::lround(84.f * f));
    metrics.cellH = static_cast<int>(std::lround(58.f * f));
    metrics.iconPx = static_cast<int>(std::lround(28.f * f));
    metrics.glowPad = static_cast<int>(std::lround(3.f * f));
    metrics.labelPadX = static_cast<int>(std::lround(12.f * f));
    metrics.radius = 8.f * f;
    metrics.iconTop = 5.f * f;
    metrics.labelPx = metrics.labelPxBase * f;
    metrics.labelGap = 3.f * f;
    metrics.labelH = metrics.labelHBase * f;
    metrics.labelBelowGap = 12.f * f;
    metrics.labelInsetX = 5.f * f;
    metrics.shadowDx = 1.5f * f;
    metrics.shadowDy = 1.5f * f;
    metrics.shadowBlur = 3.f * f;
    if (dpiChanged) {
        FlushAllPaintCaches();
    }
}

void GlowTile::ApplyScale(int scale)
{
    ApplyDpi(96 * (scale >= 2 ? 2 : 1));
}

void GlowTile::ApplyProductRibbonMetrics()
{
    ApplyDpi(96);
}

RECT GlowTile::PaintBounds(const RECT& cell, LabelMode mode, bool hasCaption) const
{
    const int pad = ChromeBleed();
    RECT r{cell.left - pad, cell.top - pad, cell.right + pad, cell.bottom + pad};
    if (mode == LabelMode::Below && hasCaption) {
        r.bottom += static_cast<int>(metrics.labelBelowGap + metrics.labelH + 2.f);
    }
    return r;
}

StateCfg& GlowTile::at(ToolBtnState s)
{
    return states_[StateIndex(s)];
}

const StateCfg& GlowTile::at(ToolBtnState s) const
{
    return states_[StateIndex(s)];
}

int GlowTile::LabeledWidth(const wchar_t* caption) const
{
    const int minW = (std::max)(1, metrics.cellW);
    const int maxW = (std::max)(minW, metrics.cellWMax);
    if (!caption || !caption[0]) {
        return minW;
    }
    const int textW = MeasureCaptionExtent(UiFontFace(), metrics.labelPx, caption);
    const int natural = (std::max)(minW, textW + metrics.labelPadX * 2);
    return (std::min)(natural, maxW);
}

TileFx GlowTile::Resolve(const Draw& d) const
{
    const bool over = d.enabled && d.hot && hot.enabled && d.state != ToolBtnState::Pressing;
    const StateCfg& c = at(over ? ToolBtnState::Hover : d.state);
    const bool light = d.light;
    const float pulse = d.pulse;
    TileFx fx;
    fx.bloom = c.bloom && d.glowOn;
    fx.bloomScale = c.bloomScale + c.bloomScalePulse * pulse;
    fx.bloomAlpha = c.bloomAlpha + c.bloomAlphaPulse * pulse;
    fx.tint = c.tint;
    fx.tintA = ClampByte(static_cast<float>(c.tintA.At(light)) + static_cast<float>(c.tintAPulse) * pulse);
    fx.edge = c.edge;
    fx.edgeA = ClampByte(static_cast<float>(c.edgeA.At(light)) + static_cast<float>(c.edgeAPulse) * pulse);
    fx.edgeWidth = c.edgeWidth;
    fx.glass = c.glass;
    fx.hiA = ClampByte(static_cast<float>(c.hiA.At(light)) + static_cast<float>(c.hiAPulse) * pulse);
    fx.loA = c.loA.At(light);
    fx.shadow = c.shadow && (d.shadowOn || c.shadowEven);
    fx.shadowEven = c.shadowEven;
    fx.shadowScale = 1.f;
    fx.shadowA = c.shadowA.At(light);
    fx.pressNudge = c.pressNudge;
    fx.darken = c.darken;
    fx.darkenGain = c.darkenGain;
    fx.iconGain = c.iconGain.At(light) + c.iconGainPulse * pulse;

    const COLORREF accent = d.accent;
    fx.bloomRgb = c.glow.At(light, accent);
    fx.shadowRgb = c.shade.At(light, accent);
    fx.tintRgb = c.bg.At(light, accent);
    fx.edgeRgb = c.frame.fromAccent ? MixTowardWhite(accent, c.edgeTowardWhite) : c.frame.At(light, accent);
    fx.glassRgb = c.glassRgb.At(light);
    fx.bevelHiRgb = c.bevelHi.At(light);
    fx.bevelLoRgb = c.bevelLo.fromAccent
        ? ScaleRgb(accent, light ? c.bevelLoGainLight : c.bevelLoGainDark)
        : c.bevelLo.At(light, accent);
    fx.labelRgb = c.label.At(light);
    fx.labelA = c.labelA.At(light);
    fx.iconA = c.iconA.At(light);
    fx.labelPx = c.labelPx > 0.5f ? c.labelPx : metrics.labelPx;

    fx.progress = c.progressFill && d.progress >= 0.f;
    if (fx.progress) {
        float p = d.progress * 0.01f;
        if (p < 0.f) {
            p = 0.f;
        }
        if (p > 1.f) {
            p = 1.f;
        }
        fx.progress01 = p;
        fx.progressA = ClampByte(c.progressBlend * 255.f);
        fx.progressFromRgb = c.progressFrom.At(light, accent);
        fx.progressToRgb = c.progressTo.fromAccent ? MixTowardWhite(accent, 0.38f) : c.progressTo.At(light, accent);
    }

    if (!d.enabled) {
        fx.bloom = false;
        fx.shadow = false;
        fx.glass = false;
        fx.iconA = ClampByte(static_cast<float>(fx.iconA) * 0.42f);
        fx.labelA = ClampByte(static_cast<float>(fx.labelA) * 0.42f);
        fx.iconGain *= 0.75f;
    }

    return fx;
}

void GlowTile::PaintChromeGdi(Gdiplus::Graphics& g, const Gdiplus::RectF& cell, const Draw& d, const TileFx& fx) const
{
    (void)d;
    const float radius = metrics.radius;
    const float maxExpand = static_cast<float>(metrics.glowPad);

    if (fx.bloom) {
        PaintBloomGdi(g, cell, radius, maxExpand * fx.bloomScale, fx.bloomAlpha, fx.bloomRgb);
    }

    if (fx.shadow && fx.shadowA >= 2) {
        if (fx.shadowEven) {
            const float cellS = metrics.cellH / 58.f;
            const float spread = ((fx.shadowScale > 0.2f) ? fx.shadowScale : 1.f) * cellS;
            for (int i = 8; i >= 1; --i) {
                const float t = static_cast<float>(i) / 8.f;
                const float a = static_cast<float>(fx.shadowA) * std::exp(-t * t * 2.1f);
                if (a < 1.f) {
                    continue;
                }
                const float ex = (6.f + t * 16.f) * spread;
                const Gdiplus::RectF halo(cell.X - ex, cell.Y - ex, cell.Width + ex * 2.f, cell.Height + ex * 2.f);
                Gdiplus::GraphicsPath sh;
                AddRoundRect(sh, halo, radius + t * 3.f);
                Gdiplus::SolidBrush br(Gp(ClampByte(a), fx.shadowRgb));
                g.FillPath(&br, &sh);
            }
        } else {
            const float sc = (fx.shadowScale > 0.2f) ? fx.shadowScale : 1.f;
            DrawDropGdi(g, cell, radius, metrics.shadowDx * sc, metrics.shadowDy * sc, metrics.shadowBlur * sc,
                fx.shadowA, fx.shadowRgb);
        }
    }

    if (fx.tint && fx.tintA > 0) {
        Gdiplus::GraphicsPath fill;
        AddRoundRect(fill, cell, radius);
        const COLORREF washRgb = fx.darken ? ScaleRgb(fx.tintRgb, fx.darkenGain) : fx.tintRgb;
        Gdiplus::SolidBrush wash(Gp(fx.tintA, washRgb));
        g.FillPath(&wash, &fill);
    }

    if (fx.progress && fx.progressA >= 2 && fx.progress01 > 0.f) {
        const float fillW = cell.Width * fx.progress01;
        if (fillW >= 0.5f) {
            Gdiplus::GraphicsPath clip;
            AddRoundRect(clip, cell, radius);
            g.SetClip(&clip, Gdiplus::CombineModeIntersect);
            Gdiplus::LinearGradientBrush bar(Gdiplus::PointF(cell.X, cell.Y), Gdiplus::PointF(cell.X + fillW, cell.Y),
                Gp(fx.progressA, fx.progressFromRgb), Gp(fx.progressA, fx.progressToRgb));
            bar.SetWrapMode(Gdiplus::WrapModeClamp);
            g.FillRectangle(&bar, cell.X, cell.Y, fillW, cell.Height);
            g.ResetClip();
        }
    }

    if (fx.glass && fx.hiA >= 2) {
        Gdiplus::GraphicsPath clip;
        AddRoundRect(clip, cell, radius);
        g.SetClip(&clip, Gdiplus::CombineModeIntersect);
        const float h = cell.Height * metrics.glassHeight;
        Gdiplus::LinearGradientBrush br(Gdiplus::PointF(cell.X, cell.Y), Gdiplus::PointF(cell.X, cell.Y + h),
            Gp(fx.hiA, fx.glassRgb), Gp(0, fx.glassRgb));
        br.SetWrapMode(Gdiplus::WrapModeClamp);
        g.FillRectangle(&br, cell.X, cell.Y, cell.Width, h);
        g.ResetClip();
    }

    if (fx.edge && cell.Width >= 8.f && cell.Height >= 8.f) {
        const float x0 = cell.X;
        const float y0 = cell.Y;
        const float x1 = cell.GetRight();
        const float y1 = cell.GetBottom();
        const float r = (std::max)(1.f, (std::min)(radius, (std::min)(cell.Width, cell.Height) * 0.5f));
        const float diam = r * 2.f;
        if (fx.edgeA > 0) {
            Gdiplus::GraphicsPath rim;
            AddRoundRect(rim, cell, radius);
            Gdiplus::Pen edge(Gp(fx.edgeA, fx.edgeRgb), fx.edgeWidth);
            edge.SetLineJoin(Gdiplus::LineJoinRound);
            g.DrawPath(&edge, &rim);
        }
        if (fx.hiA > 0) {
            Gdiplus::Pen hi(Gp(fx.hiA, fx.bevelHiRgb), 1.f);
            hi.SetStartCap(Gdiplus::LineCapRound);
            hi.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&hi, x0 + r, y0 + 0.6f, x1 - r, y0 + 0.6f);
            g.DrawLine(&hi, x0 + 0.6f, y0 + r, x0 + 0.6f, y1 - r);
            g.DrawArc(&hi, x0, y0, diam, diam, 180.f, 90.f);
        }
        if (fx.loA > 0) {
            Gdiplus::Pen lo(Gp(fx.loA, fx.bevelLoRgb), 1.f);
            lo.SetStartCap(Gdiplus::LineCapRound);
            lo.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&lo, x0 + r, y1 - 0.6f, x1 - r, y1 - 0.6f);
            g.DrawLine(&lo, x1 - 0.6f, y0 + r, x1 - 0.6f, y1 - r);
            g.DrawArc(&lo, x1 - diam, y1 - diam, diam, diam, 0.f, 90.f);
        }
    }
}

void GlowTile::PaintContentGdi(Gdiplus::Graphics& g, const Gdiplus::RectF& cell, const Draw& d, const TileFx& fx) const
{
    const BYTE ar = GetRValue(d.accent);
    const BYTE ag = GetGValue(d.accent);
    const BYTE ab = GetBValue(d.accent);
    const BYTE ir = static_cast<BYTE>(ar * fx.iconGain);
    const BYTE ig = static_cast<BYTE>(ag * fx.iconGain);
    const BYTE ib = static_cast<BYTE>(ab * fx.iconGain);
    const BYTE ia = fx.iconA;
    const bool hasCap = d.caption && d.caption[0];
    const bool labeled = d.labelMode == LabelMode::Inside && cell.Height > 40.f;
    const bool inside = labeled && hasCap && d.paintCaption;
    const bool below = d.labelMode == LabelMode::Below && hasCap && d.paintCaption;
    const float iconPx = IconDrawPx(metrics, d.labelMode, cell.Width, cell.Height);
    const float nudge = fx.pressNudge ? 1.f : 0.f;
    const float iy = std::floor((labeled ? cell.Y + metrics.iconTop : cell.Y + (cell.Height - iconPx) * 0.5f) + nudge);
    const float ix = std::floor(cell.X + (cell.Width - iconPx) * 0.5f);
    if (d.iconGdi) {
        d.iconGdi(g, Gdiplus::RectF(ix, iy, iconPx, iconPx), d.iconStem, d.state,
            Gdiplus::Color(ia, ir, ig, ib));
    }
    if (!inside && !below) {
        return;
    }
    const float inset = metrics.labelInsetX;
    const Gdiplus::RectF labelRc = inside
        ? Gdiplus::RectF(cell.X + inset, cell.Y + metrics.iconTop + iconPx + metrics.labelGap + nudge,
              cell.Width - inset * 2.f, metrics.labelH)
        : Gdiplus::RectF(cell.X - 8.f, cell.Y + cell.Height + metrics.labelBelowGap, cell.Width + 16.f,
              metrics.labelH);
    FillUiTextGdi(g, d.caption, fx.labelPx > 0.5f ? fx.labelPx : metrics.labelPx, labelRc,
        Gp(fx.labelA, fx.labelRgb), Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter, true);
}

bool GlowTile::PaintGdi(HDC dest, const RECT& cell, const Draw& d) const
{
    if (!dest) {
        return false;
    }
    const int pad = ChromeBleed();
    const RECT bleed = PaintBounds(cell, d.labelMode, d.caption && d.caption[0]);
    const ClipBleed unclip(dest, bleed);
    const int cw = cell.right - cell.left;
    const int ch = cell.bottom - cell.top;
    if (cw <= 2 || ch <= 2) {
        return false;
    }
    const int below = (d.labelMode == LabelMode::Below && d.caption && d.caption[0])
        ? static_cast<int>(metrics.labelBelowGap + metrics.labelH + 2.f)
        : 0;
    const int bw = cw + pad * 2;
    const int bh = ch + pad * 2 + below;
    if (!EnsureScratch(bw, bh)) {
        return false;
    }
    {
        Gdiplus::Bitmap wrap(g_scratch.capW, g_scratch.capH, g_scratch.capW * 4, PixelFormat32bppPARGB,
            static_cast<BYTE*>(g_scratch.bits));
        Gdiplus::Graphics g(&wrap);
        if (g.GetLastStatus() == Gdiplus::Ok) {
            g.SetPageUnit(Gdiplus::UnitPixel);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
            g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
            g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
            const Gdiplus::RectF local(static_cast<float>(pad), static_cast<float>(pad), static_cast<float>(cw),
                static_cast<float>(ch));
            const TileFx fx = Resolve(d);
            PaintChromeGdi(g, local, d, fx);
            PaintContentGdi(g, local, d, fx);
        }
    }

    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    return AlphaBlend(dest, cell.left - pad, cell.top - pad, bw, bh, g_scratch.mem, 0, 0, bw, bh, bf) != FALSE;
}

void GlowTile::PaintD2d(ID2D1RenderTarget* rt, ID2D1Factory* factory, IDWriteFactory* dwrite,
    IDWriteTextFormat* labelTf, const RECT& cell, const Draw& d) const
{
    if (!rt) {
        return;
    }
    const TileFx fx = Resolve(d);
    const D2D1_RECT_F rc = D2D1::RectF(static_cast<float>(cell.left), static_cast<float>(cell.top),
        static_cast<float>(cell.right), static_cast<float>(cell.bottom));
    const float radius = metrics.radius;
    const float maxExpand = static_cast<float>(metrics.glowPad);

    if (fx.bloom) {
        PaintBloomD2d(rt, rc, radius, maxExpand * fx.bloomScale, fx.bloomAlpha, fx.bloomRgb);
    }

    if (fx.shadow && fx.shadowA >= 2) {
        if (fx.shadowEven) {
            const float peakA = fx.shadowA / 255.f;
            const float cellS = metrics.cellH / 58.f;
            const float spread = ((fx.shadowScale > 0.2f) ? fx.shadowScale : 1.f) * cellS;
            for (int i = 8; i >= 1; --i) {
                const float t = static_cast<float>(i) / 8.f;
                const float a = peakA * std::exp(-t * t * 2.1f);
                if (a < 1.f / 255.f) {
                    continue;
                }
                const float ex = (6.f + t * 16.f) * spread;
                const D2D1_RECT_F halo = D2D1::RectF(rc.left - ex, rc.top - ex, rc.right + ex, rc.bottom + ex);
                ID2D1SolidColorBrush* br = nullptr;
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2Color(fx.shadowRgb, a), &br))) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(halo, radius + t * 3.f, radius + t * 3.f), br);
                    br->Release();
                }
            }
        } else {
            const float sc = (fx.shadowScale > 0.2f) ? fx.shadowScale : 1.f;
            const Gdiplus::RectF cell(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
            DrawDropD2d(rt, cell, radius, metrics.shadowDx * sc, metrics.shadowDy * sc, metrics.shadowBlur * sc,
                fx.shadowA, fx.shadowRgb);
        }
    }

    if (fx.tint && fx.tintA > 0) {
        const COLORREF washRgb = fx.darken ? ScaleRgb(fx.tintRgb, fx.darkenGain) : fx.tintRgb;
        ID2D1SolidColorBrush* wash = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2Color(washRgb, fx.tintA / 255.f), &wash))) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), wash);
            wash->Release();
        }
    }

    if (fx.progress && fx.progressA >= 2 && fx.progress01 > 0.f) {
        const float fillW = (rc.right - rc.left) * fx.progress01;
        if (fillW >= 0.5f) {
            D2D1_GRADIENT_STOP stops[2]{};
            stops[0].position = 0.f;
            stops[0].color = D2Color(fx.progressFromRgb, fx.progressA / 255.f);
            stops[1].position = 1.f;
            stops[1].color = D2Color(fx.progressToRgb, fx.progressA / 255.f);
            ID2D1GradientStopCollection* col = nullptr;
            if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &col))) {
                ID2D1LinearGradientBrush* br = nullptr;
                const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = {D2D1::Point2F(rc.left, rc.top),
                    D2D1::Point2F(rc.left + fillW, rc.top)};
                if (SUCCEEDED(rt->CreateLinearGradientBrush(props, col, &br)) && br) {
                    rt->PushAxisAlignedClip(D2D1::RectF(rc.left, rc.top, rc.left + fillW, rc.bottom),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    rt->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), br);
                    rt->PopAxisAlignedClip();
                    br->Release();
                }
                col->Release();
            }
        }
    }

    if (fx.glass && fx.hiA >= 2) {
        const float y1 = rc.top + (rc.bottom - rc.top) * metrics.glassHeight;
        D2D1_GRADIENT_STOP stops[2]{};
        stops[0].position = 0.f;
        stops[0].color = D2Color(fx.glassRgb, fx.hiA / 255.f);
        stops[1].position = 1.f;
        stops[1].color = D2D1::ColorF(stops[0].color.r, stops[0].color.g, stops[0].color.b, 0.f);
        ID2D1GradientStopCollection* col = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &col))) {
            ID2D1LinearGradientBrush* br = nullptr;
            const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = {D2D1::Point2F(rc.left, rc.top),
                D2D1::Point2F(rc.left, y1)};
            if (SUCCEEDED(rt->CreateLinearGradientBrush(props, col, &br)) && br) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), br);
                br->Release();
            }
            col->Release();
        }
    }

    if (fx.edge) {
        const float x0 = rc.left;
        const float y0 = rc.top;
        const float x1 = rc.right;
        const float y1 = rc.bottom;
        const float r = (std::max)(1.f, (std::min)(radius, (std::min)(x1 - x0, y1 - y0) * 0.5f));
        if (fx.edgeA > 0) {
            ID2D1SolidColorBrush* edge = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2Color(fx.edgeRgb, fx.edgeA / 255.f), &edge))) {
                rt->DrawRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), edge, fx.edgeWidth);
                edge->Release();
            }
        }
        if (fx.hiA > 0) {
            ID2D1SolidColorBrush* hi = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2Color(fx.bevelHiRgb, fx.hiA / 255.f), &hi))) {
                rt->DrawLine(D2D1::Point2F(x0 + r, y0 + 0.6f), D2D1::Point2F(x1 - r, y0 + 0.6f), hi, 1.f);
                rt->DrawLine(D2D1::Point2F(x0 + 0.6f, y0 + r), D2D1::Point2F(x0 + 0.6f, y1 - r), hi, 1.f);
                DrawCornerArcD2d(factory, rt, D2D1::Point2F(x0, y0 + r), D2D1::Point2F(x0 + r, y0), r, hi, 1.f);
                hi->Release();
            }
        }
        if (fx.loA > 0) {
            ID2D1SolidColorBrush* lo = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2Color(fx.bevelLoRgb, fx.loA / 255.f), &lo))) {
                rt->DrawLine(D2D1::Point2F(x0 + r, y1 - 0.6f), D2D1::Point2F(x1 - r, y1 - 0.6f), lo, 1.f);
                rt->DrawLine(D2D1::Point2F(x1 - 0.6f, y0 + r), D2D1::Point2F(x1 - 0.6f, y1 - r), lo, 1.f);
                DrawCornerArcD2d(factory, rt, D2D1::Point2F(x1, y1 - r), D2D1::Point2F(x1 - r, y1), r, lo, 1.f);
                lo->Release();
            }
        }
    }

    const float ia = fx.iconA / 255.f;
    const D2D1_COLOR_F ic = D2Color(d.accent, ia);
    const D2D1_COLOR_F iconC = D2D1::ColorF(ic.r * fx.iconGain, ic.g * fx.iconGain, ic.b * fx.iconGain, ic.a);
    const bool hasCap = d.caption && d.caption[0];
    const bool labeled = d.labelMode == LabelMode::Inside && (rc.bottom - rc.top) > 40.f;
    const bool inside = labeled && hasCap && d.paintCaption;
    const bool below = d.labelMode == LabelMode::Below && hasCap && d.paintCaption;
    const float nudge = fx.pressNudge ? 1.f : 0.f;
    const float cellW = rc.right - rc.left;
    const float cellH = rc.bottom - rc.top;
    const float iconPx = IconDrawPx(metrics, d.labelMode, cellW, cellH);
    const float iy = (labeled ? rc.top + metrics.iconTop : rc.top + (cellH - iconPx) * 0.5f) + nudge;
    const float ix = rc.left + (cellW - iconPx) * 0.5f;
    const D2D1_RECT_F iconBox = D2D1::RectF(ix, iy, ix + iconPx, iy + iconPx);
    if (d.iconD2d) {
        d.iconD2d(rt, factory, iconBox, d.iconStem, d.state, iconC);
    }

    IDWriteTextFormat* tf = labelTf;
    IDWriteTextFormat* owned = nullptr;
    if (dwrite && fx.labelPx > 0.5f && std::fabs(fx.labelPx - metrics.labelPx) > 0.05f) {
        if (SUCCEEDED(dwrite->CreateTextFormat(UiFontFace(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fx.labelPx, L"en-us", &owned))
            && owned) {
            owned->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            owned->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            owned->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            {
                IDWriteInlineObject* ellipsis = nullptr;
                if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(owned, &ellipsis)) && ellipsis) {
                    DWRITE_TRIMMING trim{};
                    trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                    owned->SetTrimming(&trim, ellipsis);
                    ellipsis->Release();
                }
            }
            tf = owned;
        }
    }
    if ((inside || below) && tf) {
        ID2D1SolidColorBrush* text = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2Color(fx.labelRgb, fx.labelA / 255.f), &text))) {
            const float inset = metrics.labelInsetX;
            const D2D1_RECT_F labelRc = inside
                ? D2D1::RectF(rc.left + inset, rc.top + metrics.iconTop + iconPx + metrics.labelGap + nudge,
                      rc.right - inset,
                      rc.top + metrics.iconTop + iconPx + metrics.labelGap + metrics.labelH + nudge)
                : D2D1::RectF(rc.left - 8.f, rc.bottom + metrics.labelBelowGap, rc.right + 8.f,
                      rc.bottom + metrics.labelBelowGap + metrics.labelH);
            rt->DrawText(d.caption, static_cast<UINT32>(wcslen(d.caption)), tf, labelRc, text);
            text->Release();
        }
    }
    if (owned) {
        owned->Release();
    }
}

} // namespace glowplay
