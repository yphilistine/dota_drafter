/*
 * portrait_runner.cpp
 *
 * Захватывает портреты каждые 500 мс пока running == true.
 * НЕ сохраняет PNG. При score >= 0.5 и имя != "NULL" пишет в livepicks.
 *
 * Кнопка [D] — прозрачный overlay поверх Dota 2 (без фона).
 * Видна всегда пока открыта Dota 2, независимо от фазы и фокуса окна.
 * Клик — выводит главное окно приложения на передний план.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
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

// Размер overlay-кнопки — минимально под текст [D] с небольшим отступом
static constexpr int OVERLAY_BTN = 78;

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
// Overlay — кнопка [D] без фона (прозрачный WS_EX_LAYERED window)
// Ищет главное окно Dota_Drafter и выводит его на передний план по клику.
// ─────────────────────────────────────────────────────────────────────────────

// Цвет-ключ прозрачности: RGB(1,1,1) — почти чёрный, не перекрывает текст
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
        SetWindowPos(overlay, HWND_TOPMOST, 10, 20,
                     OVERLAY_BTN, OVERLAY_BTN, SWP_NOACTIVATE);
        return;
    }
    HWND game = cap->gameWindowHandle();
    RECT fr{};
    if (SUCCEEDED(DwmGetWindowAttribute(game, 9, &fr, sizeof(fr)))) {
        int frameH = fr.bottom - fr.top;
        int gearH  = (frameH > 0) ? (std::max)(5, (int)(frameH * 0.02f)) : 20;
        SetWindowPos(overlay, HWND_TOPMOST,
                     fr.left + 10, fr.top + gearH,
                     OVERLAY_BTN, OVERLAY_BTN, SWP_NOACTIVATE);
    } else {
        RECT wr{};
        GetWindowRect(game, &wr);
        SetWindowPos(overlay, HWND_TOPMOST,
                     wr.left + 10, wr.top + 20,
                     OVERLAY_BTN, OVERLAY_BTN, SWP_NOACTIVATE);
    }
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

        if (!oc->cap->isWindowFound())
            oc->cap->findGameWindow();

        if (oc->cap->isWindowFound()) {
            // Показываем ТОЛЬКО когда Dota — активное (foreground) окно
            HWND fg      = GetForegroundWindow();
            HWND dotaHwnd = oc->cap->gameWindowHandle();
            bool dotaActive = (fg == dotaHwnd);

            if (dotaActive) {
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
        bringAppToFront();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);

        // Прозрачный фон — LWA_ALPHA делает всё окно полупрозрачным (39%)
        // Заливаем чёрным, который при alpha=100 почти незаметен
        HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &r, bg);
        DeleteObject(bg);

        // "[D]" — белый жирный текст, видим даже при низком alpha
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));

        int fs = (int)((r.bottom - r.top) * 0.55f);
        if (fs < 8) fs = 8;
        HFONT f = CreateFontW(fs, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT of = (HFONT)SelectObject(dc, f);
        DrawTextW(dc, L"[D]", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
        DeleteObject(f);

        EndPaint(hwnd, &ps);
        return 0;
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
    static constexpr wchar_t CLASS[] = L"DotaDOverlay";

    WNDCLASSW wc{};
    wc.lpfnWndProc   = overlayProc;
    wc.hInstance     = hi;
    wc.lpszClassName = CLASS;
    wc.hCursor       = LoadCursor(nullptr, IDC_HAND);
    RegisterClassW(&wc);

    // WS_EX_LAYERED для прозрачности. БЕЗ WS_EX_TRANSPARENT — весь 132×132 кликабелен.
    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        CLASS, L"[D]", WS_POPUP,
        10, 20, OVERLAY_BTN, OVERLAY_BTN,
        nullptr, nullptr, hi, oc);

    if (!hw) return 1;

    // LWA_ALPHA=100 (39% opacity): вся 132×132 кликабельна, фон почти прозрачен
    SetLayeredWindowAttributes(hw, 0, 100, LWA_ALPHA);

    ShowWindow(hw, SW_HIDE); // таймер сам покажет когда найдёт Dota

    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DestroyWindow(hw);
    UnregisterClassW(CLASS, hi);
    return 0;
}

static void clearHeroSlot(sqlite3* db, int slot) {
    char col[16];
    if (slot < 5) std::snprintf(col, sizeof(col), "r%d_hero", slot + 1);
    else          std::snprintf(col, sizeof(col), "d%d_hero", slot - 4);
    char sql[128];
    std::snprintf(sql, sizeof(sql),
        "UPDATE livepicks SET %s=0, updated_at=?;", col);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, nowMs());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// ─────────────────────────────────────────────────────────────────────────────
// startDotaOverlay — запускает [D] кнопку один раз при старте приложения.
// Кнопка видна всегда, пока открыто окно Dota 2, независимо от фазы.
// Использует static-хранилище — безопасно вызывать один раз из любого потока.
// ─────────────────────────────────────────────────────────────────────────────

static Dota2Capture  s_overlayCap("");
static OverlayCtx    s_overlayCtx{ &s_overlayCap };
static HANDLE        s_overlayHandle = nullptr;

void startDotaOverlay() {
    if (s_overlayHandle) return;   // уже запущен
    s_overlayHandle = CreateThread(nullptr, 0, overlayThread, &s_overlayCtx, 0, nullptr);
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
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;",   nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    auto nameToId = loadHeroNameToId(db);
    std::printf("[portrait] Справочник героев: %zu записей\n", nameToId.size());

#ifdef HERO_DB_LOADED
    HeroRecognizer recognizer(g_hero_db, g_hero_db_size);
    std::printf("[portrait] Hash-база: %zu героев\n", recognizer.size());
#else
    std::puts("[portrait] ВНИМАНИЕ: hero_hashes.h не найден");
#endif

    // Overlay уже запущен через startDotaOverlay() из оркестратора.
    // Здесь создаём отдельный Dota2Capture только для захвата экрана.
    Dota2Capture cap("");

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

        int   lastHeroId[10] = {};
        float lastScore[10]  = {};    // хранимая уверенность для каждого слота
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
                std::memset(lastScore,  0, sizeof(lastScore));
                std::puts("[portrait] Новая игра — слоты сброшены");
            }

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
                if (!m.name || m.score < 0.5f) continue;  // ниже порога

                bool isNullHero  = (std::strcmp(m.name, "NULL") == 0);
                int  detectedId  = isNullHero ? 0 : lookupHeroId(nameToId, m.name);

                // Обновляем SharedPortraitState (для не-NULL)
                if (!isNullHero && detectedId > 0) {
                    std::lock_guard<std::mutex> lk(out.mtx);
                    out.slots[slot].heroName = m.name;
                    out.slots[slot].heroId   = detectedId;
                    out.slots[slot].score    = m.score;
                }

                if (detectedId != lastHeroId[slot]) {
                    // Правила замены:
                    // • слот пуст (NULL/0) → герой заполняет при любой score >= 0.5
                    // • слот занят героем → замена (другим героем или NULL)
                    //   только если новая уверенность ВЫШЕ сохранённой
                    bool slotEmpty   = (lastHeroId[slot] == 0);
                    bool shouldUpdate = slotEmpty
                        ? (detectedId > 0)               // герой всегда заполняет пустой слот
                        : (m.score > lastScore[slot]);   // замена только с большей уверенностью

                    if (shouldUpdate) {
                        if (detectedId == 0) {
                            // NULL с большей уверенностью → очищаем
                            clearHeroSlot(db, slot);
                            {
                                std::lock_guard<std::mutex> lk(out.mtx);
                                out.slots[slot] = {};
                            }
                            const char* team = (slot < 5) ? "Radiant" : "Dire";
                            int idx = (slot < 5) ? slot+1 : slot-4;
                            std::printf("[portrait] %s #%d → NULL  score=%.3f\n",
                                        team, idx, m.score);
                        } else {
                            updateSlot(db, slot, detectedId);
                            const char* team = (slot < 5) ? "Radiant" : "Dire";
                            int idx = (slot < 5) ? slot+1 : slot-4;
                            std::printf("[portrait] %s #%d → %-22s  score=%.3f\n",
                                        team, idx, m.name, m.score);
                        }
                        lastHeroId[slot] = detectedId;
                        lastScore[slot]  = m.score;
                    }
                } else if (m.score > lastScore[slot]) {
                    lastScore[slot] = m.score;
                }
#endif
            }

            auto elapsed   = std::chrono::steady_clock::now() - frameStart;
            auto remaining = std::chrono::milliseconds(500) - elapsed;
            if (remaining > std::chrono::milliseconds(0))
                std::this_thread::sleep_for(remaining);
        }
    }

cleanup:
    // overlay живёт всё время приложения — не останавливаем
    sqlite3_close(db);
    Gdiplus::GdiplusShutdown(gdipToken);
    std::puts("[portrait] Захват остановлен");
}