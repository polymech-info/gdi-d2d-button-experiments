#include "FilmStripThumb.h"
#include "FilmStrip.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

// WIC for image loading
#include <wincodec.h>

namespace fs = std::filesystem;

namespace filmstrip {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

template <typename T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

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

bool IsImageFile(const std::wstring& path)
{
    return EndsWithExt(path, L".jpg") || EndsWithExt(path, L".jpeg") || EndsWithExt(path, L".png");
}

// Get filename from path for display label
std::wstring GetFilenameFromPath(const std::wstring& path)
{
    size_t lastSlash = path.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        return path.substr(lastSlash + 1);
    }
    return path;
}

// Get extension from path
std::wstring GetExtension(const std::wstring& path)
{
    size_t lastDot = path.find_last_of(L'.');
    if (lastDot != std::wstring::npos && lastDot > path.find_last_of(L"\\/")) {
        return path.substr(lastDot);
    }
    return L"";
}

} // anonymous namespace

// ============================================================================
// Thumb Base Class
// ============================================================================

D2D1_ROUNDED_RECT Thumb::MakeRoundedRect(float cx, float cy, float size, float radius)
{
    const float half = size * 0.5f;
    return D2D1::RoundedRect(D2D1::RectF(cx - half, cy - half, cx + half, cy + half), radius, radius);
}

void Thumb::RenderPlaceholder(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& rect) const
{
    // Default placeholder: gray rounded rect
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.15f, 0.18f, 1.0f), &brush)) && brush) {
        rt->FillRoundedRectangle(rect, brush);
        brush->Release();
    }

    // Border
    ID2D1SolidColorBrush* stroke = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.35f, 0.40f, 0.5f), &stroke)) && stroke) {
        rt->DrawRoundedRectangle(rect, stroke, 1.0f);
        stroke->Release();
    }
}

// ============================================================================
// ImageThumb Implementation
// ============================================================================

ImageThumb::ImageThumb(std::wstring path)
    : path_(std::move(path))
{
}

ImageThumb::~ImageThumb()
{
    SafeRelease(bitmap_);
}

std::wstring ImageThumb::GetLabel() const
{
    return GetFilenameFromPath(path_);
}

D2D1_SIZE_F ImageThumb::GetPreferredSize() const
{
    return D2D1::SizeF(thumb::kDefaultThumbSize, thumb::kDefaultThumbSize);
}

// Global WIC factory (defined in FilmStrip.cpp)
extern IWICImagingFactory* g_wic;

void ImageThumb::EnsureLoaded(ID2D1RenderTarget* rt) const
{
    if (bitmap_ || attemptedLoad_) {
        return;
    }

    if (!rt || path_.empty()) {
        return;
    }

    attemptedLoad_ = true;

    // Use the global WIC factory
    IWICImagingFactory* wic = g_wic;
    if (!wic) {
        return;
    }

    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = wic->CreateDecoderFromFilename(path_.c_str(), nullptr, GENERIC_READ,
                                        WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) {
        return;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(hr) || !frame) {
        return;
    }

    UINT iw = 0, ih = 0;
    frame->GetSize(&iw, &ih);
    if (iw == 0 || ih == 0) {
        frame->Release();
        return;
    }

    // Scale to thumb size
    constexpr UINT kMaxThumbEdge = 600;
    UINT tw = iw, th = ih;
    const UINT maxEdge = (std::max)(iw, ih);
    if (maxEdge > kMaxThumbEdge) {
        const double scale = static_cast<double>(kMaxThumbEdge) / static_cast<double>(maxEdge);
        tw = static_cast<UINT>(std::lround(static_cast<double>(iw) * scale));
        th = static_cast<UINT>(std::lround(static_cast<double>(ih) * scale));
        tw = (std::max)(tw, 1u);
        th = (std::max)(th, 1u);
    }

    IWICBitmapScaler* scaler = nullptr;
    hr = wic->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr) && scaler) {
        hr = scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant);
        frame->Release();
    } else {
        frame->Release();
        return;
    }

    if (FAILED(hr)) {
        scaler->Release();
        return;
    }

    IWICFormatConverter* converter = nullptr;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        scaler->Release();
        return;
    }

    hr = converter->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                               nullptr, 0.f, WICBitmapPaletteTypeMedianCut);
    scaler->Release();

    if (FAILED(hr)) {
        converter->Release();
        return;
    }

    // Create D2D bitmap
    hr = rt->CreateBitmapFromWicBitmap(converter, nullptr, &bitmap_);
    converter->Release();

    if (SUCCEEDED(hr) && bitmap_) {
        bitmapWidth_ = tw;
        bitmapHeight_ = th;
    }
}

void ImageThumb::Render(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& rect, float opacity) const
{
    EnsureLoaded(rt);

    if (!bitmap_) {
        RenderPlaceholder(rt, rect);
        return;
    }

    const float bw = static_cast<float>(bitmap_->GetPixelSize().width);
    const float bh = static_cast<float>(bitmap_->GetPixelSize().height);
    const float cw = rect.rect.right - rect.rect.left;
    const float ch = rect.rect.bottom - rect.rect.top;

    if (bw < 1.f || bh < 1.f || cw < 1.f || ch < 1.f) {
        return;
    }

    // Fill mode: Cover (fills cell, crops overflow) or Contain (letterboxed)
    const bool useCover = (options::thumbOptions::fill == options::thumbOptions::ThumbFill::Cover);
    const float s = useCover ? (std::max)(cw / bw, ch / bh) : (std::min)(cw / bw, ch / bh);

    const float dw = bw * s;
    const float dh = bh * s;
    const float ox = rect.rect.left + (cw - dw) * 0.5f;
    const float oy = rect.rect.top + (ch - dh) * 0.5f;

    D2D1_RECT_F dst{ox, oy, ox + dw, oy + dh};
    D2D1_RECT_F src{0.f, 0.f, bw, bh};

    // Create clip layer for rounded corners
    ID2D1Factory* factory = nullptr;
    rt->GetFactory(&factory);
    if (!factory) {
        return;
    }

    ID2D1RoundedRectangleGeometry* geo = nullptr;
    if (FAILED(factory->CreateRoundedRectangleGeometry(rect, &geo)) || !geo) {
        factory->Release();
        return;
    }
    factory->Release();

    D2D1_LAYER_PARAMETERS lp{};
    lp.contentBounds = D2D1::InfiniteRect();
    lp.geometricMask = geo;
    lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    lp.maskTransform = D2D1::IdentityMatrix();
    lp.opacity = opacity;
    lp.opacityBrush = nullptr;
    lp.layerOptions = D2D1_LAYER_OPTIONS_NONE;

    rt->PushLayer(&lp, nullptr);
    rt->DrawBitmap(bitmap_, &dst, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
    rt->PopLayer();
    geo->Release();
}

void ImageThumb::Preload(ID2D1RenderTarget* rt)
{
    EnsureLoaded(rt);
}

void ImageThumb::Unload()
{
    SafeRelease(bitmap_);
    attemptedLoad_ = false;
    bitmapWidth_ = 0;
    bitmapHeight_ = 0;
}

// ============================================================================
// ThumbFactory Implementation
// ============================================================================

std::unique_ptr<Thumb> ThumbFactory::CreateFromPath(const std::wstring& path)
{
    if (path.empty()) {
        return nullptr;
    }

    if (IsImageFile(path)) {
        return std::make_unique<ImageThumb>(path);
    }

    // Future: Add IconThumb for text files, documents, etc.
    // if (IsTextFile(path)) {
    //     return std::make_unique<IconThumb>(path, IconType::Document);
    // }

    return nullptr;
}

bool ThumbFactory::IsSupported(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    return IsImageFile(path);
}

ThumbType ThumbFactory::GetThumbType(const std::wstring& path)
{
    if (IsImageFile(path)) {
        return ThumbType::Image;
    }
    return ThumbType::Placeholder;
}

// ============================================================================
// Thumb Utilities
// ============================================================================

namespace thumb {

void DrawChrome(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& rect, bool selected)
{
    // Selection ring
    const D2D1_COLOR_F ringColor = selected
        ? D2D1::ColorF(0.45f, 0.92f, 1.f, 0.92f)
        : D2D1::ColorF(0.82f, 0.86f, 0.94f, 0.12f);
    const float ringWidth = selected ? 1.85f : 0.85f;

    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(rt->CreateSolidColorBrush(ringColor, &brush)) && brush) {
        rt->DrawRoundedRectangle(rect, brush, ringWidth);
        brush->Release();
    }

    // Halo for selected item
    if (selected) {
        for (int i = 3; i >= 1; --i) {
            const float inflate = static_cast<float>(i) * 2.2f;
            const float a = 0.055f * static_cast<float>(i);
            D2D1_ROUNDED_RECT outer = rect;
            outer.rect.left -= inflate;
            outer.rect.top -= inflate;
            outer.rect.right += inflate;
            outer.rect.bottom += inflate;
            outer.radiusX += inflate * 0.35f;
            outer.radiusY += inflate * 0.35f;

            ID2D1SolidColorBrush* halo = nullptr;
            if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.88f, 1.f, a * 0.92f), &halo)) && halo) {
                rt->FillRoundedRectangle(outer, halo);
                halo->Release();
            }
        }
    }
}

} // namespace thumb

} // namespace filmstrip
