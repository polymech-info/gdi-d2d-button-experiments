// Film strip experiment: Direct2D + WIC on top of the existing ULW / GDI+ shell (see FilmStrip.h).
// Thumbs use WIC scaling; DC render target uses GDI-compatible mode for the shared 32bpp DIB.

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Thumb class headers

// Include Win32 before d2d1.h, then undef GDI/User macros that break D2D interface declarations.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef CreateSolidBrush
#undef DrawText

#include <d2d1.h>
#include <d2d1helper.h>
#undef CreateSolidBrush
#undef DrawText

#include <dwrite.h>
#include <objbase.h>
#include <wincodec.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "ole32.lib")

#ifdef CreateSolidBrush
#undef CreateSolidBrush
#endif
#ifdef DrawText
#undef DrawText
#endif
#include "FilmStrip.h"
#include "FilmStripThumb.h"

namespace fs = std::filesystem;

namespace filmstrip {

constexpr UINT_PTR kAnimTimerId = 0x504D4653u; // 'PMFS'

// Cover-flow hover constants (defined early so layout can reference them).
constexpr float kFocusMaxBoost = 0.45f;
constexpr float kMaxScale = 1.0f + kFocusMaxBoost; // 1.45f

// Layout constants (tunable via options::layout in FilmStrip.h)
constexpr int kThumbPx = options::layout::thumbSize;
constexpr int kThumbGap = options::layout::thumbGap;
constexpr int kPreviewPad = options::layout::previewPad;
constexpr int kPadLeft = options::layout::padding::left;
constexpr int kPadRight = options::layout::padding::right;

// Scale-aware layout:
// The scaled thumb (145px) grows from its center, with additional glow/shadow/halo overflow.
// We need space for: visual overflow + scaled thumb + visual overflow, plus content padding.
constexpr int kMaxThumbPx = static_cast<int>(static_cast<float>(kThumbPx) * kMaxScale);
// Total vertical space needed = padding + overflow + scaled thumb + overflow
constexpr int kDockH = options::layout::padding::top + options::layout::visual::overflowTop
                       + kMaxThumbPx
                       + options::layout::visual::overflowBottom + options::layout::padding::bottom;
// The "content area" for thumbs starts after padding+overflow, sized to fit max thumb centered
constexpr int kContentTop = options::layout::padding::top + options::layout::visual::overflowTop;
// Center of scaled thumb within the content area
constexpr float kScaledThumbCenterY = static_cast<float>(kContentTop) + static_cast<float>(kMaxThumbPx) * 0.5f;
// Unscaled thumb shares same center
constexpr int kThumbYInDock = static_cast<int>(kScaledThumbCenterY - static_cast<float>(kThumbPx) * 0.5f);

constexpr int kThumbDecodeMax = 600;
constexpr int kPreviewMaxEdge = 1600;

// Cover-flow hover tuning (smoothstep falloff so ~2+ neighbours each side stay visibly lifted).
constexpr float kFocusInfluenceRadiusCells = 1.55f;
constexpr float kFocusSpringAccel = 0.34f;
constexpr float kFocusSpringDrag = 0.74f;
constexpr float kFocusLerpStrength = 0.26f;

template <typename T>
void Release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void ClearBitmapMap();

bool EndsWithExt(const std::wstring& name, const wchar_t* ext)
{
    const size_t nl = name.size();
    const size_t el = wcslen(ext);
    if (nl < el) {
        return false;
    }
    for (size_t i = 0; i < el; ++i) {
        if (towlower(name[nl - el + i]) != towlower(ext[i])) {
            return false;
        }
    }
    return true;
}

bool IsImageFile(const fs::path& p)
{
    const std::wstring w = p.wstring();
    return EndsWithExt(w, L".jpg") || EndsWithExt(w, L".jpeg") || EndsWithExt(w, L".png");
}

HWND g_host = nullptr;
std::vector<std::wstring> g_paths;
std::vector<std::unique_ptr<Thumb>> g_thumbs;  // New: Thumb objects for rendering
size_t g_sel = 0;
float g_scroll = 0.f;
float g_scrollTarget = 0.f;
bool g_animTimer = false;

// Smoothed pointer focus in "thumb index" space (fractional OK). Strength gates the whole effect off-strip.
float g_focusCenter = 0.f;
float g_focusStrength = 0.f;
float g_focusCenterTarget = 0.f;
float g_focusStrengthTarget = 0.f;
float g_focusVel = 0.f;

// Last known pointer (client coords) so wheel / scroll animation can refresh focus under cursor.
int g_lastClientX = 0;
int g_lastClientY = 0;
int g_lastWindowW = 0;
int g_lastWindowH = 0;

ID2D1Factory* g_d2d = nullptr;
ID2D1DCRenderTarget* g_dcRt = nullptr;
IWICImagingFactory* g_wic = nullptr;
IDWriteFactory* g_dwrite = nullptr;
IDWriteTextFormat* g_fmtUi = nullptr;

int g_rtW = 0;
int g_rtH = 0;
UINT g_rtDpi = 0;

struct BitmapEntry {
    ID2D1Bitmap* bmp = nullptr;
};

std::unordered_map<std::wstring, BitmapEntry> g_thumbBmps;
std::wstring g_previewPath;
ID2D1Bitmap* g_previewBmp = nullptr;
bool g_didInitialScroll = false;

bool EnsureFactories()
{
    if (g_d2d && g_wic && g_dwrite) {
        return true;
    }
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) {
        return false;
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
            reinterpret_cast<void**>(&g_wic)))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&g_dwrite)))) {
        return false;
    }
    if (FAILED(g_dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.f, L"en-us", &g_fmtUi))) {
        if (FAILED(g_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.f, L"en-us", &g_fmtUi))) {
            return false;
        }
    }
    g_fmtUi->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmtUi->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return true;
}

bool EnsureDcRenderTarget(int w, int h)
{
    if (!g_d2d) {
        return false;
    }
    const UINT dpiNow = g_host ? GetDpiForWindow(g_host) : 96u;
    if (g_dcRt && w == g_rtW && h == g_rtH && dpiNow == g_rtDpi) {
        return true;
    }

    // Bitmaps are bound to the previous DC render target; the ULW DIB is also recreated on resize.
    Release(g_dcRt);
    ClearBitmapMap();
    // ImageThumb bitmaps are also bound to the old render target — invalidate them.
    for (auto& t : g_thumbs) {
        t->Unload();
    }
    g_rtW = 0;
    g_rtH = 0;
    g_rtDpi = 0;

    D2D1_RENDER_TARGET_PROPERTIES rtp{};
    rtp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rtp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    rtp.dpiX = 0.f;
    rtp.dpiY = 0.f;
    rtp.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;

    if (FAILED(g_d2d->CreateDCRenderTarget(&rtp, &g_dcRt)) || !g_dcRt) {
        return false;
    }
    g_dcRt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_dcRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    g_dcRt->SetDpi(static_cast<float>(dpiNow), static_cast<float>(dpiNow));
    g_rtW = w;
    g_rtH = h;
    g_rtDpi = dpiNow;
    return true;
}

void ClearBitmapMap()
{
    for (auto& e : g_thumbBmps) {
        Release(e.second.bmp);
    }
    g_thumbBmps.clear();
    Release(g_previewBmp);
    g_previewPath.clear();
}

void ClearThumbs()
{
    g_thumbs.clear();
}

void SyncThumbsFromPaths()
{
    // Rebuild thumb array from paths (clear and recreate)
    ClearThumbs();
    g_thumbs.reserve(g_paths.size());
    for (const auto& path : g_paths) {
        auto thumb = ThumbFactory::CreateFromPath(path);
        if (thumb) {
            g_thumbs.push_back(std::move(thumb));
        }
    }
}

HRESULT CreateScaledWicBitmap(const std::wstring& path, UINT maxEdge, IWICBitmapSource** outSource)
{
    if (!g_wic || maxEdge < 8) {
        return E_INVALIDARG;
    }
    IWICBitmapDecoder* dec = nullptr;
    HRESULT hr = g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr) || !dec) {
        return hr;
    }
    IWICBitmapFrameDecode* frame = nullptr;
    hr = dec->GetFrame(0, &frame);
    Release(dec);
    if (FAILED(hr) || !frame) {
        return hr;
    }

    UINT iw = 0;
    UINT ih = 0;
    (void)frame->GetSize(&iw, &ih);
    if (iw == 0 || ih == 0) {
        Release(frame);
        return E_FAIL;
    }

    UINT tw = iw;
    UINT th = ih;
    const UINT m = (std::max)(iw, ih);
    if (m > maxEdge) {
        const double scale = static_cast<double>(maxEdge) / static_cast<double>(m);
        tw = static_cast<UINT>(std::lround(static_cast<double>(iw) * scale));
        th = static_cast<UINT>(std::lround(static_cast<double>(ih) * scale));
        tw = (std::max)(tw, 1u);
        th = (std::max)(th, 1u);
    }

    IWICBitmapScaler* scaler = nullptr;
    hr = g_wic->CreateBitmapScaler(&scaler);
    if (FAILED(hr) || !scaler) {
        Release(frame);
        return hr;
    }
    hr = scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant);
    Release(frame);
    if (FAILED(hr)) {
        Release(scaler);
        return hr;
    }

    IWICFormatConverter* conv = nullptr;
    hr = g_wic->CreateFormatConverter(&conv);
    if (FAILED(hr) || !conv) {
        Release(scaler);
        return hr;
    }
    hr = conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.f,
        WICBitmapPaletteTypeMedianCut);
    Release(scaler);
    if (FAILED(hr)) {
        Release(conv);
        return hr;
    }
    *outSource = conv;
    return S_OK;
}

ID2D1Bitmap* BitmapFromPath(ID2D1RenderTarget* rt, const std::wstring& path, UINT maxEdge)
{
    IWICBitmapSource* src = nullptr;
    if (FAILED(CreateScaledWicBitmap(path, maxEdge, &src)) || !src) {
        return nullptr;
    }
    ID2D1Bitmap* bmp = nullptr;
    const HRESULT hr = rt->CreateBitmapFromWicBitmap(src, nullptr, &bmp);
    Release(src);
    if (FAILED(hr)) {
        return nullptr;
    }
    return bmp;
}

ID2D1Bitmap* GetThumbBitmap(const std::wstring& path)
{
    if (!g_dcRt) {
        return nullptr;
    }
    auto it = g_thumbBmps.find(path);
    if (it != g_thumbBmps.end() && it->second.bmp) {
        return it->second.bmp;
    }
    ID2D1Bitmap* bmp = BitmapFromPath(g_dcRt, path, kThumbDecodeMax);
    if (bmp) {
        g_thumbBmps[path] = BitmapEntry{bmp};
    }
    return bmp;
}

void EnsurePreviewForSelection()
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (!g_dcRt || count == 0) {
        return;
    }

    // Get path from either g_paths or g_thumbs
    std::wstring p;
    if (g_sel < g_paths.size()) {
        p = g_paths[g_sel];
    } else if (g_sel < g_thumbs.size()) {
        p = g_thumbs[g_sel]->GetPath();
    } else {
        return;
    }

    if (g_previewBmp && g_previewPath == p) {
        return;
    }
    Release(g_previewBmp);
    g_previewBmp = BitmapFromPath(g_dcRt, p, kPreviewMaxEdge);
    g_previewPath = p;
}

float CellStride()
{
    return static_cast<float>(kThumbPx + kThumbGap);
}

float ContentWidth(int windowW)
{
    return static_cast<float>((std::max)(0, windowW - kPadLeft - kPadRight));
}

float ThumbFocusScale(size_t i);

float ThumbWidth(size_t i)
{
    return static_cast<float>(kThumbPx) * ThumbFocusScale(i);
}

float TotalThumbRowWidth()
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (count == 0) {
        return 0.f;
    }
    float sum = 0.f;
    for (size_t i = 0; i < count; ++i) {
        sum += ThumbWidth(i);
        if (i + 1 < count) {
            sum += static_cast<float>(kThumbGap);
        }
    }
    return sum;
}

// Width of the strip if every thumb were scale=1 (stable hover bounds — avoids layout↔pointer feedback).
float NominalThumbRowWidth()
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (count == 0) {
        return 0.f;
    }
    return static_cast<float>(count) * static_cast<float>(kThumbPx)
        + static_cast<float>((count - 1) * kThumbGap);
}

void ComputeThumbLeftEdges(float x0, std::vector<float>& outLeft)
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    outLeft.resize(count);
    float x = x0;
    for (size_t i = 0; i < count; ++i) {
        outLeft[i] = x;
        x += ThumbWidth(i);
        if (i + 1 < count) {
            x += static_cast<float>(kThumbGap);
        }
    }
}

// `PresentLayered` sizes the ULW DIB from `GetWindowRect`; WM_* mouse coords are client-relative.
// Map client → same pixel space as D2D/GDI+ so hit-testing and hover line up on framed / DPI setups.
static bool GetHostLayerExtents(int* outW, int* outH)
{
    if (!g_host || !outW || !outH) {
        return false;
    }
    RECT wr{};
    if (!GetWindowRect(g_host, &wr)) {
        return false;
    }
    *outW = wr.right - wr.left;
    *outH = wr.bottom - wr.top;
    return *outW > 0 && *outH > 0;
}

static int EffectiveLayerW(int fallbackW)
{
    int w = 0;
    int h = 0;
    if (g_host && GetHostLayerExtents(&w, &h)) {
        (void)h;
        return w;
    }
    return fallbackW;
}

static float ClientXToLayerX(float clientX)
{
    if (!g_host) {
        return clientX;
    }
    RECT wr{};
    if (!GetWindowRect(g_host, &wr)) {
        return clientX;
    }
    POINT o{0, 0};
    ClientToScreen(g_host, &o);
    return clientX + static_cast<float>(o.x - wr.left);
}

static float ClientYToLayerY(float clientY)
{
    if (!g_host) {
        return clientY;
    }
    RECT wr{};
    if (!GetWindowRect(g_host, &wr)) {
        return clientY;
    }
    POINT o{0, 0};
    ClientToScreen(g_host, &o);
    return clientY + static_cast<float>(o.y - wr.top);
}

float MaxScroll(int windowW)
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (count == 0) {
        return 0.f;
    }
    const float row = TotalThumbRowWidth();
    const int w = EffectiveLayerW(windowW);
    const float vw = ContentWidth(w);
    return (std::max)(0.f, row - vw);
}

void ClampScrollTarget(int windowW)
{
    const int w = EffectiveLayerW(windowW);
    if (w <= 0) {
        return;
    }
    g_scrollTarget = (std::clamp)(g_scrollTarget, 0.f, MaxScroll(w));
}

void CenterOnSelection(int windowW)
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (count == 0 || g_sel >= count) {
        return;
    }
    float c = 0.f;
    for (size_t i = 0; i < g_sel; ++i) {
        c += ThumbWidth(i) + static_cast<float>(kThumbGap);
    }
    const float tw = ThumbWidth(g_sel);
    const int w = EffectiveLayerW(windowW);
    const float vw = ContentWidth(w);
    g_scrollTarget = c + tw * 0.5f - vw * 0.5f;
    ClampScrollTarget(w);
}

void KickAnim()
{
    if (!g_host || !FEATURE_FILMSTRIP) {
        return;
    }
    if (!g_animTimer) {
        SetTimer(g_host, kAnimTimerId, 16, nullptr);
        g_animTimer = true;
    }
}

bool FocusAnimSettled()
{
    if (!options::focusAnimation) {
        return true;
    }
    const float dc = std::fabs(g_focusCenter - g_focusCenterTarget);
    const float ds = std::fabs(g_focusStrength - g_focusStrengthTarget);
    const float dv = std::fabs(g_focusVel);
    return dc < 0.004f && ds < 0.004f && dv < 0.008f;
}

void StopAnimIfIdle()
{
    if (!g_host) {
        return;
    }
    const float d = std::fabs(g_scroll - g_scrollTarget);
    const bool scrollIdle = d < 0.35f;
    if (scrollIdle && FocusAnimSettled() && g_animTimer) {
        g_scroll = g_scrollTarget;
        if (options::focusAnimation) {
            g_focusCenter = g_focusCenterTarget;
            g_focusStrength = g_focusStrengthTarget;
            g_focusVel = 0.f;
        }
        KillTimer(g_host, kAnimTimerId);
        g_animTimer = false;
    }
}

bool StepAnimScroll()
{
    const float d = g_scrollTarget - g_scroll;
    if (std::fabs(d) < 0.25f) {
        if (g_scroll != g_scrollTarget) {
            g_scroll = g_scrollTarget;
            return true;
        }
        return false;
    }
    g_scroll += d * 0.38f;
    return true;
}

bool StepFocusAnim()
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (!options::focusAnimation || count == 0) {
        g_focusVel = 0.f;
        return false;
    }
    bool moved = false;
    const float lastIdx = static_cast<float>(count - 1);
    const float err = g_focusCenterTarget - g_focusCenter;
    g_focusVel += err * kFocusSpringAccel;
    g_focusVel *= kFocusSpringDrag;
    g_focusCenter += g_focusVel;
    g_focusCenter = (std::clamp)(g_focusCenter, 0.f, lastIdx);
    if (g_focusCenter <= 0.f && g_focusVel < 0.f) {
        g_focusVel = 0.f;
    }
    if (g_focusCenter >= lastIdx && g_focusVel > 0.f) {
        g_focusVel = 0.f;
    }
    if (std::fabs(err) > 0.0012f || std::fabs(g_focusVel) > 0.006f) {
        moved = true;
    }
    const float ds = g_focusStrengthTarget - g_focusStrength;
    if (std::fabs(ds) > 0.0015f) {
        g_focusStrength += ds * kFocusLerpStrength;
        moved = true;
    } else if (ds != 0.f) {
        g_focusStrength = g_focusStrengthTarget;
        moved = true;
    }
    return moved;
}

float StripTopY(int windowH)
{
    return static_cast<float>(windowH - kDockH);
}

bool PtInStrip(int clientY, int windowH)
{
    (void)windowH;
    int lw = 0;
    int lh = 0;
    if (!GetHostLayerExtents(&lw, &lh)) {
        return false;
    }
    const float ly = ClientYToLayerY(static_cast<float>(clientY));
    return ly >= StripTopY(lh) && ly < static_cast<float>(lh);
}

int HitThumbIndex(float clientX, int clientY, int windowW, int windowH)
{
    (void)windowH;
    if (!PtInStrip(clientY, 0)) {
        return -1;
    }
    const float layerX = ClientXToLayerX(clientX);
    const float x0 = static_cast<float>(kPadLeft) - g_scroll;
    const float localX = layerX - x0;
    if (localX < 0.f) {
        return -1;
    }
    float pos = 0.f;
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    for (size_t i = 0; i < count; ++i) {
        const float w = ThumbWidth(i);
        if (localX < pos + w) {
            (void)windowW;
            return static_cast<int>(i);
        }
        pos += w;
        if (i + 1 < count) {
            const float gapEnd = pos + static_cast<float>(kThumbGap);
            if (localX < gapEnd) {
                return -1;
            }
            pos = gapEnd;
        }
    }
    (void)windowW;
    return -1;
}

float SmoothStep01(float t)
{
    t = (std::clamp)(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

float ThumbFocusScale(size_t i)
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (!options::focusAnimation || count == 0 || g_focusStrength <= 0.0001f) {
        return 1.f;
    }
    const float d = std::fabs(g_focusCenter - static_cast<float>(i));
    if (d >= kFocusInfluenceRadiusCells) {
        return 1.f;
    }
    const float t = 1.f - d / kFocusInfluenceRadiusCells;
    const float w = SmoothStep01(t);
    return 1.f + kFocusMaxBoost * g_focusStrength * w;
}

// `left` is the left edge of the scaled square (flow layout); neighbors are pushed by cumulative widths.
D2D1_ROUNDED_RECT ScaledThumbRectFromLeft(float left, float yThumb, float scale)
{
    const float w = static_cast<float>(kThumbPx) * scale;
    const float cx = left + w * 0.5f;
    const float cy = yThumb + static_cast<float>(kThumbPx) * 0.5f;
    const float half = w * 0.5f;
    const float rad = 9.f * scale;
    return D2D1::RoundedRect(D2D1::RectF(cx - half, cy - half, cx + half, cy + half), rad, rad);
}

void DrawThumbDropShadow(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& baseRr, float shadowSize, float hardness)
{
    if (shadowSize <= 0.5f) {
        return;
    }
    const float h = (std::clamp)(hardness, 0.f, 1.f);
    const int layers = 4 + static_cast<int>(shadowSize * 0.4f + h * 5.f);
    const int L = (std::clamp)(layers, 4, 14);
    for (int k = L; k >= 1; --k) {
        const float t = static_cast<float>(k) / static_cast<float>(L);
        const float inflate = shadowSize * t;
        D2D1_ROUNDED_RECT sh = baseRr;
        sh.rect.left -= inflate;
        sh.rect.top -= inflate;
        sh.rect.right += inflate;
        sh.rect.bottom += inflate;
        sh.radiusX += inflate * 0.35f;
        sh.radiusY += inflate * 0.35f;
        const float u = static_cast<float>(L - k + 1) / static_cast<float>(L);
        const float falloff = std::pow((std::max)(0.f, 1.f - u) + 0.08f, 1.f + h * 5.5f);
        const float a = (0.055f + 0.26f * h) * falloff * ((std::min)(shadowSize, 14.f) / 14.f * 0.55f + 0.45f);
        ID2D1SolidColorBrush* br = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, a), &br)) && br) {
            rt->FillRoundedRectangle(sh, br);
            Release(br);
        }
    }
}

void DrawRoundedThumb(ID2D1RenderTarget* rt, ID2D1Bitmap* bmp, const D2D1_ROUNDED_RECT& rr, float opacity)
{
    if (!bmp) {
        return;
    }
    const float bw = static_cast<float>(bmp->GetPixelSize().width);
    const float bh = static_cast<float>(bmp->GetPixelSize().height);
    const float cw = rr.rect.right - rr.rect.left;
    const float ch = rr.rect.bottom - rr.rect.top;
    if (bw < 1.f || bh < 1.f || cw < 1.f || ch < 1.f) {
        return;
    }
    const float s = (options::thumbOptions::fill == options::thumbOptions::ThumbFill::Cover) ? (std::max)(cw / bw, ch / bh) : (std::min)(cw / bw, ch / bh);
    const float dw = bw * s;
    const float dh = bh * s;
    const float ox = rr.rect.left + (cw - dw) * 0.5f;
    const float oy = rr.rect.top + (ch - dh) * 0.5f;

    D2D1_RECT_F dst{ox, oy, ox + dw, oy + dh};
    D2D1_RECT_F src{0.f, 0.f, bw, bh};

    ID2D1RoundedRectangleGeometry* geo = nullptr;
    if (FAILED(g_d2d->CreateRoundedRectangleGeometry(rr, &geo)) || !geo) {
        return;
    }
    D2D1_LAYER_PARAMETERS lp{};
    lp.contentBounds = D2D1::InfiniteRect();
    lp.geometricMask = geo;
    lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    lp.maskTransform = D2D1::IdentityMatrix();
    lp.opacity = opacity;
    lp.opacityBrush = nullptr;
    lp.layerOptions = D2D1_LAYER_OPTIONS_NONE;
    rt->PushLayer(&lp, nullptr);
    rt->DrawBitmap(bmp, &dst, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
    rt->PopLayer();
    Release(geo);
}

void DrawSelectionHalo(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& rr)
{
    ID2D1SolidColorBrush* b = nullptr;
    for (int i = 3; i >= 1; --i) {
        const float inflate = static_cast<float>(i) * 2.2f;
        const float a = 0.055f * static_cast<float>(i);
        D2D1_ROUNDED_RECT outer = rr;
        outer.rect.left -= inflate;
        outer.rect.top -= inflate;
        outer.rect.right += inflate;
        outer.rect.bottom += inflate;
        outer.radiusX += inflate * 0.35f;
        outer.radiusY += inflate * 0.35f;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.88f, 1.f, a * 0.92f), &b)) && b) {
            rt->FillRoundedRectangle(outer, b);
            Release(b);
        }
    }
}

void DrawStripChrome(ID2D1RenderTarget* rt, const D2D1_RECT_F& stripRc, float windowW)
{
    ID2D1LinearGradientBrush* grad = nullptr;
    ID2D1GradientStopCollection* stops = nullptr;
    const D2D1_GRADIENT_STOP gs[4] = {
        {0.f, D2D1::ColorF(0.02f, 0.03f, 0.06f, 0.f)},
        {0.38f, D2D1::ColorF(0.04f, 0.06f, 0.11f, 0.38f)},
        {0.74f, D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.50f)},
        {1.f, D2D1::ColorF(0.02f, 0.03f, 0.08f, 0.62f)},
    };
    if (SUCCEEDED(rt->CreateGradientStopCollection(gs, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stops)) && stops) {
        const D2D1_POINT_2F p0{stripRc.left, stripRc.top - static_cast<float>(options::layout::padding::top)};
        const D2D1_POINT_2F p1{stripRc.left, stripRc.bottom};
        const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lgp = D2D1::LinearGradientBrushProperties(p0, p1);
        const D2D1_BRUSH_PROPERTIES bp = D2D1::BrushProperties(1.f, D2D1::IdentityMatrix());
        if (SUCCEEDED(rt->CreateLinearGradientBrush(&lgp, &bp, stops, &grad)) && grad) {
            rt->FillRectangle(stripRc, grad);
        }
        Release(grad);
        Release(stops);
    }

    ID2D1SolidColorBrush* line = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.45f, 0.82f, 0.98f, 0.32f), &line)) && line) {
        rt->DrawLine(D2D1::Point2F(14.f, stripRc.top + 0.5f), D2D1::Point2F(windowW - 14.f, stripRc.top + 0.5f), line, 1.f);
        Release(line);
    }
    if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.62f, 0.42f, 0.98f, 0.18f), &line)) && line) {
        rt->DrawLine(D2D1::Point2F(14.f, stripRc.top + 2.f), D2D1::Point2F(windowW * 0.5f, stripRc.top + 2.f), line, 0.75f);
        Release(line);
    }
}

void DrawLetterboxedPreview(ID2D1RenderTarget* rt, ID2D1Bitmap* bmp, const D2D1_RECT_F& box)
{
    if (!bmp) {
        return;
    }
    const float bw = static_cast<float>(bmp->GetPixelSize().width);
    const float bh = static_cast<float>(bmp->GetPixelSize().height);
    const float cw = box.right - box.left;
    const float ch = box.bottom - box.top;
    if (cw < 2.f || ch < 2.f) {
        return;
    }
    const float s = (std::min)(cw / bw, ch / bh);
    const float dw = bw * s;
    const float dh = bh * s;
    const float ox = box.left + (cw - dw) * 0.5f;
    const float oy = box.top + (ch - dh) * 0.5f;
    D2D1_RECT_F dst{ox, oy, ox + dw, oy + dh};
    D2D1_RECT_F src{0.f, 0.f, bw, bh};

    // No background fill - let host window background show through
    // No border - clean edge-to-edge image display
    rt->DrawBitmap(bmp, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
}

void RequestPaint()
{
    if (g_host) {
        InvalidateRect(g_host, nullptr, FALSE);
    }
}

int DockHeightPx()
{
    if (!FEATURE_FILMSTRIP) {
        return 0;
    }
    return kDockH;
}

bool InitFromArgv(int argc, wchar_t** argv)
{
    if (!FEATURE_FILMSTRIP) {
        return true;
    }
    g_didInitialScroll = false;
    g_paths.clear();
    g_sel = 0;
    g_scroll = g_scrollTarget = 0.f;
    g_focusVel = 0.f;

    std::wstring rel = L"tests\\assets\\agent";
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) {
            continue;
        }
        if (_wcsicmp(argv[i], L"--src") == 0 || _wcsicmp(argv[i], L"--source") == 0) {
            if (i + 1 < argc && argv[i + 1]) {
                rel = argv[i + 1];
                ++i;
            }
        }
    }

    wchar_t cwdBuf[MAX_PATH]{};
    DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cwdBuf);
    if (cwdLen == 0 || cwdLen >= MAX_PATH) {
        cwdBuf[0] = L'.';
        cwdBuf[1] = L'\0';
    }

    fs::path base = fs::path(cwdBuf);
    fs::path folder = rel.empty() ? base : (fs::path(rel).is_absolute() ? fs::path(rel) : (base / rel));
    std::error_code ec;
    if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec)) {
        return true; // host still runs; empty strip
    }

    std::vector<fs::path> files;
    for (const auto& ent : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
        if (!ent.is_regular_file(ec)) {
            continue;
        }
        if (IsImageFile(ent.path())) {
            files.push_back(ent.path());
        }
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    g_paths.reserve(files.size());
    for (const auto& p : files) {
        g_paths.push_back(p.wstring());
    }
    SyncThumbsFromPaths();
    return true;
}

void Shutdown()
{
    if (g_host && g_animTimer) {
        KillTimer(g_host, kAnimTimerId);
    }
    g_animTimer = false;
    g_focusCenter = 0.f;
    g_focusStrength = 0.f;
    g_focusCenterTarget = 0.f;
    g_focusStrengthTarget = 0.f;
    g_focusVel = 0.f;
    g_lastClientX = 0;
    g_lastClientY = 0;
    g_lastWindowW = 0;
    g_lastWindowH = 0;
    g_didInitialScroll = false;
    ClearThumbs();
    ClearBitmapMap();
    Release(g_dcRt);
    g_rtW = g_rtH = 0;
    g_rtDpi = 0;
    Release(g_fmtUi);
    Release(g_dwrite);
    Release(g_wic);
    Release(g_d2d);
    g_paths.clear();
    g_host = nullptr;
}

void SetHost(HWND hwnd)
{
    g_host = hwnd;
}

void PaintOverGdi(HDC memDc, int windowW, int windowH, int titleBarPx, int originX, int originY)
{
    if (!FEATURE_FILMSTRIP || !memDc || windowW < 32 || windowH < 64) {
        return;
    }
    if (!EnsureFactories()) {
        return;
    }
    if (!EnsureDcRenderTarget(windowW, windowH) || !g_dcRt) {
        return;
    }

    RECT bind{originX, originY, originX + windowW, originY + windowH};
    if (FAILED(g_dcRt->BindDC(memDc, &bind))) {
        return;
    }

    g_dcRt->BeginDraw();
    g_dcRt->SetTransform(D2D1::IdentityMatrix());

    ClampScrollTarget(windowW);
    g_scroll = (std::clamp)(g_scroll, 0.f, MaxScroll(windowW));
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (!g_didInitialScroll && count > 0) {
        CenterOnSelection(windowW);
        g_scroll = g_scrollTarget;
        g_didInitialScroll = true;
    }

    const float stripY = StripTopY(windowH);
    // Use layout padding.left/right to prevent clipping on edges
    const D2D1_RECT_F previewBox = D2D1::RectF(static_cast<float>(kPadLeft),
        static_cast<float>(titleBarPx + options::layout::previewPad),
        static_cast<float>(windowW - kPadRight),
        stripY - static_cast<float>(options::layout::previewPad));

    EnsurePreviewForSelection();

    // No background panel - clean image display against host background
    DrawLetterboxedPreview(g_dcRt, g_previewBmp, previewBox);

    const D2D1_RECT_F stripRc = D2D1::RectF(0.f, stripY, static_cast<float>(windowW), static_cast<float>(windowH));
    DrawStripChrome(g_dcRt, stripRc, static_cast<float>(windowW));

    const float yThumb = stripY + static_cast<float>(kThumbYInDock);
    const float x0 = static_cast<float>(kPadLeft) - g_scroll;

    std::vector<float> thumbLefts;
    ComputeThumbLeftEdges(x0, thumbLefts);

    std::vector<size_t> drawOrder;
    drawOrder.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const float left = thumbLefts[i];
        const float w = ThumbWidth(i);
        if (left > static_cast<float>(windowW) + CellStride()) {
            continue;
        }
        if (left + w < -CellStride()) {
            continue;
        }
        drawOrder.push_back(i);
    }
    std::sort(drawOrder.begin(), drawOrder.end(), [](size_t a, size_t b) {
        const float sa = ThumbFocusScale(a);
        const float sb = ThumbFocusScale(b);
        if (sa != sb) {
            return sa < sb;
        }
        return a < b;
    });

    for (const size_t i : drawOrder) {
        const float left = thumbLefts[i];
        const float scale = ThumbFocusScale(i);
        const D2D1_ROUNDED_RECT rr = ScaledThumbRectFromLeft(left, yThumb, scale);
        ID2D1Bitmap* tb = GetThumbBitmap(i < g_paths.size() ? g_paths[i] : L"");
        if (options::thumbOptions::dropShadowSize > 0.5f) {
            DrawThumbDropShadow(g_dcRt, rr, options::thumbOptions::dropShadowSize * scale, options::thumbOptions::dropShadowHardness);
        }
        if (i == g_sel) {
            DrawSelectionHalo(g_dcRt, rr);
        }
        DrawRoundedThumb(g_dcRt, tb, rr, i == g_sel ? 1.f : 0.78f);

        ID2D1SolidColorBrush* stroke = nullptr;
        const D2D1_COLOR_F ring =
            (i == g_sel) ? D2D1::ColorF(0.45f, 0.92f, 1.f, 0.92f) : D2D1::ColorF(0.82f, 0.86f, 0.94f, 0.12f);
        const float sw = (i == g_sel) ? 1.85f : 0.85f;
        if (SUCCEEDED(g_dcRt->CreateSolidColorBrush(ring, &stroke)) && stroke) {
            g_dcRt->DrawRoundedRectangle(rr, stroke, sw);
            Release(stroke);
        }
    }

    if (count == 0) {
        const wchar_t* msg = L"No .jpg / .png in --src folder";
        D2D1_RECT_F tr = D2D1::RectF(static_cast<float>(kPreviewPad), stripY + 8.f,
            static_cast<float>(windowW - kPreviewPad), static_cast<float>(windowH - 6.f));
        ID2D1SolidColorBrush* br = nullptr;
        if (SUCCEEDED(g_dcRt->CreateSolidColorBrush(D2D1::ColorF(0.62f, 0.74f, 0.88f, 0.72f), &br)) && br
            && g_fmtUi) {
            g_dcRt->DrawText(msg, static_cast<UINT32>(wcslen(msg)), g_fmtUi, tr, br,
                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT, DWRITE_MEASURING_MODE_NATURAL);
            Release(br);
        }
    }

    const HRESULT hr = g_dcRt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        Release(g_dcRt);
        g_rtW = g_rtH = 0;
        g_rtDpi = 0;
        ClearBitmapMap();
        // Unload ImageThumb bitmaps — they are bound to the old render target
        for (auto& t : g_thumbs) {
            t->Unload();
        }
    }
}

bool OnKeyDown(HWND hwnd, UINT vk)
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (!FEATURE_FILMSTRIP || count == 0) {
        return false;
    }
    RECT wr{};
    GetClientRect(hwnd, &wr);
    const int ww = wr.right - wr.left;
    const int wh = wr.bottom - wr.top;
    bool changed = false;
    switch (vk) {
    case VK_LEFT:
        if (g_sel > 0) {
            --g_sel;
            changed = true;
        }
        break;
    case VK_RIGHT:
        if (g_sel + 1 < count) {
            ++g_sel;
            changed = true;
        }
        break;
    case VK_HOME:
        g_sel = 0;
        changed = true;
        break;
    case VK_END:
        g_sel = count == 0 ? 0 : (count - 1);
        changed = true;
        break;
    case VK_PRIOR: {
        const size_t step = 5;
        g_sel = (g_sel > step) ? (g_sel - step) : 0;
        changed = true;
        break;
    }
    case VK_NEXT: {
        const size_t step = 5;
        g_sel = (std::min)(count - 1, g_sel + step);
        changed = true;
        break;
    }
    default:
        return false;
    }
    if (!changed) {
        return true;
    }
    Release(g_previewBmp);
    g_previewPath.clear();
    CenterOnSelection(ww);
    ClampScrollTarget(ww);
    KickAnim();
    RequestPaint();
    return true;
}

// Uses current `g_scroll` (including mid-lerp) so focus tracks what is drawn. Returns true when pointer
// is in the horizontal strip hover band (strength target = 1).
static bool ApplyStripPointerFocus(int clientX, int clientY, int windowW, int windowH)
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (!options::focusAnimation || count == 0) {
        return false;
    }
    (void)windowW;
    (void)windowH;
    if (!PtInStrip(clientY, 0)) {
        if (g_focusStrengthTarget > 0.f) {
            g_focusStrengthTarget = 0.f;
            g_focusVel = 0.f;
        }
        return false;
    }
    const float layerX = ClientXToLayerX(static_cast<float>(clientX));
    const float x0 = static_cast<float>(kPadLeft) - g_scroll;
    const float localRaw = layerX - x0;
    const float rowEnd = NominalThumbRowWidth() + static_cast<float>(kThumbPx) * kFocusMaxBoost * 2.5f;
    if (localRaw >= 0.f && localRaw <= rowEnd) {
        const float lastIdx = static_cast<float>(count - 1);
        const float focusLocal = localRaw
            - (options::thumbOptions::autoPointerLocalXHalfCellBias ? (0.5f * CellStride()) : 0.f);
        const float idxF = focusLocal / CellStride();
        g_focusCenterTarget = (std::clamp)(idxF, 0.f, lastIdx);
        g_focusStrengthTarget = 1.f;
        return true;
    }
    if (g_focusStrengthTarget > 0.f) {
        g_focusStrengthTarget = 0.f;
        g_focusVel = 0.f;
    }
    return false;
}

static void KickStripFocusAnimBurst()
{
    for (int k = 0; k < 4; ++k) {
        (void)StepFocusAnim();
    }
}

bool OnMouseWheel(HWND hwnd, int delta, int clientX, int clientY, int titleBarPx, int windowW, int windowH)
{
    if (!FEATURE_FILMSTRIP) {
        return false;
    }
    (void)titleBarPx;
    g_lastClientX = clientX;
    g_lastClientY = clientY;
    g_lastWindowW = windowW;
    g_lastWindowH = windowH;
    if (!PtInStrip(clientY, 0)) {
        return false;
    }
    const float step = 48.f * (static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA));
    g_scrollTarget -= step;
    ClampScrollTarget(windowW);
    KickAnim();
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (options::focusAnimation && count > 0) {
        if (ApplyStripPointerFocus(clientX, clientY, windowW, windowH)) {
            KickStripFocusAnimBurst();
        }
    }
    RequestPaint();
    (void)hwnd;
    return true;
}

void OnMouseMove(HWND hwnd, int clientX, int clientY, int windowW, int windowH)
{
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (!FEATURE_FILMSTRIP || count == 0) {
        (void)hwnd;
        (void)clientX;
        (void)clientY;
        (void)windowW;
        (void)windowH;
        return;
    }
    g_lastClientX = clientX;
    g_lastClientY = clientY;
    g_lastWindowW = windowW;
    g_lastWindowH = windowH;

    if (!options::focusAnimation) {
        (void)hwnd;
        return;
    }

    const float prevStrengthTarget = g_focusStrengthTarget;
    if (ApplyStripPointerFocus(clientX, clientY, windowW, windowH)) {
        KickAnim();
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        (void)TrackMouseEvent(&tme);
        KickStripFocusAnimBurst();
        RequestPaint();
        return;
    }
    if (prevStrengthTarget > 0.f && g_focusStrengthTarget < 0.5f) {
        KickAnim();
        RequestPaint();
    }
}

void OnMouseLeaveClient(HWND hwnd)
{
    (void)hwnd;
    if (!FEATURE_FILMSTRIP || !options::focusAnimation) {
        return;
    }
    if (g_focusStrengthTarget > 0.f || g_focusStrength > 0.01f) {
        g_focusStrengthTarget = 0.f;
        g_focusVel = 0.f;
        KickAnim();
        RequestPaint();
    }
}

bool OnLButtonDown(HWND hwnd, int clientX, int clientY, int titleBarPx, int windowW, int windowH)
{
    if (!FEATURE_FILMSTRIP || !PtInStrip(clientY, 0)) {
        return false;
    }
    (void)titleBarPx;
    const int idx = HitThumbIndex(static_cast<float>(clientX), clientY, windowW, windowH);
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (idx < 0 || static_cast<size_t>(idx) >= count) {
        return true; // ate click in strip
    }
    if (static_cast<size_t>(idx) != g_sel) {
        g_sel = static_cast<size_t>(idx);
        Release(g_previewBmp);
        g_previewPath.clear();
        CenterOnSelection(windowW);
        ClampScrollTarget(windowW);
        KickAnim();
        RequestPaint();
    }
    (void)hwnd;
    return true;
}

bool OnTimer(HWND hwnd, WPARAM timerId)
{
    if (!FEATURE_FILMSTRIP || timerId != kAnimTimerId) {
        return false;
    }
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    const bool movedScroll = StepAnimScroll();
    if (movedScroll && options::focusAnimation && count > 0) {
        if (ApplyStripPointerFocus(g_lastClientX, g_lastClientY, g_lastWindowW, g_lastWindowH)) {
            KickStripFocusAnimBurst();
        }
    }
    const bool movedFocus = StepFocusAnim();
    StopAnimIfIdle();
    if (movedScroll || movedFocus) {
        RequestPaint();
    }
    (void)hwnd;
    return true;
}

// ============================================================================
// FilmStripWidget Implementation (PIMPL)
// ============================================================================

// The widget implementation reuses the internal functions above by forwarding
// to a global instance for backward compatibility. Full isolation would
// require moving all globals into Impl, but this allows gradual migration.

class FilmStripWidget::Impl {
public:
    // Callback storage
    FilmStripWidget::SelectionChangedCallback onSelectionChanged_;
    FilmStripWidget::HoverChangedCallback onHoverChanged_;
    FilmStripWidget::PathsChangedCallback onPathsChanged_;
    FilmStripWidget::ScrollChangedCallback onScrollChanged_;

    // Track previous values for callback triggering
    size_t lastCallbackSel_ = SIZE_MAX;
    size_t lastCallbackHover_ = SIZE_MAX;

    Impl() = default;
    ~Impl() { Shutdown(); }

    bool Initialize(HWND host, const std::vector<std::wstring>& paths) {
        Shutdown();
        g_host = host;
        g_paths = paths;
        g_sel = 0;
        g_scroll = g_scrollTarget = 0.f;
        g_didInitialScroll = false;
        ClearThumbs();
        ClearBitmapMap();  // Clear any cached thumbs from previous session
        SyncThumbsFromPaths();
        return true;
    }

    bool InitFromArgv(int argc, wchar_t** argv) {
        return ::filmstrip::InitFromArgv(argc, argv);
    }

    void SetPaths(const std::vector<std::wstring>& paths) {
        // Preserve selection if still valid, otherwise reset to 0
        size_t oldSel = g_sel;
        g_paths = paths;
        if (oldSel >= g_paths.size()) {
            g_sel = g_paths.empty() ? 0 : (g_paths.size() - 1);
        }
        g_scroll = g_scrollTarget = 0.f;
        g_didInitialScroll = false;
        ClearThumbs();       // Clear thumb objects
        ClearBitmapMap();    // Clear cached thumbs for old paths
        Release(g_previewBmp);
        g_previewPath.clear();

        // Sync thumbs from new paths
        SyncThumbsFromPaths();

        // Notify callback
        if (onPathsChanged_) {
            onPathsChanged_(g_paths.size());
        }

        // Trigger selection callback if selection changed
        if (onSelectionChanged_ && g_sel != lastCallbackSel_) {
            lastCallbackSel_ = g_sel;
            if (g_sel < g_paths.size()) {
                onSelectionChanged_(g_sel, g_paths[g_sel]);
            }
        }

        RequestPaint();
    }

    bool LoadFromFolder(const std::wstring& folderPath) {
        std::vector<std::wstring> paths;
        fs::path folder = folderPath;
        std::error_code ec;

        if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec)) {
            SetPaths({});  // Clear to empty
            return false;
        }

        std::vector<fs::path> files;
        for (const auto& ent : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) {
            if (!ent.is_regular_file(ec)) {
                continue;
            }
            if (IsImageFile(ent.path())) {
                files.push_back(ent.path());
            }
        }
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        paths.reserve(files.size());
        for (const auto& p : files) {
            paths.push_back(p.wstring());
        }

        SetPaths(paths);
        return true;
    }

    void Shutdown() {
        ::filmstrip::Shutdown();
    }

    bool HasContent() const {
        return !g_thumbs.empty() || !g_paths.empty();
    }

    void Paint(HDC hdc, int x, int y, int width, int height, int titleBarPx) {
        if (!hdc || width < 32 || height < DockHeightPx()) {
            return;
        }
        // For now, delegate to the global PaintOverGdi which paints at bottom
        // A full implementation would respect x, y positioning
        (void)x;
        (void)y;
        ::filmstrip::PaintOverGdi(hdc, width, height, titleBarPx);
    }

    void PaintOverGdi(HDC memDc, int windowW, int windowH, int titleBarPx) {
        ::filmstrip::PaintOverGdi(memDc, windowW, windowH, titleBarPx);
    }

    void RequestPaint() {
        ::filmstrip::RequestPaint();
    }

    bool OnKeyDown(UINT vk) {
        if (!g_host) return false;
        RECT rc;
        GetClientRect(g_host, &rc);
        return ::filmstrip::OnKeyDown(g_host, vk);
    }

    bool OnMouseWheel(int delta, int clientX, int clientY, int titleBarPx, int windowW, int windowH) {
        if (!g_host) return false;
        return ::filmstrip::OnMouseWheel(g_host, delta, clientX, clientY, titleBarPx, windowW, windowH);
    }

    bool OnLButtonDown(int clientX, int clientY, int titleBarPx, int windowW, int windowH) {
        if (!g_host) return false;
        return ::filmstrip::OnLButtonDown(g_host, clientX, clientY, titleBarPx, windowW, windowH);
    }

    void OnMouseMove(int clientX, int clientY, int windowW, int windowH) {
        if (!g_host) return;
        ::filmstrip::OnMouseMove(g_host, clientX, clientY, windowW, windowH);
    }

    void OnMouseLeave() {
        if (!g_host) return;
        ::filmstrip::OnMouseLeaveClient(g_host);
    }

    bool OnTimer(WPARAM timerId) {
        if (!g_host) return false;
        return ::filmstrip::OnTimer(g_host, timerId);
    }

    size_t GetSelection() const {
        return g_sel;
    }

    void SetSelection(size_t index) {
        const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
        if (index < count && index != g_sel) {
            g_sel = index;
            Release(g_previewBmp);
            g_previewPath.clear();

            // Notify callback
            if (onSelectionChanged_ && index < g_paths.size()) {
                lastCallbackSel_ = g_sel;
                onSelectionChanged_(g_sel, g_paths[g_sel]);
            }

            RequestPaint();
        }
    }

    const std::wstring& GetCurrentPath() const {
        const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
        if (count == 0) {
            return g_previewPath;
        }
        // Use paths if available, otherwise get from thumb
        if (g_sel < g_paths.size()) {
            return g_paths[g_sel];
        }
        if (g_sel < g_thumbs.size()) {
            return g_thumbs[g_sel]->GetPath();
        }
        return g_previewPath;
    }

    void SetHost(HWND hwnd) {
        g_host = hwnd;
    }
};

// ============================================================================
// FilmStripWidget Public API
// ============================================================================

FilmStripWidget::FilmStripWidget() : pImpl_(new Impl()) {}
FilmStripWidget::~FilmStripWidget() { delete pImpl_; }

bool FilmStripWidget::Initialize(HWND host, const std::vector<std::wstring>& paths) {
    return pImpl_->Initialize(host, paths);
}

bool FilmStripWidget::InitFromArgv(int argc, wchar_t** argv) {
    return pImpl_->InitFromArgv(argc, argv);
}

void FilmStripWidget::SetPaths(const std::vector<std::wstring>& paths) {
    pImpl_->SetPaths(paths);
}

bool FilmStripWidget::LoadFromFolder(const std::wstring& folderPath) {
    return pImpl_->LoadFromFolder(folderPath);
}

void FilmStripWidget::Shutdown() {
    pImpl_->Shutdown();
}

int FilmStripWidget::DockHeightPx() {
    return ::filmstrip::DockHeightPx();
}

bool FilmStripWidget::HasContent() const {
    return pImpl_->HasContent();
}

void FilmStripWidget::Paint(HDC hdc, int x, int y, int width, int height, int titleBarPx) {
    pImpl_->Paint(hdc, x, y, width, height, titleBarPx);
}

void FilmStripWidget::PaintOverGdi(HDC memDc, int windowW, int windowH, int titleBarPx) {
    pImpl_->PaintOverGdi(memDc, windowW, windowH, titleBarPx);
}

void FilmStripWidget::RequestPaint() {
    pImpl_->RequestPaint();
}

bool FilmStripWidget::OnKeyDown(UINT vk) {
    return pImpl_->OnKeyDown(vk);
}

bool FilmStripWidget::OnMouseWheel(int delta, int clientX, int clientY, int titleBarPx, int windowW, int windowH) {
    return pImpl_->OnMouseWheel(delta, clientX, clientY, titleBarPx, windowW, windowH);
}

bool FilmStripWidget::OnLButtonDown(int clientX, int clientY, int titleBarPx, int windowW, int windowH) {
    return pImpl_->OnLButtonDown(clientX, clientY, titleBarPx, windowW, windowH);
}

void FilmStripWidget::OnMouseMove(int clientX, int clientY, int windowW, int windowH) {
    pImpl_->OnMouseMove(clientX, clientY, windowW, windowH);
}

void FilmStripWidget::OnMouseLeave() {
    pImpl_->OnMouseLeave();
}

bool FilmStripWidget::OnTimer(WPARAM timerId) {
    return pImpl_->OnTimer(timerId);
}

size_t FilmStripWidget::GetSelection() const {
    return pImpl_->GetSelection();
}

void FilmStripWidget::SetSelection(size_t index) {
    pImpl_->SetSelection(index);
}

const std::wstring& FilmStripWidget::GetCurrentPath() const {
    return pImpl_->GetCurrentPath();
}

size_t FilmStripWidget::GetHoverIndex() const {
    // Calculate hover from g_focusCenter (the animated focus center)
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (!options::focusAnimation || count == 0) {
        return SIZE_MAX;
    }
    const int idx = static_cast<int>(std::round(g_focusCenter));
    if (idx >= 0 && static_cast<size_t>(idx) < count) {
        return static_cast<size_t>(idx);
    }
    return SIZE_MAX;
}

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

void FilmStripWidget::OnSelectionChanged(SelectionChangedCallback callback) {
    pImpl_->onSelectionChanged_ = std::move(callback);
    // Trigger immediately with current state if valid
    const size_t count = g_thumbs.empty() ? g_paths.size() : g_thumbs.size();
    if (pImpl_->onSelectionChanged_ && g_sel < count && g_sel < g_paths.size()) {
        pImpl_->lastCallbackSel_ = g_sel;
        pImpl_->onSelectionChanged_(g_sel, g_paths[g_sel]);
    }
}

void FilmStripWidget::OnHoverChanged(HoverChangedCallback callback) {
    pImpl_->onHoverChanged_ = std::move(callback);
}

void FilmStripWidget::OnPathsChanged(PathsChangedCallback callback) {
    pImpl_->onPathsChanged_ = std::move(callback);
}

void FilmStripWidget::OnScrollChanged(ScrollChangedCallback callback) {
    pImpl_->onScrollChanged_ = std::move(callback);
}

// ============================================================================
// Legacy Global API
// ============================================================================
// The legacy functions operate directly on the global state variables (g_host,
// g_paths, etc.) that are also used by FilmStripWidget::Impl. This maintains
// backward compatibility with existing code while allowing migration to the
// Widget class for new code.

} // namespace filmstrip
