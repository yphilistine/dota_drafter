#include "playerdatafetcher.h"

std::string fetchHeroesList() {
    std::string url = "https://api.opendota.com/api/heroes";
    LOG_INFO("GET heroes: " << url);
    return httpGet(url);
}

std::vector<HeroInfo> parseHeroesList(const std::string& jsonStr) {
    auto j = json::parse(jsonStr, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error(
        "parseHeroesList: не удалось разобрать JSON (" + std::to_string(jsonStr.size()) + " байт)");
    if (!j.is_array()) throw std::runtime_error(
        "parseHeroesList: ожидался массив, получен: " + std::string(j.type_name()));
    std::vector<HeroInfo> heroes;
    heroes.reserve(j.size());
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        HeroInfo info;
        info.id             = item.value("id", 0LL);
        info.name           = item.value("name", "");
        info.localized_name = item.value("localized_name", "");
        heroes.push_back(std::move(info));
    }
    return heroes;
}

std::string fetchPlayerHeroesStats(const std::string& accountId) {
    std::string url = "https://api.opendota.com/api/players/" + accountId + "/heroes";
    LOG_INFO("GET player heroes (all): " << url);
    return httpGet(url);
}

std::string fetchPlayerHeroesRankedStats(const std::string& accountId) {
    std::string url = "https://api.opendota.com/api/players/" + accountId + "/heroes?lobby_type=7";
    LOG_INFO("GET player heroes (ranked): " << url);
    return httpGet(url);
}

std::vector<HeroStats> parseHeroesStats(const std::string& jsonStr) {
    auto j = json::parse(jsonStr, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error(
        "parseHeroesStats: не удалось разобрать JSON (" + std::to_string(jsonStr.size()) + " байт)");
    if (!j.is_array()) throw std::runtime_error(
        "parseHeroesStats: ожидался массив, получен: " + std::string(j.type_name()));
    std::vector<HeroStats> heroes;
    heroes.reserve(j.size());
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        HeroStats stats;
        stats.hero_id = item.value("hero_id", 0LL);
        stats.games   = item.value("games",   0LL);
        stats.wins    = item.value("win",      0LL);
        heroes.push_back(stats);
    }
    return heroes;
}

std::vector<long long> fetchRecentMatchIds(long long accountId) {
    std::string url = "https://api.opendota.com/api/players/"
                    + std::to_string(accountId)
                    + "/matches?lobby_type=7&date=90";
    LOG_INFO("GET match_id (90 days): " << url);
    std::string response = httpGet(url);
    auto j = json::parse(response, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error(
        "fetchRecentMatchIds: не удалось разобрать JSON (" + std::to_string(response.size()) + " байт)");
    if (!j.is_array()) throw std::runtime_error(
        "fetchRecentMatchIds: ожидался массив, получен: " + std::string(j.type_name()));
    std::vector<long long> ids;
    ids.reserve(j.size());
    for (const auto& item : j) {
        long long mid = item.value("match_id", 0LL);
        if (mid) ids.push_back(mid);
    }
    LOG_INFO("Получено match_id: " << ids.size());
    return ids;
}

std::string buildMatchesBatchQuery(const std::vector<long long>& matchIds) {
    std::ostringstream q;
    q.imbue(std::locale::classic());
    q << "query MatchesBatch {\n";
    for (long long mid : matchIds) {
        q << "  m" << mid << ": match(id: " << mid << ") {\n";
        q << "    didRadiantWin\n    actualRank\n";
        q << "    players {\n      steamAccountId\n      playerSlot\n      heroId\n      position\n    }\n";
        q << "    pickBans {\n      isPick\n      heroId\n      playerIndex\n      order\n    }\n";
        q << "  }\n";
    }
    q << "}\n";
    return q.str();
}

std::string sendStratzMatchesBatch(const std::string& authToken,
    const std::vector<long long>& matchIds, size_t batchNum)
{
    std::string url = "https://api.stratz.com/graphql";
    json requestBody;
    requestBody["query"] = buildMatchesBatchQuery(matchIds);
    LOG_INFO("POST STRATZ batch " << batchNum << ": " << matchIds.size() << " матчей");
    try {
        return httpPost(url, requestBody.dump(), authToken);
    } catch (const std::exception& e) {
        LOG_ERR("STRATZ batch exception: " << e.what());
        throw;
    }
}

static int positionToInt(const std::string& pos) {
    if (pos == "POSITION_1") return 1;
    if (pos == "POSITION_2") return 2;
    if (pos == "POSITION_3") return 3;
    if (pos == "POSITION_4") return 4;
    if (pos == "POSITION_5") return 5;
    return 0;
}

void createHeroTableIfNotExists(sqlite3* db) {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS heroes (
            id             INTEGER PRIMARY KEY,
            name           TEXT NOT NULL,
            localized_name TEXT NOT NULL
        );
    )";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg); throw std::runtime_error("Ошибка создания таблицы heroes: " + e); }
}

void createPlayerHeroTableIfNotExists(sqlite3* db, const std::string& tableName) {
    std::string sql =
        "CREATE TABLE IF NOT EXISTS " + tableName + " ("
        "account_id INTEGER NOT NULL, hero_id INTEGER NOT NULL, games INTEGER NOT NULL, wins INTEGER NOT NULL, "
        "PRIMARY KEY (account_id, hero_id),"
        "FOREIGN KEY (hero_id) REFERENCES heroes(id) ON DELETE RESTRICT ON UPDATE CASCADE);";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg); throw std::runtime_error("Ошибка создания таблицы " + tableName + ": " + e); }
}

void createPlayerRecentMatchesTableIfNotExists(sqlite3* db) {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS playerrecentmatches (
            match_id             INTEGER NOT NULL,
            account_id           INTEGER NOT NULL,
            hero_id              INTEGER NOT NULL,
            position             INTEGER NOT NULL,
            won                  INTEGER NOT NULL,
            radiantpick1         INTEGER, radiantpick2 INTEGER, radiantpick3 INTEGER, radiantpick4 INTEGER, radiantpick5 INTEGER,
            radiantplayeronpick1 INTEGER, radiantplayeronpick2 INTEGER, radiantplayeronpick3 INTEGER, radiantplayeronpick4 INTEGER, radiantplayeronpick5 INTEGER,
            radiantheropick1pos  INTEGER, radiantheropick2pos  INTEGER, radiantheropick3pos  INTEGER, radiantheropick4pos  INTEGER, radiantheropick5pos  INTEGER,
            direpick1            INTEGER, direpick2 INTEGER, direpick3 INTEGER, direpick4 INTEGER, direpick5 INTEGER,
            direplayeronpick1    INTEGER, direplayeronpick2 INTEGER, direplayeronpick3 INTEGER, direplayeronpick4 INTEGER, direplayeronpick5 INTEGER,
            direheropick1pos     INTEGER, direheropick2pos  INTEGER, direheropick3pos  INTEGER, direheropick4pos  INTEGER, direheropick5pos  INTEGER,
            PRIMARY KEY (match_id, account_id),
            FOREIGN KEY (hero_id) REFERENCES heroes(id) ON DELETE CASCADE
        );
    )";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg); throw std::runtime_error("Ошибка создания таблицы playerrecentmatches: " + e); }
}

void createRelevantPlayerByPosTableIfNotExists(sqlite3* db) {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS relevantplayerherobyposstats (
            account_id INTEGER NOT NULL,
            heroId     INTEGER NOT NULL,
            position   INTEGER NOT NULL,
            games      INTEGER NOT NULL,
            wins       INTEGER NOT NULL,
            PRIMARY KEY (account_id, heroId, position),
            FOREIGN KEY (heroId) REFERENCES heroes(id) ON DELETE CASCADE
        );
    )";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg); throw std::runtime_error("Ошибка создания таблицы relevantplayerherobyposstats: " + e); }
}

void createPlayerHeroVsHeroByPosTableIfNotExists(sqlite3* db) {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS playerherovsherobyposstats (
            account_id  INTEGER NOT NULL, hero_id INTEGER NOT NULL, position INTEGER NOT NULL,
            vs_hero_id  INTEGER NOT NULL, vs_position INTEGER NOT NULL,
            games INTEGER NOT NULL, wins INTEGER NOT NULL,
            PRIMARY KEY (account_id, hero_id, position, vs_hero_id, vs_position),
            FOREIGN KEY (hero_id)    REFERENCES heroes(id) ON DELETE CASCADE,
            FOREIGN KEY (vs_hero_id) REFERENCES heroes(id) ON DELETE CASCADE
        );
    )";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg); throw std::runtime_error("Ошибка создания таблицы playerherovsherobyposstats: " + e); }
}

void createPlayerHeroWithHeroByPosTableIfNotExists(sqlite3* db) {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS playerherowithherobyposstats (
            account_id    INTEGER NOT NULL, hero_id INTEGER NOT NULL, position INTEGER NOT NULL,
            with_hero_id  INTEGER NOT NULL, with_position INTEGER NOT NULL,
            games INTEGER NOT NULL, wins INTEGER NOT NULL,
            PRIMARY KEY (account_id, hero_id, position, with_hero_id, with_position),
            FOREIGN KEY (hero_id)      REFERENCES heroes(id) ON DELETE CASCADE,
            FOREIGN KEY (with_hero_id) REFERENCES heroes(id) ON DELETE CASCADE
        );
    )";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg); throw std::runtime_error("Ошибка создания таблицы playerherowithherobyposstats: " + e); }
}

void createIndexesIfNotExist(sqlite3* db) {
    const char* sql = R"(
        CREATE INDEX IF NOT EXISTS idx_playerrecent_account ON playerrecentmatches(account_id);
        CREATE INDEX IF NOT EXISTS idx_playerrecent_hero    ON playerrecentmatches(hero_id);
        CREATE INDEX IF NOT EXISTS idx_relevantpos_account  ON relevantplayerherobyposstats(account_id, heroId);
        CREATE INDEX IF NOT EXISTS idx_vspos_account_hero   ON playerherovsherobyposstats(account_id, hero_id, position);
        CREATE INDEX IF NOT EXISTS idx_withpos_account_hero ON playerherowithherobyposstats(account_id, hero_id, position);
        CREATE INDEX IF NOT EXISTS idx_playerheroes_account ON playerheroes(account_id);
        CREATE INDEX IF NOT EXISTS idx_playerranked_account ON playerheroesranked(account_id);
    )";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg); LOG_WARN("Ошибка создания индексов: " << e); }
}

void storeHeroTable(sqlite3* db, const std::vector<HeroInfo>& heroes) {
    const char* sql = "INSERT OR IGNORE INTO heroes (id, name, localized_name) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) throw std::runtime_error("Ошибка подготовки запроса для heroes: " + std::string(sqlite3_errmsg(db)));
    SqliteTransaction txn(db);
    for (const auto& h : heroes) {
        sqlite3_bind_int64(stmt, 1, h.id);
        sqlite3_bind_text (stmt, 2, h.name.c_str(),           -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 3, h.localized_name.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) { std::string e = sqlite3_errmsg(db); sqlite3_finalize(stmt); throw std::runtime_error("Ошибка вставки героя: " + e); }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    txn.commit();
}

void storePlayerHeroStatsTable(sqlite3* db, long long accountId,
    const std::vector<HeroStats>& heroes, const std::string& tablename)
{
    std::string sql = "INSERT OR REPLACE INTO " + tablename + " (account_id, hero_id, games, wins) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) throw std::runtime_error("Ошибка подготовки запроса для " + tablename + ": " + std::string(sqlite3_errmsg(db)));
    SqliteTransaction txn(db);
    for (const auto& hero : heroes) {
        sqlite3_bind_int64(stmt, 1, accountId);
        sqlite3_bind_int64(stmt, 2, hero.hero_id);
        sqlite3_bind_int64(stmt, 3, hero.games);
        sqlite3_bind_int64(stmt, 4, hero.wins);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) { std::string e = sqlite3_errmsg(db); sqlite3_finalize(stmt); throw std::runtime_error("Ошибка вставки в " + tablename + ": " + e); }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    txn.commit();
}

void storePlayerRecentMatches(sqlite3* db, long long accountId,
    const std::vector<MatchDraft>& matches)
{
    const char* sql = R"(
        INSERT OR REPLACE INTO playerrecentmatches
            (match_id, account_id, hero_id, position, won,
             radiantpick1, radiantpick2, radiantpick3, radiantpick4, radiantpick5,
             radiantplayeronpick1, radiantplayeronpick2, radiantplayeronpick3, radiantplayeronpick4, radiantplayeronpick5,
             radiantheropick1pos, radiantheropick2pos, radiantheropick3pos, radiantheropick4pos, radiantheropick5pos,
             direpick1, direpick2, direpick3, direpick4, direpick5,
             direplayeronpick1, direplayeronpick2, direplayeronpick3, direplayeronpick4, direplayeronpick5,
             direheropick1pos, direheropick2pos, direheropick3pos, direheropick4pos, direheropick5pos)
        VALUES (?, ?, ?, ?, ?,
                ?, ?, ?, ?, ?,
                ?, ?, ?, ?, ?,
                ?, ?, ?, ?, ?,
                ?, ?, ?, ?, ?,
                ?, ?, ?, ?, ?,
                ?, ?, ?, ?, ?);
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) throw std::runtime_error("Ошибка подготовки запроса для playerrecentmatches: " + std::string(sqlite3_errmsg(db)));

    for (const auto& m : matches) {
        auto getHero    = [](const std::vector<std::tuple<long long, long long, int>>& p, int i) -> long long { return i < (int)p.size() ? std::get<0>(p[i]) : 0LL; };
        auto getPlayer  = [](const std::vector<std::tuple<long long, long long, int>>& p, int i) -> long long { return i < (int)p.size() ? std::get<1>(p[i]) : 0LL; };
        auto getPos     = [](const std::vector<std::tuple<long long, long long, int>>& p, int i) -> long long { return i < (int)p.size() ? std::get<2>(p[i]) : 0LL; };
        auto bindOrNull = [&](sqlite3_stmt* s, int col, long long val) {
            if (val) sqlite3_bind_int64(s, col, val); else sqlite3_bind_null(s, col);
        };
        sqlite3_bind_int64(stmt, 1, m.matchId);
        sqlite3_bind_int64(stmt, 2, accountId);
        sqlite3_bind_int64(stmt, 3, m.playerHeroId);
        sqlite3_bind_int64(stmt, 4, m.playerPosition);
        sqlite3_bind_int  (stmt, 5, m.playerWon ? 1 : 0);
        for (int i = 0; i < 5; ++i) bindOrNull(stmt,  6+i, getHero  (m.radiantPicks, i));
        for (int i = 0; i < 5; ++i) bindOrNull(stmt, 11+i, getPlayer(m.radiantPicks, i));
        for (int i = 0; i < 5; ++i) bindOrNull(stmt, 16+i, getPos   (m.radiantPicks, i));
        for (int i = 0; i < 5; ++i) bindOrNull(stmt, 21+i, getHero  (m.direPicks,    i));
        for (int i = 0; i < 5; ++i) bindOrNull(stmt, 26+i, getPlayer(m.direPicks,    i));
        for (int i = 0; i < 5; ++i) bindOrNull(stmt, 31+i, getPos   (m.direPicks,    i));
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) { std::string e = sqlite3_errmsg(db); sqlite3_finalize(stmt); throw std::runtime_error("Ошибка вставки в playerrecentmatches: " + e); }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

void storeRelevantPlayerByPos(sqlite3* db, long long accountId,
    const std::vector<std::tuple<long long, int, long long, long long>>& rows)
{
    const char* sql = "INSERT OR REPLACE INTO relevantplayerherobyposstats (account_id, heroId, position, games, wins) VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) throw std::runtime_error("Ошибка подготовки запроса для relevantplayerherobyposstats: " + std::string(sqlite3_errmsg(db)));
    for (const auto& row : rows) {
        sqlite3_bind_int64(stmt, 1, accountId);
        sqlite3_bind_int64(stmt, 2, std::get<0>(row));
        sqlite3_bind_int64(stmt, 3, std::get<1>(row));
        sqlite3_bind_int64(stmt, 4, std::get<2>(row));
        sqlite3_bind_int64(stmt, 5, std::get<3>(row));
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) { std::string e = sqlite3_errmsg(db); sqlite3_finalize(stmt); throw std::runtime_error("Ошибка вставки в relevantplayerherobyposstats: " + e); }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

void storePlayerHeroVsHeroByPos(sqlite3* db, long long accountId,
    const std::vector<std::tuple<long long, int, long long, int, long long, long long>>& rows)
{
    const char* sql = "INSERT OR REPLACE INTO playerherovsherobyposstats (account_id, hero_id, position, vs_hero_id, vs_position, games, wins) VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) throw std::runtime_error("Ошибка подготовки запроса для playerherovsherobyposstats: " + std::string(sqlite3_errmsg(db)));
    for (const auto& row : rows) {
        sqlite3_bind_int64(stmt, 1, accountId);
        sqlite3_bind_int64(stmt, 2, std::get<0>(row));
        sqlite3_bind_int64(stmt, 3, std::get<1>(row));
        sqlite3_bind_int64(stmt, 4, std::get<2>(row));
        sqlite3_bind_int64(stmt, 5, std::get<3>(row));
        sqlite3_bind_int64(stmt, 6, std::get<4>(row));
        sqlite3_bind_int64(stmt, 7, std::get<5>(row));
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) { std::string e = sqlite3_errmsg(db); sqlite3_finalize(stmt); throw std::runtime_error("Ошибка вставки в playerherovsherobyposstats: " + e); }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

void storePlayerHeroWithHeroByPos(sqlite3* db, long long accountId,
    const std::vector<std::tuple<long long, int, long long, int, long long, long long>>& rows)
{
    const char* sql = "INSERT OR REPLACE INTO playerherowithherobyposstats (account_id, hero_id, position, with_hero_id, with_position, games, wins) VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) throw std::runtime_error("Ошибка подготовки запроса для playerherowithherobyposstats: " + std::string(sqlite3_errmsg(db)));
    for (const auto& row : rows) {
        sqlite3_bind_int64(stmt, 1, accountId);
        sqlite3_bind_int64(stmt, 2, std::get<0>(row));
        sqlite3_bind_int64(stmt, 3, std::get<1>(row));
        sqlite3_bind_int64(stmt, 4, std::get<2>(row));
        sqlite3_bind_int64(stmt, 5, std::get<3>(row));
        sqlite3_bind_int64(stmt, 6, std::get<4>(row));
        sqlite3_bind_int64(stmt, 7, std::get<5>(row));
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) { std::string e = sqlite3_errmsg(db); sqlite3_finalize(stmt); throw std::runtime_error("Ошибка вставки в playerherowithherobyposstats: " + e); }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

void parseAndStoreBatchMatches(sqlite3* db, long long accountId, const std::string& response) {
    if (response.size() >= 2 &&
        static_cast<unsigned char>(response[0]) == 0x1F &&
        static_cast<unsigned char>(response[1]) == 0x8B) {
        LOG_ERR("STRATZ ответ в gzip, размер: " << response.size() << " байт");
        return;
    }
    json jsonResponse = json::parse(sanitizeUtf8(response), nullptr, false);
    if (jsonResponse.is_discarded()) { LOG_ERR("Не удалось разобрать JSON ответа STRATZ"); return; }
    if (jsonResponse.contains("errors")) {
        LOG_WARN("GraphQL вернул ошибки:");
        for (const auto& err : jsonResponse["errors"])
            LOG_WARN("  " << err.dump(-1, ' ', false, json::error_handler_t::replace));
    }
    if (!jsonResponse.contains("data") || jsonResponse["data"].is_null()) { LOG_ERR("Ответ не содержит data"); return; }

    std::vector<MatchDraft> recentMatches;
    std::map<std::pair<long long, int>, std::pair<long long, long long>> aggPos;
    std::map<std::tuple<long long, int, long long, int>, std::pair<long long, long long>> aggVs;
    std::map<std::tuple<long long, int, long long, int>, std::pair<long long, long long>> aggWith;
    std::set<long long> allVsHeroIds;

    for (auto& [key, matchNode] : jsonResponse["data"].items()) {
        if (matchNode.is_null() || key.size() < 2 || key[0] != 'm') continue;
        long long matchId = std::stoll(key.substr(1));
        if (!matchNode.contains("players") || matchNode["players"].is_null()) continue;

        bool didRadiantWin = matchNode.value("didRadiantWin", false);
        MatchDraft md;
        md.matchId = matchId;
        bool foundPlayer = false, playerIsRadiant = false;

        for (const auto& p : matchNode["players"]) {
            if (p.is_null() || p.value("steamAccountId", 0LL) != accountId) continue;
            int slot = p.value("playerSlot", 255);
            playerIsRadiant   = (slot < 128);
            md.playerHeroId   = p.value("heroId", 0LL);
            md.playerPosition = p["position"].is_null() ? 0 : positionToInt(p["position"].get<std::string>());
            md.playerWon      = playerIsRadiant ? didRadiantWin : !didRadiantWin;
            foundPlayer = true;
            break;
        }
        if (!foundPlayer || md.playerHeroId == 0 || md.playerPosition == 0) continue;

        for (const auto& p : matchNode["players"]) {
            if (p.is_null()) continue;
            long long heroId = p.value("heroId", 0LL);
            long long sid    = p.value("steamAccountId", 0LL);
            int slot         = p.value("playerSlot", 255);
            bool isRadiant   = (slot < 128);
            int vsPos        = p["position"].is_null() ? 0 : positionToInt(p["position"].get<std::string>());
            if (heroId == 0) continue;
            md.allPicks.emplace_back(heroId, isRadiant, sid, vsPos);
            if (isRadiant != playerIsRadiant && vsPos != 0) {
                auto k = std::make_tuple(md.playerHeroId, md.playerPosition, heroId, vsPos);
                aggVs[k].first += 1; aggVs[k].second += md.playerWon ? 1 : 0;
                allVsHeroIds.insert(heroId);
            }
            if (isRadiant == playerIsRadiant && heroId != md.playerHeroId && vsPos != 0) {
                auto k = std::make_tuple(md.playerHeroId, md.playerPosition, heroId, vsPos);
                aggWith[k].first += 1; aggWith[k].second += md.playerWon ? 1 : 0;
            }
        }

        if (matchNode.contains("pickBans") && matchNode["pickBans"].is_array()) {
            std::vector<std::tuple<int, long long, long long, bool, int>> picksOrdered;
            std::map<int, std::pair<long long, int>> playerIndexMap;
            int idx = 0;
            for (const auto& p : matchNode["players"]) {
                if (p.is_null()) { ++idx; continue; }
                int slot = p.value("playerSlot", 255);
                int pIdx = (slot < 128) ? slot : (slot - 128 + 5);
                playerIndexMap[pIdx] = {p.value("steamAccountId", 0LL),
                    p["position"].is_null() ? 0 : positionToInt(p["position"].get<std::string>())};
                ++idx;
            }
            for (const auto& pb : matchNode["pickBans"]) {
                if (pb.is_null() || pb["heroId"].is_null() || pb["order"].is_null()) continue;
                bool isPick = !pb["isPick"].is_null() && pb["isPick"].get<bool>();
                if (!isPick) continue;
                long long hid = pb["heroId"].get<long long>();
                int order     = pb["order"].get<int>();
                int pIdx = pb["playerIndex"].is_null() ? -1 : pb["playerIndex"].get<int>();
                bool isRad = (pIdx >= 0 && pIdx <= 4);
                long long sid = 0; int pos = 0;
                if (pIdx >= 0) { auto it = playerIndexMap.find(pIdx); if (it != playerIndexMap.end()) { sid = it->second.first; pos = it->second.second; } }
                picksOrdered.emplace_back(order, hid, sid, isRad, pos);
            }
            std::sort(picksOrdered.begin(), picksOrdered.end());
            for (const auto& [ord, hid, sid, isRad, pos] : picksOrdered) {
                if (isRad) md.radiantPicks.emplace_back(hid, sid, pos);
                else       md.direPicks.emplace_back(hid, sid, pos);
            }
        }

        auto posKey = std::make_pair(md.playerHeroId, md.playerPosition);
        aggPos[posKey].first += 1; aggPos[posKey].second += md.playerWon ? 1 : 0;
        recentMatches.push_back(std::move(md));
    }
    jsonResponse = json{};
    LOG_INFO("Разобрано матчей: " << recentMatches.size());

    std::lock_guard<std::mutex> dbLock(g_dbWriteMutex);
    SqliteTransaction txn(db);
    try {
        if (!recentMatches.empty()) {
            storePlayerRecentMatches(db, accountId, recentMatches);
            LOG_INFO("Сохранено " << recentMatches.size() << " -> playerrecentmatches");
        }
        {
            std::set<long long> allHeroIds;
            for (const auto& [k, _] : aggPos) allHeroIds.insert(k.first);
            for (long long hid : allVsHeroIds) allHeroIds.insert(hid);
            for (long long hid : allHeroIds)
                for (int pos = 1; pos <= 5; ++pos)
                    aggPos.emplace(std::make_pair(hid, pos), std::make_pair(0LL, 0LL));
            std::vector<std::tuple<long long, int, long long, long long>> posRows;
            posRows.reserve(aggPos.size());
            for (const auto& [k, v] : aggPos) posRows.emplace_back(k.first, k.second, v.first, v.second);
            if (!posRows.empty()) { storeRelevantPlayerByPos(db, accountId, posRows); LOG_INFO("Сохранено " << posRows.size() << " -> relevantplayerherobyposstats"); }
        }
        {
            std::vector<std::tuple<long long, int, long long, int, long long, long long>> vsRows;
            vsRows.reserve(aggVs.size());
            for (const auto& [k, v] : aggVs) vsRows.emplace_back(std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k), v.first, v.second);
            if (!vsRows.empty()) { storePlayerHeroVsHeroByPos(db, accountId, vsRows); LOG_INFO("Сохранено " << vsRows.size() << " -> playerherovsherobyposstats"); }
        }
        {
            std::vector<std::tuple<long long, int, long long, int, long long, long long>> withRows;
            withRows.reserve(aggWith.size());
            for (const auto& [k, v] : aggWith) withRows.emplace_back(std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k), v.first, v.second);
            if (!withRows.empty()) { storePlayerHeroWithHeroByPos(db, accountId, withRows); LOG_INFO("Сохранено " << withRows.size() << " -> playerherowithherobyposstats"); }
        }
        txn.commit();
    } catch (...) { throw; }
}

void fetchAndStorePlayerRecentData(sqlite3* db, const std::string& authToken, long long accountId) {
    try {
        std::vector<long long> matchIds = fetchRecentMatchIds(accountId);
        if (matchIds.empty()) { LOG_WARN("Нет матчей за 90 дней, игрок " << accountId); return; }

        const size_t BATCH_SIZE = 100;
        size_t totalBatches = (matchIds.size() + BATCH_SIZE - 1) / BATCH_SIZE;
        std::vector<std::future<std::string>> futures;
        futures.reserve(totalBatches);
        for (size_t b = 0; b < totalBatches; ++b) {
            size_t offset = b * BATCH_SIZE;
            size_t end    = (std::min)(offset + BATCH_SIZE, matchIds.size());
            std::vector<long long> batch(matchIds.begin() + offset, matchIds.begin() + end);
            LOG_INFO("Батч " << (b + 1) << ": матчи " << (offset + 1) << "-" << end << " [async]");
            futures.push_back(std::async(std::launch::async,
                [authToken, batch, b]() { return sendStratzMatchesBatch(authToken, batch, b + 1); }));
        }
        for (size_t b = 0; b < futures.size(); ++b) {
            LOG_INFO("Обработка батча " << (b + 1) << "...");
            parseAndStoreBatchMatches(db, accountId, futures[b].get());
        }
    } catch (const std::exception& e) {
        LOG_ERR("Ошибка запроса матчей: " << e.what());
    }
}
