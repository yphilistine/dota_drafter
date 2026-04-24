#include "common.h"
#include "playerdatafetcher.h"
#include "clouddatafetcher.h"

int main(int argc, char* argv[]) {
    initConsole();
    ensureLogsDir();
    try { std::locale::global(std::locale("ru_RU.UTF-8")); }
    catch (...) { std::setlocale(LC_ALL, "ru_RU.UTF-8"); }
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    auto start_time = std::chrono::high_resolution_clock::now();
    CurlGlobal curlInit;

    try {
        long long accountId = 0;
        std::string stratzToken;

        if (argc < 2) {
            std::cout << "Введите account_id игрока (числовой Steam ID): ";
            std::string input;
            std::getline(std::cin, input);
            if (input.empty()) throw std::runtime_error("ID не может быть пустым.");
            try { accountId = std::stoll(input); }
            catch (const std::invalid_argument&) { throw std::runtime_error("Неверный формат account_id: \"" + input + "\""); }
            catch (const std::out_of_range&)     { throw std::runtime_error("account_id \"" + input + "\" выходит за допустимый диапазон."); }
            const char* envToken = std::getenv("STRATZ_API_KEY");
            if (envToken) { stratzToken = envToken; LOG_INFO("Токен STRATZ: переменная окружения"); }
            else          { stratzToken = DEFAULT_STRATZ_TOKEN; LOG_INFO("Токен STRATZ: андрей"); }
        } else {
            try { accountId = std::stoll(argv[1]); }
            catch (const std::invalid_argument&) { throw std::runtime_error(std::string("Неверный формат account_id: \"") + argv[1] + "\""); }
            catch (const std::out_of_range&)     { throw std::runtime_error(std::string("account_id \"") + argv[1] + "\" выходит за допустимый диапазон."); }
            if (argc >= 3) {
                stratzToken = argv[2];
            } else {
                const char* envToken = std::getenv("STRATZ_API_KEY");
                if (envToken) { stratzToken = envToken; }
                else          { stratzToken = DEFAULT_STRATZ_TOKEN; LOG_INFO("Токен STRATZ: андрей"); }
            }
            if (stratzToken.empty()) LOG_WARN("Токен STRATZ не указан, STRATZ запросы пропущены");
        }

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
                if (rc != SQLITE_OK) { std::string e = errMsg; sqlite3_free(errMsg); throw std::runtime_error("Ошибка очистки таблицы: " + e); }
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
        auto futureHeroes       = std::async(std::launch::async, [&]() { return fetchPlayerHeroesStats(accountIdStr); });
        auto futureHeroesRanked = std::async(std::launch::async, [&]() { return fetchPlayerHeroesRankedStats(accountIdStr); });

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
        LOG_ERR("Критическая ошибка: " << ex.what());
        return 1;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    LOG_INFO("Время выполнения: " << duration.count() << " мс ("
             << std::chrono::duration<double>(duration).count() << " с)");
    std::cout.flush();
    std::cerr.flush();
    if (g_logFile.is_open()) { g_logFile.flush(); g_logFile.close(); }
    if (g_curlDebugFile) { fflush(g_curlDebugFile); fclose(g_curlDebugFile); g_curlDebugFile = nullptr; }
    return 0;
}
