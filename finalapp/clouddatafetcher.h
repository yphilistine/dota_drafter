#pragma once
#include "common.h"

// Строка из PostgreSQL таблицы proherostats (с позицией)
struct ProHeroStats {
    int hero_id;
    int pos;    // позиция 1-5
    int games;
    int wins;
    int bans;
};

// Строка для immortalherostats (агрегат из recentimmortalmatches)
struct ImmortalHeroStats {
    int hero_id;
    int pos;
    int games;
    int wins;
    int bans;   // всегда 0 — в recentimmortalmatches нет данных о банах
};

// Подключается к PostgreSQL, читает proherostats (hero_id, pos, games, wins, bans),
// создаёт/обновляет таблицу proherostats в SQLite.
void fetchAndStoreProHeroStats(sqlite3* db, const std::string& connStr);

// Подключается к PostgreSQL, читает recentimmortalmatches,
// агрегирует статистику по (hero_id, pos) и сохраняет в SQLite таблицу immortalherostats.
void fetchAndStoreImmortalHeroStats(sqlite3* db, const std::string& connStr);