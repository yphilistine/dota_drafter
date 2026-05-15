#pragma once

/*
 * dota2_capture.h
 * Dota 2 Hero Portrait Extractor
 *
 * Captures hero portraits from the Dota 2 HUD via Windows screen capture.
 * Supports 1280x720, 1920x1080, 2560x1440 and 3840x2160 resolutions.
 *
 * Dependencies: Windows GDI/GDI+, optional OpenCV for saving
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objidl.h>   // IStream, ISequentialStream — нужен GDI+ до включения gdiplus.h
#include <gdiplus.h>
#include <string>
#include <vector>
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <filesystem>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace dota2 {

// ─────────────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────────────

struct Resolution {
    int width;
    int height;
};

// A captured bitmap stored in memory
struct Bitmap {
    std::vector<uint8_t> pixels; // BGRA, row-major
    int width  = 0;
    int height = 0;

    bool empty() const { return pixels.empty(); }
};

// Portrait slot — 0..4 = Radiant, 5..9 = Dire
struct PortraitRegion {
    int slot;        // 0-9
    RECT rect;       // absolute screen coords
};

// ─────────────────────────────────────────────────────────────────────────────
// HUD Layout Database
// All values are *fractions* of the full game window size so they scale
// correctly across resolutions.
// ─────────────────────────────────────────────────────────────────────────────

struct HudLayout {
    // All values are fractions of the window size (0.0 .. 1.0).
    float radiant_x_start;   // left edge of first Radiant portrait
    float radiant_y_start;   // top  edge
    float portrait_w;        // width  of one portrait
    float portrait_h;        // height of one portrait
    float portrait_gap;      // horizontal gap between portraits
    float dire_x_start;      // left edge of first Dire portrait
};

// ── Strategy screen layouts per aspect ratio ─────────────────────────────────
// All fractions measured on real captures. Add more as needed.

static constexpr HudLayout STRATEGY_LAYOUT_16_9 = {
    /* radiant_x_start */ 0.13109f,
    /* radiant_y_start */ 0.00650f,
    /* portrait_w      */ 0.01789f,
    /* portrait_h      */ 0.03000f,
    /* portrait_gap    */ 0.04656f,
    /* dire_x_start    */ 0.59242f,
};

static constexpr HudLayout STRATEGY_LAYOUT_16_10 = {
    /* radiant_x_start */ 0.07496f,
    /* radiant_y_start */ 0.00650f,
    /* portrait_w      */ 0.02000f,
    /* portrait_h      */ 0.03000f,
    /* portrait_gap    */ 0.05188f,
    /* dire_x_start    */ 0.61793f,
};

static constexpr HudLayout STRATEGY_LAYOUT_21_9 = {
    /* radiant_x_start */ 0.0560f,
    /* radiant_y_start */ 0.0065f,
    /* portrait_w      */ 0.0152f,
    /* portrait_h      */ 0.0300f,
    /* portrait_gap    */ 0.0393f,
    /* dire_x_start    */ 0.6396f,
};

static constexpr HudLayout STRATEGY_LAYOUT_4_3 = {
    /* radiant_x_start */ 0.0994f,
    /* radiant_y_start */ 0.0065f,
    /* portrait_w      */ 0.0266f,
    /* portrait_h      */ 0.0300f,
    /* portrait_gap    */ 0.0693f,
    /* dire_x_start    */ 0.5841f,
};

// ── Select layout by aspect ratio ────────────────────────────────────────────
// Called automatically in findGameWindow() with the physical window dimensions.
inline HudLayout selectStrategyLayout(int w, int h) {
    if (h <= 0) return STRATEGY_LAYOUT_16_9;
    float ratio = static_cast<float>(w) / static_cast<float>(h);
    // Boundaries: midpoints between known ratios
    //   4:3  = 1.333
    //   16:10= 1.600   midpoint with 4:3  → 1.467
    //   16:9 = 1.778   midpoint with 16:10→ 1.689
    //   21:9 = 2.333   midpoint with 16:9 → 2.056
    if      (ratio < 1.467f) return STRATEGY_LAYOUT_4_3;
    else if (ratio < 1.689f) return STRATEGY_LAYOUT_16_10;
    else if (ratio < 2.056f) return STRATEGY_LAYOUT_16_9;
    else                     return STRATEGY_LAYOUT_21_9;
}
 
static constexpr HudLayout INGAME_LAYOUT = {
    /* radiant_x_start */ 0.0052f,
    /* radiant_y_start */ 0.9120f,
    /* portrait_w      */ 0.0365f,
    /* portrait_h      */ 0.0840f,
    /* portrait_gap    */ 0.0010f,
    /* dire_x_start    */ 0.6220f,
};
 
// Placeholder — overwritten in findGameWindow() by selectStrategyLayout()
static constexpr HudLayout DEFAULT_LAYOUT = STRATEGY_LAYOUT_16_9;
 
// ─────────────────────────────────────────────────────────────────────────────
// GDI+ RAII helpers
// ─────────────────────────────────────────────────────────────────────────────
 
class GdiplusSession {
public:
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&token_, &in, nullptr);
    }
    ~GdiplusSession() { Gdiplus::GdiplusShutdown(token_); }
    GdiplusSession(const GdiplusSession&)            = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;
private:
    ULONG_PTR token_{};
};
 
// ─────────────────────────────────────────────────────────────────────────────
// Core interface
// ─────────────────────────────────────────────────────────────────────────────
 
class Dota2Capture {
public:
    // output_dir — where PNG files are saved (created if absent)
    explicit Dota2Capture(std::filesystem::path output_dir = "portraits");
 
    // Find the Dota 2 window (returns false if not running / not in-game)
    bool findGameWindow();
 
    // Capture one frame and extract all visible hero portraits.
    // Returns the number of portrait slots extracted (0-10).
    int capturePortraits();
 
    // Returns the last set of captured portraits
    const std::vector<Bitmap>& portraits() const { return portraits_; }
 
    // Save all captured portraits as PNG files
    // Filenames: radiant_0.png … radiant_4.png, dire_5.png … dire_9.png
    void savePortraits(const std::filesystem::path& dir = "") const;
 
    // Register a callback invoked after each successful capturePortraits()
    // signature: void(int slot, const Bitmap&)
    using PortraitCallback = std::function<void(int, const Bitmap&)>;
    void setCallback(PortraitCallback cb) { callback_ = std::move(cb); }
 
    // Run a capture loop at the given interval (milliseconds)
    void runLoop(int interval_ms = 500);
 
    // Stop a running loop (call from another thread)
    void stopLoop() { running_ = false; }
 
    Resolution gameResolution()  const { return res_; }
    bool       isWindowFound()  const { return hwnd_ != nullptr; }
    HWND       gameWindowHandle() const { return hwnd_; }
 
    // Debug helpers
    Bitmap captureFullWindow();  // raw full-window capture (no cropping)
    const std::vector<PortraitRegion>& regions() const { return regions_; }
 
private:
    Bitmap captureWindow();
    Bitmap cropBitmap(const Bitmap& src, RECT r) const;
    void   computeRegions();
    bool   saveBitmapAsPng(const Bitmap& bmp,
                           const std::filesystem::path& path) const;
 
    HWND                      hwnd_     = nullptr;
    Resolution                res_      = {};
    HudLayout                 layout_   = DEFAULT_LAYOUT;
    std::vector<PortraitRegion> regions_;
    std::vector<Bitmap>       portraits_;
    std::filesystem::path     output_dir_;
    PortraitCallback          callback_;
    volatile bool             running_  = false;
};
 
// Free diagnostic function — prints all visible windows with class names
void ListAllWindows();
 
} // namespace dota2