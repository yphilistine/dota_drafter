#pragma once
/*
 * hero_meta_stats.h — живая мета-статистика героев (STRATZ heroStats, фаза 1a).
 * Не требует accountId. Отдельный пайплайн от истории матчей игрока
 * (playerdatafetcher.h) — другой источник, другое время запуска.
 */

#include "common.h"

// ─── STRATZ: живая статистика героев (heroStats, DIVINE_IMMORTAL, по позициям) ──
struct HeroWeekStat {
    long long heroId;
    int       pos;         // позиция 1-5
    long long matchCount;
    long long winCount;
};

long long lastCompletedWeekTimestamp();
std::string buildHeroStatsQuery(long long week);
std::string sendStratzHeroStats(const std::string& authToken, long long week);
std::vector<HeroWeekStat> parseHeroStatsResponse(const std::string& response);

void createHeroStatsTableIfNotExists(sqlite3* db);
void storeHeroStatsTable(sqlite3* db, const std::vector<HeroWeekStat>& rows);

// Главная функция: STRATZ heroStats (последняя завершённая неделя) → SQLite `stats`
void fetchAndStoreHeroStats(sqlite3* db, const std::string& authToken);
