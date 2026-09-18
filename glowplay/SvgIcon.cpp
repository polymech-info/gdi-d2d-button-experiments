#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "SvgIcon.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <thorvg.h>

#ifndef PM_TABLER_FILLED_DIR
#define PM_TABLER_FILLED_DIR ""
#endif

namespace glowplay {
namespace {

std::once_flag g_tvgOnce;
bool g_tvgOk = false;

struct CacheKey {
    std::wstring stem;
    int dim = 0;
    BYTE r = 0;
    BYTE g = 0;
    BYTE b = 0;

    bool operator==(const CacheKey& o) const
    {
        return dim == o.dim && r == o.r && g == o.g && b == o.b && stem == o.stem;
    }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const
    {
        size_t h = std::hash<std::wstring>{}(k.stem);
        h ^= (static_cast<size_t>(k.dim) * 16777619u) + (h << 6) + (h >> 2);
        h ^= (static_cast<size_t>(k.r) << 16) | (static_cast<size_t>(k.g) << 8) | k.b;
        return h;
    }
};

std::mutex g_cacheMu;
std::unordered_map<CacheKey, std::vector<std::uint32_t>, CacheKeyHash> g_cache;

void ReplaceCurrentColor(std::string& s, BYTE red, BYTE green, BYTE blue)
{
    char repl[16]{};
    (void)std::snprintf(repl, sizeof(repl), "#%02X%02X%02X", red, green, blue);
    const std::string from = "currentColor";
    for (;;) {
        const size_t p = s.find(from);
        if (p == std::string::npos) {
            break;
        }
        s.replace(p, from.size(), repl);
    }
}

// Tabler outline SVGs put stroke/fill on the root <svg>; ThorVG does not inherit them.
void NormalizeOutlineSvg(std::string& s)
{
    if (s.find("fill=\"none\"") == std::string::npos) {
        return;
    }
    std::string strokeColor;
    const size_t sc = s.find("stroke=\"#");
    if (sc != std::string::npos) {
        const size_t qs = sc + 8;
        const size_t qe = s.find('"', qs);
        if (qe != std::string::npos) {
            strokeColor = s.substr(qs, qe - qs);
        }
    }
    if (strokeColor.empty()) {
        return;
    }
    std::string strokeWidth = "2";
    const size_t swp = s.find("stroke-width=\"");
    if (swp != std::string::npos) {
        const size_t qs = swp + 14;
        const size_t qe = s.find('"', qs);
        if (qe != std::string::npos) {
            strokeWidth = s.substr(qs, qe - qs);
        }
    }
    char inject[256];
    (void)std::snprintf(inject, sizeof(inject),
        " fill=\"none\" stroke=\"%s\" stroke-width=\"%s\" stroke-linecap=\"round\" stroke-linejoin=\"round\"",
        strokeColor.c_str(), strokeWidth.c_str());
    const std::string injectStr(inject);
    static const char* const kShapeTags[] = {
        "<path ", "<line ", "<polyline ", "<polygon ", "<rect ", "<circle ", "<ellipse ", nullptr};
    for (const char* const* tag = kShapeTags; *tag; ++tag) {
        const std::string t(*tag);
        size_t pos = 0;
        while ((pos = s.find(t, pos)) != std::string::npos) {
            const size_t elemEnd = s.find('>', pos);
            const std::string elem = (elemEnd != std::string::npos) ? s.substr(pos, elemEnd - pos) : std::string{};
            if (elem.find("stroke=") == std::string::npos) {
                s.insert(pos + t.size() - 1, injectStr);
                pos += t.size() + injectStr.size();
            } else {
                pos += t.size();
            }
        }
    }
}

bool ReadFileUtf8(const std::wstring& path, std::string& out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return !out.empty();
}

bool RasterToPargb(const std::string& svg, int dim, BYTE red, BYTE green, BYTE blue, std::vector<std::uint32_t>& out)
{
    out.clear();
    if (svg.empty() || dim < 4) {
        return false;
    }
    std::string body = svg;
    ReplaceCurrentColor(body, red, green, blue);
    NormalizeOutlineSvg(body);

    auto* pic = tvg::Picture::gen();
    if (!pic) {
        return false;
    }
    if (pic->load(body.data(), static_cast<uint32_t>(body.size()), "svg", nullptr, true) != tvg::Result::Success) {
        tvg::Paint::rel(pic);
        return false;
    }
    pic->size(static_cast<float>(dim), static_cast<float>(dim));

    auto* canvas = tvg::SwCanvas::gen();
    if (!canvas) {
        tvg::Paint::rel(pic);
        return false;
    }
    std::vector<uint32_t> buf(static_cast<size_t>(dim) * static_cast<size_t>(dim), 0);
    if (canvas->target(buf.data(), static_cast<uint32_t>(dim), static_cast<uint32_t>(dim),
            static_cast<uint32_t>(dim), tvg::ColorSpace::ABGR8888S) != tvg::Result::Success) {
        tvg::Paint::rel(pic);
        delete canvas;
        return false;
    }
    if (canvas->add(pic) != tvg::Result::Success) {
        tvg::Paint::rel(pic);
        delete canvas;
        return false;
    }
    (void)canvas->update();
    (void)canvas->draw(true);
    (void)canvas->sync();
    delete canvas;

    out.resize(buf.size());
    for (size_t i = 0; i < buf.size(); ++i) {
        const uint32_t p = buf[i];
        const BYTE tr = static_cast<BYTE>(p & 0xFFu);
        const BYTE tg = static_cast<BYTE>((p >> 8) & 0xFFu);
        const BYTE tb = static_cast<BYTE>((p >> 16) & 0xFFu);
        const BYTE ta = static_cast<BYTE>((p >> 24) & 0xFFu);
        if (ta == 0) {
            out[i] = 0;
            continue;
        }
        const unsigned mb = (static_cast<unsigned>(tb) * ta + 127u) / 255u;
        const unsigned mg = (static_cast<unsigned>(tg) * ta + 127u) / 255u;
        const unsigned mr = (static_cast<unsigned>(tr) * ta + 127u) / 255u;
        out[i] = static_cast<uint32_t>(mb) | (static_cast<uint32_t>(mg) << 8) | (static_cast<uint32_t>(mr) << 16)
            | (static_cast<uint32_t>(ta) << 24);
    }
    return true;
}

} // namespace

bool SvgIconInit()
{
    std::call_once(g_tvgOnce, [] {
        g_tvgOk = (tvg::Initializer::init(0) == tvg::Result::Success);
    });
    return g_tvgOk;
}

void SvgIconShutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        g_cache.clear();
    }
    if (g_tvgOk) {
        (void)tvg::Initializer::term();
        g_tvgOk = false;
    }
}

const wchar_t* TablerFilledDir()
{
    static std::wstring dir;
    if (dir.empty()) {
        const char* u8 = PM_TABLER_FILLED_DIR;
        if (u8 && u8[0]) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
            if (n > 1) {
                dir.resize(static_cast<size_t>(n - 1));
                MultiByteToWideChar(CP_UTF8, 0, u8, -1, dir.data(), n);
            }
        }
    }
    return dir.c_str();
}

bool SvgIconRaster(const wchar_t* stem, int dim, BYTE red, BYTE green, BYTE blue, const std::uint32_t*& bits,
    int& strideBytes)
{
    bits = nullptr;
    strideBytes = 0;
    if (!stem || !stem[0] || dim < 4 || !SvgIconInit()) {
        return false;
    }
    CacheKey key{stem, dim, red, green, blue};
    std::lock_guard<std::mutex> lock(g_cacheMu);
    auto it = g_cache.find(key);
    if (it == g_cache.end()) {
        std::wstring path = TablerFilledDir();
        if (path.empty()) {
            return false;
        }
        if (path.back() != L'\\' && path.back() != L'/') {
            path += L'/';
        }
        path += stem;
        path += L".svg";
        std::string svg;
        if (!ReadFileUtf8(path, svg)) {
            return false;
        }
        std::vector<std::uint32_t> pixels;
        if (!RasterToPargb(svg, dim, red, green, blue, pixels)) {
            return false;
        }
        it = g_cache.emplace(std::move(key), std::move(pixels)).first;
    }
    bits = it->second.data();
    strideBytes = dim * 4;
    return true;
}

bool SvgIconBlitGdi(Gdiplus::Graphics& gfx, const Gdiplus::RectF& box, const wchar_t* stem, BYTE red, BYTE green,
    BYTE blue, BYTE alpha)
{
    const int dim = (std::max)(8, static_cast<int>(box.Width + 0.5f));
    const std::uint32_t* bits = nullptr;
    int stride = 0;
    if (!SvgIconRaster(stem, dim, red, green, blue, bits, stride) || !bits) {
        return false;
    }
    Gdiplus::Bitmap bmp(dim, dim, stride, PixelFormat32bppPARGB, const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bits)));
    if (bmp.GetLastStatus() != Gdiplus::Ok) {
        return false;
    }
    const Gdiplus::InterpolationMode prevInterp = gfx.GetInterpolationMode();
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    Gdiplus::Status st = Gdiplus::GenericError;
    if (alpha >= 255) {
        st = gfx.DrawImage(&bmp, box);
    } else {
        Gdiplus::ColorMatrix cm{};
        cm.m[0][0] = cm.m[1][1] = cm.m[2][2] = cm.m[4][4] = 1.f;
        cm.m[3][3] = alpha / 255.f;
        Gdiplus::ImageAttributes attr;
        attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
        st = gfx.DrawImage(&bmp, box, 0.f, 0.f, static_cast<float>(dim), static_cast<float>(dim), Gdiplus::UnitPixel,
            &attr);
    }
    gfx.SetInterpolationMode(prevInterp);
    return st == Gdiplus::Ok;
}

bool SvgIconBlitD2d(ID2D1RenderTarget* rt, const D2D1_RECT_F& box, const wchar_t* stem, BYTE red, BYTE green,
    BYTE blue, float alpha)
{
    if (!rt) {
        return false;
    }
    const int dim = (std::max)(8, static_cast<int>((box.right - box.left) + 0.5f));
    const std::uint32_t* bits = nullptr;
    int stride = 0;
    if (!SvgIconRaster(stem, dim, red, green, blue, bits, stride) || !bits) {
        return false;
    }
    D2D1_BITMAP_PROPERTIES props{};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96.f;
    props.dpiY = 96.f;
    ID2D1Bitmap* bmp = nullptr;
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(dim), static_cast<UINT32>(dim));
    if (FAILED(rt->CreateBitmap(size, bits, static_cast<UINT32>(stride), props, &bmp)) || !bmp) {
        return false;
    }
    rt->DrawBitmap(bmp, box, (std::min)(1.f, (std::max)(0.f, alpha)), D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    bmp->Release();
    return true;
}

} // namespace glowplay
