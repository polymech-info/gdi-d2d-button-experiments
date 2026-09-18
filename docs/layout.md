# Film Strip Layout Calculations

This document describes the layout math for the film strip dock, accounting for scale-aware positioning to prevent clipping when thumbnails enlarge on hover.

## Overview

The film strip is positioned at the bottom of the window. Thumbnails scale up to 1.45x on hover (cover-flow effect). The layout must ensure scaled thumbnails don't clip at the top or bottom edges.

## Key Concepts

- **Unscaled thumb**: Base 100px thumbnail
- **Scaled thumb**: Up to 145px (1.45x) when hovered
- **Visual overflow**: Drop shadows, glow, and halos extend beyond the thumb rectangle
- **Center-point scaling**: Thumbs grow/shrink from their center point
- **Content area**: The space inside padding where scaled thumbs (with overflow) must fit

## Constants

### From `options::layout` (FilmStrip.h)

| Constant | Value | Description |
|----------|-------|-------------|
| `thumbSize` | 100 | Base thumbnail width/height (px) |
| `thumbGap` | 25 | Horizontal gap between thumbnails (px) |
| `padding.top` | 5 | Space above visual overflow (px) |
| `padding.bottom` | 25 | Space below visual overflow (px) |
| `padding.left` | 14 | Left inset for thumb row (px) |
| `padding.right` | 14 | Right inset for thumb row (px) |
| `visual.glowTop` | 0 | Additional glow above padding.top |
| `visual.glowBottom` | 0 | Additional glow below padding.bottom |
| `visual.shadowV` | 4 | Vertical shadow extent (from `dropShadowSize` + softness) |
| `visual.halo` | 8 | Selection halo/ring overflow around thumb |
| `visual.overflowTop` | 4 | Total overflow above scaled thumb |
| `visual.overflowBottom` | 10 | Total overflow below scaled thumb |
| `previewPad` | 14 | Padding around preview image (px) |

### Visual Overflow Calculation

```
overflowTop    = glowTop + shadowV/2 + halo/2
               = 0 + 2 + 4
               = 6 px (approx, rounded)

overflowBottom = glowBottom + shadowV/2 + halo/2
               = 0 + 2 + 8
               = 10 px (approx, rounded)
```

### From animation constants (FilmStrip.cpp)

| Constant | Value | Description |
|----------|-------|-------------|
| `kFocusMaxBoost` | 0.45 | Max scale increase (45%) |
| `kMaxScale` | 1.45 | Maximum thumb scale factor |

## Calculations

### Maximum Scaled Thumb Size

```
kMaxThumbPx = thumbSize * kMaxScale
            = 100 * 1.45
            = 145 px
```

### Film Strip Height (Dock Height)

The strip must accommodate visual overflow, the full scaled thumb, plus padding:

```
kContentTop = padding.top + visual.overflowTop
            = 5 + 4
            = 9 px

kDockH = padding.top + visual.overflowTop + kMaxThumbPx + visual.overflowBottom + padding.bottom
       = 5 + 4 + 145 + 10 + 25
       = 189 px
```

### Unscaled Thumb Vertical Position

To ensure the scaled thumb (with overflow) is centered in the content area, we calculate where to place the unscaled thumb so both share the same center point.

**Step 1: Find content area start**
```
kContentTop = padding.top + visual.overflowTop
            = 5 + 4
            = 9 px (from strip top)
```

**Step 2: Find center of scaled thumb within content area**
```
kScaledThumbCenterY = kContentTop + (kMaxThumbPx / 2)
                    = 9 + 72.5
                    = 81.5 px (from strip top)
```

**Step 3: Position unscaled thumb at same center**
```
kThumbYInDock = kScaledThumbCenterY - (thumbSize / 2)
              = 81.5 - 50
              = 31.5 px (from strip top)
```

### Visual Verification

| State | Top Edge | Visual Top | Thumb Top | Center | Thumb Bottom | Visual Bottom | Bottom Edge |
|-------|----------|------------|-----------|--------|--------------|---------------|-------------|
| Unscaled (100px) | - | - | 31.5 | 81.5 | 131.5 | - | - |
| Scaled (145px) | - | - | 9 | 81.5 | 154 | - | - |
| With Overflow | 5 | 9 | - | 81.5 | - | 164 | 189 |

When scaled with visual effects:
- Visual top (glow/shadow/halo) at 5px (at `padding.top`)
- Thumb top at 9px
- Thumb bottom at 154px
- Visual bottom at 164px
- Clearance to strip bottom: 189 - 164 = 25px (matches `padding.bottom`)

## Horizontal Layout

### Content Width

```
ContentWidth = windowW - padding.left - padding.right
             = windowW - 14 - 14
             = windowW - 28
```

### Thumb Row Starting Position

```
x0 = padding.left - scrollOffset
   = 14 - g_scroll
```

## Tuning Guide

### Problem: Thumbs clipped at bottom
**Solution**: Increase `padding.bottom`
- Effect: Moves unscaled thumb UP, increases clearance below scaled thumb
- Example: Change 25 → 35 adds 10px bottom clearance

### Problem: Thumbs clipped at top
**Solution**: Increase `padding.top`
- Effect: Moves unscaled thumb DOWN, increases clearance above scaled thumb
- Example: Change 5 → 15 adds 10px top clearance

### Problem: Need more horizontal margin
**Solution**: Increase `padding.left` and/or `padding.right`
- Effect: Shrinks content area, thumbs start further from window edges

### Problem: Scaled thumb too large (excessive clipping)
**Solution**: Reduce `kFocusMaxBoost`
- Effect: Reduces max scale from 1.45x to lower value
- Must also adjust `padding.top` and `padding.bottom` proportionally

### Problem: Shadow/halo clipped at edges
**Solution**: Increase `visual.shadowV` or `visual.halo`
- Effect: Increases dock height to accommodate larger visual effects
- Check that `overflowTop` and `overflowBottom` cover your shadow size

### Problem: Glow extends too far into content area
**Solution**: Increase `visual.glowTop` or `padding.top`
- Effect: Moves glow up and away from the preview image area

## Formula Summary

```cpp
// Visual overflow (half of shadow/halo on each side)
overflowTop     = visual.glowTop + visual.shadowV/2 + visual.halo/2;     // ~4
overflowBottom  = visual.glowBottom + visual.shadowV/2 + visual.halo/2;      // ~10

// Derived constants (computed at compile time)
kMaxScale       = 1.0f + kFocusMaxBoost;           // 1.45
kMaxThumbPx     = thumbSize * kMaxScale;           // 145
kContentTop     = padding.top + overflowTop;       // 9
kDockH          = padding.top + overflowTop + kMaxThumbPx + overflowBottom + padding.bottom;
                                                 // 189
kThumbYInDock   = kContentTop + kMaxThumbPx/2 - thumbSize/2;
                                                 // 31.5

// Runtime: strip top position
stripY          = windowH - kDockH;

// Runtime: thumb baseline Y (unscaled)
yThumb          = stripY + kThumbYInDock;
```

## Relationship Between Padding and Position

Increasing `padding.bottom` actually moves the unscaled thumb **up** (closer to strip top), which seems counter-intuitive but is correct:

1. More bottom padding increases `kDockH`
2. `kScaledThumbCenterY` stays fixed relative to strip top (depends on `padding.top`)
3. Unscaled thumb is always positioned at `center - 50`
4. The scaled thumb therefore has more room to grow downward

Similarly, increasing `padding.top` moves the unscaled thumb **down**.

## See Also

- `FilmStrip.h`: Tunable constants in `options::layout`
- `FilmStrip.cpp`: Calculation code (lines 63-78)
- `main.cpp`: Uses `DockHeightPx()` to reserve space for the strip

## Related Visual Effect Settings

From `thumbOptions` (FilmStrip.h):

| Constant | Value | Description |
|----------|-------|-------------|
| `dropShadowSize` | 3.0 | Base shadow blur size |
| `dropShadowHardness` | 0.15 | Shadow falloff (0=soft, 1=hard) |

Increase `visual.shadowV` if you increase `dropShadowSize` to prevent shadow clipping.
