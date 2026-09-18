#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <d2d1.h>
#include <gdiplus.h>

#include <cstdint>

namespace glowplay {

// ThorVG raster of Tabler `icons/filled` SVGs (`currentColor` → accent).
// Same stack as FEATURE_SVG_BUTTONS in the main app.
bool SvgIconInit();
void SvgIconShutdown();

const wchar_t* TablerFilledDir();

// Premultiplied BGRA, dim×dim, top-down. False if the SVG is missing or ThorVG fails.
bool SvgIconRaster(const wchar_t* stem, int dim, BYTE red, BYTE green, BYTE blue, const std::uint32_t*& bits,
    int& strideBytes);

bool SvgIconBlitGdi(Gdiplus::Graphics& gfx, const Gdiplus::RectF& box, const wchar_t* stem, BYTE red, BYTE green,
    BYTE blue, BYTE alpha);
bool SvgIconBlitD2d(ID2D1RenderTarget* rt, const D2D1_RECT_F& box, const wchar_t* stem, BYTE red, BYTE green,
    BYTE blue, float alpha);

} // namespace glowplay
