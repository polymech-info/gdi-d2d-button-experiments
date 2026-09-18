#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace filmstrip {

// ============================================================================
// Feature Gate
// ============================================================================

// Gate the experiment without touching the legacy shell path.
// Parked for the glow playground. Flip on to bring the dock back — do not delete this tree.
constexpr bool FEATURE_FILMSTRIP = false;

// ============================================================================
// Configuration Options
// ============================================================================

namespace options {

// Cover-flow style hover: focused thumb + neighbors scale up.
constexpr bool focusAnimation = true;

// Layout tunables (scale-aware: scaled thumbs up to 1.45x must fit without clipping).
// The unscaled thumb is positioned so that when scaled, it's vertically centered
// in the available space: [padding.top] + [visual.overflow] + [scaled thumb] + [visual.overflow] + [padding.bottom]
namespace layout {
    constexpr int thumbSize = 100;           // base thumbnail width/height in px
    constexpr int thumbGap = 25;             // horizontal gap between thumbnails

    // Padding namespace for clear separation of concerns.
    namespace padding {
        constexpr int top = 5;               // space above visual overflow (px)
        constexpr int bottom = 25;           // space below visual overflow (px)
        constexpr int left = 14;             // horizontal inset for thumb row start
        constexpr int right = 14;            // horizontal inset for thumb row end
    } // namespace padding

    // Visual effects that extend beyond the thumb bounds (drop shadow, glow, halo).
    // These add to the effective "occupied" space and must fit within padding.
    namespace visual {
        constexpr int glowTop = 0;           // additional glow/bleed above padding.top
        constexpr int glowBottom = 0;          // additional glow below padding.bottom
        constexpr int shadowV = 4;           // vertical shadow extent (dropShadowSize * scale + softness)
        constexpr int halo = 8;              // selection halo/ring overflow around thumb
        // Effective visual height above/below the scaled thumb center
        constexpr int overflowTop = glowTop + shadowV/2 + halo/2;
        constexpr int overflowBottom = glowBottom + shadowV/2 + halo/2;
    } // namespace visual

    constexpr int previewPad = 14;           // padding around the preview image area (legacy)
    // Note: strip height = padding.top + visual.overflowTop + maxScaledThumb + visual.overflowBottom + padding.bottom
} // namespace layout

// Tunables for dock thumbnails (D2D). `dropShadowSize` <= 0 disables shadow.
namespace thumbOptions {
    // How decoded pixels map into each rounded thumb cell.
    enum class ThumbFill {
        Contain,    // Letterboxed to fit
        Cover,      // Fills cell, crops overflow
    };

    constexpr ThumbFill fill = ThumbFill::Cover;
    constexpr float dropShadowSize = 3.f;      // Blur envelope in DIPs; scales with hover
    constexpr float dropShadowHardness = 0.15f; // 0=softer/more rings, 1=harder falloff
    constexpr bool autoPointerLocalXHalfCellBias = true; // ULW+DWM X mapping correction
} // namespace thumbOptions

} // namespace options

// ============================================================================
// Widget Interface
// ============================================================================

// FilmStripWidget is a self-contained film strip component that can be
// integrated into any window. It handles its own rendering, input, and state.
//
// Usage:
//   FilmStripWidget strip;
//   strip.Initialize(hwnd, paths);  // or InitFromArgv for CLI usage
//   
//   // In your window's paint handler:
//   strip.Paint(hdc, x, y, width, height, titleBarHeight);
//
//   // In your window's message handler, forward relevant messages:
//   strip.HandleMouseMove(x, y, clientW, clientH);
//   strip.HandleMouseWheel(delta, x, y, titleBarH, clientW, clientH);
//   // ... etc
//
//   strip.Shutdown();
//
class FilmStripWidget {
public:
    FilmStripWidget();
    ~FilmStripWidget();

    // Disable copy/move - widget owns D2D/DWrite resources
    FilmStripWidget(const FilmStripWidget&) = delete;
    FilmStripWidget& operator=(const FilmStripWidget&) = delete;
    FilmStripWidget(FilmStripWidget&&) = delete;
    FilmStripWidget& operator=(FilmStripWidget&&) = delete;

    // ------------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------------

    // Initialize with explicit list of image file paths (jpg/png).
    bool Initialize(HWND host, const std::vector<std::wstring>& paths);

    // Initialize from command line arguments (--src folder to scan).
    // --src <folder> or --source <folder> to specify image directory.
    bool InitFromArgv(int argc, wchar_t** argv);

    // Update the image list dynamically (preserves selection index if valid).
    void SetPaths(const std::vector<std::wstring>& paths);

    // Scan a folder for images and load them (sorted alphabetically).
    bool LoadFromFolder(const std::wstring& folderPath);

    // Clean up resources. Safe to call multiple times.
    void Shutdown();

    // ------------------------------------------------------------------------
    // Layout Queries (for host window sizing)
    // ------------------------------------------------------------------------

    // Height of the dock area reserved for the film strip.
    static int DockHeightPx();

    // Whether this widget is enabled/configured to show content.
    bool HasContent() const;

    // ------------------------------------------------------------------------
    // Rendering
    // ------------------------------------------------------------------------

    // Paint the film strip into the given DC at the specified position.
    // The widget paints at (x, y + height - DockHeightPx()) within the rect.
    // titleBarPx is used to position the preview image area.
    void Paint(HDC hdc, int x, int y, int width, int height, int titleBarPx);

    // Convenience: Paint at bottom of a full-window HDC.
    void PaintOverGdi(HDC memDc, int windowW, int windowH, int titleBarPx);

    // Request a repaint of the host window (calls InvalidateRect).
    void RequestPaint();

    // ------------------------------------------------------------------------
    // Input Handling
    // ------------------------------------------------------------------------

    // Keyboard navigation. Returns true if key was handled.
    bool OnKeyDown(UINT vk);

    // Mouse wheel (horizontal scroll). Returns true if handled (was over strip).
    bool OnMouseWheel(int delta, int clientX, int clientY, int titleBarPx, int windowW, int windowH);

    // Left click (thumb selection). Returns true if handled (click was in strip).
    bool OnLButtonDown(int clientX, int clientY, int titleBarPx, int windowW, int windowH);

    // Mouse move (hover effects, focus animation). Safe to call frequently.
    void OnMouseMove(int clientX, int clientY, int windowW, int windowH);

    // Mouse left client area (cancel hover).
    void OnMouseLeave();

    // Animation timer tick. Returns true if animation is active (needs more frames).
    bool OnTimer(WPARAM timerId);

    // ------------------------------------------------------------------------
    // Callbacks / Events
    // ------------------------------------------------------------------------

    // Callback types for widget events.
    using SelectionChangedCallback = std::function<void(size_t index, const std::wstring& path)>;
    using HoverChangedCallback = std::function<void(size_t index, const std::wstring& path)>;
    using PathsChangedCallback = std::function<void(size_t count)>;
    using ScrollChangedCallback = std::function<void(float scrollPosition, float maxScroll)>;

    // Called when user selects a different thumb (click, key nav, etc).
    void OnSelectionChanged(SelectionChangedCallback callback);

    // Called when hover changes (mouse over different thumb).
    // Index is -1 (size_t max) when mouse leaves the strip.
    void OnHoverChanged(HoverChangedCallback callback);

    // Called when the image list changes (SetPaths, LoadFromFolder, etc).
    void OnPathsChanged(PathsChangedCallback callback);

    // Called during scroll animation (useful for syncing external UI).
    void OnScrollChanged(ScrollChangedCallback callback);

    // ------------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------------

    // Current selection index.
    size_t GetSelection() const;
    void SetSelection(size_t index);

    // Current image path (for preview display).
    const std::wstring& GetCurrentPath() const;

    // Currently hovered index (SIZE_MAX if none).
    size_t GetHoverIndex() const;

private:
    class Impl;
    Impl* pImpl_;  // PIMPL idiom to hide implementation details
};

// ============================================================================
// Legacy C-style API (for backward compatibility with existing code)
// ============================================================================

// These functions operate on a global singleton instance for simple integration.
// For multi-instance or more control, use the FilmStripWidget class directly.

// Vertical space reserved at the bottom (dock + glow).
// Used by host window for layout calculations.
int DockHeightPx();

// Global initialization from command line.
bool InitFromArgv(int argc, wchar_t** argv);

// Global cleanup.
void Shutdown();

// Set the host window for the global instance.
void SetHost(HWND hwnd);

// Rendering for global instance. `originX/Y` shift the D2D bind rect (shadow gutter).
void PaintOverGdi(HDC memDc, int windowW, int windowH, int titleBarPx, int originX = 0, int originY = 0);

// Input handling for global instance.
bool OnKeyDown(HWND hwnd, UINT vk);
bool OnMouseWheel(HWND hwnd, int delta, int clientX, int clientY, int titleBarPx, int windowW, int windowH);
bool OnLButtonDown(HWND hwnd, int clientX, int clientY, int titleBarPx, int windowW, int windowH);
void OnMouseMove(HWND hwnd, int clientX, int clientY, int windowW, int windowH);
void OnMouseLeaveClient(HWND hwnd);
bool OnTimer(HWND hwnd, WPARAM timerId);

} // namespace filmstrip
