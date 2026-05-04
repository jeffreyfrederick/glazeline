# Glazeline

A high-performance Windows news ticker built with Win32 + Direct2D + DirectWrite.

Smooth horizontal scrolling, vertical carousel cycling, configurable typography and effects, and zero-stutter hot-reload of content from a JSON file.

> **Status: alpha.** Single-file implementation, single-author project, single-platform (Windows). API and config schema may change.

---

## Features

- **Hardware-accelerated rendering** via Direct2D + DirectWrite
- **Hot-reload** of `content.json` with no frame drops, no scroll reset, no visible flicker
- **Immutable tape model**: items have persistent positions; reloading content does not shift visible text
- **Pre-computed text layouts**: DirectWrite shaping happens off the render thread
- **Configurable** layout, typography, colors, gradients, animation, effects via JSON
- **Optional brand logo** (PNG) in the brand panel
- **Window persistence** (position, width, snap & always-on-top toggles) across launches
- **Edge snapping** during drag, horizontal-only resize, locked vertical height
- **Right-click menu**: Always on top · Snap to edges · Close

---

## How hot-reload works

The reload pipeline is built around a **build → validate → swap** snapshot model with strict thread separation:

1. A **background worker thread** polls `assets/content.json`'s mtime every ~400 ms.
2. On change, the worker reads the file, parses the JSON, runs validation (rejects empty/malformed snapshots), uppercases text, creates `IDWriteTextLayout` instances, computes per-item content hashes (FNV-1a 64-bit), and assigns sequential tape offsets — **all on the worker**.
3. The completed `ContentSnapshot` is published into a mutex-protected slot. Latest snapshot wins.
4. The **render thread** picks up the pending snapshot at the top of `Render()`, atomically swaps it into live state, and hands the old data off to a trash queue.
5. The worker thread drains the trash queue on its next tick, so destruction of old layouts/strings happens on the worker — never on the render thread.

The render thread's per-reload cost is just a mutex-acquire, a few pointer swaps, and a `fmodf` — well under 100 µs.

A single bad save does **not** cause a retry storm: mtime is recorded regardless of build outcome, so each file change gets exactly one rebuild attempt.

---

## Architecture: the immutable tape model

Each `CarouselGroup` is a horizontal "tape". Each `CarouselItem` carries a persistent `tapeOffsetX` (its position along the tape) and a persistent `slotWidth` (item width + loop gap). Both are populated when the snapshot is built on the worker; the renderer reads them directly:

```cpp
DrawItem(item, zoneX, curY, g_carouselScrollX - item.tapeOffsetX);
```

That's it. No cumulative summing per frame. No measurement on the render thread. Tape positions only change when content structurally changes (items added/removed/reordered, or text edits that change visible width upstream of an item).

`g_carouselScrollX` is **not** reset on reload. Animation timers, group index, and hold-timer state all persist across snapshot swaps. The reload is structurally invisible to the user — visible text stays exactly where it was.

### Single source of truth for text metrics

`cachedWidth` is read directly from the same `IDWriteTextLayout` instance that `DrawTextLayout` will render. There is no parallel measurement helper — eliminating any chance of metric drift between spacing math and rasterization.

### Why two `CreateTextLayout` calls per element?

Badge text needs `maxWidth = textWidth + 2·padding` for `DWRITE_TEXT_ALIGNMENT_CENTER` to anchor correctly. Since `textWidth` is unknown until measurement, we create a temporary layout (max width 8000) to measure, then create the **persistent** layout born with the correct bounds — no `SetMaxWidth` post-mutation, which would leave the centering anchor pointing at the original wide max width.

---

## Configuration

All visual and behavioral knobs live in JSON files under `assets/`:

| File | Purpose | Hot-reloaded |
|------|---------|--------------|
| `config.json` | Layout dimensions, typography, animation, effects, window defaults | No (loaded at startup) |
| `content.json` | What the ticker shows (groups, items, badges, text) | **Yes** |
| `colors.json` | Brand / clock / column / shadow / badge palette and gradients | No (loaded at startup) |
| `settings.json` | Persisted user state (window position, width, toggles) | Auto-generated |
| `images/logo.png` | Optional brand logo (replaces brand text when present) | No |

Edit `content.json`, save, and the ticker swaps to the new content within one render frame.

`settings.json` is **app-managed** — never edit by hand. It's written on every meaningful state change (window move/resize, toggle change, exit) using deterministic field ordering for clean diffs.

### Color value syntax

Solid: `"#RRGGBB"` or `"#RRGGBBAA"` (8-digit form for per-stop alpha)

Gradient: `"linear(angle,stop,...)"` where `angle` is in CSS degrees (0 = bottom→top, 90 = left→right, 180 = top→bottom) and each stop is `"#RRGGBB[AA] [pos%]"`. Example:

```
"linear(180,#000000FF 0%,#00000000 50%,#000000FF 100%)"
```

---

## Building

Requires:
- Windows 10 1703 or later (Direct2D 1.3 SVG + ID2D1DeviceContext5 path; SVG support not currently used but the QI exists)
- Visual Studio 2022 Build Tools (or full Visual Studio 2022) with the C++ workload
- Windows 10 SDK

From a **Developer Command Prompt for VS** (so `cl.exe` is on PATH):

```
build.bat
```

Or manually:

```
cl /nologo /EHsc /std:c++17 ^
   /Fo:build\main.obj /Fe:build\glazeline.exe ^
   src\main.cpp ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib
```

Output: `build\glazeline.exe`

Run from anywhere — the exe resolves `assets/` relative to its parent directory (`<exeDir>\..\assets\`).

---

## Project layout

```
glazeline/
├── src/
│   └── main.cpp           Single-file implementation (~1800 LOC).
│                          Section dividers organize: config model,
│                          settings, data types, globals, JSON utilities,
│                          loaders, content hot-reload, render, window proc,
│                          and entry point.
├── assets/
│   ├── config.json        App configuration
│   ├── content.json       Carousel content (hot-reloaded)
│   ├── colors.json        Color/gradient definitions
│   ├── settings.json      User state (auto-generated)
│   ├── images/            Optional brand logo (logo.png)
│   └── fonts/             Roboto Condensed font files
├── build/                 Compile output (gitignored)
├── build.bat              Convenience build script
└── README.md
```

### Suggested future structure

When the codebase grows beyond a single file, a logical split could be:

```
src/
├── core/      AppConfig, UserSettings, JSON utilities
├── data/      ColorSpec, BadgeStyle, ContentSnapshot, parsers, hot-reload worker
├── render/    Direct2D init, brushes, RenderCarousel, DrawItem, text layout
├── window/    WndProc, message loop, drag/snap logic, menu
└── main.cpp   WinMain only
```

Single-file is fine at the current scope; this split becomes useful around 3000+ LOC or once a second platform target is added.

---

## Configuration knobs (selected)

**`config.json` highlights:**
- `layout.ticker_height` — overall ticker height in DIPs
- `layout.brand.{width, padding, alignment, shadow_width, image.*}` — brand panel
- `layout.clock.{width, padding, alignment, fade_width}` — clock panel
- `layout.spacing.badge.{padding, corner_radius, gap}` and `layout.spacing.text_gap`
- `typography.family` and per-role `{size, weight, style}`
- `animation.{scroll, carousel, loop_gap, max_delta_time, uppercase_text}`
- `effects.{badge_text_shadow, global_shadow}`
- `window.{min_width, resize_border, snap_distance, defaults}`

See `assets/config.json` for the full schema with defaults.

---

## Contributing

This is a personal alpha. Issues and PRs are welcome but expect responses to be slow.

When proposing changes, please verify the four invariants the codebase has been tuned for:

1. **No frame drops on `content.json` save** — all heavy work stays on the worker thread.
2. **No visible shift on hot-reload** — `g_carouselScrollX` and tape offsets persist across snapshot swaps.
3. **Single source of truth for text metrics** — `cachedWidth` always reads from the layout that will render.
4. **Strict snapshot validation** — empty / malformed / partial snapshots never replace live state.

---

## License

TBD — pending alpha release. Not yet open-source-licensed; all rights reserved by the author until a license is added.
