#pragma once
/*
 * clouddatafetcher.h - синхронизация данных из PostgreSQL в SQLite.
 * Таблицы: proherostats, immortalherostats (агрегат recentimmortalmatches).
 */

#include "common.h"

// Строка про-статистики героя на позиции (PG -> SQLite)
struct ProHeroStats {
    int hero_id;
    int pos;    // позиция 1-5
    int games;
    int wins;
    int bans;
};

// Строка immortal-статистики (агрегат матчей immortal-рейтинга)
struct ImmortalHeroStats {
    int hero_id;
    int pos;
    int games;
    int wins;
    int bans;   // всегда 0 - в recentimmortalmatches нет данных о банах
};

// PG proherostats -> SQLite proherostats. Полная перезапись.
void fetchAndStoreProHeroStats(sqlite3* db, const std::string& connStr);

// PG recentimmortalmatches -> агрегация по (hero_id, pos) -> SQLite immortalherostats
void fetchAndStoreImmortalHeroStats(sqlite3* db, const std::string& connStr);