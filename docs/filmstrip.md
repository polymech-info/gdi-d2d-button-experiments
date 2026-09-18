# FilmStrip Widget Integration Guide

The `FilmStrip` is a self-contained widget that displays a scrollable row of image thumbnails with a preview pane. It can be integrated into any Win32 window.

## Quick Start

```cpp
#include "filmstrip/FilmStrip.h"

// Simple integration using the global singleton
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        filmstrip::InitFromArgv(__argc, __wargv);
        filmstrip::SetHost(hwnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // ... paint your content ...
        // Film strip paints at bottom
        filmstrip::PaintOverGdi(hdc, width, height, titleBarHeight);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        filmstrip::Shutdown();
        PostQuitMessage(0);
        return 0;
    }

    // Forward input messages
    if (filmstrip::OnKeyDown(hwnd, static_cast<UINT>(wParam))) return 0;
    // ... etc

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
```

## Layout Integration

The film strip occupies a dock area at the bottom of the window. Reserve this space in your layout:

```cpp
// Reserve space at bottom
int dockHeight = filmstrip::DockHeightPx();  // Currently ~189px with visual overflow

// Preview image area (above the dock)
RECT previewArea = {
    padding.left,
    titleBarHeight + padding.top,
    windowWidth - padding.right,
    windowHeight - dockHeight - padding.bottom
};

// Film strip occupies:
// Y: windowHeight - dockHeight to windowHeight
// X: 0 to windowWidth (full width)
```

See [layout.md](./layout.md) for detailed layout calculations.

## Loading Image Sources

There are several ways to provide images to the widget:

### Direct Vector of Paths

Pass a `vector<wstring>` with explicit file paths (jpg/png supported):

```cpp
std::vector<std::wstring> myImages = {
    L"C:\\Photos\\vacation\\beach.jpg",
    L"C:\\Photos\\vacation\\mountain.png",
    L"C:\\Photos\\vacation\\city.jpg"
};

filmstrip::FilmStripWidget strip;
strip.Initialize(hwnd, myImages);
```

### Scan Folder

Automatically scan a directory for images (sorted alphabetically):

```cpp
strip.LoadFromFolder(L"C:\\Photos\\vacation");
// or relative:
strip.LoadFromFolder(L"..\\assets\\thumbnails");
```

### Command Line Arguments

Use `--src <folder>` to specify a folder to scan:

```cpp
strip.InitFromArgv(__argc, __wargv);
// Command: app.exe --src C:\Photos
```

### Dynamic Updates

Change the image list at runtime (preserves selection if still valid):

```cpp
// User picked a different folder in your UI
void OnFolderChanged(const std::wstring& newFolder) {
    std::vector<std::wstring> newPaths = ScanFolder(newFolder);
    strip.SetPaths(newPaths);  // Updates immediately
}
```

## Callbacks / Event Handling

Register callbacks to respond to user interactions:

```cpp
filmstrip::FilmStripWidget strip;

// Selection changed (user clicked or navigated with keys)
strip.OnSelectionChanged([](size_t index, const std::wstring& path) {
    // Update your preview pane
    LoadLargePreview(path);
    // Update status bar
    SetStatusText(L"Image " + std::to_wstring(index + 1) + L" of " + std::to_wstring(strip.GetSelection()));
});

// Hover changed (mouse moved over different thumb)
strip.OnHoverChanged([](size_t index, const std::wstring& path) {
    if (index == SIZE_MAX) {
        // Mouse left the strip
        HideTooltip();
    } else {
        // Show tooltip with filename
        ShowTooltip(path);
    }
});

// Paths changed (images loaded or updated)
strip.OnPathsChanged([](size_t count) {
    // Update UI that depends on image count
    EnableControls(count > 0);
});

// Scroll position changed (during animation)
strip.OnScrollChanged([](float position, float maxScroll) {
    // Sync an external scrollbar, if you have one
    UpdateScrollbar(position, maxScroll);
});
```

### Callback Types

| Callback | Trigger | Parameters |
|----------|---------|------------|
| `OnSelectionChanged` | User clicks thumb, key navigation, or `SetSelection()` | `(index, path)` |
| `OnHoverChanged` | Mouse moves over different thumb | `(index, path)` - `SIZE_MAX` when leaving |
| `OnPathsChanged` | `SetPaths()`, `LoadFromFolder()`, `Initialize()` | `(count)` |
| `OnScrollChanged` | Wheel scroll, animation, key navigation | `(position, maxScroll)` |

## Two Integration Modes

### 1. Global Singleton (Simple)

Best for single-window applications. Uses a global instance internally.

```cpp
// Initialization
filmstrip::InitFromArgv(argc, argv);  // Or InitFromArgv(0, nullptr) for empty
filmstrip::SetHost(hwnd);

// Rendering
filmstrip::PaintOverGdi(hdc, width, height, titleBarHeight);

// Input forwarding
filmstrip::OnMouseMove(hwnd, x, y, width, height);
filmstrip::OnMouseWheel(hwnd, delta, x, y, titleBarH, width, height);
filmstrip::OnLButtonDown(hwnd, x, y, titleBarH, width, height);
filmstrip::OnKeyDown(hwnd, vk);
filmstrip::OnTimer(hwnd, timerId);
filmstrip::OnMouseLeaveClient(hwnd);

// Cleanup
filmstrip::Shutdown();
```

### 2. Widget Class (Flexible)

Best for multiple instances, custom positioning, or embedded use.

```cpp
class MyWindow {
    filmstrip::FilmStripWidget m_filmstrip;

public:
    bool Initialize(HWND hwnd) {
        // Option 1: Pass explicit vector of paths
        std::vector<std::wstring> paths = {
            L"C:\\Images\\photo1.jpg",
            L"C:\\Images\\photo2.png",
            L"C:\\Images\\photo3.jpg"
        };
        return m_filmstrip.Initialize(hwnd, paths);

        // Option 2: Scan folder automatically
        // return m_filmstrip.LoadFromFolder(L"C:\\Images");
    }

    void OnPaint(HDC hdc, int w, int h) {
        // Paint content above strip...

        // Paint strip at specific position (or use PaintOverGdi for full window)
        int dockH = m_filmstrip.DockHeightPx();
        m_filmstrip.Paint(hdc, 0, 0, w, h, m_titleBarHeight);
    }

    void OnMouseMove(int x, int y) {
        m_filmstrip.OnMouseMove(x, y, m_width, m_height);
    }

    void Shutdown() {
        m_filmstrip.Shutdown();
    }
};
```

## Message Forwarding Reference

Forward these messages from your window proc to the widget:

| Message | Handler | Returns | Notes |
|---------|---------|---------|-------|
| `WM_MOUSEMOVE` | `OnMouseMove(x, y, w, h)` | void | Call every mouse move |
| `WM_MOUSEWHEEL` | `OnMouseWheel(delta, x, y, titleH, w, h)` | bool | Return 0 if true |
| `WM_LBUTTONDOWN` | `OnLButtonDown(x, y, titleH, w, h)` | bool | Return 0 if true |
| `WM_KEYDOWN` | `OnKeyDown(vk)` | bool | Return 0 if true |
| `WM_TIMER` | `OnTimer(timerId)` | bool | Return 0 if true |
| `WM_MOUSELEAVE` | `OnMouseLeave()` | void | Cancel hover |

### WM_MOUSELEAVE Handling

The widget needs to know when the mouse leaves the window to cancel hover effects:

```cpp
case WM_MOUSEMOVE: {
    // ... forward to filmstrip ...
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
    TrackMouseEvent(&tme);
    break;
}

case WM_MOUSELEAVE:
    filmstrip::OnMouseLeaveClient(hwnd);
    return 0;
```

## Customization

### Visual Options

Edit `options` in `FilmStrip.h`:

```cpp
namespace options {
    // Disable hover animation
    constexpr bool focusAnimation = false;

    namespace layout {
        // Change thumb size
        constexpr int thumbSize = 120;

        // Adjust padding if clipped
        namespace padding {
            constexpr int bottom = 35;  // More bottom clearance
        }

        // Adjust visual effects
        namespace visual {
            constexpr int shadowV = 6;  // Larger shadow
        }
    }

    namespace thumbOptions {
        // Disable shadows
        constexpr float dropShadowSize = 0.f;

        // Letterbox instead of crop
        constexpr ThumbFill fill = ThumbFill::Contain;
    }
}
```

### Positioning

By default, the film strip docks to the bottom. For custom positioning, use the widget class:

```cpp
// Paint at custom Y position (not at bottom)
void PaintAt(HDC hdc, int x, int y, int width, int titleBarH) {
    int dockH = FilmStripWidget::DockHeightPx();

    // Create a memory DC of dock size
    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, width, dockH);
    SelectObject(memDc, bmp);

    // Paint strip into memory DC
    m_strip.Paint(memDc, 0, 0, width, dockH, titleBarH);

    // Blit to final position
    BitBlt(hdc, x, y, width, dockH, memDc, 0, 0, SRCCOPY);

    // Cleanup...
}
```

## Threading

The widget is **not thread-safe**. All calls must be from the same thread that owns the HWND.

Direct2D and WIC factories are created lazily on first paint.

## Resource Management

- `Initialize()` / `Shutdown()` can be called multiple times
- The widget cleans up D2D/DWrite resources on `Shutdown()`
- Thumbnail bitmaps are cached and released when the render target is recreated

## Troubleshooting

### Thumbnails not showing
- Check that `InitFromArgv()` found images (verify paths)
- Ensure `PaintOverGdi()` is called after your GDI+ content

### Clipping at window edges
- Increase `options::layout::padding::bottom` (moves thumbs up)
- Increase `options::layout::visual::shadowV` (if shadow clipped)
- See [layout.md](./layout.md)

### Hit testing issues
- Ensure mouse coordinates are client-relative (not screen)
- Verify window dimensions passed to handlers match actual client rect

### High DPI issues
- The widget reads DPI from the host window (`GetDpiForWindow`)
- Ensure your host window is per-monitor DPI aware

## Example: Minimal Host

```cpp
#include <windows.h>
#include "filmstrip/FilmStrip.h"

#pragma comment(lib, "gdiplus.lib")

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        filmstrip::InitFromArgv(__argc, __wargv);
        filmstrip::SetHost(hwnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        filmstrip::PaintOverGdi(hdc, rc.right, rc.bottom, 0);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        filmstrip::OnMouseMove(hwnd, pt.x, pt.y, rc.right, rc.bottom);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        break;
    }

    case WM_MOUSELEAVE:
        filmstrip::OnMouseLeaveClient(hwnd);
        return 0;

    case WM_DESTROY:
        filmstrip::Shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"FilmStripHost";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Film Strip Host",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, wc.hInstance, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
```

## API Reference

### FilmStripWidget Class

| Method | Description |
|--------|-------------|
| `Initialize(hwnd, paths)` | Initialize with explicit image paths |
| `SetPaths(paths)` | Update image list dynamically |
| `LoadFromFolder(path)` | Scan folder for images (jpg/png) |
| `InitFromArgv(argc, argv)` | Initialize from command line (`--src folder`) |
| `Shutdown()` | Clean up resources |
| `DockHeightPx()` | Static: height of dock area |
| `HasContent()` | Returns true if has images to show |
| `Paint(hdc, x, y, w, h, titleH)` | Paint at specific position |
| `PaintOverGdi(hdc, w, h, titleH)` | Paint at bottom of full-window DC |
| `RequestPaint()` | Invalidate host window |
| `OnKeyDown(vk)` | Keyboard navigation |
| `OnMouseWheel(...)` | Horizontal scroll |
| `OnLButtonDown(...)` | Click selection |
| `OnMouseMove(...)` | Hover effects |
| `OnMouseLeave()` | Cancel hover |
| `OnTimer(timerId)` | Animation frame |
| `Get/SetSelection(idx)` | Current selection |
| `GetCurrentPath()` | Path of selected image |
| `GetHoverIndex()` | Currently hovered thumb (SIZE_MAX if none) |
| `OnSelectionChanged(fn)` | Callback when selection changes |
| `OnHoverChanged(fn)` | Callback when hover changes |
| `OnPathsChanged(fn)` | Callback when image list changes |
| `OnScrollChanged(fn)` | Callback during scroll animation |

### Legacy Functions

Same API as widget, but operates on global singleton. See `FilmStrip.h` for signatures.
