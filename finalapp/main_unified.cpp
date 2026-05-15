/*
 * main_unified.cpp
 *
 * Правила работы модулей:
 *
 *  Portrait  — запускается при входе в HERO_SELECTION,
 *              работает ещё 30 сек после выхода из HERO_SELECTION,
 *              затем останавливается.
 *
 *  Picker    — запускается при входе в DRAFT (HERO_SELECTION / STRATEGY_TIME),
 *              работает всю игру (INGAME), останавливается при POSTGAME / IDLE.
 *
 *  Кнопка    — видна всегда пока открыта Dota 2 (в portrait_runner).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#define CURL_STATICLIB
#include <windows.h>
#include <winsock2.h>

#include "shared_types.h"
#include "common.h"
#include "playerdatafetcher.h"

#include <thread>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Конфигурация
// ─────────────────────────────────────────────────────────────────────────────

static const char* DB_PATH    = "playerandlivestats.db";
static const char* MODEL_PATH = "draft_helper_v3";

static const int PORTRAIT_TAIL_SEC = 30; // сколько секунд работает после HERO_SELECTION

// ─────────────────────────────────────────────────────────────────────────────
// livepicks
// ─────────────────────────────────────────────────────────────────────────────

static void createLivePicksIfNotExists(sqlite3* db) {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS livepicks (
            match_id       INTEGER NOT NULL DEFAULT 0,
            our_account_id INTEGER NOT NULL DEFAULT 0,
            our_side       INTEGER NOT NULL DEFAULT 1,
            our_slot       INTEGER NOT NULL DEFAULT 1,
            updated_at     INTEGER NOT NULL DEFAULT 0,
            r1_hero INTEGER DEFAULT 0, r1_pos INTEGER DEFAULT 0,
            r2_hero INTEGER DEFAULT 0, r2_pos INTEGER DEFAULT 0,
            r3_hero INTEGER DEFAULT 0, r3_pos INTEGER DEFAULT 0,
            r4_hero INTEGER DEFAULT 0, r4_pos INTEGER DEFAULT 0,
            r5_hero INTEGER DEFAULT 0, r5_pos INTEGER DEFAULT 0,
            d1_hero INTEGER DEFAULT 0, d1_pos INTEGER DEFAULT 0,
            d2_hero INTEGER DEFAULT 0, d2_pos INTEGER DEFAULT 0,
            d3_hero INTEGER DEFAULT 0, d3_pos INTEGER DEFAULT 0,
            d4_hero INTEGER DEFAULT 0, d4_pos INTEGER DEFAULT 0,
            d5_hero INTEGER DEFAULT 0, d5_pos INTEGER DEFAULT 0
        );
    )";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err; sqlite3_free(err);
        throw std::runtime_error("livepicks create: " + e);
    }
}

static void clearLivePicks(sqlite3* db) {
    char* err = nullptr;
    sqlite3_exec(db, "DELETE FROM livepicks;", nullptr, nullptr, &err);
    if (err) { LOG_WARN("clearLivePicks: " << err); sqlite3_free(err); }
    else LOG_INFO("livepicks очищена (новая игра)");
}

static void initLivePicksRow(sqlite3* db, long long matchId, long long accountId,
                              int ourSide, int ourSlot) {
    sqlite3_exec(db, "DELETE FROM livepicks;", nullptr, nullptr, nullptr);
    const char* sql =
        "INSERT INTO livepicks (match_id, our_account_id, our_side, our_slot, updated_at) "
        "VALUES (?,?,?,?,?);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, matchId);
    sqlite3_bind_int64(st, 2, accountId);
    sqlite3_bind_int(st,   3, ourSide);
    sqlite3_bind_int(st,   4, ourSlot);
    sqlite3_bind_int64(st, 5,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    sqlite3_step(st);
    sqlite3_finalize(st);
    LOG_INFO("livepicks: строка инициализирована  match=" << matchId
             << " side=" << ourSide << " slot=" << ourSlot);
}

// ─────────────────────────────────────────────────────────────────────────────
// Фаза 1
// ─────────────────────────────────────────────────────────────────────────────

static void runDataFetcherPhase(long long accountId, const std::string& stratzToken) {
    LOG_INFO("╔══════════════════════════════════════╗");
    LOG_INFO("║  Фаза 1: DataFetcher                 ║");
    LOG_INFO("╚══════════════════════════════════════╝");
    auto t0 = std::chrono::high_resolution_clock::now();
    int rc = runDataFetcher(accountId, stratzToken);
    auto dt = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::high_resolution_clock::now() - t0);
    if (rc != 0) { LOG_ERR("DataFetcher завершился с ошибкой"); throw std::runtime_error("datafetcher failed"); }
    LOG_INFO("DataFetcher завершён за " << dt.count() << " сек");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    initConsole();
    ensureLogsDir();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // DPI awareness
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI* SPDA_t)(void*);
        if (auto fn = (SPDA_t)GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
            fn((void*)(intptr_t)-4);
    }

    CurlGlobal curlInit;

    // ── Параметры ─────────────────────────────────────────────────────────
    long long   accountId   = 0;
    std::string steamKey    = "F54FDF3461131271522AA4679FB980D7";
    std::string stratzToken;

    if (argc >= 2) { try { accountId = std::stoll(argv[1]); } catch (...) {} }
    if (!accountId) {
        std::cout << "account_id (32-bit Steam ID): ";
        std::string s; std::getline(std::cin, s);
        try { accountId = std::stoll(s); }
        catch (...) { LOG_ERR("Неверный account_id"); return 1; }
    }
    if (argc >= 3) steamKey    = argv[2];
    else if (const char* e = std::getenv("STEAM_API_KEY")) steamKey = e;
    if (argc >= 4) stratzToken = argv[3];
    else if (const char* e = std::getenv("STRATZ_API_KEY")) stratzToken = e;
    else stratzToken = DEFAULT_STRATZ_TOKEN;

    LOG_INFO("account_id=" << accountId);

    // ── БД ───────────────────────────────────────────────────────────────
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        LOG_ERR("Не удалось открыть БД: " << sqlite3_errmsg(db)); return 1;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;",   nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    createLivePicksIfNotExists(db);
    sqlite3_exec(db, "DELETE FROM livepicks;", nullptr, nullptr, nullptr);

    // ═══════════════════════════════════════════════════════════════════════
    // Фаза 1: DataFetcher
    // ═══════════════════════════════════════════════════════════════════════
    try { runDataFetcherPhase(accountId, stratzToken); }
    catch (const std::exception& e) { LOG_ERR(e.what()); return 1; }

    // ═══════════════════════════════════════════════════════════════════════
    // Фаза 2: GSI + Portrait + Picker
    // ═══════════════════════════════════════════════════════════════════════
    LOG_INFO("╔══════════════════════════════════════╗");
    LOG_INFO("║  Фаза 2: GSI + Portrait + Picker     ║");
    LOG_INFO("╚══════════════════════════════════════╝");
    LOG_INFO("Ожидаем матч...");

    GameInfo gameInfo;

    // GSI-сервер в фоне
    std::thread([&]{ runGsiServer(gameInfo, steamKey); }).detach();

    SharedPortraitState portraitState;

    // Состояние потоков
    std::atomic<bool> portraitRunning{false};
    std::atomic<bool> pickerRunning{false};
    std::thread portraitThread, pickerThread;

    std::string lastMatchId;

    // Для 30-секундного хвоста portrait после конца HERO_SELECTION
    using Clock = std::chrono::steady_clock;
    Clock::time_point heroSelEndTime;
    bool heroSelEndPending = false;  // ждём 30 сек после конца HERO_SELECTION

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // ── Читаем состояние ───────────────────────────────────────────────
        GamePhase   phase;
        std::string matchId;
        int         ourSide, ourSlot;
        bool        newMatch, isHeroSel;
        {
            std::lock_guard<std::mutex> lk(gameInfo.mtx);
            phase    = gameInfo.phase;
            matchId  = gameInfo.matchId;
            ourSide  = gameInfo.ourSide;
            ourSlot  = gameInfo.ourSlot;
            newMatch = gameInfo.newMatch;
            isHeroSel = gameInfo.isHeroSelection;
            gameInfo.newMatch = false;
        }

        bool isDraftOrIngame = (phase == GamePhase::DRAFT || phase == GamePhase::INGAME);

        // ── Новая игра ────────────────────────────────────────────────────
        if (newMatch && !matchId.empty()) {
            LOG_INFO("Новый матч: " << matchId
                     << "  side=" << (ourSide ? "radiant" : "dire")
                     << "  slot=" << ourSlot);

            // Стопаем всё
            if (pickerRunning.load()) {
                pickerRunning.store(false);
                if (pickerThread.joinable()) pickerThread.join();
            }
            if (portraitRunning.load()) {
                portraitRunning.store(false);
                if (portraitThread.joinable()) portraitThread.join();
                portraitState.clear();
            }
            heroSelEndPending = false;

            try {
                long long mid = std::stoll(matchId);
                initLivePicksRow(db, mid, accountId, ourSide, ourSlot);
            } catch (...) {}
            lastMatchId = matchId;
        }

        // ══════════════════════════════════════════════════════════════════
        // PICKER: запуск при входе в DRAFT/INGAME, стоп при IDLE/POSTGAME
        // ══════════════════════════════════════════════════════════════════
        if (isDraftOrIngame && !pickerRunning.load()) {
            LOG_INFO("Picker: старт (phase=" << phaseName(phase) << ")");
            pickerRunning.store(true);
            pickerThread = std::thread([&]{
                runPicker(MODEL_PATH, DB_PATH, pickerRunning, &portraitState);
                LOG_INFO("Picker остановлен");
            });
        }
        if (!isDraftOrIngame && pickerRunning.load()) {
            LOG_INFO("Picker: стоп (phase=" << phaseName(phase) << ")");
            pickerRunning.store(false);
            if (pickerThread.joinable()) pickerThread.join();
        }

        // ══════════════════════════════════════════════════════════════════
        // PORTRAIT: запуск при HERO_SELECTION, стоп через 30 сек после
        // ══════════════════════════════════════════════════════════════════

        if (isHeroSel && !portraitRunning.load()) {
            // Входим в HERO_SELECTION → запускаем portrait
            LOG_INFO("Portrait: старт (HERO_SELECTION)");
            heroSelEndPending = false;
            portraitRunning.store(true);
            portraitState.clear();
            {
                std::lock_guard<std::mutex> lk(portraitState.mtx);
                portraitState.active = true;
            }
            portraitThread = std::thread([&]{
                runPortraitCapture(gameInfo, DB_PATH, portraitRunning, portraitState);
                LOG_INFO("Portrait capture остановлен");
            });
        }

        if (!isHeroSel && portraitRunning.load()) {
            // Вышли из HERO_SELECTION — начинаем отсчёт 30 сек
            if (!heroSelEndPending) {
                heroSelEndPending = true;
                heroSelEndTime    = Clock::now();
                LOG_INFO("Portrait: HERO_SELECTION закончился, ещё "
                         << PORTRAIT_TAIL_SEC << " сек...");
            }
            // Прошло 30 сек → останавливаем
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                Clock::now() - heroSelEndTime).count();
            if (elapsed >= PORTRAIT_TAIL_SEC) {
                LOG_INFO("Portrait: стоп (хвост " << PORTRAIT_TAIL_SEC << " сек истёк)");
                portraitRunning.store(false);
                if (portraitThread.joinable()) portraitThread.join();
                portraitState.clear();
                heroSelEndPending = false;
            }
        }

        if (isHeroSel) heroSelEndPending = false; // сбрасываем если снова HERO_SELECTION
    }

    sqlite3_close(db);
    return 0;
}