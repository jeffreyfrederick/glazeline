#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <optional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <ctime>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "ole32")

// Brand drop shadow opacity. Visual-tuning constant — kept hardcoded; not surfaced in config.
static constexpr float BRAND_SHADOW_A = 0.95f;

// WM_TIMER id used to drive Render() during the Win32 modal move/resize loop, where
// the message pump is owned by DefWindowProc and our PeekMessage idle path doesn't run.
static constexpr UINT_PTR DRAG_RENDER_TIMER_ID = 1;

// ─── Fixed colors ────────────────────────────────────────────────────────────
static const D2D1_COLOR_F COL_BG    = {0.051f, 0.059f, 0.078f, 1.0f}; // #0D0F14
static const D2D1_COLOR_F COL_WHITE = {1.000f, 1.000f, 1.000f, 1.0f};

// ─── Config model ────────────────────────────────────────────────────────────
// Hierarchical mirror of config.json. Every field has a sensible default so the
// app remains fully functional if the config file is missing or partial.
enum class TextAlign { Left, Center, Right };

struct PaddingLR { float left = 0.0f, right = 0.0f; };
struct PointXY   { float x    = 0.0f, y     = 0.0f; };

struct BrandImage {
    std::wstring path;                       // relative to project root (e.g. "assets/images/logo.png"); empty disables the image
    float        width     = 0.0f;           // 0 falls back to brand.width
    TextAlign    alignment = TextAlign::Center;
};

struct BrandLayout {
    float      width        = 160.0f;
    PaddingLR  padding      = {0.0f, 8.0f};
    float      marginRight  = 28.0f;
    TextAlign  alignment    = TextAlign::Center;
    float      shadowWidth  = 14.0f;
    BrandImage image;
};

struct ClockLayout {
    float     width      = 140.0f;
    PaddingLR padding;
    TextAlign alignment  = TextAlign::Center;
    float     fadeWidth  = 140.0f;
};

struct BadgeSpacing {
    PointXY   padding      = {9.0f, 8.0f};
    float     cornerRadius = 14.0f;
    PaddingLR gap          = {40.0f, 12.0f};
};

struct Spacing {
    BadgeSpacing badge;
    float        textGap = 40.0f;
};

struct LayoutConfig {
    float       tickerHeight = 40.0f;
    BrandLayout brand;
    ClockLayout clock;
    Spacing     spacing;
};

struct FontRole {
    float              size   = 16.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_BLACK;
    DWRITE_FONT_STYLE  style  = DWRITE_FONT_STYLE_NORMAL;
};

struct Typography {
    std::wstring family   = L"Roboto Condensed";
    FontRole     brand    = {26.0f, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC};
    FontRole     badge    = {16.0f, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_NORMAL};
    FontRole     clock    = {26.0f, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_NORMAL};
    FontRole     carousel = {26.0f, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_NORMAL};
};

struct ScrollAnim   { float delay = 3.5f;  float speed      = 100.0f; };
struct CarouselAnim { float hold  = 20.0f; float transition = 0.45f;  };

struct AnimationConfig {
    ScrollAnim   scroll;
    CarouselAnim carousel;
    float        loopGap        = 80.0f;
    float        maxDeltaTime   = 0.033f;
    bool         uppercaseText  = true;
};

struct BadgeTextShadow {
    PointXY offset  = {0.0f, 1.5f};
    int     blur    = 1;
    float   opacity = 0.95f;
};

struct GlobalShadow { float opacity = 0.0f; };

struct EffectsConfig {
    BadgeTextShadow badgeTextShadow;
    GlobalShadow    globalShadow;
};

struct WindowDefaults {
    bool snapEnabled = true;
    bool alwaysOnTop = false;
};

struct WindowConfig {
    int            minWidth     = 320;
    int            resizeBorder = 6;
    int            snapDistance = 20;
    WindowDefaults defaults;
};

struct AppConfig {
    LayoutConfig    layout;
    Typography      typography;
    AnimationConfig animation;
    EffectsConfig   effects;
    WindowConfig    window;
};

static AppConfig g_cfg;  // populated by LoadAppConfig at startup

// ─── User settings model ─────────────────────────────────────────────────────
// settings.json captures app-controlled state that persists across runs.
// Layered with config.json: config supplies defaults, settings overrides at runtime.
struct WinPos  { int x = 0, y = 0; };
struct WinSize { int width = 0; };  // height is locked by WM_GETMINMAXINFO; absent by design

struct WindowSettings {
    WinPos  position;
    WinSize size;
    bool    alwaysOnTop = false;
    bool    snapEnabled = true;
};

struct UserSettings {
    int            version = 1;
    WindowSettings window;
};

static UserSettings g_settings;
static std::wstring g_settingsPath;  // resolved at startup; reused by every save call

static DWRITE_TEXT_ALIGNMENT ToDWriteAlign(TextAlign a)
{
    switch (a) {
        case TextAlign::Left:  return DWRITE_TEXT_ALIGNMENT_LEADING;
        case TextAlign::Right: return DWRITE_TEXT_ALIGNMENT_TRAILING;
        default:               return DWRITE_TEXT_ALIGNMENT_CENTER;
    }
}

// ─── Data model ──────────────────────────────────────────────────────────────
struct ColorSpec {
    D2D1_COLOR_F                    solid      = {1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<D2D1_GRADIENT_STOP> gradStops;
    float                           gradAngle  = 90.0f;
    bool                            isGradient = false;
};

struct BadgeStyle {
    ColorSpec    bg;
    D2D1_COLOR_F color = {1.0f, 1.0f, 1.0f, 1.0f};
};

enum class ElemType { Badge, Text };

struct Element {
    ElemType     type;
    std::wstring text;
    std::wstring badgeType;          // raw "badge_type" string; part of identity hash
    BadgeStyle   style;
    float        cachedWidth = 0.0f; // populated by PostProcessGroups (worker thread)
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;  // precomputed shaping; render uses DrawTextLayout
};

struct CarouselItem {
    std::vector<Element> elements;
    uint64_t             id = 0;     // content-derived FNV-1a hash; populated by PostProcessGroups
    // Immutable tape model: each item has a persistent position in its group's tape.
    // Computed sequentially in PostProcessGroups; the renderer reads it directly. Across reloads
    // this preserves stability — items whose preceding items are unchanged keep the same offset.
    float                tapeOffsetX = 0.0f;
    float                slotWidth   = 0.0f;  // item width + LOOP_GAP (the slot the item owns)
};

// A <carousel> block: one or more <item> rows that cycle vertically in the zone.
struct CarouselGroup {
    std::vector<CarouselItem> items;
    float totalWidth = 0.0f;  // = sum of items' slotWidth; tape length for this group
};

// Atomic build target for hot-reload: parsed + measured groups, ready to swap into g_groups.
struct ContentSnapshot {
    std::vector<CarouselGroup> groups;
};

// ─── Globals ─────────────────────────────────────────────────────────────────
static ID2D1Factory*           g_factory  = nullptr;
static ID2D1HwndRenderTarget*  g_rt       = nullptr;
static ID2D1Bitmap*            g_brandBitmap = nullptr;  // optional brand logo (assets/images/logo.png); when present, replaces brand text
static IWICImagingFactory*     g_wic         = nullptr;  // for PNG decoding
static IDWriteFactory*         g_dw       = nullptr;
static IDWriteFontCollection1* g_fontColl = nullptr;

static IDWriteTextFormat* g_fmtBrand    = nullptr;
static IDWriteTextFormat* g_fmtBadge    = nullptr;
static IDWriteTextFormat* g_fmtClock    = nullptr;
static IDWriteTextFormat* g_fmtCarousel = nullptr;

static ID2D1SolidColorBrush*     g_wBrush        = nullptr; // white, never changes
static ID2D1SolidColorBrush*     g_dynBrush      = nullptr; // reused with SetColor
static ID2D1LinearGradientBrush* g_bgBrush          = nullptr;
static ID2D1LinearGradientBrush* g_clockBgBrush     = nullptr;
static ID2D1LinearGradientBrush* g_brandShadow      = nullptr;
static ID2D1LinearGradientBrush* g_clockFade        = nullptr;
static ID2D1Layer*               g_clockFadeLayer    = nullptr;

static ColorSpec g_shadow;  // shadow gradient (any angle, any stops, optional alpha) — opacity multiplier lives in g_cfg.effects.globalShadow.opacity

// Edge-snap drag state: previous cursor-natural rect, used to detect approach vs. retreat
static bool g_prevMoveValid = false;
static RECT g_prevMoveNat   = {};
static bool g_snapEnabled   = true;  // toggled via right-click menu
static bool g_alwaysOnTop   = false; // toggled via right-click menu

// Color specs loaded from colors.json; defaults match original hardcoded values
static ColorSpec g_colBg;
static ColorSpec g_clockBg;
static ColorSpec g_brandBg;
static ColorSpec g_brandText;
static ColorSpec g_clockText;
static ColorSpec g_colText;

static std::vector<CarouselGroup> g_groups;
static int   g_groupIdx        = 0;  // current <carousel> index
static float g_carouselHoldT   = 0.0f;
static float g_carouselAnimT   = 0.0f;
static float g_carouselScrollX = 0.0f;
static float g_carouselItemW   = 0.0f;

static std::chrono::steady_clock::time_point g_lastTime;

// content.json hot-reload state
static std::map<std::wstring, BadgeStyle>     g_badgeStyles;  // resolved at startup; worker reads, never modified after init
static std::wstring                           g_contentPath;  // resolved at startup; worker reads, never modified after init

// Worker thread: builds snapshots off the render thread; render only does an atomic pickup.
static std::thread                  g_workerThread;
static std::atomic<bool>            g_workerRun{false};
static std::mutex                   g_workerCvMu;
static std::condition_variable      g_workerCv;
static std::mutex                   g_pendingMu;
static std::optional<ContentSnapshot> g_pendingSnap;  // worker writes, render takes
static constexpr int                CONTENT_POLL_MS = 400;  // worker mtime poll cadence

// Deferred-destruction queue: old g_groups vectors handed off here on swap so the heavy
// work of destructing wstrings/sub-vectors happens on the worker thread, not in render.
static std::mutex                              g_trashMu;
static std::vector<std::vector<CarouselGroup>> g_trash;

// ─── Font collection ─────────────────────────────────────────────────────────
static IDWriteFontCollection1* LoadFontCollection(IDWriteFactory* dw, const wchar_t* fontDir)
{
    IDWriteFactory3* dw3 = nullptr;
    if (FAILED(dw->QueryInterface(__uuidof(IDWriteFactory3), (void**)&dw3))) return nullptr;

    IDWriteFontSetBuilder* builder = nullptr;
    if (FAILED(dw3->CreateFontSetBuilder(&builder))) { dw3->Release(); return nullptr; }

    const wchar_t* files[] = {
        L"RobotoCondensed-Regular.ttf",   L"RobotoCondensed-Medium.ttf",
        L"RobotoCondensed-SemiBold.ttf",  L"RobotoCondensed-Bold.ttf",
        L"RobotoCondensed-ExtraBold.ttf", L"RobotoCondensed-Black.ttf",
        L"RobotoCondensed-BlackItalic.ttf",
    };
    for (const wchar_t* f : files) {
        wchar_t path[MAX_PATH]; wcscpy_s(path, fontDir); wcscat_s(path, f);
        IDWriteFontFaceReference* ref = nullptr;
        if (SUCCEEDED(dw3->CreateFontFaceReference(path, nullptr, 0, DWRITE_FONT_SIMULATIONS_NONE, &ref))) {
            builder->AddFontFaceReference(ref); ref->Release();
        }
    }

    IDWriteFontSet* fs = nullptr; builder->CreateFontSet(&fs); builder->Release();
    IDWriteFontCollection1* coll = nullptr;
    if (fs) { dw3->CreateFontCollectionFromFontSet(fs, &coll); fs->Release(); }
    dw3->Release();
    return coll;
}

// ─── Color / gradient helpers ────────────────────────────────────────────────
// Accepts "#RRGGBB" or "#RRGGBBAA" (alpha optional, defaults to 1.0)
static D2D1_COLOR_F ParseHexColor(const std::wstring& hex)
{
    if (hex.size() < 7 || hex[0] != L'#') return {0.5f, 0.5f, 0.5f, 1.0f};
    auto hv = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        return 0;
    };
    auto isHex = [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f');
    };
    D2D1_COLOR_F col = {
        (hv(hex[1]) * 16 + hv(hex[2])) / 255.0f,
        (hv(hex[3]) * 16 + hv(hex[4])) / 255.0f,
        (hv(hex[5]) * 16 + hv(hex[6])) / 255.0f,
        1.0f
    };
    if (hex.size() >= 9 && isHex(hex[7]) && isHex(hex[8]))
        col.a = (hv(hex[7]) * 16 + hv(hex[8])) / 255.0f;
    return col;
}

// Parses "#RRGGBB" or "linear(angle,stop,...)" where each stop is "#RRGGBB" or "#RRGGBB pos%"
static ColorSpec ParseColorSpec(const std::wstring& val)
{
    ColorSpec cs;
    if (val.empty()) return cs;
    if (val[0] == L'#') { cs.solid = ParseHexColor(val); return cs; }
    if (val.rfind(L"linear(", 0) != 0) return cs;

    size_t close = val.find(L')', 7);
    std::wstring inner = val.substr(7, close == std::wstring::npos ? val.size()-7 : close-7);

    std::vector<std::wstring> parts;
    size_t p = 0;
    while (true) {
        size_t c = inner.find(L',', p);
        std::wstring part = inner.substr(p, c == std::wstring::npos ? c : c-p);
        size_t a = part.find_first_not_of(L" \t"), b = part.find_last_not_of(L" \t");
        if (a != std::wstring::npos) parts.push_back(part.substr(a, b-a+1));
        if (c == std::wstring::npos) break;
        p = c + 1;
    }
    if (parts.size() < 2) return cs;

    cs.gradAngle  = (float)wcstol(parts[0].c_str(), nullptr, 10);
    cs.isGradient = true;

    for (size_t i = 1; i < parts.size(); ++i) {
        const std::wstring& pt = parts[i];
        size_t h = pt.find(L'#');
        if (h == std::wstring::npos) continue;
        // Find end of hex digits after '#' so we know where the optional position field starts
        size_t hexEnd = h + 1;
        while (hexEnd < pt.size() &&
               ((pt[hexEnd] >= L'0' && pt[hexEnd] <= L'9') ||
                (pt[hexEnd] >= L'A' && pt[hexEnd] <= L'F') ||
                (pt[hexEnd] >= L'a' && pt[hexEnd] <= L'f'))) ++hexEnd;
        D2D1_COLOR_F col = ParseHexColor(pt.substr(h, hexEnd - h));

        float pos = -1.0f;
        size_t pct = pt.find(L'%', hexEnd);
        if (pct != std::wstring::npos) {
            size_t ns = pct;
            while (ns > hexEnd && (iswdigit(pt[ns-1]) || pt[ns-1]==L'.')) --ns;
            pos = (float)wcstof(pt.substr(ns, pct-ns).c_str(), nullptr) / 100.0f;
        }
        cs.gradStops.push_back({pos, col});
    }

    // Resolve auto-spaced stops (pos == -1)
    size_t n = cs.gradStops.size();
    for (size_t i = 0; i < n; ++i)
        if (cs.gradStops[i].position < 0.0f)
            cs.gradStops[i].position = (n == 1) ? 0.0f : (float)i / (float)(n-1);

    if (!cs.gradStops.empty()) cs.solid = cs.gradStops[0].color;
    return cs;
}

// Creates a gradient brush for cs within rect r. Returns nullptr if solid (use cs.solid directly).
// Caller must Release() the returned brush.
static ID2D1LinearGradientBrush* MakeGradBrush(const ColorSpec& cs, D2D1_RECT_F r)
{
    if (!cs.isGradient || cs.gradStops.size() < 2) return nullptr;
    ID2D1GradientStopCollection* sc = nullptr;
    g_rt->CreateGradientStopCollection(cs.gradStops.data(), (UINT32)cs.gradStops.size(), &sc);
    if (!sc) return nullptr;
    float rad = cs.gradAngle * 3.14159265f / 180.0f;
    float gx = sinf(rad), gy = -cosf(rad);
    float ccx = (r.left+r.right)*0.5f, ccy = (r.top+r.bottom)*0.5f;
    float len = fabsf(gx)*(r.right-r.left)*0.5f + fabsf(gy)*(r.bottom-r.top)*0.5f;
    ID2D1LinearGradientBrush* gb = nullptr;
    g_rt->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties({ccx-gx*len,ccy-gy*len},{ccx+gx*len,ccy+gy*len}),
        sc, &gb);
    sc->Release();
    return gb;
}

// ─── JSON utilities ──────────────────────────────────────────────────────────
static void JSkipWS(const std::wstring& s, size_t& p) {
    while (p < s.size() && (s[p]==L' '||s[p]==L'\t'||s[p]==L'\r'||s[p]==L'\n')) ++p;
}

static std::wstring JReadStr(const std::wstring& s, size_t& p) {
    if (p >= s.size() || s[p] != L'"') return {};
    ++p;
    std::wstring r;
    while (p < s.size() && s[p] != L'"') {
        if (s[p] == L'\\' && p+1 < s.size()) {
            ++p;
            switch (s[p]) {
                case L'"':  r += L'"';  break; case L'\\': r += L'\\'; break;
                case L'n':  r += L'\n'; break; case L't':  r += L'\t'; break;
                default:    r += s[p];  break;
            }
        } else { r += s[p]; }
        ++p;
    }
    if (p < s.size()) ++p;
    return r;
}

static size_t JMatchClose(const std::wstring& s, size_t pos) {
    wchar_t open  = s[pos];
    wchar_t close = (open==L'[') ? L']' : (open==L'{') ? L'}' : 0;
    if (!close) return std::wstring::npos;
    int depth = 0; bool inStr = false;
    for (size_t i = pos; i < s.size(); ++i) {
        if (inStr) { if (s[i]==L'\\') ++i; else if (s[i]==L'"') inStr = false; }
        else {
            if      (s[i]==L'"')               inStr = true;
            else if (s[i]==open)               ++depth;
            else if (s[i]==close && --depth==0) return i;
        }
    }
    return std::wstring::npos;
}

// Returns the value for 'key' in a JSON object string (outer {} optional)
static std::wstring JGetVal(const std::wstring& obj, const std::wstring& key) {
    size_t p = 0;
    JSkipWS(obj, p);
    if (p < obj.size() && obj[p] == L'{') ++p;
    while (p < obj.size()) {
        JSkipWS(obj, p);
        if (p >= obj.size() || obj[p] == L'}') break;
        if (obj[p] == L',') { ++p; continue; }
        if (obj[p] != L'"') break;
        std::wstring k = JReadStr(obj, p);
        JSkipWS(obj, p);
        if (p >= obj.size() || obj[p] != L':') break;
        ++p; JSkipWS(obj, p);
        if (k == key) {
            if (p >= obj.size()) return {};
            if (obj[p] == L'"') return JReadStr(obj, p);
            if (obj[p]==L'['||obj[p]==L'{') {
                size_t cl = JMatchClose(obj, p);
                return cl==std::wstring::npos ? std::wstring{} : obj.substr(p, cl-p+1);
            }
            size_t e = p;
            while (e < obj.size() && obj[e]!=L','&&obj[e]!=L'}'&&obj[e]!=L'\n'&&obj[e]!=L'\r') ++e;
            std::wstring v = obj.substr(p, e-p);
            size_t a = v.find_first_not_of(L" \t"), b = v.find_last_not_of(L" \t");
            return (a!=std::wstring::npos) ? v.substr(a, b-a+1) : L"";
        }
        // Skip non-matching value
        if (p >= obj.size()) break;
        if (obj[p] == L'"') JReadStr(obj, p);
        else if (obj[p]==L'['||obj[p]==L'{') {
            size_t cl = JMatchClose(obj, p);
            if (cl==std::wstring::npos) break;
            p = cl + 1;
        } else { while (p < obj.size() && obj[p]!=L','&&obj[p]!=L'}') ++p; }
    }
    return {};
}

// Splits a JSON array into element substrings (objects/arrays kept whole; strings unquoted)
static std::vector<std::wstring> JArrayElems(const std::wstring& arr) {
    std::vector<std::wstring> result;
    size_t p = 0;
    JSkipWS(arr, p);
    if (p >= arr.size() || arr[p] != L'[') return result;
    ++p;
    while (true) {
        JSkipWS(arr, p);
        if (p >= arr.size() || arr[p] == L']') break;
        if (arr[p] == L',') { ++p; continue; }
        if (arr[p]==L'{'||arr[p]==L'[') {
            size_t cl = JMatchClose(arr, p);
            if (cl==std::wstring::npos) break;
            result.push_back(arr.substr(p, cl-p+1));
            p = cl + 1;
        } else if (arr[p] == L'"') {
            result.push_back(JReadStr(arr, p));
        } else {
            size_t e = p;
            while (e < arr.size() && arr[e]!=L','&&arr[e]!=L']') ++e;
            result.push_back(arr.substr(p, e-p));
            p = e;
        }
    }
    return result;
}

// Returns {key, rawValue} pairs for every entry in a JSON object string.
// Object/array values are returned as the full {...}/{[...]} substring; strings are unquoted.
static std::vector<std::pair<std::wstring,std::wstring>> JObjectEntries(const std::wstring& obj)
{
    std::vector<std::pair<std::wstring,std::wstring>> out;
    size_t p = 0; JSkipWS(obj, p);
    if (p < obj.size() && obj[p] == L'{') ++p;
    while (p < obj.size()) {
        JSkipWS(obj, p);
        if (p >= obj.size() || obj[p] == L'}') break;
        if (obj[p] == L',') { ++p; continue; }
        if (obj[p] != L'"') break;
        std::wstring key = JReadStr(obj, p);
        JSkipWS(obj, p);
        if (p >= obj.size() || obj[p] != L':') break;
        ++p; JSkipWS(obj, p);
        if (p >= obj.size()) break;
        std::wstring val;
        if (obj[p] == L'"') {
            val = JReadStr(obj, p);
        } else if (obj[p]==L'['||obj[p]==L'{') {
            size_t cl = JMatchClose(obj, p);
            if (cl == std::wstring::npos) break;
            val = obj.substr(p, cl-p+1); p = cl+1;
        } else {
            size_t e = p;
            while (e < obj.size() && obj[e]!=L','&&obj[e]!=L'}') ++e;
            size_t a = obj.find_first_not_of(L" \t", p), b = obj.find_last_not_of(L" \t", e-1);
            val = (a!=std::wstring::npos && a<e) ? obj.substr(a, b-a+1) : L"";
            p = e;
        }
        out.push_back({key, val});
    }
    return out;
}

// ─── Config loader ───────────────────────────────────────────────────────────
// Each helper writes to its target ONLY when the key is present in the JSON block.
// Missing keys leave struct defaults intact, so config.json can carry overrides only.
static void JLoadF(const std::wstring& blk, const wchar_t* key, float& out) {
    auto v = JGetVal(blk, key); if (!v.empty()) out = wcstof(v.c_str(), nullptr);
}
static void JLoadI(const std::wstring& blk, const wchar_t* key, int& out) {
    auto v = JGetVal(blk, key); if (!v.empty()) out = (int)wcstol(v.c_str(), nullptr, 10);
}
static void JLoadB(const std::wstring& blk, const wchar_t* key, bool& out) {
    auto v = JGetVal(blk, key); if (!v.empty()) out = (v == L"true" || v == L"1");
}
static void JLoadS(const std::wstring& blk, const wchar_t* key, std::wstring& out) {
    auto v = JGetVal(blk, key); if (!v.empty()) out = v;
}

static TextAlign ParseAlignment(const std::wstring& s) {
    if (s == L"left")  return TextAlign::Left;
    if (s == L"right") return TextAlign::Right;
    return TextAlign::Center;
}
static DWRITE_FONT_WEIGHT ParseFontWeight(const std::wstring& s) {
    if (s == L"thin")      return DWRITE_FONT_WEIGHT_THIN;
    if (s == L"light")     return DWRITE_FONT_WEIGHT_LIGHT;
    if (s == L"regular")   return DWRITE_FONT_WEIGHT_REGULAR;
    if (s == L"medium")    return DWRITE_FONT_WEIGHT_MEDIUM;
    if (s == L"semibold")  return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    if (s == L"bold")      return DWRITE_FONT_WEIGHT_BOLD;
    if (s == L"extrabold") return DWRITE_FONT_WEIGHT_EXTRA_BOLD;
    if (s == L"black")     return DWRITE_FONT_WEIGHT_BLACK;
    return DWRITE_FONT_WEIGHT_REGULAR;
}
static DWRITE_FONT_STYLE ParseFontStyle(const std::wstring& s) {
    if (s == L"italic")  return DWRITE_FONT_STYLE_ITALIC;
    if (s == L"oblique") return DWRITE_FONT_STYLE_OBLIQUE;
    return DWRITE_FONT_STYLE_NORMAL;
}

static void LoadPaddingLR(const std::wstring& blk, PaddingLR& p) {
    JLoadF(blk, L"left",  p.left);
    JLoadF(blk, L"right", p.right);
}
static void LoadPointXY(const std::wstring& blk, PointXY& p) {
    JLoadF(blk, L"x", p.x);
    JLoadF(blk, L"y", p.y);
}
static void LoadAlignmentField(const std::wstring& blk, const wchar_t* key, TextAlign& a) {
    auto v = JGetVal(blk, key); if (!v.empty()) a = ParseAlignment(v);
}

static void LoadBrandLayout(const std::wstring& blk, BrandLayout& b) {
    JLoadF(blk, L"width",        b.width);
    JLoadF(blk, L"margin_right", b.marginRight);
    JLoadF(blk, L"shadow_width", b.shadowWidth);
    LoadPaddingLR(JGetVal(blk, L"padding"), b.padding);
    LoadAlignmentField(blk, L"alignment", b.alignment);
    auto imgBlk = JGetVal(blk, L"image");
    JLoadS(imgBlk, L"path",  b.image.path);
    JLoadF(imgBlk, L"width", b.image.width);
    LoadAlignmentField(imgBlk, L"alignment", b.image.alignment);
}
static void LoadClockLayout(const std::wstring& blk, ClockLayout& c) {
    JLoadF(blk, L"width",      c.width);
    JLoadF(blk, L"fade_width", c.fadeWidth);
    LoadPaddingLR(JGetVal(blk, L"padding"), c.padding);
    LoadAlignmentField(blk, L"alignment", c.alignment);
}
static void LoadSpacing(const std::wstring& blk, Spacing& s) {
    JLoadF(blk, L"text_gap", s.textGap);
    auto badgeBlk = JGetVal(blk, L"badge");
    JLoadF(badgeBlk, L"corner_radius", s.badge.cornerRadius);
    LoadPointXY  (JGetVal(badgeBlk, L"padding"), s.badge.padding);
    LoadPaddingLR(JGetVal(badgeBlk, L"gap"),     s.badge.gap);
}

static void LoadFontRole(const std::wstring& blk, FontRole& r) {
    JLoadF(blk, L"size", r.size);
    auto w = JGetVal(blk, L"weight"); if (!w.empty())  r.weight = ParseFontWeight(w);
    auto s = JGetVal(blk, L"style");  if (!s.empty())  r.style  = ParseFontStyle(s);
}
static void LoadTypography(const std::wstring& blk, Typography& t) {
    JLoadS(blk, L"family", t.family);
    auto roles = JGetVal(blk, L"roles");
    LoadFontRole(JGetVal(roles, L"brand"),    t.brand);
    LoadFontRole(JGetVal(roles, L"badge"),    t.badge);
    LoadFontRole(JGetVal(roles, L"clock"),    t.clock);
    LoadFontRole(JGetVal(roles, L"carousel"), t.carousel);
}

static void LoadAnimationConfig(const std::wstring& blk, AnimationConfig& a) {
    JLoadF(blk, L"loop_gap",       a.loopGap);
    JLoadF(blk, L"max_delta_time", a.maxDeltaTime);
    JLoadB(blk, L"uppercase_text", a.uppercaseText);
    auto scrollBlk = JGetVal(blk, L"scroll");
    JLoadF(scrollBlk, L"delay", a.scroll.delay);
    JLoadF(scrollBlk, L"speed", a.scroll.speed);
    auto carBlk = JGetVal(blk, L"carousel");
    JLoadF(carBlk, L"hold",       a.carousel.hold);
    JLoadF(carBlk, L"transition", a.carousel.transition);
}

static void LoadEffectsConfig(const std::wstring& blk, EffectsConfig& e) {
    auto bts = JGetVal(blk, L"badge_text_shadow");
    LoadPointXY(JGetVal(bts, L"offset"), e.badgeTextShadow.offset);
    JLoadI(bts, L"blur",    e.badgeTextShadow.blur);
    JLoadF(bts, L"opacity", e.badgeTextShadow.opacity);
    auto gs = JGetVal(blk, L"global_shadow");
    JLoadF(gs, L"opacity", e.globalShadow.opacity);
}

static void LoadWindowConfig(const std::wstring& blk, WindowConfig& w) {
    JLoadI(blk, L"min_width",     w.minWidth);
    JLoadI(blk, L"resize_border", w.resizeBorder);
    JLoadI(blk, L"snap_distance", w.snapDistance);
    auto def = JGetVal(blk, L"defaults");
    JLoadB(def, L"snap_enabled",  w.defaults.snapEnabled);
    JLoadB(def, L"always_on_top", w.defaults.alwaysOnTop);
}

static AppConfig LoadAppConfig(const wchar_t* path)
{
    AppConfig cfg;  // defaults from struct initializers

    std::ifstream file(path);
    if (!file.is_open()) return cfg;
    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    int wlen = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, nullptr, 0);
    if (wlen <= 1) return cfg;
    std::wstring src(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, &src[0], wlen);

    auto layoutBlk = JGetVal(src, L"layout");
    JLoadF(layoutBlk, L"ticker_height", cfg.layout.tickerHeight);
    LoadBrandLayout(JGetVal(layoutBlk, L"brand"),   cfg.layout.brand);
    LoadClockLayout(JGetVal(layoutBlk, L"clock"),   cfg.layout.clock);
    LoadSpacing    (JGetVal(layoutBlk, L"spacing"), cfg.layout.spacing);

    LoadTypography      (JGetVal(src, L"typography"), cfg.typography);
    LoadAnimationConfig (JGetVal(src, L"animation"),  cfg.animation);
    LoadEffectsConfig   (JGetVal(src, L"effects"),    cfg.effects);
    LoadWindowConfig    (JGetVal(src, L"window"),     cfg.window);

    return cfg;
}

// ─── User settings load / save ───────────────────────────────────────────────
// settings.json holds runtime-mutable state (window geometry + toggles).
// Defaults in the struct initializers cover the missing-file case cleanly.
static UserSettings LoadUserSettings(const wchar_t* path)
{
    UserSettings s;

    std::ifstream file(path);
    if (!file.is_open()) return s;
    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    int wlen = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, nullptr, 0);
    if (wlen <= 1) return s;
    std::wstring src(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, &src[0], wlen);

    JLoadI(src, L"version", s.version);
    auto winBlk  = JGetVal(src, L"window");
    auto posBlk  = JGetVal(winBlk, L"position");
    auto sizeBlk = JGetVal(winBlk, L"size");
    JLoadI(posBlk,  L"x",     s.window.position.x);
    JLoadI(posBlk,  L"y",     s.window.position.y);
    JLoadI(sizeBlk, L"width", s.window.size.width);
    JLoadB(winBlk,  L"always_on_top", s.window.alwaysOnTop);
    JLoadB(winBlk,  L"snap_enabled",  s.window.snapEnabled);
    return s;
}

// Manual writer — small, dependency-free, deterministic field order for clean diffs.
static void SaveUserSettings(const wchar_t* path, const UserSettings& s)
{
    if (!path || !*path) return;
    std::ostringstream out;
    out << "{\n";
    out << "  \"version\": " << s.version << ",\n";
    out << "  \"window\": {\n";
    out << "    \"position\":      { \"x\": " << s.window.position.x
        <<                      ", \"y\": " << s.window.position.y << " },\n";
    out << "    \"size\":          { \"width\": " << s.window.size.width << " },\n";
    out << "    \"always_on_top\": " << (s.window.alwaysOnTop ? "true" : "false") << ",\n";
    out << "    \"snap_enabled\":  " << (s.window.snapEnabled ? "true" : "false") << "\n";
    out << "  }\n";
    out << "}\n";
    std::ofstream file(path);
    if (file.is_open()) file << out.str();
}

// ─── Color loader ─────────────────────────────────────────────────────────────
// Parses assets/colors.json. Top-level sections:
//   "brand"   → background, color
//   "clock"   → background, color
//   "column"  → background, color
//   "shadow"  → background (gradient; opacity multiplier lives in config.json)
//   "badges"  → named badge palette entries (each: background, color)
// Any color value accepts "#RRGGBB"/"#RRGGBBAA" or "linear(angle,stop,...)".
static std::map<std::wstring, BadgeStyle> LoadColors(const wchar_t* path)
{
    // Default background: the original hardcoded vertical gradient
    const std::vector<D2D1_GRADIENT_STOP> kBgStops = {
        {0.0000f, {0.161f, 0.161f, 0.161f, 1.0f}}, // #292929
        {0.1442f, {0.161f, 0.161f, 0.161f, 1.0f}}, // #292929
        {0.3510f, {0.027f, 0.027f, 0.027f, 1.0f}}, // #070707
        {0.7981f, {0.098f, 0.098f, 0.098f, 1.0f}}, // #191919
        {1.0000f, {0.000f, 0.000f, 0.000f, 1.0f}}, // #000000
    };
    g_colBg      = ColorSpec{{0.161f, 0.161f, 0.161f, 1.0f}, kBgStops, 180.0f, true};
    g_clockBg    = g_colBg;
    g_brandBg    = ColorSpec{{0.800f, 0.000f, 0.000f, 1.0f}, {}, 90.0f, false};
    g_brandText  = ColorSpec{COL_WHITE, {}, 90.0f, false};
    g_clockText  = ColorSpec{COL_WHITE, {}, 90.0f, false};
    g_colText    = ColorSpec{COL_WHITE, {}, 90.0f, false};

    // Default shadow: top/bottom edge vignette (overridden by colors.json "shadow.background")
    const std::vector<D2D1_GRADIENT_STOP> kShadowStops = {
        {0.00f, {0.0f, 0.0f, 0.0f, 1.0f}},
        {0.50f, {0.0f, 0.0f, 0.0f, 0.0f}},
        {1.00f, {0.0f, 0.0f, 0.0f, 1.0f}},
    };
    g_shadow = ColorSpec{{0.0f, 0.0f, 0.0f, 1.0f}, kShadowStops, 180.0f, true};

    std::map<std::wstring, BadgeStyle> result;
    std::ifstream file(path);
    if (!file.is_open()) return result;

    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    int wlen = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, nullptr, 0);
    if (wlen <= 1) return result;
    std::wstring src(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, &src[0], wlen);

    // Brand section
    std::wstring brandBlk = JGetVal(src, L"brand");
    if (!brandBlk.empty()) {
        std::wstring bg = JGetVal(brandBlk, L"background");
        std::wstring tx = JGetVal(brandBlk, L"color");
        if (!bg.empty()) g_brandBg   = ParseColorSpec(bg);
        if (!tx.empty()) g_brandText = ParseColorSpec(tx);
    }

    // Clock section
    std::wstring clockBlk = JGetVal(src, L"clock");
    if (!clockBlk.empty()) {
        std::wstring bg = JGetVal(clockBlk, L"background");
        std::wstring tx = JGetVal(clockBlk, L"color");
        if (!bg.empty()) g_clockBg   = ParseColorSpec(bg);
        if (!tx.empty()) g_clockText = ParseColorSpec(tx);
    }

    // Column section
    std::wstring colBlk = JGetVal(src, L"column");
    if (!colBlk.empty()) {
        std::wstring bg = JGetVal(colBlk, L"background");
        std::wstring tx = JGetVal(colBlk, L"color");
        if (!bg.empty()) g_colBg   = ParseColorSpec(bg);
        if (!tx.empty()) g_colText = ParseColorSpec(tx);
    }

    // Shadow section: gradient stops only (use #RRGGBBAA for per-stop alpha).
    // The opacity multiplier lives in config.json under effects.global_shadow.opacity.
    std::wstring shadowBlk = JGetVal(src, L"shadow");
    if (!shadowBlk.empty()) {
        std::wstring bg = JGetVal(shadowBlk, L"background");
        if (!bg.empty()) g_shadow = ParseColorSpec(bg);
    }

    // Badges section
    std::wstring badgesBlk = JGetVal(src, L"badges");
    for (auto& [name, blk] : JObjectEntries(badgesBlk)) {
        BadgeStyle style;
        std::wstring bg = JGetVal(blk, L"background");
        std::wstring tc = JGetVal(blk, L"color");
        if (!bg.empty()) style.bg = ParseColorSpec(bg);
        if (!tc.empty()) style.color = ParseHexColor(tc);
        result[name] = style;
    }
    return result;
}

// ─── Content parser (JSON) ───────────────────────────────────────────────────
// Badge elements resolve style from colors.json by "badge_type" name.
// Inline overrides: "background": "#hex"|"linear(...)",  "color": "#hex"|"linear(...)"
static Element ParseJElement(const std::wstring& obj,
                              const std::map<std::wstring, BadgeStyle>& styles)
{
    Element e;
    std::wstring type = JGetVal(obj, L"type");
    e.text = JGetVal(obj, L"text");

    if (type == L"badge") {
        e.type = ElemType::Badge;
        BadgeStyle s;
        e.badgeType = JGetVal(obj, L"badge_type");
        if (!e.badgeType.empty()) {
            auto it = styles.find(e.badgeType);
            if (it != styles.end()) s = it->second;
        }
        std::wstring bg = JGetVal(obj, L"background");
        if (!bg.empty()) s.bg = ParseColorSpec(bg);
        e.style = s;
    } else {
        e.type = ElemType::Text;
    }
    return e;
}

// content.json: top-level array of carousel objects, each with an "items" array,
// each item with an "elements" array of badge/text element objects.
static std::vector<CarouselGroup> ParseContent(const wchar_t* path,
                                                const std::map<std::wstring, BadgeStyle>& styles)
{
    std::vector<CarouselGroup> groups;
    std::ifstream file(path);
    if (!file.is_open()) return groups;

    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    int wlen = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, nullptr, 0);
    if (wlen <= 1) return groups;
    std::wstring src(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, &src[0], wlen);

    for (const auto& cObj : JArrayElems(src)) {
        CarouselGroup group;
        for (const auto& iObj : JArrayElems(JGetVal(cObj, L"items"))) {
            CarouselItem item;
            for (const auto& eObj : JArrayElems(JGetVal(iObj, L"elements"))) {
                Element e = ParseJElement(eObj, styles);
                if (!e.text.empty() || e.type == ElemType::Badge)
                    item.elements.push_back(std::move(e));
            }
            if (!item.elements.empty()) group.items.push_back(std::move(item));
        }
        if (!group.items.empty()) groups.push_back(std::move(group));
    }
    return groups;
}

// Width measurement lives in PostProcessGroups: every cachedWidth is read directly from the
// IDWriteTextLayout instance that DrawTextLayout will render. Any standalone MeasureText
// helper would risk a second source of truth (different layout instance → metric drift) —
// not added by design.

static float MeasureItemWidth(const CarouselItem& item)
{
    float total    = 0.0f;
    bool  first    = true;
    bool  prevText = false;
    for (const auto& e : item.elements) {
        if (e.type == ElemType::Badge) {
            total += (first ? 0.0f : g_cfg.layout.spacing.badge.gap.left) + e.cachedWidth + 2.0f * g_cfg.layout.spacing.badge.padding.x + g_cfg.layout.spacing.badge.gap.right;
            prevText = false;
        } else {
            if (!first && prevText) total += g_cfg.layout.spacing.textGap;
            total += e.cachedWidth;
            prevText = true;
        }
        first = false;
    }
    return total;
}

// ─── content.json hot-reload ─────────────────────────────────────────────────
// Atomic write helper. Available for future content-edit features; not used by the read path.
static bool WriteAtomic(const wchar_t* path, const std::string& contents)
{
    std::wstring tmp = std::wstring(path) + L".tmp";
    {
        std::ofstream file(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        file.write(contents.data(), (std::streamsize)contents.size());
        file.flush();
        if (file.fail()) return false;
    }
    return MoveFileExW(tmp.c_str(), path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// FNV-1a 64-bit over (type + badgeType + text) for each element. Stable across runs and edits;
// two items with identical visible content + identical badge type collapse to the same ID.
static uint64_t ComputeItemID(const CarouselItem& item)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mixByte = [&h](uint8_t b) { h ^= (uint64_t)b; h *= 0x100000001b3ULL; };
    auto mixStr  = [&](const std::wstring& s) {
        for (wchar_t c : s) {
            uint16_t u = (uint16_t)c;
            mixByte((uint8_t)(u & 0xFF));
            mixByte((uint8_t)((u >> 8) & 0xFF));
        }
        mixByte(0x1F);  // unit separator → boundary between fields
    };
    for (const auto& e : item.elements) {
        mixByte((uint8_t)e.type);
        mixStr(e.badgeType);
        mixStr(e.text);
    }
    return h;
}

// One place that uppercases, creates layouts (with metrics), and assigns IDs. ALL DirectWrite
// shaping happens here — on whichever thread calls this (worker for hot-reload, render thread
// only at startup). The render hot path then uses DrawTextLayout, which only rasterizes.
static void PostProcessGroups(std::vector<CarouselGroup>& groups)
{
    for (auto& group : groups) {
        for (auto& item : group.items) {
            for (auto& elem : item.elements) {
                bool doUpper = (elem.type == ElemType::Badge) || g_cfg.animation.uppercaseText;
                if (doUpper && !elem.text.empty())
                    CharUpperBuffW(&elem.text[0], (DWORD)elem.text.size());

                IDWriteTextFormat* fmt = (elem.type == ElemType::Badge) ? g_fmtBadge : g_fmtCarousel;
                elem.cachedWidth = 0.0f;
                elem.layout.Reset();

                if (!elem.text.empty() && fmt && g_dw) {
                    // Phase 1: temp layout solely to learn the measured width for badge sizing.
                    IDWriteTextLayout* tmp = nullptr;
                    float measured = 0.0f;
                    if (SUCCEEDED(g_dw->CreateTextLayout(
                            elem.text.c_str(), (UINT32)elem.text.size(), fmt,
                            8000.0f, g_cfg.layout.tickerHeight, &tmp)) && tmp) {
                        DWRITE_TEXT_METRICS m;
                        tmp->GetMetrics(&m);
                        measured = m.widthIncludingTrailingWhitespace;
                        tmp->Release();
                    }

                    // Phase 2: persistent layout BORN with the bounds we'll render at.
                    float layoutW = (elem.type == ElemType::Badge)
                                  ? measured + 2.0f * g_cfg.layout.spacing.badge.padding.x
                                  : 8000.0f;
                    IDWriteTextLayout* raw = nullptr;
                    if (SUCCEEDED(g_dw->CreateTextLayout(
                            elem.text.c_str(), (UINT32)elem.text.size(), fmt,
                            layoutW, g_cfg.layout.tickerHeight, &raw)) && raw) {
                        // CRITICAL: cachedWidth must come from THE SAME layout instance that
                        // DrawTextLayout will render. Sourcing it from the temp layout (or any
                        // other measurement) introduces a per-instance metric drift that shows
                        // up as a sub-pixel horizontal shift on freshly-rebuilt visible items.
                        DWRITE_TEXT_METRICS m;
                        raw->GetMetrics(&m);
                        elem.cachedWidth = m.widthIncludingTrailingWhitespace;
                        elem.layout.Attach(raw);
                    }
                }
            }
            item.id = ComputeItemID(item);
        }

        // Tape pass: assign every item its persistent tapeOffsetX and slotWidth.
        // Sequential layout means items whose preceding items are unchanged keep the same
        // offset across reloads; only width changes upstream cause downstream shifts.
        float x = 0.0f;
        for (auto& item : group.items) {
            item.tapeOffsetX = x;
            item.slotWidth   = MeasureItemWidth(item) + g_cfg.animation.loopGap;
            x += item.slotWidth;
        }
        group.totalWidth = x;
    }
}

// Build a snapshot from disk: read → parse → strict validate → post-process.
// nullopt on ANY failure (file missing, malformed JSON, partial mid-write, empty after parse,
// empty group, empty item). The worker never publishes a snapshot that doesn't pass these gates.
static std::optional<ContentSnapshot> BuildContentSnapshot(
    const wchar_t* path, const std::map<std::wstring, BadgeStyle>& styles)
{
    auto groups = ParseContent(path, styles);

    if (groups.empty()) return std::nullopt;
    for (const auto& g : groups) {
        if (g.items.empty()) return std::nullopt;
        for (const auto& it : g.items) {
            if (it.elements.empty()) return std::nullopt;
        }
    }

    PostProcessGroups(groups);
    return ContentSnapshot{ std::move(groups) };
}

// Immutable tape model: the snapshot becomes the new backing data, and that's it.
// The renderer reads each item's persistent tapeOffsetX (already populated by PostProcessGroups),
// so no scroll/timer/index recomputation is needed — and that's the point. g_carouselScrollX,
// g_carouselAnimT, g_carouselHoldT, and g_groupIdx all persist across reloads. Visible content
// stays exactly where it was; only items with structural changes upstream shift.
static void ApplyContentSnapshot(ContentSnapshot snap)
{
    // HARD GUARD — never replace live g_groups with anything empty/invalid.
    if (snap.groups.empty()) return;
    for (const auto& g : snap.groups) {
        if (g.items.empty()) return;
        for (const auto& it : g.items) {
            if (it.elements.empty()) return;
        }
    }

    // O(1) hot-path swap with deferred destruction on the worker thread.
    {
        std::vector<CarouselGroup> oldGroups;
        oldGroups.swap(g_groups);
        g_groups = std::move(snap.groups);
        if (!oldGroups.empty()) {
            std::lock_guard<std::mutex> lk(g_trashMu);
            g_trash.push_back(std::move(oldGroups));
        }
    }

    // No reset of g_groupIdx / g_carouselScrollX / g_carouselAnimT / g_carouselHoldT.
    // The only adjustments allowed are clamps that prevent reading off the end of the new tape.
    if (g_groupIdx >= (int)g_groups.size()) g_groupIdx = 0;

    // Refresh the cached current-tape length, then wrap scroll into [0, totalWidth).
    // This wrap is a true tape wrap (not a content-driven adjustment) — it only fires when
    // a structural change shrunk the tape under the current scroll position.
    g_carouselItemW = g_groups[g_groupIdx].totalWidth;
    if (g_carouselItemW > 0.0f) {
        g_carouselScrollX = fmodf(g_carouselScrollX, g_carouselItemW);
        if (g_carouselScrollX < 0.0f) g_carouselScrollX += g_carouselItemW;
    }
}

// Render-thread pickup: cheap. Lock + maybe-move + unlock. No I/O, no parse, no measurement.
static void TickContentReload()
{
    std::optional<ContentSnapshot> snap;
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (g_pendingSnap) snap = std::move(g_pendingSnap);
    }
    if (snap) ApplyContentSnapshot(std::move(*snap));
}

// Worker thread: polls mtime on its own cadence, builds snapshots, publishes to the pending
// slot. DirectWrite's shared factory + immutable text formats are thread-safe, so MeasureText
// can run here without coordination. Only the published snapshot crosses the boundary.
static void ContentWorkerProc()
{
    FILETIME knownMtime = {};
    if (!g_contentPath.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(g_contentPath.c_str(), GetFileExInfoStandard, &fad))
            knownMtime = fad.ftLastWriteTime;
    }

    auto drainTrash = []() {
        // Move the queue out under the lock; destruction happens after the lock is released.
        std::vector<std::vector<CarouselGroup>> dead;
        {
            std::lock_guard<std::mutex> lk(g_trashMu);
            dead.swap(g_trash);
        }
        // 'dead' goes out of scope here → recursive destructors run on this (worker) thread.
    };

    while (g_workerRun.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(g_workerCvMu);
            g_workerCv.wait_for(lk, std::chrono::milliseconds(CONTENT_POLL_MS),
                                [] { return !g_workerRun.load(std::memory_order_acquire); });
        }
        if (!g_workerRun.load(std::memory_order_acquire)) break;

        // Drain any old g_groups vectors handed off by render-thread swaps since last tick.
        drainTrash();

        if (g_contentPath.empty()) continue;

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(g_contentPath.c_str(), GetFileExInfoStandard, &fad)) continue;
        if (CompareFileTime(&fad.ftLastWriteTime, &knownMtime) == 0) continue;

        // Record mtime regardless of build outcome → a malformed save doesn't retry-storm.
        knownMtime = fad.ftLastWriteTime;
        auto snap = BuildContentSnapshot(g_contentPath.c_str(), g_badgeStyles);
        if (!snap) continue;

        // Publish; latest snapshot wins if render hasn't picked up the previous one yet.
        std::lock_guard<std::mutex> lk(g_pendingMu);
        g_pendingSnap = std::move(snap);
    }

    // Final trash drain on shutdown so we don't leak old vectors into static teardown.
    drainTrash();
}

static void StartContentWorker()
{
    if (g_workerThread.joinable()) return;
    g_workerRun.store(true, std::memory_order_release);
    g_workerThread = std::thread(ContentWorkerProc);
}

static void StopContentWorker()
{
    if (!g_workerThread.joinable()) return;
    g_workerRun.store(false, std::memory_order_release);
    g_workerCv.notify_all();
    g_workerThread.join();
}

// ─── Text format helper ──────────────────────────────────────────────────────
static IDWriteTextFormat* MakeFmt(
    const wchar_t* family, IDWriteFontCollection* coll,
    DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, float size,
    DWRITE_TEXT_ALIGNMENT ha, DWRITE_PARAGRAPH_ALIGNMENT va)
{
    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(g_dw->CreateTextFormat(family, coll, weight, style,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt)))
        g_dw->CreateTextFormat(L"Segoe UI", nullptr, weight, style,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt);
    if (fmt) {
        fmt->SetTextAlignment(ha);
        fmt->SetParagraphAlignment(va);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return fmt;
}

// ─── Easing ──────────────────────────────────────────────────────────────────
static float EaseInOut(float t) { return t * t * (3.0f - 2.0f * t); }

// ─── Init ────────────────────────────────────────────────────────────────────
static bool InitD2D(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd, &rc);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory))) return false;
    if (FAILED(g_factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt)))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), (IUnknown**)&g_dw)))
        return false;
    // WIC for PNG decoding. Failure is non-fatal — we just won't be able to load PNG logos.
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&g_wic));

    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (wchar_t* sl = wcsrchr(exeDir, L'\\')) *(sl + 1) = L'\0';

    wchar_t fontDir[MAX_PATH]; wcscpy_s(fontDir, exeDir); wcscat_s(fontDir, L"..\\assets\\fonts\\");
    g_fontColl = LoadFontCollection(g_dw, fontDir);

    const wchar_t* family = g_fontColl ? g_cfg.typography.family.c_str() : L"Segoe UI";
    IDWriteFontCollection* coll = g_fontColl;
    const auto& T = g_cfg.typography;

    g_fmtBrand    = MakeFmt(family, coll, T.brand.weight,    T.brand.style,    T.brand.size,
                            ToDWriteAlign(g_cfg.layout.brand.alignment), DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_fmtBadge    = MakeFmt(family, coll, T.badge.weight,    T.badge.style,    T.badge.size,
                            DWRITE_TEXT_ALIGNMENT_CENTER,                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_fmtClock    = MakeFmt(family, coll, T.clock.weight,    T.clock.style,    T.clock.size,
                            ToDWriteAlign(g_cfg.layout.clock.alignment), DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_fmtCarousel = MakeFmt(family, coll, T.carousel.weight, T.carousel.style, T.carousel.size,
                            DWRITE_TEXT_ALIGNMENT_LEADING,               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (!g_fmtBrand || !g_fmtBadge || !g_fmtClock || !g_fmtCarousel) return false;

    if (FAILED(g_rt->CreateSolidColorBrush(COL_WHITE, &g_wBrush)))   return false;
    if (FAILED(g_rt->CreateSolidColorBrush(COL_BG,    &g_dynBrush))) return false;

    // Load colors first so background brushes are built from colors.json values
    wchar_t colorsPath[MAX_PATH];  wcscpy_s(colorsPath,  exeDir); wcscat_s(colorsPath,  L"..\\assets\\colors.json");
    g_badgeStyles = LoadColors(colorsPath);

    // Background gradient brushes from loaded color specs (vertical gradient: rect width is irrelevant)
    g_bgBrush     = MakeGradBrush(g_colBg,   {0.0f, 0.0f, 1.0f, g_cfg.layout.tickerHeight});
    g_clockBgBrush = MakeGradBrush(g_clockBg, {0.0f, 0.0f, 1.0f, g_cfg.layout.tickerHeight});
    if (!g_bgBrush) return false;

    // Brand right-edge drop shadow
    {
        D2D1_GRADIENT_STOP stops[] = {
            {0.0f, {0.0f, 0.0f, 0.0f, BRAND_SHADOW_A}},
            {1.0f, {0.0f, 0.0f, 0.0f, 0.0f}},
        };
        ID2D1GradientStopCollection* sc = nullptr;
        g_rt->CreateGradientStopCollection(stops, 2, &sc);
        if (sc) {
            g_rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    {g_cfg.layout.brand.width, 0.0f}, {g_cfg.layout.brand.width + g_cfg.layout.brand.shadowWidth, 0.0f}),
                sc, &g_brandShadow);
            sc->Release();
        }
    }

    // Opacity mask for carousel right-edge fade: content alpha 1→0 (no dark overlay)
    {
        D2D1_GRADIENT_STOP stops[] = {
            {0.0f, {1.0f, 1.0f, 1.0f, 1.0f}},
            {1.0f, {1.0f, 1.0f, 1.0f, 0.0f}},
        };
        ID2D1GradientStopCollection* sc = nullptr;
        g_rt->CreateGradientStopCollection(stops, 2, &sc);
        if (sc) {
            g_rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties({0.0f, 0.0f}, {g_cfg.layout.clock.fadeWidth, 0.0f}),
                sc, &g_clockFade);
            sc->Release();
        }
    }
    g_rt->CreateLayer(nullptr, &g_clockFadeLayer);

    // Optional brand logo: image path comes from config (brand.image.path, relative to project root).
    // Empty path → no image, Render falls back to brand text. Decode failures also fall through cleanly.
    if (g_wic && !g_cfg.layout.brand.image.path.empty()) {
        wchar_t imgPath[MAX_PATH];
        wcscpy_s(imgPath, exeDir);
        wcscat_s(imgPath, L"..\\");  // exe dir → project root
        wcscat_s(imgPath, g_cfg.layout.brand.image.path.c_str());
        DWORD attr = GetFileAttributesW(imgPath);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            IWICBitmapDecoder* decoder = nullptr;
            if (SUCCEEDED(g_wic->CreateDecoderFromFilename(imgPath, nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnLoad, &decoder))) {
                IWICBitmapFrameDecode* frame = nullptr;
                if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                    IWICFormatConverter* converter = nullptr;
                    if (SUCCEEDED(g_wic->CreateFormatConverter(&converter))) {
                        if (SUCCEEDED(converter->Initialize(
                                frame, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0f,
                                WICBitmapPaletteTypeMedianCut))) {
                            g_rt->CreateBitmapFromWicBitmap(converter, nullptr, &g_brandBitmap);
                        }
                        converter->Release();
                    }
                    frame->Release();
                }
                decoder->Release();
            }
        }
    }

    wchar_t contentPath[MAX_PATH]; wcscpy_s(contentPath, exeDir); wcscat_s(contentPath, L"..\\assets\\content.json");
    g_contentPath = contentPath;
    g_groups = ParseContent(contentPath, g_badgeStyles);

    // Fallback if content.json is missing or empty
    if (g_groups.empty()) {
        {
            CarouselGroup grp;
            CarouselItem ci;
            Element e; e.type = ElemType::Text; e.text = L"WELCOME TO GLAZELINE!";
            ci.elements.push_back(e);
            grp.items.push_back(std::move(ci));
            g_groups.push_back(std::move(grp));
        }
        {
            CarouselGroup grp;
            CarouselItem ci;
            BadgeStyle as; as.bg.solid = {0.831f, 0.329f, 0.0f, 1.0f};
            Element b; b.type = ElemType::Badge; b.text = L"ALERT"; b.style = as;
            Element t; t.type = ElemType::Text; t.text = L"ADD ITEMS TO assets/content.json";
            ci.elements.push_back(b); ci.elements.push_back(t);
            grp.items.push_back(std::move(ci));
            g_groups.push_back(std::move(grp));
        }
    }

    // Single post-process pass: uppercase, measure widths, assign content IDs.
    PostProcessGroups(g_groups);

    g_groupIdx        = 0;
    g_carouselHoldT   = 0.0f;
    g_carouselAnimT   = 0.0f;
    g_carouselScrollX = 0.0f;
    g_carouselItemW   = g_groups[0].totalWidth;
    g_lastTime = std::chrono::steady_clock::now();

    // All shared resources are now ready; safe to start the background reload worker.
    StartContentWorker();
    return true;
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────
static void Cleanup()
{
    // Join the worker BEFORE releasing DirectWrite/D2D — it may be mid-build using g_dw/g_fmts.
    StopContentWorker();

    // Drop all CarouselGroup containers explicitly so the IDWriteTextLayout objects they own
    // get Released NOW, while their parent factories are still alive. Static destruction of
    // these globals would otherwise run after Cleanup releases g_dw → use-after-free.
    g_pendingSnap.reset();
    g_groups.clear();
    g_groups.shrink_to_fit();
    {
        std::lock_guard<std::mutex> lk(g_trashMu);
        g_trash.clear();
    }

    auto SR = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    SR(g_fmtBrand); SR(g_fmtBadge); SR(g_fmtClock); SR(g_fmtCarousel);
    SR(g_wBrush); SR(g_dynBrush); SR(g_bgBrush); SR(g_clockBgBrush); SR(g_brandShadow); SR(g_clockFade); SR(g_clockFadeLayer);
    SR(g_brandBitmap); SR(g_wic);
    SR(g_fontColl); SR(g_rt); SR(g_dw); SR(g_factory);
}

// ─── Draw one carousel item at vertical offset y ─────────────────────────────
// Elements are laid out left → right. cachedWidth avoids per-frame measurement.
static void DrawItem(const CarouselItem& item, float zoneX, float y, float scrollX = 0.0f)
{
    float cx        = zoneX - scrollX;
    bool  firstElem = true;
    bool  prevText  = false;

    for (const auto& elem : item.elements) {
        if (elem.type == ElemType::Badge) {
            float badgeW = elem.cachedWidth + 2.0f * g_cfg.layout.spacing.badge.padding.x;
            float badgeH = g_cfg.layout.tickerHeight - 2.0f * g_cfg.layout.spacing.badge.padding.y;
            float bx     = cx + (firstElem ? 0.0f : g_cfg.layout.spacing.badge.gap.left);
            D2D1_RECT_F    br = {bx, y + g_cfg.layout.spacing.badge.padding.y, bx + badgeW, y + g_cfg.layout.spacing.badge.padding.y + badgeH};
            D2D1_ROUNDED_RECT rr = {br, g_cfg.layout.spacing.badge.cornerRadius, g_cfg.layout.spacing.badge.cornerRadius};

            {
                ID2D1LinearGradientBrush* gb = MakeGradBrush(elem.style.bg, br);
                if (gb) {
                    g_rt->FillRoundedRectangle(rr, gb);
                    gb->Release();
                } else {
                    g_dynBrush->SetColor(elem.style.bg.solid);
                    g_rt->FillRoundedRectangle(rr, g_dynBrush);
                }
            }

            if (elem.layout) {
                int   steps     = 2 * g_cfg.effects.badgeTextShadow.blur + 1;
                float passAlpha = g_cfg.effects.badgeTextShadow.opacity / (float)(steps * steps);
                for (int bby = -g_cfg.effects.badgeTextShadow.blur; bby <= g_cfg.effects.badgeTextShadow.blur; ++bby) {
                    for (int bbx = -g_cfg.effects.badgeTextShadow.blur; bbx <= g_cfg.effects.badgeTextShadow.blur; ++bbx) {
                        float ox = g_cfg.effects.badgeTextShadow.offset.x + (float)bbx;
                        float oy = g_cfg.effects.badgeTextShadow.offset.y + (float)bby;
                        g_dynBrush->SetColor({0.0f, 0.0f, 0.0f, passAlpha});
                        g_rt->DrawTextLayout({bx + ox, y + oy}, elem.layout.Get(), g_dynBrush);
                    }
                }
                g_dynBrush->SetColor(elem.style.color);
                g_rt->DrawTextLayout({bx, y}, elem.layout.Get(), g_dynBrush);
            }

            cx = bx + badgeW + g_cfg.layout.spacing.badge.gap.right;
            prevText = false;

        } else { // Text
            if (elem.layout) {
                if (!firstElem && prevText) cx += g_cfg.layout.spacing.textGap;
                D2D1_RECT_F tr = {cx, y, cx + 8000.0f, y + g_cfg.layout.tickerHeight};
                ID2D1LinearGradientBrush* gb = MakeGradBrush(g_colText, tr);
                ID2D1Brush* brush = gb ? (ID2D1Brush*)gb : (g_dynBrush->SetColor(g_colText.solid), (ID2D1Brush*)g_dynBrush);
                g_rt->DrawTextLayout({cx, y}, elem.layout.Get(), brush);
                if (gb) gb->Release();
                cx += elem.cachedWidth;
            }
            prevText = true;
        }
        firstElem = false;
    }
}

// ─── Carousel rendering ──────────────────────────────────────────────────────
// Each <carousel> block scrolls as a single horizontal tape: [item0][gap][item1][gap]...
// Multiple <carousel> blocks cycle vertically over time.
static void RenderCarousel(float zoneX, float zoneW)
{
    if (g_groups.empty() || zoneW <= 0.0f) return;

    int nextGI = (g_groupIdx + 1) % (int)g_groups.size();
    const CarouselGroup& curGroup  = g_groups[g_groupIdx];
    const CarouselGroup& nextGroup = g_groups[nextGI];

    float t     = (g_carouselAnimT > 0.0f) ? EaseInOut(g_carouselAnimT / g_cfg.animation.carousel.transition) : 0.0f;
    float curY  = -t * g_cfg.layout.tickerHeight;
    float nextY = (1.0f - t) * g_cfg.layout.tickerHeight;

    // Opacity mask layer: content alpha fades to 0 over the last g_cfg.layout.clock.fadeWidth pixels
    float fadeX = zoneX + zoneW - g_cfg.layout.clock.fadeWidth;
    if (g_clockFade && g_clockFadeLayer) {
        g_clockFade->SetStartPoint({fadeX, 0.0f});
        g_clockFade->SetEndPoint({zoneX + zoneW, 0.0f});
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
            D2D1::InfiniteRect(), nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::IdentityMatrix(), 1.0f,
            g_clockFade, D2D1_LAYER_OPTIONS_NONE);
        g_rt->PushLayer(&lp, g_clockFadeLayer);
    }

    // Clip left at 0 so text can scroll underneath the brand panel
    g_rt->PushAxisAlignedClip({0.0f, 0.0f, zoneX + zoneW, g_cfg.layout.tickerHeight}, D2D1_ANTIALIAS_MODE_ALIASED);

    // Current group: draw each item at its persistent tapeOffsetX; add loop copy when overflowing.
    // Renderer is a pure function of (tapeOffsetX, scrollX) — no cumulative summing per frame.
    for (const auto& item : curGroup.items) {
        DrawItem(item, zoneX, curY, g_carouselScrollX - item.tapeOffsetX);
        if (g_carouselScrollX > 0.0f && curGroup.totalWidth > zoneW)
            DrawItem(item, zoneX, curY, g_carouselScrollX - item.tapeOffsetX - curGroup.totalWidth);
    }

    // Incoming group (starts at scrollX=0, slides in from below)
    if (g_carouselAnimT > 0.0f) {
        for (const auto& item : nextGroup.items) {
            DrawItem(item, zoneX, nextY, -item.tapeOffsetX);
        }
    }

    g_rt->PopAxisAlignedClip();

    if (g_clockFade && g_clockFadeLayer)
        g_rt->PopLayer();
}

// ─── Frame render ────────────────────────────────────────────────────────────
static void Render(HWND /*hwnd*/)
{
    // Hot-reload: poll content.json mtime and swap snapshot atomically. Runs BEFORE any
    // animation/state mutation so the same frame sees consistent data end-to-end.
    TickContentReload();

    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_lastTime).count();
    g_lastTime = now;
    if (dt > g_cfg.animation.maxDeltaTime) dt = g_cfg.animation.maxDeltaTime;

    float winW  = g_rt->GetSize().width;
    float zoneX = g_cfg.layout.brand.width + g_cfg.layout.brand.marginRight;
    float zoneW = std::max(0.0f, winW - g_cfg.layout.brand.width - g_cfg.layout.brand.marginRight - g_cfg.layout.clock.width);

    // Carousel timer + horizontal scroll for overflowing items
    if (g_carouselAnimT > 0.0f) {
        g_carouselAnimT += dt;
        if (g_carouselAnimT >= g_cfg.animation.carousel.transition) {
            g_groupIdx        = (g_groupIdx + 1) % (int)g_groups.size();
            g_carouselAnimT   = 0.0f;
            g_carouselScrollX = 0.0f;  // group cycle is normal animation behavior — fresh tape
            g_carouselItemW   = g_groups.empty() ? 0.0f : g_groups[g_groupIdx].totalWidth;
        }
    } else {
        g_carouselHoldT += dt;
        if (g_carouselItemW > zoneW && g_carouselHoldT > g_cfg.animation.scroll.delay) {
            g_carouselScrollX += dt * g_cfg.animation.scroll.speed;
            if (g_carouselScrollX >= g_carouselItemW) g_carouselScrollX -= g_carouselItemW;
        }
        if ((int)g_groups.size() > 1 && g_carouselHoldT >= g_cfg.animation.carousel.hold) {
            g_carouselHoldT = 0.0f;
            g_carouselAnimT = dt;
        }
    }

    g_rt->BeginDraw();
    g_rt->FillRectangle({0.0f, 0.0f, winW, g_cfg.layout.tickerHeight}, g_bgBrush);
    if (g_clockBgBrush)
        g_rt->FillRectangle({winW - g_cfg.layout.clock.width, 0.0f, winW, g_cfg.layout.tickerHeight}, g_clockBgBrush);

    // Carousel drawn first; brand panel drawn after so text slides underneath it
    RenderCarousel(zoneX, zoneW);

    {
        D2D1_RECT_F brandR = {0.0f, 0.0f, g_cfg.layout.brand.width, g_cfg.layout.tickerHeight};
        ID2D1LinearGradientBrush* gb = MakeGradBrush(g_brandBg, brandR);
        if (gb) { g_rt->FillRectangle(brandR, gb); gb->Release(); }
        else    { g_dynBrush->SetColor(g_brandBg.solid); g_rt->FillRectangle(brandR, g_dynBrush); }

        if (g_brandBitmap) {
            // Image uses its own width and alignment from brand.image.* — does NOT inherit
            // brand.padding or brand.alignment (those drive the text fallback only).
            // Aspect always preserved; height clamped to ticker height; vertical center always.
            D2D1_SIZE_F bs = g_brandBitmap->GetSize();
            float imgW = g_cfg.layout.brand.image.width > 0.0f
                         ? g_cfg.layout.brand.image.width
                         : g_cfg.layout.brand.width;
            float imgH = imgW * (bs.height / bs.width);
            if (imgH > g_cfg.layout.tickerHeight) {
                float k = g_cfg.layout.tickerHeight / imgH;
                imgW *= k;
                imgH *= k;
            }
            float x = 0.0f;
            switch (g_cfg.layout.brand.image.alignment) {
                case TextAlign::Center: x = (g_cfg.layout.brand.width - imgW) * 0.5f; break;
                case TextAlign::Right:  x =  g_cfg.layout.brand.width - imgW;         break;
                case TextAlign::Left:
                default:                                                              break;
            }
            float y = (g_cfg.layout.tickerHeight - imgH) * 0.5f;
            D2D1_RECT_F dest = {x, y, x + imgW, y + imgH};
            g_rt->DrawBitmap(g_brandBitmap, dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            D2D1_RECT_F brandTR = {g_cfg.layout.brand.padding.left, 0.0f, g_cfg.layout.brand.width - g_cfg.layout.brand.padding.right, g_cfg.layout.tickerHeight};
            ID2D1LinearGradientBrush* tgb = MakeGradBrush(g_brandText, brandTR);
            ID2D1Brush* tbrush = tgb ? (ID2D1Brush*)tgb : (g_dynBrush->SetColor(g_brandText.solid), (ID2D1Brush*)g_dynBrush);
            g_rt->DrawTextW(L"GLAZELINE", 9, g_fmtBrand, brandTR, tbrush);
            if (tgb) tgb->Release();
        }
    }
    if (g_brandShadow)
        g_rt->FillRectangle({g_cfg.layout.brand.width, 0.0f, g_cfg.layout.brand.width + g_cfg.layout.brand.shadowWidth, g_cfg.layout.tickerHeight}, g_brandShadow);

    // Clock
    time_t tt = time(nullptr);
    struct tm tm; localtime_s(&tm, &tt);
    wchar_t clockBuf[32]; wcsftime(clockBuf, 32, L"%I:%M %p", &tm);
    const wchar_t* clockStr = (clockBuf[0] == L'0') ? clockBuf + 1 : clockBuf;
    {
        D2D1_RECT_F clockR = {winW - g_cfg.layout.clock.width + g_cfg.layout.clock.padding.left, 0.0f, winW - g_cfg.layout.clock.padding.right, g_cfg.layout.tickerHeight};
        ID2D1LinearGradientBrush* gb = MakeGradBrush(g_clockText, clockR);
        ID2D1Brush* brush = gb ? (ID2D1Brush*)gb : (g_dynBrush->SetColor(g_clockText.solid), (ID2D1Brush*)g_dynBrush);
        g_rt->DrawTextW(clockStr, (UINT32)wcslen(clockStr), g_fmtClock, clockR, brush);
        if (gb) gb->Release();
    }

    // Shadow drawn LAST so it layers over carousel text, badges, and clock text — but not the brand.
    // Brush is built per-frame from the actual rect so gradient orientation matches the full span.
    const float shadowOpacity = g_cfg.effects.globalShadow.opacity;
    if (shadowOpacity > 0.0f && g_shadow.isGradient && g_shadow.gradStops.size() >= 2) {
        D2D1_RECT_F sr = {g_cfg.layout.brand.width, 0.0f, winW, g_cfg.layout.tickerHeight};
        ColorSpec spec = g_shadow;
        for (auto& s : spec.gradStops) s.color.a *= shadowOpacity;
        ID2D1LinearGradientBrush* gb = MakeGradBrush(spec, sr);
        if (gb) {
            g_rt->FillRectangle(sr, gb);
            gb->Release();
        }
    }

    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { Cleanup(); PostQuitMessage(0); }
}

// ─── Window procedure ────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        if (!InitD2D(hwnd)) {
            MessageBoxW(nullptr, L"Direct2D initialization failed.", L"Glazeline", MB_ICONERROR);
            return -1;
        }
        return 0;
    case WM_PAINT:
        Render(hwnd); ValidateRect(hwnd, nullptr); return 0;
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam))); return 0;
    case WM_NCCALCSIZE:
        // Borderless: treat the entire window rect as client area (no system frame painted)
        if (wParam == TRUE) return 0;
        break;
    case WM_GETMINMAXINFO: {
        // Lock height to g_cfg.layout.tickerHeight; allow horizontal resize down to a sensible minimum
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.y = (int)g_cfg.layout.tickerHeight;
        mmi->ptMaxTrackSize.y = (int)g_cfg.layout.tickerHeight;
        mmi->ptMinTrackSize.x = g_cfg.window.minWidth;
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pt = {(int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam)};
        RECT rc; GetWindowRect(hwnd, &rc);
        if (pt.x >= rc.left  && pt.x <  rc.left  + g_cfg.window.resizeBorder) return HTLEFT;
        if (pt.x >= rc.right - g_cfg.window.resizeBorder && pt.x < rc.right)  return HTRIGHT;
        return HTCAPTION;
    }
    case WM_ENTERSIZEMOVE:
        g_prevMoveValid = false;
        // Drive Render via WM_TIMER for the duration of the modal loop so the ticker keeps animating.
        SetTimer(hwnd, DRAG_RENDER_TIMER_ID, 16, nullptr);
        return 0;
    case WM_EXITSIZEMOVE: {
        KillTimer(hwnd, DRAG_RENDER_TIMER_ID);
        // Persist final window geometry after a move/resize completes.
        RECT wr; GetWindowRect(hwnd, &wr);
        g_settings.window.position.x = wr.left;
        g_settings.window.position.y = wr.top;
        g_settings.window.size.width = wr.right - wr.left;
        SaveUserSettings(g_settingsPath.c_str(), g_settings);
        return 0;
    }
    case WM_TIMER:
        if (wParam == DRAG_RENDER_TIMER_ID) Render(hwnd);
        return 0;
    case WM_MOVING: {
        // Directional snap: an edge only snaps when the cursor is APPROACHING it (its natural
        // distance to the work-area edge is decreasing). As soon as the cursor moves away,
        // snap releases and the window follows the cursor — no stickiness, but snap can
        // re-engage as many times as the user re-approaches an edge during the same drag.
        RECT* rc = (RECT*)lParam;
        RECT natural = *rc;  // cursor-based natural position, captured BEFORE we modify rc
        HMONITOR mon = MonitorFromRect(rc, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        if (g_snapEnabled && GetMonitorInfoW(mon, &mi)) {
            const RECT& wa = mi.rcWork;
            int w = rc->right  - rc->left;
            int h = rc->bottom - rc->top;
            auto closer = [&](LONG prev, LONG curr, LONG target) {
                return !g_prevMoveValid || abs((int)(curr - target)) < abs((int)(prev - target));
            };
            bool approachL = closer(g_prevMoveNat.left,   natural.left,   wa.left);
            bool approachR = closer(g_prevMoveNat.right,  natural.right,  wa.right);
            bool approachT = closer(g_prevMoveNat.top,    natural.top,    wa.top);
            bool approachB = closer(g_prevMoveNat.bottom, natural.bottom, wa.bottom);
            if      (approachL && abs((int)(rc->left  - wa.left))  < g_cfg.window.snapDistance) { rc->left  = wa.left;  rc->right  = rc->left + w;  }
            else if (approachR && abs((int)(rc->right - wa.right)) < g_cfg.window.snapDistance) { rc->right = wa.right; rc->left   = rc->right - w; }
            if      (approachT && abs((int)(rc->top    - wa.top))    < g_cfg.window.snapDistance) { rc->top    = wa.top;    rc->bottom = rc->top + h;    }
            else if (approachB && abs((int)(rc->bottom - wa.bottom)) < g_cfg.window.snapDistance) { rc->bottom = wa.bottom; rc->top    = rc->bottom - h; }
        }
        g_prevMoveNat   = natural;
        g_prevMoveValid = true;
        return TRUE;
    }
    case WM_NCRBUTTONUP: {
        POINT pt = {(int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam)};
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (g_alwaysOnTop ? MF_CHECKED : MF_UNCHECKED), 3, L"Always on top");
        AppendMenuW(menu, MF_STRING | (g_snapEnabled ? MF_CHECKED : MF_UNCHECKED), 2, L"Snap to edges");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 1, L"Close");
        int cmd = (int)TrackPopupMenuEx(menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_VERNEGANIMATION,
            pt.x, pt.y, hwnd, nullptr);
        DestroyMenu(menu);
        if      (cmd == 1) PostQuitMessage(0);
        else if (cmd == 2) {
            g_snapEnabled = !g_snapEnabled;
            g_settings.window.snapEnabled = g_snapEnabled;
            SaveUserSettings(g_settingsPath.c_str(), g_settings);
        }
        else if (cmd == 3) {
            g_alwaysOnTop = !g_alwaysOnTop;
            SetWindowPos(hwnd, g_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            g_settings.window.alwaysOnTop = g_alwaysOnTop;
            SaveUserSettings(g_settingsPath.c_str(), g_settings);
        }
        return 0;
    }
    case WM_DESTROY:
        // Defensive final save in case any state changed without a triggering event.
        SaveUserSettings(g_settingsPath.c_str(), g_settings);
        Cleanup(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Entry point ─────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // required by WIC

    // Load config.json (defaults) and settings.json (persisted user state) before window
    // creation. Layered: config supplies defaults, settings overrides at runtime.
    {
        wchar_t exeDir[MAX_PATH];
        GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        if (wchar_t* sl = wcsrchr(exeDir, L'\\')) *(sl + 1) = L'\0';

        wchar_t configPath[MAX_PATH];
        wcscpy_s(configPath, exeDir);
        wcscat_s(configPath, L"..\\assets\\config.json");
        g_cfg = LoadAppConfig(configPath);

        wchar_t settingsPath[MAX_PATH];
        wcscpy_s(settingsPath, exeDir);
        wcscat_s(settingsPath, L"..\\assets\\settings.json");
        g_settingsPath = settingsPath;
    }

    // Seed runtime toggles from config defaults; settings.json overrides if present.
    g_snapEnabled = g_cfg.window.defaults.snapEnabled;
    g_alwaysOnTop = g_cfg.window.defaults.alwaysOnTop;
    {
        // Pre-load settings with current toggle defaults so a missing file preserves config behavior.
        g_settings.window.snapEnabled = g_snapEnabled;
        g_settings.window.alwaysOnTop = g_alwaysOnTop;
        g_settings = LoadUserSettings(g_settingsPath.c_str());
        g_snapEnabled = g_settings.window.snapEnabled;
        g_alwaysOnTop = g_settings.window.alwaysOnTop;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GlazelineTicker";
    RegisterClassExW(&wc);

    // Restored geometry from settings if width > 0; otherwise compute from work area on first run.
    RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int wx, wy, ww;
    if (g_settings.window.size.width > 0) {
        wx = g_settings.window.position.x;
        wy = g_settings.window.position.y;
        ww = g_settings.window.size.width;
    } else {
        wx = work.left;
        wy = work.top;
        ww = work.right - work.left;
    }

    // WS_THICKFRAME enables horizontal resize via DefWindowProc when NCHITTEST returns HTLEFT/HTRIGHT;
    // the visual frame is suppressed by WM_NCCALCSIZE returning 0.
    DWORD exStyle = WS_EX_TOOLWINDOW | (g_alwaysOnTop ? WS_EX_TOPMOST : 0);
    HWND hwnd = CreateWindowExW(
        exStyle, L"GlazelineTicker", L"Glazeline",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_VISIBLE,
        wx, wy, ww, (int)g_cfg.layout.tickerHeight,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
        } else {
            Render(hwnd);
        }
    }
    return 0;
}
