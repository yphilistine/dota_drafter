#include "hero_meta_stats.h"

// --- STRATZ: живая статистика героев (heroStats, DIVINE_IMMORTAL) -------------

// Понедельник 00:00 UTC последней полностью завершившейся недели
long long lastCompletedWeekTimestamp() {
    long long now = static_cast<long long>(std::time(nullptr));
    constexpr long long DAY = 86400, WEEK = 7 * DAY;
    // 1 Jan 1970 - четверг, поэтому понедельник текущей недели:
    long long daysSinceEpoch  = now / DAY;
    long long weekdayFromMon  = (daysSinceEpoch + 3) % 7; // 0=понедельник
    long long mondayThisWeek  = (daysSinceEpoch - weekdayFromMon) * DAY;
    return mondayThisWeek - WEEK; // понедельник прошлой (полностью завершённой) недели
}

std::string buildHeroStatsQuery(long long week) {
    std::ostringstream q;
    q.imbue(std::locale::classic());
    q << "query liveq {\n";
    q << "  heroStats {\n";
    q << "    stats(\n";
    q << "      bracketBasicIds: [DIVINE_IMMORTAL]\n";
    q << "      groupByPosition: true\n";
    q << "      groupByBracket: true\n";
    q << "      week: " << week << "\n";
    q << "    ) {\n";
    q << "      heroId\n      position\n      matchCount\n      winCount\n";
    q << "    }\n";
    q << "  }\n";
    q << "}\n";
    return q.str();
}

std::string sendStratzHeroStats(const std::string& authToken, long long week) {
    std::string url = "https://api.stratz.com/graphql";
    json requestBody;
    requestBody["query"] = buildHeroStatsQuery(week);
    LOG_INFO("POST STRATZ heroStats: week=" << week);
    try {
        return httpPost(url, requestBody.dump(), authToken);
    } catch (const std::exception& e) {
        LOG_ERR("STRATZ heroStats exception: " << e.what());
        throw;
    }
}

std::vector<HeroWeekStat> parseHeroStatsResponse(const std::string& response) {
    if (response.size() >= 2 &&
        static_cast<unsigned char>(response[0]) == 0x1F &&
        static_cast<unsigned char>(response[1]) == 0x8B)
        throw std::runtime_error("STRATZ heroStats ответ в gzip, размер: "
            + std::to_string(response.size()) + " байт");

    std::string clean = sanitizeUtf8(response);
    auto j = json::parse(clean, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error(
        "parseHeroStatsResponse: не удалось разобрать JSON (" + std::to_string(clean.size()) + " байт)");
    if (j.contains("errors"))
        throw std::runtime_error("STRATZ heroStats вернул errors: " + j["errors"].dump());
    if (!j.contains("data") || !j["data"].contains("heroStats") || !j["data"]["heroStats"].contains("stats"))
        throw std::runtime_error("parseHeroStatsResponse: неожиданная структура ответа");

    std::vector<HeroWeekStat> rows;
    for (const auto& item : j["data"]["heroStats"]["stats"]) {
        if (!item.is_object()) continue;
        int pos = positionToInt(item.value("position", ""));
        if (pos == 0) continue;
        HeroWeekStat r;
        r.heroId     = item.value("heroId", 0LL);
        r.pos        = pos;
        r.matchCount = item.value("matchCount", 0LL);
        r.winCount   = item.value("winCount", 0LL);
        if (r.heroId > 0) rows.push_back(r);
    }
    return rows;
}

// --- SQLite: создание таблицы -------------------------------------------------
void createHeroStatsTableIfNotExists(sqlite3* db) {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS stats (
            hero_id INTEGER NOT NULL,
            pos     INTEGER NOT NULL,
            games   INTEGER NOT NULL DEFAULT 0,
            wins    INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (hero_id, pos)
        );
    )";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg); throw std::runtime_error("Ошибка создания таблицы stats: " + e); }
}

// --- SQLite: запись данных ----------------------------------------------------
void storeHeroStatsTable(sqlite3* db, const std::vector<HeroWeekStat>& rows) {
    {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db, "DELETE FROM stats;", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg); throw std::runtime_error("Ошибка очистки stats: " + e); }
    }
    const char* sql = "INSERT OR REPLACE INTO stats (hero_id, pos, games, wins) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) throw std::runtime_error("Ошибка подготовки запроса для stats: " + std::string(sqlite3_errmsg(db)));
    SqliteTransaction txn(db);
    for (const auto& row : rows) {
        sqlite3_bind_int64(stmt, 1, row.heroId);
        sqlite3_bind_int  (stmt, 2, row.pos);
        sqlite3_bind_int64(stmt, 3, row.matchCount);
        sqlite3_bind_int64(stmt, 4, row.winCount);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) { std::string e = sqlite3_errmsg(db); sqlite3_finalize(stmt); throw std::runtime_error("Ошибка вставки в stats: " + e); }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    txn.commit();
}

// --- OpenDota-фолбек (если STRATZ недоступен): без разбивки по позициям ------
// OpenDota heroStats не даёт срез по позициям, поэтому один и тот же
// агрегат Immortal (бакет "8_*") записывается во все 5 позиций.
std::string fetchHeroStatsOpenDotaFallback() {
    std::string url = "https://api.opendota.com/api/heroStats";
    LOG_INFO("GET OpenDota heroStats (фолбек): " << url);
    return httpGet(url);
}

std::vector<HeroWeekStat> parseHeroStatsOpenDotaFallback(const std::string& response) {
    auto j = json::parse(sanitizeUtf8(response), nullptr, false);
    if (j.is_discarded()) throw std::runtime_error(
        "parseHeroStatsOpenDotaFallback: не удалось разобрать JSON (" + std::to_string(response.size()) + " байт)");
    if (!j.is_array()) throw std::runtime_error(
        "parseHeroStatsOpenDotaFallback: ожидался массив, получен: " + std::string(j.type_name()));

    std::vector<HeroWeekStat> rows;
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        long long heroId = item.value("id", 0LL);
        if (heroId <= 0) continue;
        long long matches = item.value("8_pick", 0LL);
        long long wins    = item.value("8_win",  0LL);
        for (int pos = 1; pos <= 5; ++pos)
            rows.push_back({heroId, pos, matches, wins});
    }
    return rows;
}

// --- STRATZ heroStats (последняя завершённая неделя) → SQLite `stats` --------
void fetchAndStoreHeroStats(sqlite3* db, const std::string& authToken) {
    if (authToken.empty()) {
        LOG_WARN("HeroStats (meta): нет STRATZ токена, сразу фолбек на OpenDota");
    } else {
        try {
            long long week = lastCompletedWeekTimestamp();
            std::string response = sendStratzHeroStats(authToken, week);
            auto rows = parseHeroStatsResponse(response);
            createHeroStatsTableIfNotExists(db);
            storeHeroStatsTable(db, rows);
            LOG_INFO("SQLite: сохранено " << rows.size() << " строк -> stats (week=" << week << ")");
            return;
        } catch (const std::exception& e) {
            LOG_ERR("HeroStats (meta) fetch failed: " << e.what());
        }
    }

    try {
        std::string response = fetchHeroStatsOpenDotaFallback();
        auto rows = parseHeroStatsOpenDotaFallback(response);
        createHeroStatsTableIfNotExists(db);
        storeHeroStatsTable(db, rows);
        LOG_WARN("STRATZ недоступен, использован фолбек OpenDota: сохранено " << rows.size() << " строк -> stats");
    } catch (const std::exception& e) {
        LOG_ERR("HeroStats OpenDota fallback failed: " << e.what());
    }
}
