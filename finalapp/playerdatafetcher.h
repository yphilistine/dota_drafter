#pragma once
/*
 * playerdatafetcher.h - загрузка данных игрока из OpenDota и STRATZ.
 * Запись в SQLite: герои, статистика, матчи, vs/with агрегаты.
 */

#include "common.h"
#include "hero_meta_stats.h"

// Драфт одного матча (из STRATZ GraphQL)
struct MatchDraft {
    long long matchId;
    long long playerHeroId;
    int  playerPosition = 0;
    bool playerWon;
    std::vector<std::tuple<long long, long long, int>> radiantPicks; // (heroId, steamId, pos)
    std::vector<std::tuple<long long, long long, int>> direPicks;
    std::vector<std::tuple<long long, bool, long long, int>> allPicks; // (heroId, isRadiant, steamId, pos)
};

// --- OpenDota: справочник героев ---------------------------------------------
std::string fetchHeroesList();
std::vector<HeroInfo> parseHeroesList(const std::string& jsonStr);

// --- STRATZ-фолбек: справочник героев, если OpenDota недоступен -------------
std::string fetchHeroesListStratz(const std::string& authToken);
std::vector<HeroInfo> parseHeroesListStratz(const std::string& jsonStr);

// --- OpenDota: статистика героев игрока ---------------------------------------
std::string fetchPlayerHeroesStats(const std::string& accountId);
std::string fetchPlayerHeroesRankedStats(const std::string& accountId);
std::vector<HeroStats> parseHeroesStats(const std::string& jsonStr);

// --- OpenDota: список матчей (90 дней, ranked) -------------------------------
std::vector<long long> fetchRecentMatchIds(long long accountId);

// --- STRATZ-фолбек: список match_id игрока, если OpenDota недоступен --------
std::vector<long long> fetchRecentMatchIdsStratz(const std::string& authToken, long long accountId);

// --- STRATZ: батч-запрос деталей матчей ---------------------------------------
std::string buildMatchesBatchQuery(const std::vector<long long>& matchIds);
std::string sendStratzMatchesBatch(const std::string& authToken,
    const std::vector<long long>& matchIds, size_t batchNum);

// --- SQLite: создание таблиц --------------------------------------------------
void createHeroTableIfNotExists(sqlite3* db);
void createPlayerHeroTableIfNotExists(sqlite3* db, const std::string& tableName);
void createPlayerRecentMatchesTableIfNotExists(sqlite3* db);
void createRelevantPlayerByPosTableIfNotExists(sqlite3* db);
void createPlayerHeroVsHeroByPosTableIfNotExists(sqlite3* db);
void createPlayerHeroWithHeroByPosTableIfNotExists(sqlite3* db);
void createIndexesIfNotExist(sqlite3* db);

// --- SQLite: запись данных ----------------------------------------------------
void storeHeroTable(sqlite3* db, const std::vector<HeroInfo>& heroes);
void storePlayerHeroStatsTable(sqlite3* db, long long accountId,
    const std::vector<HeroStats>& heroes, const std::string& tablename);
void storePlayerRecentMatches(sqlite3* db, long long accountId,
    const std::vector<MatchDraft>& matches);
void storeRelevantPlayerByPos(sqlite3* db, long long accountId,
    const std::vector<std::tuple<long long, int, long long, long long>>& rows);
void storePlayerHeroVsHeroByPos(sqlite3* db, long long accountId,
    const std::vector<std::tuple<long long, int, long long, int, long long, long long>>& rows);
void storePlayerHeroWithHeroByPos(sqlite3* db, long long accountId,
    const std::vector<std::tuple<long long, int, long long, int, long long, long long>>& rows);

// --- Парсинг STRATZ ответа и запись в SQLite ----------------------------------
void parseAndStoreBatchMatches(sqlite3* db, long long accountId, const std::string& response);

// --- Главная функция: матчи -> батчи STRATZ -> парсинг -> SQLite ----------------
void fetchAndStorePlayerRecentData(sqlite3* db, const std::string& authToken, long long accountId);
