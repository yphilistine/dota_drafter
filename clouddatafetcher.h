#pragma once
#include "common.h"

struct ProHeroStats {
    int hero_id;
    int pos;
    int games;
    int wins;
    int bans;
};

struct ImmortalHeroStats {
    int hero_id;
    int pos;
    int games;
    int wins;
    int bans;
};

void fetchAndStoreProHeroStats(sqlite3* db, const std::string& connStr);
void fetchAndStoreImmortalHeroStats(sqlite3* db, const std::string& connStr);
