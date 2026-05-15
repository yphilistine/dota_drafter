/*
 * portrait_runner.cpp
 *
 * Захватывает портреты каждые 500 мс пока running == true.
 * НЕ сохраняет PNG. При score >= 0.5 и имя != "NULL" пишет в livepicks.
 *
 * Кнопка "▶ Bring picker" видна всегда пока открыта Dota 2,
 * независимо от фазы игры и фокуса окна.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "portrait_runner.h"
#include "dota2_capture.h"
#include "dhash.h"

#if __has_include("hero_hashes.h")
#  include "hero_hashes.h"
#  define HERO_DB_LOADED 1
#endif

#include <map>
#include <string>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <sqlite3.h>

using namespace dota2;

// ─────────────────────────────────────────────────────────────────────────────
// Утилиты
// ─────────────────────────────────────────────────────────────────────────────

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::map<std::string, int> loadHeroNameToId(sqlite3* db) {
    std::map<std::string, int> m;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, localized_name FROM heroes",
                           -1, &st, nullptr) != SQLITE_OK) return m;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int id = sqlite3_column_int(st, 0);
        const char* raw = (const char*)sqlite3_column_text(st, 1);
        if (!raw) continue;
        std::string name = raw;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        m[name] = id;
    }
    sqlite3_finalize(st);
    return m;
}

static int lookupHeroId(const std::map<std::string, int>& nameMap, const char* rawName) {
    if (!rawName) return 0;
    std::string lower = rawName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto it = nameMap.find(lower);
    return it != nameMap.end() ? it->second : 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// livepicks
// ─────────────────────────────────────────────────────────────────────────────

static void updateSlot(sqlite3* db, int slot, int heroId) {
    char col[16], colPos[16];
    if (slot < 5) {
        std::snprintf(col,    sizeof(col),    "r%d_hero", slot + 1);
        std::snprintf(colPos, sizeof(colPos), "r%d_pos",  slot + 1);
    } else {
        std::snprintf(col,    sizeof(col),    "d%d_hero", slot - 4);
        std::snprintf(colPos, sizeof(colPos), "d%d_pos",  slot - 4);
    }
    int pos = (slot < 5) ? (slot + 1) : (slot - 4);
    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "UPDATE livepicks SET %s=?, %s=?, updated_at=?;", col, colPos);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(st,   1, heroId);
    sqlite3_bind_int(st,   2, pos);
    sqlite3_bind_int64(st, 3, nowMs());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void clearHeroSlots(sqlite3* db) {
    sqlite3_exec(db,
        "UPDATE livepicks SET "
        "r1_hero=0,r2_hero=0,r3_hero=0,r4_hero=0,r5_hero=0,"
        "d1_hero=0,d2_hero=0,d3_hero=0,d4_hero=0,d5_hero=0,"
        "updated_at=0;",
        nullptr, nullptr, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Overlay-кнопка
// Видна ВСЕГДА пока окно Dota 2 существует (независимо от фазы и фокуса)
// ─────────────────────────────────────────────────────────────────────────────

struct OverlayCtx {
    Dota2Capture* cap;
    HWND          consoleHwnd;
};

static void bringConsoleToFront(HWND consoleHwnd) {
    if (!consoleHwnd || !IsWindow(consoleHwnd)) return;
    if (IsIconic(consoleHwnd)) ShowWindow(consoleHwnd, SW_RESTORE);
    DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD my = GetCurrentThreadId();
    if (fg != my) AttachThreadInput(my, fg, TRUE);
    SetForegroundWindow(consoleHwnd);
    BringWindowToTop(consoleHwnd);
    if (fg != my) AttachThreadInput(my, fg, FALSE);
}

static void updateOverlayPos(HWND overlay, Dota2Capture* cap) {
    RECT gr;
    if (cap && cap->isWindowFound() &&
        GetWindowRect(cap->gameWindowHandle(), &gr)) {
        int w = (std::max)(80, (int)(gr.right - gr.left) / 10);
        int h = (std::max)(24, (int)(gr.bottom - gr.top) / 25);
        SetWindowPos(overlay, HWND_TOPMOST,
                     gr.left + 10, gr.top + 10 + h, w, h, SWP_NOACTIVATE);
    } else {
        SetWindowPos(overlay, HWND_TOPMOST, 10, 44, 180, 36, SWP_NOACTIVATE);
    }
}

static LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto* oc = reinterpret_cast<OverlayCtx*>(
            reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)oc);
        SetTimer(hwnd, 1, 500, nullptr); // проверяем каждые 500 мс
        return 0;
    }
    case WM_TIMER: {
        auto* oc = reinterpret_cast<OverlayCtx*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (!oc) break;

        // Пытаемся найти окно если ещё нет
        if (!oc->cap->isWindowFound())
            oc->cap->findGameWindow();

        if (oc->cap->isWindowFound()) {
            // Показываем всегда когда Dota открыта
            if (!IsWindowVisible(hwnd))
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            updateOverlayPos(hwnd, oc->cap);
        } else {
            // Dota закрыта — прячем
            if (IsWindowVisible(hwnd))
                ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        auto* oc = reinterpret_cast<OverlayCtx*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (oc) bringConsoleToFront(oc->consoleHwnd);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        HBRUSH bg = CreateSolidBrush(RGB(30, 30, 60));
        FillRect(dc, &r, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(180, 210, 255));
        int fs = (std::max)(8, (int)(r.bottom - r.top) / 2);
        HFONT f = CreateFontW(fs, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT of = (HFONT)SelectObject(dc, f);
        DrawTextW(dc, L"\u25BA Bring picker", -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of); DeleteObject(f);
        EndPaint(hwnd, &ps); return 0;
    }
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
    static constexpr wchar_t CLASS[] = L"PortraitOverlayBtn";
    WNDCLASSW wc{};
    wc.lpfnWndProc   = overlayProc;
    wc.hInstance     = hi;
    wc.lpszClassName = CLASS;
    wc.hCursor       = LoadCursor(nullptr, IDC_HAND);
    RegisterClassW(&wc);
    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CLASS, L"", WS_POPUP,
        10, 44, 180, 36, nullptr, nullptr, hi, oc);
    if (!hw) return 1;
    ShowWindow(hw, SW_HIDE); // таймер сам покажет когда найдёт Dota
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    DestroyWindow(hw);
    UnregisterClassW(CLASS, hi);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// runPortraitCapture
// ─────────────────────────────────────────────────────────────────────────────

void runPortraitCapture(GameInfo&           gameInfo,
                        const std::string&  dbPath,
                        std::atomic<bool>&  running,
                        SharedPortraitState& out)
{
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        std::fprintf(stderr, "[portrait] Не удалось открыть БД\n");
        Gdiplus::GdiplusShutdown(gdipToken);
        return;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    auto nameToId = loadHeroNameToId(db);
    std::printf("[portrait] Справочник героев: %zu записей\n", nameToId.size());

#ifdef HERO_DB_LOADED
    HeroRecognizer recognizer(g_hero_db, g_hero_db_size);
    std::printf("[portrait] Hash-база: %zu героев\n", recognizer.size());
#else
    std::puts("[portrait] ВНИМАНИЕ: hero_hashes.h не найден");
#endif

    // Capture-объект (без сохранения PNG)
    Dota2Capture cap("");

    // Overlay-кнопка — запускаем сразу, она сама найдёт Dota
    OverlayCtx overlayCtx{ &cap, GetConsoleWindow() };
    HANDLE overlayH = CreateThread(nullptr, 0, overlayThread, &overlayCtx, 0, nullptr);

    // Ждём Dota 2
    if (!cap.findGameWindow()) {
        std::puts("[portrait] Dota 2 не найдена, ожидаем...");
        while (running.load() && !cap.findGameWindow())
            std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!running.load()) goto cleanup;

    {
        auto res = cap.gameResolution();
        std::printf("[portrait] Окно Dota 2: %dx%d\n", res.width, res.height);
        clearHeroSlots(db);

        int lastHeroId[10] = {};
        std::puts("[portrait] Захват каждые 500 мс (HERO_SELECTION)...");
        std::puts("[portrait] ─────────────────────────────────────────");

        while (running.load()) {
            auto frameStart = std::chrono::steady_clock::now();

            // Сброс слотов при новой игре
            bool gameChanged = false;
            {
                std::lock_guard<std::mutex> lk(gameInfo.mtx);
                if (gameInfo.newMatch) gameChanged = true;
            }
            if (gameChanged) {
                clearHeroSlots(db);
                std::memset(lastHeroId, 0, sizeof(lastHeroId));
                std::puts("[portrait] Новая игра — слоты сброшены");
            }

            // Проверяем окно
            if (!cap.isWindowFound() || !IsWindow(cap.gameWindowHandle())) {
                cap.findGameWindow();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            int captured = cap.capturePortraits();
            if (captured <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            const auto& portraits = cap.portraits();

            for (int slot = 0; slot < 10 && slot < (int)portraits.size(); ++slot) {
                const Bitmap& bmp = portraits[slot];
                if (bmp.empty()) continue;

#ifdef HERO_DB_LOADED
                HeroMatch m = recognizer.recognize(bmp);
                if (!m.name) continue;

                // "NULL" — пустой слот, пропускаем при любом score
                bool isNullHero = (std::strcmp(m.name, "NULL") == 0);

                // Обновляем SharedPortraitState (для picker)
                if (!isNullHero) {
                    int heroId = lookupHeroId(nameToId, m.name);
                    std::lock_guard<std::mutex> lk(out.mtx);
                    out.slots[slot].heroName = m.name;
                    out.slots[slot].heroId   = heroId;
                    out.slots[slot].score    = m.score;
                }

                // В livepicks: score >= 0.5, не NULL, герой найден, новый
                if (!isNullHero && m.score >= 0.5f) {
                    int heroId = lookupHeroId(nameToId, m.name);
                    if (heroId > 0 && heroId != lastHeroId[slot]) {
                        updateSlot(db, slot, heroId);
                        lastHeroId[slot] = heroId;
                        const char* team = (slot < 5) ? "Radiant" : "Dire";
                        int idx = (slot < 5) ? slot + 1 : slot - 4;
                        std::printf("[portrait] %s #%d → %-22s  score=%.3f\n",
                                    team, idx, m.name, m.score);
                    }
                }
#endif
            }

            // 500 мс между кадрами
            auto elapsed   = std::chrono::steady_clock::now() - frameStart;
            auto remaining = std::chrono::milliseconds(500) - elapsed;
            if (remaining > std::chrono::milliseconds(0))
                std::this_thread::sleep_for(remaining);
        }
    }

cleanup:
    if (overlayH) {
        PostThreadMessage(GetThreadId(overlayH), WM_QUIT, 0, 0);
        WaitForSingleObject(overlayH, 2000);
        CloseHandle(overlayH);
    }
    sqlite3_close(db);
    Gdiplus::GdiplusShutdown(gdipToken);
    std::puts("[portrait] Захват остановлен");
}