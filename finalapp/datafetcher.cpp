#include "common.h"
#include "playerdatafetcher.h"
#include "shared_types.h"

// Фаза 1a: таблицы + справочник героев + живая мета-стата (без accountId)
int runDataFetcherInit(const std::string& stratzToken) {
    try {
        std::string heroesJson = fetchHeroesList();
        auto HeroesList = parseHeroesList(heroesJson);
        LOG_INFO("Справочник героев: " << HeroesList.size() << " записей");

        SqliteDB db("playerandlivestats.db");

        createHeroTableIfNotExists(db.get());
        createPlayerHeroTableIfNotExists(db.get(), "playerheroes");
        createPlayerHeroTableIfNotExists(db.get(), "playerheroesranked");
        createPlayerRecentMatchesTableIfNotExists(db.get());
        createRelevantPlayerByPosTableIfNotExists(db.get());
        createPlayerHeroVsHeroByPosTableIfNotExists(db.get());
        createPlayerHeroWithHeroByPosTableIfNotExists(db.get());
        storeHeroTable(db.get(), HeroesList);
        createIndexesIfNotExist(db.get());

        LOG_INFO("Таблицы и справочник героев готовы");

        if (!stratzToken.empty())
            fetchAndStoreHeroStats(db.get(), stratzToken);
        else
            LOG_WARN("HeroStats (meta) пропущен: нет STRATZ токена");
    } catch (const std::exception& ex) {
        LOG_ERR("DataFetcherInit: " << ex.what());
        return 1;
    }
    return 0;
}

// Фаза 1b: данные игрока (требует accountId)
int runDataFetcher(long long accountId, const std::string& stratzToken) {
    try {
        SqliteDB db("playerandlivestats.db");

        {
            auto execDel = [&](const std::string& sql) {
                char* errMsg = nullptr;
                int rc = sqlite3_exec(db.get(), sql.c_str(), nullptr, nullptr, &errMsg);
                if (rc != SQLITE_OK) {
                    std::string e = errMsg; sqlite3_free(errMsg);
                    throw std::runtime_error("Ошибка очистки таблицы: " + e);
                }
            };
            std::string aid = std::to_string(accountId);
            execDel("DELETE FROM playerrecentmatches WHERE account_id = " + aid);
            execDel("DELETE FROM relevantplayerherobyposstats WHERE account_id = " + aid);
            execDel("DELETE FROM playerherovsherobyposstats WHERE account_id = " + aid);
            execDel("DELETE FROM playerherowithherobyposstats WHERE account_id = " + aid);
            execDel("DELETE FROM playerheroes WHERE account_id = " + aid);
            execDel("DELETE FROM playerheroesranked WHERE account_id = " + aid);
            LOG_INFO("Данные игрока " << accountId << " очищены");
        }

        std::string accountIdStr = std::to_string(accountId);
        auto futureHeroes       = std::async(std::launch::async,
            [&]() { return fetchPlayerHeroesStats(accountIdStr); });
        auto futureHeroesRanked = std::async(std::launch::async,
            [&]() { return fetchPlayerHeroesRankedStats(accountIdStr); });

        if (!stratzToken.empty())
            fetchAndStorePlayerRecentData(db.get(), stratzToken, accountId);
        else
            LOG_WARN("STRATZ пропущен: нет токена");

        auto PlayerHeroes = parseHeroesStats(futureHeroes.get());
        LOG_INFO("Герои игрока (все): " << PlayerHeroes.size() << " записей");
        auto PlayerHeroesRanked = parseHeroesStats(futureHeroesRanked.get());
        LOG_INFO("Герои игрока (рейтинг): " << PlayerHeroesRanked.size() << " записей");
        storePlayerHeroStatsTable(db.get(), accountId, PlayerHeroes,       "playerheroes");
        storePlayerHeroStatsTable(db.get(), accountId, PlayerHeroesRanked, "playerheroesranked");

        LOG_INFO("Данные игрока сохранены в playerandlivestats.db");
    } catch (const std::exception& ex) {
        LOG_ERR("Критическая ошибка DataFetcher: " << ex.what());
        return 1;
    }
    return 0;
}
