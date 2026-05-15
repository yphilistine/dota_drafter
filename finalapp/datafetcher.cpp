#include "common.h"
#include "playerdatafetcher.h"
#include "clouddatafetcher.h"
#include "shared_types.h"

int runDataFetcher(long long accountId, const std::string& stratzToken) {
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

        try {
            const char* pgEnv = std::getenv("PG_CONN_STR");
            if (!pgEnv || pgEnv[0] == '\0')
                throw std::runtime_error("Переменная окружения PG_CONN_STR не задана");
            fetchAndStoreProHeroStats(db.get(), pgEnv);
            fetchAndStoreImmortalHeroStats(db.get(), pgEnv);
        } catch (const std::exception& e) {
            LOG_WARN("Синхронизация proherostats пропущена: " << e.what());
        }

        createIndexesIfNotExist(db.get());
        LOG_INFO("Данные сохранены в playerandlivestats.db");

    } catch (const std::exception& ex) {
        LOG_ERR("Критическая ошибка DataFetcher: " << ex.what());
        return 1;
    }
    return 0;
}
