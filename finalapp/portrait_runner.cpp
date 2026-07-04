/*
 * portrait_runner.cpp — захват портретов + позиций.
 *
 * Портреты: захват каждые 500мс → распознавание героев и позиций → запись в livepicks.
 * Позиции: своя команда = manualPos override > screen capture > 0. Враг = 0.
 * Работает без accountId. Overlay-кнопка [D] вынесена в overlay_button.cpp.
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
#include "pos_ocr.h"
#include "common.h"   // LOG_INFO/WARN/ERR — раньше диагностика шла в printf/puts,
                       // невидимые в GUI-приложении без консоли (см. mainGUI.cpp)

#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sqlite3.h>

using namespace dota2;

// ─────────────────────────────────────────────────────────────────────────────
// Загрузка hero_hashes.dat (бинарный формат: uint32 count, затем записи)
// ─────────────────────────────────────────────────────────────────────────────

struct HeroHashFile {
    std::vector<std::string>          names;
    std::vector<dota2::HeroHashEntry> entries;
    bool loaded = false;
};

static HeroHashFile loadHeroHashes(const char* path) {
    HeroHashFile result;
    std::ifstream f(path, std::ios::binary);
    if (!f) return result;

    uint32_t count = 0;
    f.read(reinterpret_cast<char*>(&count), 4);
    if (!f || count > 10000) return result;

    result.names.resize(count);
    result.entries.resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t nameLen = 0;
        f.read(reinterpret_cast<char*>(&nameLen), 2);
        if (!f) return result;

        result.names[i].resize(nameLen);
        f.read(result.names[i].data(), nameLen);
        f.read(reinterpret_cast<char*>(result.entries[i].mat.v), 64 * sizeof(float));
        if (!f) return result;

        result.entries[i].name = result.names[i].c_str();
    }
    result.loaded = true;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Утилиты
// ─────────────────────────────────────────────────────────────────────────────

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Каноническая форма имени героя: нижний регистр, без '_'/'-', без префикса
// npc_dota_hero_. Используется как ключ сопоставления вместо localized_name,
// т.к. Valve иногда временно/некорректно меняет localized_name (напр. видели
// в БД "Axe?" вместо "Axe") — это ломает распознавание. heroes.name стабилен.
static std::string canonicalHeroKey(const std::string& raw) {
    static const std::string kPrefix = "npc_dota_hero_";
    std::string s = raw;
    if (s.size() >= kPrefix.size() && s.compare(0, kPrefix.size(), kPrefix) == 0)
        s = s.substr(kPrefix.size());
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_' || c == '-') continue;
        out += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

static std::map<std::string, int> loadHeroNameToId(sqlite3* db) {
    std::map<std::string, int> m;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, name FROM heroes",
                           -1, &st, nullptr) != SQLITE_OK) return m;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int id = sqlite3_column_int(st, 0);
        const char* raw = (const char*)sqlite3_column_text(st, 1);
        if (!raw) continue;
        m[canonicalHeroKey(raw)] = id;
    }
    sqlite3_finalize(st);
    return m;
}

static int lookupHeroId(const std::map<std::string, int>& nameMap, const char* rawName) {
    if (!rawName) return 0;
    auto it = nameMap.find(canonicalHeroKey(rawName));
    return it != nameMap.end() ? it->second : 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// livepicks
// ─────────────────────────────────────────────────────────────────────────────

static void updateSlot(sqlite3* db, int slot, int heroId) {
    char col[16];
    if (slot < 5) std::snprintf(col, sizeof(col), "r%d_hero", slot + 1);
    else          std::snprintf(col, sizeof(col), "d%d_hero", slot - 4);
    char sql[128];
    std::snprintf(sql, sizeof(sql),
        "UPDATE livepicks SET %s=?, updated_at=?;", col);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(st,   1, heroId);
    sqlite3_bind_int64(st, 2, nowMs());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void updateSlotPos(sqlite3* db, int slot, int pos) {
    char col[16];
    if (slot < 5) std::snprintf(col, sizeof(col), "r%d_pos", slot + 1);
    else          std::snprintf(col, sizeof(col), "d%d_pos", slot - 4);
    char sql[128];
    std::snprintf(sql, sizeof(sql),
        "UPDATE livepicks SET %s=?, updated_at=?;", col);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(st,   1, pos);
    sqlite3_bind_int64(st, 2, nowMs());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void clearHeroSlots(sqlite3* db) {
    sqlite3_exec(db,
        "UPDATE livepicks SET "
        "r1_hero=0,r1_pos=0,r2_hero=0,r2_pos=0,r3_hero=0,r3_pos=0,"
        "r4_hero=0,r4_pos=0,r5_hero=0,r5_pos=0,"
        "d1_hero=0,d1_pos=0,d2_hero=0,d2_pos=0,d3_hero=0,d3_pos=0,"
        "d4_hero=0,d4_pos=0,d5_hero=0,d5_pos=0,"
        "updated_at=0;",
        nullptr, nullptr, nullptr);
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

// ─── Главный цикл захвата портретов ──────────────────────────────────────────

void runPortraitCapture(GameInfo&           gameInfo,
                        const std::string&  dbPath,
                        std::atomic<bool>&  running,
                        SharedPortraitState& out)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        LOG_ERR("[portrait] Не удалось открыть БД");
        Gdiplus::GdiplusShutdown(gdipToken);
        // winrt::init_apartment() выше требует парного uninit_apartment() —
        // раньше пропускался именно на этой ветке ошибки (в отличие от
        // нормального пути через cleanup: ниже).
        winrt::uninit_apartment();
        return;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;",   nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    auto nameToId = loadHeroNameToId(db);
    LOG_INFO("[portrait] Справочник героев: " << nameToId.size() << " записей");

    auto hashFile = loadHeroHashes("hero_hashes.dat");
    HeroRecognizer recognizer(nullptr, 0);
    if (hashFile.loaded) {
        recognizer = HeroRecognizer(hashFile.entries.data(), hashFile.entries.size());
        LOG_INFO("[portrait] Hash-база: " << recognizer.size() << " героев");
    } else {
        LOG_WARN("[portrait] ВНИМАНИЕ: hero_hashes.dat не найден");
    }

    PosOcrRecognizer posRecognizer;
    if (!posRecognizer.isAvailable())
        LOG_WARN("[portrait] ВНИМАНИЕ: Windows OCR недоступен, позиции только вручную");

    // Отдельный Dota2Capture для захвата (overlay запущен из оркестратора)
    Dota2Capture cap("");

    // Ждём Dota 2
    if (!cap.findGameWindow()) {
        LOG_INFO("[portrait] Dota 2 не найдена, ожидаем...");
        while (running.load() && !cap.findGameWindow())
            std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!running.load()) goto cleanup;

    {
        auto res = cap.gameResolution();
        LOG_INFO("[portrait] Окно Dota 2: " << res.width << "x" << res.height);
        clearHeroSlots(db);

        int   lastHeroId[10] = {};
        float lastScore[10]  = {};    // хранимая уверенность для каждого слота
        LOG_INFO("[portrait] Захват каждые 500 мс (HERO_SELECTION)...");

        while (running.load()) {
          try {
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
                LOG_INFO("[portrait] Новая игра — слоты сброшены");
            }

            if (!cap.isWindowFound() || !IsWindow(cap.gameWindowHandle())) {
                cap.findGameWindow();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            cap.refreshResolution();

            int captured = cap.capturePortraits();
            if (captured <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            const auto& portraits = cap.portraits();

            for (int slot = 0; slot < 10 && slot < (int)portraits.size(); ++slot) {
                const Bitmap& bmp = portraits[slot];
                if (bmp.empty()) continue;

                HeroMatch m = recognizer.recognize(bmp);
                if (!m.name || m.score < 0.5f) continue;

                bool isNullHero  = (std::strcmp(m.name, "null") == 0);
                int  detectedId  = isNullHero ? 0 : lookupHeroId(nameToId, m.name);

                if (!isNullHero && detectedId > 0) {
                    std::lock_guard<std::mutex> lk(out.mtx);
                    out.slots[slot].heroName = m.name;
                    out.slots[slot].heroId   = detectedId;
                    out.slots[slot].score    = m.score;
                }

                if (detectedId != lastHeroId[slot]) {
                    bool slotEmpty   = (lastHeroId[slot] == 0);
                    bool shouldUpdate = slotEmpty
                        ? (detectedId > 0)
                        : (m.score > lastScore[slot]);

                    if (shouldUpdate) {
                        if (detectedId == 0) {
                            clearHeroSlot(db, slot);
                            {
                                std::lock_guard<std::mutex> lk(out.mtx);
                                out.slots[slot] = {};
                            }
                            const char* team = (slot < 5) ? "Radiant" : "Dire";
                            int idx = (slot < 5) ? slot+1 : slot-4;
                            LOG_INFO("[portrait] " << team << " #" << idx
                                     << " → NULL  score=" << std::fixed << std::setprecision(3) << m.score);
                        } else {
                            updateSlot(db, slot, detectedId);
                            const char* team = (slot < 5) ? "Radiant" : "Dire";
                            int idx = (slot < 5) ? slot+1 : slot-4;
                            LOG_INFO("[portrait] " << team << " #" << idx << " → "
                                     << std::left << std::setw(22) << m.name
                                     << "  score=" << std::fixed << std::setprecision(3) << m.score);
                        }
                        lastHeroId[slot] = detectedId;
                        lastScore[slot]  = m.score;
                    }
                } else if (m.score > lastScore[slot]) {
                    lastScore[slot] = m.score;
                }
            }

            // ── Распознавание позиций (только своя команда) ──────────────
            {
                int ourSide = 0;
                {
                    std::lock_guard<std::mutex> lk(gameInfo.mtx);
                    ourSide = gameInfo.ourSide; // 1 = Radiant, 0 = Dire
                }
                int slotStart = (ourSide == 1) ? 0 : 5;
                int slotEnd   = slotStart + 5;

                const auto& posBitmaps = cap.posPortraits();

                for (int slot = slotStart; slot < slotEnd && slot < (int)posBitmaps.size(); ++slot) {
                    // GUI override имеет приоритет
                    int manual = 0;
                    {
                        std::lock_guard<std::mutex> lk(out.mtx);
                        manual = out.manualPos[slot];
                    }
                    if (manual > 0 && manual <= 5) {
                        updateSlotPos(db, slot, manual);
                        std::lock_guard<std::mutex> lk(out.mtx);
                        out.detectedPos[slot] = manual;
                        continue;
                    }

                    const Bitmap& pbmp = posBitmaps[slot];
                    if (pbmp.empty()) continue;
                    PosMatch pm = posRecognizer.recognize(pbmp);
                    int pos = pm.confident() ? pm.pos : 0;
                    updateSlotPos(db, slot, pos);
                    {
                        std::lock_guard<std::mutex> lk(out.mtx);
                        out.detectedPos[slot] = pos;
                    }
                }

                // Вражеская команда — позиции всегда 0
                int enemyStart = (ourSide == 1) ? 5 : 0;
                for (int slot = enemyStart; slot < enemyStart + 5; ++slot) {
                    updateSlotPos(db, slot, 0);
                    std::lock_guard<std::mutex> lk(out.mtx);
                    out.detectedPos[slot] = 0;
                }
            }

            auto elapsed   = std::chrono::steady_clock::now() - frameStart;
            auto remaining = std::chrono::milliseconds(500) - elapsed;
            if (remaining > std::chrono::milliseconds(0))
                std::this_thread::sleep_for(remaining);
          } catch (const std::exception& e) {
            // На итерацию, а не вокруг всей функции: раньше вся runPortraitCapture
            // не имела ни одного try/catch, и исключение из распознавания/захвата
            // кадра прибило бы поток без сетки (detach из orchestratorMain).
            LOG_ERR("[portrait] capture iteration exception: " << e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
          } catch (...) {
            LOG_ERR("[portrait] capture iteration unknown exception");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
          }
        }
    }

cleanup:
    // overlay живёт всё время приложения — не останавливаем
    sqlite3_close(db);
    Gdiplus::GdiplusShutdown(gdipToken);
    winrt::uninit_apartment();
    LOG_INFO("[portrait] Захват остановлен");
}
