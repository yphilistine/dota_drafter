/*
 * orchestrator.cpp — фоновый оркестратор: управление portrait + picker
 * потоками, GSI-сервер, livepicks/player_info, запуск фазы 1b.
 *
 * Вынесено из mainGUI.cpp (была orchestratorMain, ~315 строк — самая длинная
 * функция в проекте). Цикл раньше не имел ни одного top-level try/catch —
 * необработанное исключение в этом потоке привело бы к std::terminate()
 * и падению всего процесса. Теперь: внешний try/catch вокруг открытия БД
 * (fatal, как и раньше) + внутренний try/catch на каждую итерацию цикла
 * (логирует и продолжает, не убивая поток).
 */

#include "orchestrator.h"
#include "app_state.h"
#include "gui_draw.h"
#include "portrait_runner.h"
#include "common.h"

#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <cstring>
#include <cstdio>

// ─── Управление потоками (было глобальным в mainGUI.cpp; наружу — только
// функции startOrchestrator/stopOrchestrator/startPhase1/requestPositionRefresh) ─
static std::atomic<bool> g_pickerRunning{false};
static std::atomic<bool> g_portraitRunning{false};
static std::atomic<bool> g_orchestratorRunning{false};
static std::atomic<bool> g_posRefreshNeeded{false};
static std::thread       g_pickerThread;
static std::thread       g_portraitThread;
static std::thread       g_orchestratorThread;

void requestPositionRefresh() {
    g_posRefreshNeeded.store(true);
}

// ─── SQLite: таблица player_info ──────────────────────────────────────────────
void createPlayerInfoTable(sqlite3* db) {
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS player_info ("
        "  account_id  INTEGER PRIMARY KEY,"
        "  player_name TEXT DEFAULT ''"
        ");",
        nullptr, nullptr, nullptr);
}

long long loadSavedPlayer(sqlite3* db, char nameOut[128]) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT account_id, player_name FROM player_info LIMIT 1",
            -1, &st, nullptr) != SQLITE_OK)
        return 0;
    long long id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
        const char* n = (const char*)sqlite3_column_text(st, 1);
        if (n && nameOut) std::snprintf(nameOut, 128, "%s", n);
    }
    sqlite3_finalize(st);
    return id;
}

static void savePlayerInfo(sqlite3* db, long long accountId, const char* name) {
    sqlite3_exec(db, "DELETE FROM player_info;", nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO player_info (account_id, player_name) VALUES (?, ?)",
            -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, accountId);
    sqlite3_bind_text(st,  2, name ? name : "", -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// ─── Таблица livepicks ────────────────────────────────────────────────────────
static void createLivePicksIfNotExists(sqlite3* db) {
    sqlite3_exec(db, R"(
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
    )", nullptr, nullptr, nullptr);
}

static void initLivePicksRow(sqlite3* db, long long matchId,
                              long long accountId, int ourSide, int ourSlot) {
    sqlite3_exec(db, "DELETE FROM livepicks;", nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO livepicks "
            "(match_id,our_account_id,our_side,our_slot,updated_at) "
            "VALUES (?,?,?,?,?);",
            -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, matchId);
    sqlite3_bind_int64(st, 2, accountId);
    sqlite3_bind_int(st,   3, ourSide);
    sqlite3_bind_int(st,   4, ourSlot);
    sqlite3_bind_int64(st, 5,
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// ─── OpenDota: получение имени и аватара игрока (фоновый поток) ───────────────
static void fetchOpenDotaProfile(long long accountId) {
    std::string url = "https://api.opendota.com/api/players/"
                    + std::to_string(accountId);
    std::string resp;
    try { resp = httpGet(url); } catch (...) { return; }
    if (resp.empty()) return;

    auto j = json::parse(resp, nullptr, false);
    if (j.is_discarded() || !j.contains("profile") || !j["profile"].is_object())
        return;

    auto& profile = j["profile"];
    std::string name      = sanitizeUtf8(profile.value("personaname", ""));
    std::string avatarUrl = profile.value("avatarmedium", "");

    if (!name.empty()) {
        sqlite3* db = nullptr;
        if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
            createPlayerInfoTable(db);
            savePlayerInfo(db, accountId, name.c_str());
            sqlite3_close(db);
        }
        std::lock_guard<std::mutex> lk(g_player.mtx);
        std::snprintf(g_player.name, sizeof(g_player.name), "%s", name.c_str());
    }

    if (!avatarUrl.empty()) {
        try {
            std::string imgData = httpGet(avatarUrl);
            if (!imgData.empty()) {
                std::lock_guard<std::mutex> lk(g_player.mtx);
                g_player.avatarData.assign(imgData.begin(), imgData.end());
                g_player.avatarDataReady = true;
            }
        } catch (...) {}
    }
}

// ─── Запуск фазы 1 (DataFetcher + имя игрока) ────────────────────────────────
void startPhase1(long long accountId) {
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        g_player.phase1Running = true;
        g_player.phase1Done    = false;
        g_player.phase1Error   = false;
        std::snprintf(g_player.phase1Msg, sizeof(g_player.phase1Msg),
                      "Fetching data...");
    }

    // Сброс аватара (GUI-поток, безопасно — startPhase1 вызывается из GUI)
    if (g_avatarSRV) { g_avatarSRV->Release(); g_avatarSRV = nullptr; }

    std::thread([accountId]() {
        // Имя + аватар из OpenDota (~1с)
        fetchOpenDotaProfile(accountId);

        // Основная загрузка данных (~30-90с)
        int rc = runDataFetcher(accountId, g_stratzToken);

        std::lock_guard<std::mutex> lk(g_player.mtx);
        g_player.phase1Running = false;
        if (rc == 0) {
            g_player.phase1Done = true;
            std::snprintf(g_player.phase1Msg, sizeof(g_player.phase1Msg),
                          "Data ready");
        } else {
            g_player.phase1Error = true;
            std::snprintf(g_player.phase1Msg, sizeof(g_player.phase1Msg),
                          "Fetch error");
        }
    }).detach();
}

// ─── Состояние оркестратора (было function-local static/локальными переменными) ─
struct OrchestratorLoopState {
    long long   accountId       = 0;
    std::string lastMatchId;
    std::chrono::steady_clock::time_point phase3EndTime;
    bool        phase3EndPending = false;
    int         prevOurSlot      = -1;
    int         prevOurSide      = -1;
    bool        oneShotActive    = false;
    int         oneShotGen       = 0;
};

// Снимок состояния GSI на одну итерацию цикла.
struct GsiSnapshot {
    GamePhase   phase     = GamePhase::IDLE;
    std::string matchId;
    int         ourSide   = 1;
    int         ourSlot   = 1;
    bool        newMatch  = false;
    bool        isHeroSel = false;
};

static void refreshAccountId(OrchestratorLoopState& st) {
    std::lock_guard<std::mutex> lk(g_player.mtx);
    st.accountId = g_player.accountId;
}

// GSI-таймаут: если Dota 2 закрыта, GSI перестаёт слать данные — сброс на
// IDLE через 15 секунд без обновлений.
static GsiSnapshot readGameStateWithTimeoutWatchdog() {
    GsiSnapshot gs;
    std::lock_guard<std::mutex> lk(g_gameInfo.mtx);

    if (g_gameInfo.phase != GamePhase::IDLE &&
        g_gameInfo.lastUpdate.time_since_epoch().count() > 0)
    {
        auto elapsed = std::chrono::steady_clock::now() - g_gameInfo.lastUpdate;
        if (elapsed > std::chrono::seconds(15)) {
            g_gameInfo.phase           = GamePhase::IDLE;
            g_gameInfo.isHeroSelection = false;
            g_gameInfo.matchId.clear();
            LOG_INFO("[orchestrator] GSI timeout — reset to IDLE");
        }
    }

    gs.phase     = g_gameInfo.phase;
    gs.matchId   = g_gameInfo.matchId;
    gs.ourSide   = g_gameInfo.ourSide;
    gs.ourSlot   = g_gameInfo.ourSlot;
    gs.newMatch  = g_gameInfo.newMatch;
    gs.isHeroSel = g_gameInfo.isHeroSelection;
    g_gameInfo.newMatch = false;
    return gs;
}

// Слот/сторона изменились (GSI может прислать позже чем newMatch).
static void syncSlotSideToDb(sqlite3* db, OrchestratorLoopState& st, const GsiSnapshot& gs) {
    if ((gs.ourSlot == st.prevOurSlot && gs.ourSide == st.prevOurSide) || gs.matchId.empty())
        return;
    const char* slotSql =
        "UPDATE livepicks SET our_slot=?, our_side=?, updated_at=? WHERE 1;";
    sqlite3_stmt* ss = nullptr;
    if (sqlite3_prepare_v2(db, slotSql, -1, &ss, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(ss, 1, gs.ourSlot);
        sqlite3_bind_int(ss, 2, gs.ourSide);
        sqlite3_bind_int64(ss, 3,
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        sqlite3_step(ss);
        sqlite3_finalize(ss);
    }
    st.prevOurSlot = gs.ourSlot;
    st.prevOurSide = gs.ourSide;
}

// ID сменился — сбросить фазу 3 (кроме pickerState, драфт остаётся видимым)
// и обновить только our_account_id в livepicks.
static void handleAccountIdChanged(sqlite3* db, OrchestratorLoopState& st, const GsiSnapshot& gs) {
    bool idChanged = false;
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        if (g_player.idChanged) {
            idChanged = true;
            g_player.idChanged = false;
            st.accountId = g_player.accountId;
        }
    }
    if (!idChanged) return;

    // Пикер зависит от accountId — перезапускаем
    if (g_pickerRunning.load()) {
        g_pickerRunning.store(false);
        if (g_pickerThread.joinable()) g_pickerThread.join();
    }

    if (!gs.matchId.empty()) {
        sqlite3_exec(db,
            ("UPDATE livepicks SET our_account_id="
             + std::to_string(st.accountId) + ";").c_str(),
            nullptr, nullptr, nullptr);
        st.lastMatchId = gs.matchId;
    }

    // Запустить пикер если есть accountId и идёт драфт
    if (gs.phase == GamePhase::DRAFT && st.accountId != 0 && !g_pickerRunning.load()) {
        g_pickerRunning.store(true);
        g_pickerThread = std::thread([]{
            runPickerGui(MODEL_PATH, DB_PATH,
                         g_pickerRunning, &g_pickerState, &g_portraitState);
        });
    }
}

// Новый матч: остановить потоки, сбросить состояние, пересоздать livepicks.
static void handleNewMatch(sqlite3* db, OrchestratorLoopState& st, const GsiSnapshot& gs) {
    if (!gs.newMatch || gs.matchId.empty()) return;

    if (g_pickerRunning.load()) {
        g_pickerRunning.store(false);
        if (g_pickerThread.joinable()) g_pickerThread.join();
    }
    if (g_portraitRunning.load()) {
        g_portraitRunning.store(false);
        if (g_portraitThread.joinable()) g_portraitThread.join();
        g_portraitState.clear();
    }
    g_pickerState.reset();
    st.phase3EndPending = false;

    // safeStoll вместо голого stoll: раньше некорректный matchId молча
    // проглатывался тем же catch(...){} — поведение сохранено 1:1, но без
    // риска, что кто-то уберёт try/catch при будущей правке и получит
    // непойманное исключение в этом потоке (нет top-level сетки была/есть
    // только внешняя, вокруг всей функции — см. orchestratorMain).
    long long mid = safeStoll(gs.matchId, -1);
    if (mid >= 0) initLivePicksRow(db, mid, st.accountId, gs.ourSide, gs.ourSlot);
    st.lastMatchId = gs.matchId;
}

// Фаза 3: портреты + пикер. 3-ветка: IDLE/POSTGAME (тир-даун) / HERO_SELECTION
// (запуск) / хвост PHASE3_TAIL_SEC после конца HERO_SELECTION.
static void runPhaseStateMachine(OrchestratorLoopState& st, const GsiSnapshot& gs) {
    if (gs.phase == GamePhase::IDLE || gs.phase == GamePhase::POSTGAME) {
        if (g_pickerRunning.load()) {
            g_pickerRunning.store(false);
            if (g_pickerThread.joinable()) g_pickerThread.join();
        }
        if (g_portraitRunning.load()) {
            g_portraitRunning.store(false);
            if (g_portraitThread.joinable()) g_portraitThread.join();
            g_portraitState.clear();
        }
        st.phase3EndPending = false;
        if (g_pickerState.gameStarted) {
            g_pickerState.reset();
        }

    } else if (gs.isHeroSel) {
        st.phase3EndPending = false;

        // Portrait capture — всегда при HERO_SELECTION (заполняет livepicks)
        if (!g_portraitRunning.load()) {
            g_portraitRunning.store(true);
            g_portraitState.clear();
            {
                std::lock_guard<std::mutex> lk(g_portraitState.mtx);
                g_portraitState.active = true;
            }
            g_portraitThread = std::thread([]{
                runPortraitCapture(g_gameInfo, DB_PATH,
                                   g_portraitRunning, g_portraitState);
            });
        }

        // Picker — только при наличии accountId
        if (!g_pickerRunning.load() && st.accountId != 0) {
            g_pickerRunning.store(true);
            g_pickerThread = std::thread([]{
                runPickerGui(MODEL_PATH, DB_PATH,
                             g_pickerRunning, &g_pickerState, &g_portraitState);
            });
        }

    } else {
        // HERO_SELECTION кончился, но игра продолжается — отсчитываем хвост
        if (g_pickerRunning.load() || g_portraitRunning.load()) {
            if (!st.phase3EndPending) {
                st.phase3EndPending = true;
                st.phase3EndTime    = std::chrono::steady_clock::now();
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - st.phase3EndTime).count();
            if (elapsed >= PHASE3_TAIL_SEC) {
                g_pickerRunning.store(false);
                if (g_pickerThread.joinable()) g_pickerThread.join();
                g_portraitRunning.store(false);
                if (g_portraitThread.joinable()) g_portraitThread.join();
                g_portraitState.clear();
                st.phase3EndPending = false;
            }
        }
    }
}

// Portrait → GUI: показываем драфт без пикера (пикер ещё не запущен/остановлен,
// но portrait уже заполняет слоты).
static void syncPortraitOnlyToGui(const GsiSnapshot& gs) {
    if (!g_portraitRunning.load() || g_pickerRunning.load()) return;

    PortraitResult snap[10];
    int posSnap[10];
    {
        std::lock_guard<std::mutex> lk(g_portraitState.mtx);
        for (int i = 0; i < 10; ++i) snap[i]    = g_portraitState.slots[i];
        for (int i = 0; i < 10; ++i) posSnap[i] = g_portraitState.detectedPos[i];
    }
    std::lock_guard<std::mutex> lk(g_pickerState.mtx);
    g_pickerState.gameStarted = true;
    g_pickerState.isRadiant   = (gs.ourSide == 1);
    g_pickerState.ourSlot     = gs.ourSlot;
    for (int i = 0; i < 5; ++i) {
        HeroSlotGui& s = g_pickerState.radiant[i];
        s.heroId = snap[i].heroId;
        s.filled = snap[i].valid();
        s.isYou  = (gs.ourSide == 1 && i == gs.ourSlot - 1);
        s.pos    = posSnap[i];
        if (s.filled) std::snprintf(s.name, sizeof(s.name), "%s",
            heroDisplayName(snap[i].heroId, snap[i].heroName.c_str()).c_str());
        else s.name[0] = '\0';
    }
    for (int i = 0; i < 5; ++i) {
        HeroSlotGui& s = g_pickerState.dire[i];
        s.heroId = snap[5+i].heroId;
        s.filled = snap[5+i].valid();
        s.isYou  = (gs.ourSide == 0 && i == gs.ourSlot - 1);
        s.pos    = posSnap[5+i];
        if (s.filled) std::snprintf(s.name, sizeof(s.name), "%s",
            heroDisplayName(snap[5+i].heroId, snap[5+i].heroName.c_str()).c_str());
        else s.name[0] = '\0';
    }
    g_pickerState.recCount      = 0;
    g_pickerState.ourHeroPicked = false;
    g_pickerState.winProb       = 0.f;
}

// One-shot: ручная смена позиции вне фазы 3 (клик по бейджу в GUI) → запись
// manualPos в livepicks + одноразовый запуск пикера до первого inferenceGen.
static void runOneShotRefresh(sqlite3* db, OrchestratorLoopState& st) {
    if (g_posRefreshNeeded.exchange(false)
        && !g_pickerRunning.load()
        && g_pickerState.gameStarted
        && st.accountId != 0)
    {
        int side = 0;
        {
            std::lock_guard<std::mutex> lk(g_gameInfo.mtx);
            side = g_gameInfo.ourSide;
        }
        int base = (side == 1) ? 0 : 5;
        for (int i = 0; i < 5; ++i) {
            int slot = base + i;
            int mp = 0;
            {
                std::lock_guard<std::mutex> lk(g_portraitState.mtx);
                mp = g_portraitState.manualPos[slot];
            }
            if (mp > 0 && mp <= 5) {
                char col[16];
                if (slot < 5) std::snprintf(col, sizeof(col), "r%d_pos", slot + 1);
                else          std::snprintf(col, sizeof(col), "d%d_pos", slot - 4);
                char sql[128];
                std::snprintf(sql, sizeof(sql),
                    "UPDATE livepicks SET %s=%d, updated_at=%lld;",
                    col, mp, (long long)std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
            }
        }

        st.oneShotGen = g_pickerState.inferenceGen.load(std::memory_order_acquire);
        g_pickerRunning.store(true);
        if (g_pickerThread.joinable()) g_pickerThread.join();
        g_pickerThread = std::thread([]{
            runPickerGui(MODEL_PATH, DB_PATH,
                         g_pickerRunning, &g_pickerState, &g_portraitState);
        });
        st.oneShotActive = true;
    }

    if (st.oneShotActive &&
        g_pickerState.inferenceGen.load(std::memory_order_acquire) > st.oneShotGen)
    {
        g_pickerRunning.store(false);
        if (g_pickerThread.joinable()) g_pickerThread.join();
        st.oneShotActive = false;
    }
}

static void orchestratorMain() {
    OrchestratorLoopState st;
    { std::lock_guard<std::mutex> lk(g_player.mtx); st.accountId = g_player.accountId; }

    try {
        SqliteDB dbWrap(DB_PATH);
        sqlite3* db = dbWrap.get();
        createLivePicksIfNotExists(db);
        sqlite3_exec(db, "DELETE FROM livepicks;", nullptr, nullptr, nullptr);

        // [D] overlay кнопка — запускается один раз, видна всегда, пока открыто окно Dota 2
        startDotaOverlay();

        // GSI-сервер (всегда)
        std::thread([]{ runGsiServer(g_gameInfo); }).detach();

        while (g_orchestratorRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            try {
                refreshAccountId(st);
                GsiSnapshot gs = readGameStateWithTimeoutWatchdog();
                syncSlotSideToDb(db, st, gs);
                handleAccountIdChanged(db, st, gs);
                handleNewMatch(db, st, gs);
                runPhaseStateMachine(st, gs);
                syncPortraitOnlyToGui(gs);
                runOneShotRefresh(db, st);
            } catch (const std::exception& e) {
                // На итерацию, а не вокруг всей функции: раньше здесь не было
                // ни одного top-level try/catch, необработанное исключение
                // прибило бы весь процесс (std::terminate из std::thread).
                LOG_ERR("[orchestrator] iteration exception: " << e.what());
            } catch (...) {
                LOG_ERR("[orchestrator] unknown iteration exception");
            }
        }

        g_pickerRunning.store(false);
        g_portraitRunning.store(false);
        if (g_pickerThread.joinable())   g_pickerThread.join();
        if (g_portraitThread.joinable()) g_portraitThread.join();

    } catch (const std::exception& e) {
        LOG_ERR("Orchestrator: cannot open DB: " << e.what());
        g_appNotice.set(NoticeLevel::Error, "Could not open local database — restart the app.");
    }
}

void startOrchestrator() {
    g_orchestratorRunning.store(true);
    g_orchestratorThread = std::thread(orchestratorMain);
}

void stopOrchestrator() {
    g_orchestratorRunning.store(false);
    if (g_orchestratorThread.joinable()) g_orchestratorThread.join();
}
