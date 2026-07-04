/*
 * overlay_button.cpp — прозрачная кнопка [D] поверх Dota 2.
 *
 * Отдельное окно (WS_EX_LAYERED, per-pixel alpha) со своим WndProc и
 * своим потоком сообщений. Клик переключает фокус между приложением и Dota 2.
 * Не имеет общих функций/состояния с циклом захвата портретов (portrait_runner.cpp) —
 * оба живут в оркестраторе просто как два независимых Win32/GDI+ юнита.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>

#include "overlay_button.h"
#include "dota2_capture.h"

#include <algorithm>

using namespace dota2;

// Позиция и размер кнопки [D] относительно окна Dota 2 (всё в долях 0..1)
struct OverlayLayout { float xFrac; float yFrac; float wFrac; float hFrac; };
static constexpr OverlayLayout OVERLAY_16_9  = { 0.0058f, 0.05475f, 0.0228f, 0.024f };
static constexpr OverlayLayout OVERLAY_16_10 = { 0.0075f, 0.05475f, 0.0228f, 0.024f };
static constexpr OverlayLayout OVERLAY_21_9  = { 0.0050f, 0.05475f, 0.0178f, 0.024f };
static constexpr OverlayLayout OVERLAY_4_3   = { 0.0102f, 0.05599f, 0.0234f, 0.024f };

static OverlayLayout selectOverlayLayout(int w, int h) {
    if (h <= 0) return OVERLAY_16_9;
    float ratio = static_cast<float>(w) / static_cast<float>(h);
    if      (ratio < 1.467f) return OVERLAY_4_3;
    else if (ratio < 1.689f) return OVERLAY_16_10;
    else if (ratio < 2.056f) return OVERLAY_16_9;
    else                     return OVERLAY_21_9;
}

static int g_curBtnW = 0, g_curBtnH = 0;

// ─── Overlay-кнопка [D] (WS_EX_LAYERED, per-pixel alpha) ─────────────────────
static void paintLayeredButton(HWND hwnd, int W, int H);

static const COLORREF TRANSPARENT_KEY = RGB(1, 1, 1);

struct OverlayCtx {
    Dota2Capture* cap;
};

static HWND findAppWindow() {
    // Ищем главное окно приложения по классу
    return FindWindowW(L"Dota_Drafter", nullptr);
}

static void bringAppToFront() {
    HWND hw = findAppWindow();
    if (!hw || !IsWindow(hw)) return;
    if (IsIconic(hw)) ShowWindow(hw, SW_RESTORE);
    DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD my = GetCurrentThreadId();
    if (fg != my) AttachThreadInput(my, fg, TRUE);
    SetForegroundWindow(hw);
    BringWindowToTop(hw);
    if (fg != my) AttachThreadInput(my, fg, FALSE);
}

static void updateOverlayPos(HWND overlay, Dota2Capture* cap) {
    if (!cap || !cap->isWindowFound()) {
        SetWindowPos(overlay, HWND_TOPMOST, 10, 50,
                     80, 63, SWP_NOACTIVATE);
        return;
    }
    HWND game = cap->gameWindowHandle();
    RECT fr{};
    bool haveFr = SUCCEEDED(DwmGetWindowAttribute(game, 9, &fr, sizeof(fr)));
    if (!haveFr) { GetWindowRect(game, &fr); }

    int frameW = fr.right  - fr.left;
    int frameH = fr.bottom - fr.top;
    auto lay = selectOverlayLayout(frameW, frameH);

    int btnW = (std::max)(32, (int)(frameW * lay.wFrac));
    int btnH = (std::max)(25, (int)(frameH * lay.hFrac));

    if (btnW != g_curBtnW || btnH != g_curBtnH) {
        g_curBtnW = btnW;
        g_curBtnH = btnH;
        paintLayeredButton(overlay, btnW, btnH);
    }

    int x = fr.left + (int)(frameW * lay.xFrac);
    int y = fr.top  + (int)(frameH * lay.yFrac);
    SetWindowPos(overlay, HWND_TOPMOST, x, y,
                 btnW, btnH, SWP_NOACTIVATE);
}

// Per-pixel alpha: текст [D] подогнан так что [] касаются боков, D — верха
static void paintLayeredButton(HWND hwnd, int W, int H) {
    HDC screenDC = GetDC(nullptr);
    HDC memDC    = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = W;
    bmi.bmiHeader.biHeight      = -H;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t* pixels = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS,
                                    (void**)&pixels, nullptr, 0);
    if (!hBmp) { DeleteDC(memDC); ReleaseDC(nullptr, screenDC); return; }
    HBITMAP hOld = (HBITMAP)SelectObject(memDC, hBmp);

    for (int i = 0; i < W * H; i++) pixels[i] = 0;

    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));

    // Подбор размера шрифта: увеличиваем пока текст вмещается по ширине
    int fs = 8;
    for (int try_fs = H * 2; try_fs >= 8; --try_fs) {
        HFONT tf = CreateFontW(try_fs, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT oldF = (HFONT)SelectObject(memDC, tf);
        SIZE tsz{};
        GetTextExtentPoint32W(memDC, L"[D]", 3, &tsz);
        SelectObject(memDC, oldF);
        DeleteObject(tf);
        if (tsz.cx <= W) { fs = try_fs; break; }
    }

    HFONT f = CreateFontW(fs, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT of = (HFONT)SelectObject(memDC, f);

    // Измеряем точный размер текста
    SIZE tsz{};
    GetTextExtentPoint32W(memDC, L"[D]", 3, &tsz);

    // Центрируем по X ([] касаются краёв), текст прижат к верху (D касается верха)
    TEXTMETRICW tm{};
    GetTextMetricsW(memDC, &tm);
    int tx = (W - tsz.cx) / 2;
    int ty = -tm.tmInternalLeading;  // компенсируем внутренний отступ шрифта

    RECT r = { tx, ty, tx + tsz.cx + 1, ty + tsz.cy + 1 };
    DrawTextW(memDC, L"[D]", -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
    SelectObject(memDC, of); DeleteObject(f);

    // Цвет кнопки — серый как шестерёнка Dota 2 HUD
    const uint8_t gearR = 159, gearG = 165, gearB = 190;
    for (int i = 0; i < W * H; i++) {
        uint8_t b = (pixels[i] >>  0) & 0xFF;
        uint8_t g = (pixels[i] >>  8) & 0xFF;
        uint8_t rv= (pixels[i] >> 16) & 0xFF;
        uint8_t brightness = (uint8_t)(((uint32_t)rv + g + b) / 3);
        uint8_t a = (brightness > 20) ? brightness : 1;
        uint8_t pR = (uint8_t)((gearR * a) / 255);
        uint8_t pG = (uint8_t)((gearG * a) / 255);
        uint8_t pB = (uint8_t)((gearB * a) / 255);
        pixels[i] = ((uint32_t)a << 24) | ((uint32_t)pR << 16)
                   | ((uint32_t)pG << 8) | (uint32_t)pB;
    }

    POINT       ptSrc = {0, 0};
    SIZE        sz    = {W, H};
    BLENDFUNCTION bf  = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screenDC, nullptr, &sz, memDC,
                        &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(memDC, hOld); DeleteObject(hBmp);
    DeleteDC(memDC); ReleaseDC(nullptr, screenDC);
}

// Вывод окна Dota 2 на передний план
static void bringDotaToFront(Dota2Capture* cap) {
    if (!cap || !cap->isWindowFound()) return;
    HWND hw = cap->gameWindowHandle();
    if (!hw || !IsWindow(hw)) return;
    if (IsIconic(hw)) ShowWindow(hw, SW_RESTORE);
    DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD my = GetCurrentThreadId();
    if (fg != my) AttachThreadInput(my, fg, TRUE);
    SetForegroundWindow(hw);
    BringWindowToTop(hw);
    if (fg != my) AttachThreadInput(my, fg, FALSE);
}

// Оверлеи сторонних приложений (Discord/Steam/NVIDIA/Xbox Game Bar и т.п.),
// открытые поверх Dota 2, технически становятся foreground-окном ОС — из-за
// этого строгое сравнение fg==dotaHwnd прячет кнопку [D], хотя пользователь
// всё ещё в игре. Вместо хрупкого списка конкретных оверлеев (у каждого свой
// window class, меняется от версии к версии) проверяем общий признак игрового
// оверлея: WS_EX_TOPMOST-окно, геометрически перекрывающее окно Dota — по
// такому определению оверлей "не считается" переключением на другое приложение.
static bool isOverlayOverDota(HWND fg, HWND dotaHwnd) {
    if (!fg || !dotaHwnd || fg == dotaHwnd) return false;
    LONG_PTR exStyle = GetWindowLongPtr(fg, GWL_EXSTYLE);
    if (!(exStyle & WS_EX_TOPMOST)) return false;
    RECT fgRect{}, dotaRect{};
    if (!GetWindowRect(fg, &fgRect) || !GetWindowRect(dotaHwnd, &dotaRect)) return false;
    RECT inter{};
    return IntersectRect(&inter, &fgRect, &dotaRect) != 0;
}

static LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto* oc = reinterpret_cast<OverlayCtx*>(
            reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)oc);
        SetTimer(hwnd, 1, 500, nullptr);
        return 0;
    }
    case WM_TIMER: {
        auto* oc = reinterpret_cast<OverlayCtx*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (!oc) break;

        // Проверяем, жив ли сохранённый HWND Dota 2
        if (oc->cap->isWindowFound() &&
            !IsWindow(oc->cap->gameWindowHandle())) {
            oc->cap->findGameWindow();   // сброс + попытка найти заново
        }

        if (!oc->cap->isWindowFound())
            oc->cap->findGameWindow();

        if (oc->cap->isWindowFound()) {
            HWND fg       = GetForegroundWindow();
            HWND dotaHwnd = oc->cap->gameWindowHandle();
            HWND ourApp   = findAppWindow();
            bool shouldShow = (fg == dotaHwnd) ||
                              (ourApp && fg == ourApp) ||
                              isOverlayOverDota(fg, dotaHwnd);

            if (shouldShow) {
                if (!IsWindowVisible(hwnd))
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                updateOverlayPos(hwnd, oc->cap);
            } else {
                if (IsWindowVisible(hwnd))
                    ShowWindow(hwnd, SW_HIDE);
            }
        } else {
            if (IsWindowVisible(hwnd))
                ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        auto* oc2 = reinterpret_cast<OverlayCtx*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        HWND fg     = GetForegroundWindow();
        HWND ourApp = findAppWindow();
        if (ourApp && fg == ourApp) {
            // Наше приложение активно → выводим Dota
            bringDotaToFront(oc2 ? oc2->cap : nullptr);
        } else {
            // Dota активна → выводим наше приложение
            bringAppToFront();
        }
        return 0;
    }
    // WM_PAINT не нужен: контент задаётся через UpdateLayeredWindow
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static DWORD WINAPI overlayThread(LPVOID param) {
    auto* oc = reinterpret_cast<OverlayCtx*>(param);
    HINSTANCE hi = GetModuleHandle(nullptr);
    static constexpr wchar_t CLASS[] = L"DotaDOverlay";

    WNDCLASSW wc{};
    wc.lpfnWndProc   = overlayProc;
    wc.hInstance     = hi;
    wc.lpszClassName = CLASS;
    wc.hCursor       = LoadCursor(nullptr, IDC_HAND);
    RegisterClassW(&wc);

    // WS_EX_LAYERED для прозрачности. Без WS_EX_TRANSPARENT — вся область кликабельна.
    int initW = 80, initH = 63;
    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        CLASS, L"[D]", WS_POPUP,
        10, 20, initW, initH,
        nullptr, nullptr, hi, oc);

    if (!hw) return 1;

    g_curBtnW = initW;
    g_curBtnH = initH;
    paintLayeredButton(hw, initW, initH);

    ShowWindow(hw, SW_HIDE); // таймер сам покажет когда нужно

    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DestroyWindow(hw);
    UnregisterClassW(CLASS, hi);
    return 0;
}

// ─── startDotaOverlay — запуск кнопки [D] (один раз при старте) ───────────────

static Dota2Capture  s_overlayCap("");
static OverlayCtx    s_overlayCtx{ &s_overlayCap };
static HANDLE        s_overlayHandle = nullptr;

void startDotaOverlay() {
    if (s_overlayHandle) return;   // уже запущен
    s_overlayHandle = CreateThread(nullptr, 0, overlayThread, &s_overlayCtx, 0, nullptr);
}
