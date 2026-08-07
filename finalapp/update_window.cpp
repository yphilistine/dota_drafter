#include "update_window.h"
#include <dwmapi.h>
#include <cstdio>

static HWND  g_updateWnd      = nullptr;
static HWND  g_updateLabel    = nullptr;
static HWND  g_updatePercent  = nullptr;
static HWND  g_updateBtn      = nullptr;
static bool  g_retryClicked   = false;
// Окно сносим мы сами (DestroyUpdateWindow) - тогда WM_DESTROY не должен
// трактоваться как отказ пользователя от запуска.
static bool  g_selfDestroy    = false;
// Пользователь закрыл окно крестиком - приложение обязано завершиться.
static bool  g_aborted        = false;
static constexpr int IDC_RETRY_BTN = 101;

static LRESULT CALLBACK UpdateWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RETRY_BTN) g_retryClicked = true;
        return 0;
    case WM_DESTROY:
        // Без этого закрытие окна крестиком оставляет процесс жить: GUI ещё не
        // создан, а WaitForRetryClick() блокируется в GetMessage навсегда -
        // приложение висит в списке процессов без единого окна.
        if (!g_selfDestroy) {
            g_aborted = true;
            PostQuitMessage(0);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        HBRUSH br = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(hdc, &ps.rcPaint, br);
        DeleteObject(br);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, RGB(220, 220, 220));
        SetBkColor(hdc, RGB(20, 20, 20));
        static HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
        return (LRESULT)bg;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void CreateUpdateWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc   = UpdateWndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = L"DotaDrafterUpdate";
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 420, h = 170;

    g_updateWnd = CreateWindowExW(0,
        L"DotaDrafterUpdate", L"Dota Drafter - Update",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sw - w) / 2, (sh - h) / 2, w, h,
        nullptr, nullptr, hInst, nullptr);

    // Тёмная тема
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_updateWnd, 20, &dark, sizeof(dark));
    COLORREF capColor  = RGB(12, 12, 12);
    COLORREF textColor = RGB(240, 240, 240);
    COLORREF borColor  = RGB(99, 199, 118);
    DwmSetWindowAttribute(g_updateWnd, 35, &capColor,  sizeof(capColor));
    DwmSetWindowAttribute(g_updateWnd, 36, &textColor, sizeof(textColor));
    DwmSetWindowAttribute(g_updateWnd, 34, &borColor,  sizeof(borColor));

    // Иконка
    HICON iconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    HICON iconBg = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0);
    if (iconSm) SendMessage(g_updateWnd, WM_SETICON, ICON_SMALL, (LPARAM)iconSm);
    if (iconBg) SendMessage(g_updateWnd, WM_SETICON, ICON_BIG,   (LPARAM)iconBg);

    RECT cr;
    GetClientRect(g_updateWnd, &cr);
    int cw = cr.right - cr.left;
    int ch = cr.bottom - cr.top;

    HFONT font = CreateFontW(30, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT fontPercent = CreateFontW(35, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    g_updateLabel = CreateWindowW(L"STATIC",
        L"Checking for updates...",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        5, 8, cw - 10, 40, g_updateWnd, nullptr, hInst, nullptr);
    SendMessage(g_updateLabel, WM_SETFONT, (WPARAM)font, TRUE);

    g_updatePercent = CreateWindowW(L"STATIC",
        L"",
        WS_CHILD | SS_CENTER,
        5, 45, cw - 10, 50, g_updateWnd, nullptr, hInst, nullptr);
    SendMessage(g_updatePercent, WM_SETFONT, (WPARAM)fontPercent, TRUE);

    HFONT btnFont = CreateFontW(24, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_updateBtn = CreateWindowW(L"BUTTON",
        L"Try again",
        WS_CHILD | BS_PUSHBUTTON | BS_FLAT,
        (cw - 180) / 2, ch - 50, 180, 42,
        g_updateWnd, (HMENU)(INT_PTR)IDC_RETRY_BTN, hInst, nullptr);
    SendMessage(g_updateBtn, WM_SETFONT, (WPARAM)btnFont, TRUE);

    ShowWindow(g_updateWnd, SW_SHOW);
    UpdateWindow(g_updateWnd);
}

void PumpMessages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { g_aborted = true; return; }
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
}

bool UpdateWindowAborted() { return g_aborted; }

void SetUpdateStatus(const wchar_t* text) {
    if (g_updateLabel) SetWindowTextW(g_updateLabel, text);
    if (g_updatePercent) { SetWindowTextW(g_updatePercent, L""); ShowWindow(g_updatePercent, SW_HIDE); }
    if (g_updateWnd) RedrawWindow(g_updateWnd, nullptr, nullptr,
                                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    PumpMessages();
}

void SetUpdateProgress(const wchar_t* label, int percent) {
    static int lastPercent = -1;
    static const wchar_t* lastLabel = nullptr;

    // Обновить заголовок только если изменился
    if (label != lastLabel) {
        lastLabel = label;
        if (g_updateLabel) SetWindowTextW(g_updateLabel, label);
    }
    if (g_updatePercent) {
        ShowWindow(g_updatePercent, SW_SHOW);
        if (percent != lastPercent) {
            lastPercent = percent;
            wchar_t buf[32];
            swprintf_s(buf, L"%d%%", percent);
            SetWindowTextW(g_updatePercent, buf);
        }
    }
    PumpMessages();
}

void ShowRetryButton(bool show) {
    if (g_updateBtn) ShowWindow(g_updateBtn, show ? SW_SHOW : SW_HIDE);
    g_retryClicked = false;
}

void DestroyUpdateWindow() {
    g_selfDestroy = true;
    if (g_updateWnd) { DestroyWindow(g_updateWnd); g_updateWnd = nullptr; }
    g_updateLabel   = nullptr;
    g_updatePercent = nullptr;
    g_updateBtn   = nullptr;
}

bool WaitForRetryClick() {
    g_retryClicked = false;
    MSG msg;
    while (!g_retryClicked) {
        if (GetMessage(&msg, nullptr, 0, 0) <= 0) return false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_QUIT) return false;
    }
    return true;
}
