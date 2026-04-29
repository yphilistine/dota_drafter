#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objidl.h>
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

struct Resolution {
    int width;
    int height;
};

struct Bitmap {
    std::vector<uint8_t> pixels;
    int width  = 0;
    int height = 0;

    bool empty() const { return pixels.empty(); }
};

struct PortraitRegion {
    int slot;
    RECT rect;
};

struct HudLayout {

    float radiant_x_start;
    float radiant_y_start;
    float portrait_w;
    float portrait_h;
    float portrait_gap;
    float dire_x_start;
};

static constexpr HudLayout STRATEGY_LAYOUT_16_9 = {
     0.13109f,
     0.00650f,
     0.01789f,
     0.03000f,
     0.04656f,
     0.59242f,
};

static constexpr HudLayout STRATEGY_LAYOUT_16_10 = {
     0.07496f,
     0.00650f,
     0.02000f,
     0.03000f,
     0.05188f,
     0.61793f,
};

static constexpr HudLayout STRATEGY_LAYOUT_21_9 = {
     0.0560f,
     0.0065f,
     0.0152f,
     0.0300f,
     0.0393f,
     0.6396f,
};

static constexpr HudLayout STRATEGY_LAYOUT_4_3 = {
     0.0994f,
     0.0065f,
     0.0266f,
     0.0300f,
     0.0693f,
     0.5841f,
};

inline HudLayout selectStrategyLayout(int w, int h) {
    if (h <= 0) return STRATEGY_LAYOUT_16_9;
    float ratio = static_cast<float>(w) / static_cast<float>(h);

    if      (ratio < 1.467f) return STRATEGY_LAYOUT_4_3;
    else if (ratio < 1.689f) return STRATEGY_LAYOUT_16_10;
    else if (ratio < 2.056f) return STRATEGY_LAYOUT_16_9;
    else                     return STRATEGY_LAYOUT_21_9;
}

static constexpr HudLayout DEFAULT_LAYOUT = STRATEGY_LAYOUT_16_9;

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

class Dota2Capture {
public:

    explicit Dota2Capture(std::filesystem::path output_dir = "portraits");

    bool findGameWindow();

    int capturePortraits();

    const std::vector<Bitmap>& portraits() const { return portraits_; }

    void savePortraits(const std::filesystem::path& dir = "") const;

    using PortraitCallback = std::function<void(int, const Bitmap&)>;
    void setCallback(PortraitCallback cb) { callback_ = std::move(cb); }

    void runLoop(int interval_ms = 500);

    void stopLoop() { running_ = false; }

    Resolution gameResolution()  const { return res_; }
    bool       isWindowFound()  const { return hwnd_ != nullptr; }
    HWND       gameWindowHandle() const { return hwnd_; }

    Bitmap captureFullWindow();
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

void ListAllWindows();

}
