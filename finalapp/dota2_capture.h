#pragma once

/*
 * dota2_capture.h — захват портретов героев из HUD Dota 2.
 *
 * Использует Windows GDI (PrintWindow) для захвата окна.
 * Поддержка разрешений: 4:3, 16:10, 16:9, 21:9 + DPI-масштабирование.
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

// ─── Типы ────────────────────────────────────────────────────────────────────

struct Resolution {
    int width;
    int height;
};

// Захваченное изображение в памяти (BGRA, row-major)
struct Bitmap {
    std::vector<uint8_t> pixels;
    int width  = 0;
    int height = 0;
    bool empty() const { return pixels.empty(); }
};

// Область портрета: слот 0-4 = Radiant, 5-9 = Dire
struct PortraitRegion {
    int slot;        // 0-9
    RECT rect;       // абсолютные координаты в окне
};

// ─── Раскладки HUD ──────────────────────────────────────────────────────────
// Все значения — доли от размера окна (0.0 .. 1.0), масштабируются автоматически.

struct HudLayout {
    float radiant_x_start;   // левый край первого портрета Radiant
    float radiant_y_start;   // верхний край
    float portrait_w;        // ширина портрета
    float portrait_h;        // высота портрета
    float portrait_gap;      // горизонтальный зазор между портретами
    float dire_x_start;      // левый край первого портрета Dire
};

// ── Раскладки экрана стратегии по соотношению сторон ──────────────────────────
// Все доли измерены на реальных захватах.

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

// Выбор раскладки по соотношению сторон окна (вызывается в findGameWindow)
inline HudLayout selectStrategyLayout(int w, int h) {
    if (h <= 0) return STRATEGY_LAYOUT_16_9;
    float ratio = static_cast<float>(w) / static_cast<float>(h);
    // Границы: средние точки между известными соотношениями
    if      (ratio < 1.467f) return STRATEGY_LAYOUT_4_3;
    else if (ratio < 1.689f) return STRATEGY_LAYOUT_16_10;
    else if (ratio < 2.056f) return STRATEGY_LAYOUT_16_9;
    else                     return STRATEGY_LAYOUT_21_9;
}
 

// Раскладка по умолчанию — перезаписывается в findGameWindow()
static constexpr HudLayout DEFAULT_LAYOUT = STRATEGY_LAYOUT_16_9;

// ─── RAII-обёртки GDI+ ──────────────────────────────────────────────────────

// GDI+ Startup / Shutdown
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
 
// ─── Основной интерфейс захвата ──────────────────────────────────────────────

class Dota2Capture {
public:
    // output_dir — папка для сохранения PNG (создаётся при необходимости)
    explicit Dota2Capture(std::filesystem::path output_dir = "portraits");

    // Поиск окна Dota 2 (SDL_app/Valve001). Определяет разрешение и раскладку.
    bool findGameWindow();

    // Захват всех портретов из одного кадра. Возвращает количество слотов (0-10).
    int capturePortraits();

    // Последний набор захваченных портретов
    const std::vector<Bitmap>& portraits() const { return portraits_; }

    // Сохранение портретов как PNG: radiant_0.png .. dire_9.png
    void savePortraits(const std::filesystem::path& dir = "") const;

    // Коллбэк после каждого успешного захвата: void(slot, bitmap)
    using PortraitCallback = std::function<void(int, const Bitmap&)>;
    void setCallback(PortraitCallback cb) { callback_ = std::move(cb); }

    // Цикл захвата с заданным интервалом (мс)
    void runLoop(int interval_ms = 500);

    // Остановка цикла (вызывать из другого потока)
    void stopLoop() { running_ = false; }

    Resolution gameResolution()  const { return res_; }
    bool       isWindowFound()  const { return hwnd_ != nullptr; }
    HWND       gameWindowHandle() const { return hwnd_; }

    // Захват всего окна без обрезки (для отладки)
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
 
// Диагностика: вывод всех видимых окон с именами классов
void ListAllWindows();
 
} // namespace dota2