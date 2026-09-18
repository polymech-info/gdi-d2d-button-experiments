#pragma once

#include <windows.h>
#include <d2d1.h>
#include <string>
#include <memory>

namespace filmstrip {

// Forward declarations
namespace internal {
class ThumbFactory;
}

// ============================================================================
// Thumb Type Enum
// ============================================================================

enum class ThumbType {
    Image,      // Photo/image files (jpg, png, etc.)
    Icon,       // Generic icon-based (text files, documents, etc.)
    Placeholder // Unknown/unloadable items
};

// ============================================================================
// Base Thumb Class
// ============================================================================

// Abstract base class for all filmstrip thumbnails.
// Each thumb knows how to render itself and provides metadata for layout.
class Thumb {
public:
    virtual ~Thumb() = default;

    // ------------------------------------------------------------------------
    // Type & Identity
    // ------------------------------------------------------------------------

    virtual ThumbType GetType() const = 0;

    // Full file path (if backed by a file)
    virtual const std::wstring& GetPath() const = 0;

    // Display label (filename or description)
    virtual std::wstring GetLabel() const = 0;

    // ------------------------------------------------------------------------
    // Rendering
    // ------------------------------------------------------------------------

    // Render the thumb content into the specified rounded rectangle.
    // opacity: 0.0-1.0 for fade effects
    virtual void Render(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& rect, float opacity) const = 0;

    // Render a placeholder/loading state (called when thumb content not ready)
    virtual void RenderPlaceholder(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& rect) const;

    // ------------------------------------------------------------------------
    // Layout
    // ------------------------------------------------------------------------

    // Preferred size at scale=1.0 (thumbs can scale up with focus animation)
    virtual D2D1_SIZE_F GetPreferredSize() const = 0;

    // Whether this thumb has loaded its content and is ready to render
    virtual bool IsLoaded() const = 0;

    // Preload content (optional optimization - called before visible)
    virtual void Preload(ID2D1RenderTarget* rt) = 0;

    // Release heavy resources (keep metadata)
    virtual void Unload() = 0;

protected:
    Thumb() = default;

    // Helper: Create a rounded rect from center and size
    static D2D1_ROUNDED_RECT MakeRoundedRect(float cx, float cy, float size, float radius);
};

// ============================================================================
// ImageThumb - For photo/image files
// ============================================================================

// Renders image files (JPG, PNG) using WIC and D2D.
class ImageThumb : public Thumb {
public:
    explicit ImageThumb(std::wstring path);
    ~ImageThumb() override;

    // Thumb interface
    ThumbType GetType() const override { return ThumbType::Image; }
    const std::wstring& GetPath() const override { return path_; }
    std::wstring GetLabel() const override;

    void Render(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& rect, float opacity) const override;

    D2D1_SIZE_F GetPreferredSize() const override;
    bool IsLoaded() const override { return bitmap_ != nullptr; }

    void Preload(ID2D1RenderTarget* rt) override;
    void Unload() override;

    // Image-specific: get the D2D bitmap (for advanced usage)
    ID2D1Bitmap* GetBitmap() const { return bitmap_; }

private:
    std::wstring path_;
    mutable ID2D1Bitmap* bitmap_ = nullptr;
    mutable UINT bitmapWidth_ = 0;
    mutable UINT bitmapHeight_ = 0;
    mutable bool attemptedLoad_ = false;

    void EnsureLoaded(ID2D1RenderTarget* rt) const;
};

// ============================================================================
// Thumb Factory
// ============================================================================

// Creates the appropriate Thumb subtype based on file path.
// Future: could return IconThumb for .txt, .doc, etc.
class ThumbFactory {
public:
    // Create a thumb for the given file path.
    // Returns nullptr if path is unsupported/invalid.
    static std::unique_ptr<Thumb> CreateFromPath(const std::wstring& path);

    // Check if a file path is supported (has a known extension)
    static bool IsSupported(const std::wstring& path);

    // Get the thumb type that would be created for this path
    static ThumbType GetThumbType(const std::wstring& path);
};

// ============================================================================
// Thumb Utilities
// ============================================================================

namespace thumb {

// Default thumb size (matches options::layout::thumbSize)
constexpr float kDefaultThumbSize = 100.0f;

// Default corner radius
constexpr float kDefaultCornerRadius = 9.0f;

// Helper: Draw a rounded rectangle with shadow and border
void DrawChrome(ID2D1RenderTarget* rt, const D2D1_ROUNDED_RECT& rect, bool selected);

} // namespace thumb

} // namespace filmstrip
