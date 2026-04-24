#include "clouddatafetcher.h"
#include <libpq-fe.h>

class PgConnection {
    PGconn* conn_;
public:
    explicit PgConnection(const std::string& connStr)
        : conn_(PQconnectdb(connStr.c_str()))
    {
        if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
            std::string err = conn_ ? PQerrorMessage(conn_) : "нет памяти";
            if (conn_) { PQfinish(conn_); conn_ = nullptr; }
            throw std::runtime_error("PostgreSQL: не удалось подключиться: " + err);
        }
    }
    ~PgConnection() { if (conn_) PQfinish(conn_); }
    PGconn* get() const { return conn_; }
    PgConnection(const PgConnection&) = delete;
    PgConnection& operator=(const PgConnection&) = delete;
};

class PgResult {
    PGresult* res_;
public:
    explicit PgResult(PGresult* res) : res_(res) {}
    ~PgResult() { if (res_) PQclear(res_); }
    PGresult* get() const { return res_; }
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
};

static void createProHeroStatsTableIfNotExists(sqlite3* db) {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS proherostats (
            hero_id INTEGER NOT NULL,
            pos     INTEGER NOT NULL,
            games   INTEGER NOT NULL DEFAULT 0,
            wins    INTEGER NOT NULL DEFAULT 0,
            bans    INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (hero_id, pos)
        );
    )";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string e = errMsg; sqlite3_free(errMsg);
        throw std::runtime_error("Ошибка создания таблицы proherostats в SQLite: " + e);
    }
}

static void storeProHeroStats(sqlite3* db, const std::vector<ProHeroStats>& rows) {
    const char* sql =
        "INSERT OR REPLACE INTO proherostats (hero_id, pos, games, wins, bans) "
        "VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
        throw std::runtime_error("Ошибка подготовки запроса для proherostats: "
            + std::string(sqlite3_errmsg(db)));
    SqliteTransaction txn(db);
    for (const auto& row : rows) {
        sqlite3_bind_int(stmt, 1, row.hero_id);
        sqlite3_bind_int(stmt, 2, row.pos);
        sqlite3_bind_int(stmt, 3, row.games);
        sqlite3_bind_int(stmt, 4, row.wins);
        sqlite3_bind_int(stmt, 5, row.bans);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            std::string e = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            throw std::runtime_error("Ошибка вставки в proherostats: " + e);
        }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    txn.commit();
}

void fetchAndStoreProHeroStats(sqlite3* db, const std::string& connStr) {
    LOG_INFO("PostgreSQL: подключение для proherostats...");
    PgConnection pg(connStr);
    LOG_INFO("PostgreSQL: подключено");

    PgResult res(PQexec(pg.get(),
        "SELECT hero_id, pos, games, wins, bans FROM proherostats ORDER BY hero_id, pos;"));
    if (PQresultStatus(res.get()) != PGRES_TUPLES_OK)
        throw std::runtime_error("PostgreSQL: ошибка запроса proherostats: "
            + std::string(PQerrorMessage(pg.get())));

    int nRows     = PQntuples(res.get());
    int col_hero  = PQfnumber(res.get(), "hero_id");
    int col_pos   = PQfnumber(res.get(), "pos");
    int col_games = PQfnumber(res.get(), "games");
    int col_wins  = PQfnumber(res.get(), "wins");
    int col_bans  = PQfnumber(res.get(), "bans");
    if (col_hero < 0 || col_pos < 0 || col_games < 0 || col_wins < 0 || col_bans < 0)
        throw std::runtime_error(
            "PostgreSQL: таблица proherostats не содержит ожидаемых колонок "
            "(hero_id, pos, games, wins, bans)");

    LOG_INFO("PostgreSQL: получено строк proherostats: " << nRows);

    std::vector<ProHeroStats> rows;
    rows.reserve(static_cast<size_t>(nRows));
    for (int i = 0; i < nRows; ++i) {
        ProHeroStats row;
        row.hero_id = std::atoi(PQgetvalue(res.get(), i, col_hero));
        row.pos     = std::atoi(PQgetvalue(res.get(), i, col_pos));
        row.games   = std::atoi(PQgetvalue(res.get(), i, col_games));
        row.wins    = std::atoi(PQgetvalue(res.get(), i, col_wins));
        row.bans    = std::atoi(PQgetvalue(res.get(), i, col_bans));
        rows.push_back(row);
    }

    createProHeroStatsTableIfNotExists(db);
    {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db, "DELETE FROM proherostats;", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg);
            throw std::runtime_error("Ошибка очистки proherostats: " + e); }
    }
    storeProHeroStats(db, rows);
    LOG_INFO("SQLite: сохранено " << rows.size() << " строк -> proherostats");
}

static void createImmortalHeroStatsTableIfNotExists(sqlite3* db) {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS immortalherostats (
            hero_id INTEGER NOT NULL,
            pos     INTEGER NOT NULL,
            games   INTEGER NOT NULL DEFAULT 0,
            wins    INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (hero_id, pos)
        );
    )";
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string e = errMsg; sqlite3_free(errMsg);
        throw std::runtime_error("Ошибка создания таблицы immortalherostats в SQLite: " + e);
    }
}

static void storeImmortalHeroStats(sqlite3* db,
    const std::map<std::pair<int,int>, std::pair<int,int>>& agg)
{
    const char* sql =
        "INSERT OR REPLACE INTO immortalherostats (hero_id, pos, games, wins) "
        "VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
        throw std::runtime_error("Ошибка подготовки запроса для immortalherostats: "
            + std::string(sqlite3_errmsg(db)));
    SqliteTransaction txn(db);
    for (const auto& [key, val] : agg) {
        sqlite3_bind_int(stmt, 1, key.first);
        sqlite3_bind_int(stmt, 2, key.second);
        sqlite3_bind_int(stmt, 3, val.first);
        sqlite3_bind_int(stmt, 4, val.second);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            std::string e = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            throw std::runtime_error("Ошибка вставки в immortalherostats: " + e);
        }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    txn.commit();
}

void fetchAndStoreImmortalHeroStats(sqlite3* db, const std::string& connStr) {
    LOG_INFO("PostgreSQL: подключение для recentimmortalmatches...");
    PgConnection pg(connStr);
    LOG_INFO("PostgreSQL: подключено");

    PgResult res(PQexec(pg.get(), R"(
        SELECT radiantwon,
               radiantpick1, radiantpick2, radiantpick3, radiantpick4, radiantpick5,
               radiantheropick1pos, radiantheropick2pos, radiantheropick3pos,
               radiantheropick4pos, radiantheropick5pos,
               direpick1, direpick2, direpick3, direpick4, direpick5,
               direheropick1pos, direheropick2pos, direheropick3pos,
               direheropick4pos, direheropick5pos
        FROM recentimmortalmatches;
    )"));
    if (PQresultStatus(res.get()) != PGRES_TUPLES_OK)
        throw std::runtime_error("PostgreSQL: ошибка запроса recentimmortalmatches: "
            + std::string(PQerrorMessage(pg.get())));

    int nRows = PQntuples(res.get());
    LOG_INFO("PostgreSQL: получено строк recentimmortalmatches: " << nRows);

    int c_rwon = PQfnumber(res.get(), "radiantwon");
    int c_rp[5], c_rpos[5], c_dp[5], c_dpos[5];
    for (int i = 0; i < 5; ++i) {
        std::string n = std::to_string(i + 1);
        c_rp  [i] = PQfnumber(res.get(), ("radiantpick"     + n).c_str());
        c_rpos[i] = PQfnumber(res.get(), ("radiantheropick" + n + "pos").c_str());
        c_dp  [i] = PQfnumber(res.get(), ("direpick"        + n).c_str());
        c_dpos[i] = PQfnumber(res.get(), ("direheropick"    + n + "pos").c_str());
    }

    std::map<std::pair<int,int>, std::pair<int,int>> agg;

    for (int row = 0; row < nRows; ++row) {
        const char* rwon_str = PQgetvalue(res.get(), row, c_rwon);
        bool radiantWon = (rwon_str[0] == 't' || rwon_str[0] == 'T' || rwon_str[0] == '1');

        for (int i = 0; i < 5; ++i) {
            const char* hv = PQgetvalue(res.get(), row, c_rp[i]);
            const char* pv = PQgetvalue(res.get(), row, c_rpos[i]);
            if (!hv || hv[0] == '\0' || !pv || pv[0] == '\0') continue;
            int heroId = std::atoi(hv);
            int pos    = std::atoi(pv);
            if (heroId <= 0 || pos <= 0 || pos > 5) continue;
            auto& entry = agg[{heroId, pos}];
            entry.first  += 1;
            entry.second += radiantWon ? 1 : 0;
        }

        for (int i = 0; i < 5; ++i) {
            const char* hv = PQgetvalue(res.get(), row, c_dp[i]);
            const char* pv = PQgetvalue(res.get(), row, c_dpos[i]);
            if (!hv || hv[0] == '\0' || !pv || pv[0] == '\0') continue;
            int heroId = std::atoi(hv);
            int pos    = std::atoi(pv);
            if (heroId <= 0 || pos <= 0 || pos > 5) continue;
            auto& entry = agg[{heroId, pos}];
            entry.first  += 1;
            entry.second += radiantWon ? 0 : 1;
        }
    }

    LOG_INFO("Агрегировано пар (hero_id, pos): " << agg.size());

    createImmortalHeroStatsTableIfNotExists(db);
    {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db, "DELETE FROM immortalherostats;", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg);
            throw std::runtime_error("Ошибка очистки immortalherostats: " + e); }
    }
    storeImmortalHeroStats(db, agg);
    LOG_INFO("SQLite: сохранено " << agg.size() << " строк -> immortalherostats");
}
