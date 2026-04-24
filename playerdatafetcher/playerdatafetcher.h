#pragma once
#include "common.h"

struct MatchDraft {
    long long matchId;
    long long playerHeroId;
    int  playerPosition = 0;
    bool playerWon;
    std::vector<std::tuple<long long, long long, int>> radiantPicks;
    std::vector<std::tuple<long long, long long, int>> direPicks;
    std::vector<std::tuple<long long, bool, long long, int>> allPicks;
};

std::string fetchHeroesList();
std::vector<HeroInfo> parseHeroesList(const std::string& jsonStr);

std::string fetchPlayerHeroesStats(const std::string& accountId);
std::string fetchPlayerHeroesRankedStats(const std::string& accountId);
std::vector<HeroStats> parseHeroesStats(const std::string& jsonStr);

std::vector<long long> fetchRecentMatchIds(long long accountId);

std::string buildMatchesBatchQuery(const std::vector<long long>& matchIds);
std::string sendStratzMatchesBatch(const std::string& authToken,
    const std::vector<long long>& matchIds, size_t batchNum);

void createHeroTableIfNotExists(sqlite3* db);
void createPlayerHeroTableIfNotExists(sqlite3* db, const std::string& tableName);
void createPlayerRecentMatchesTableIfNotExists(sqlite3* db);
void createRelevantPlayerByPosTableIfNotExists(sqlite3* db);
void createPlayerHeroVsHeroByPosTableIfNotExists(sqlite3* db);
void createPlayerHeroWithHeroByPosTableIfNotExists(sqlite3* db);
void createIndexesIfNotExist(sqlite3* db);

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

void parseAndStoreBatchMatches(sqlite3* db, long long accountId, const std::string& response);
void fetchAndStorePlayerRecentData(sqlite3* db, const std::string& authToken, long long accountId);
