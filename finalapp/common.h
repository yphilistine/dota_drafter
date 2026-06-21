#pragma once
/*
 * common.h — общие утилиты: логирование, HTTP, RAII-обёртки для curl/SQLite.
 * Подключается всеми модулями проекта.
 */

#include <iostream>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <clocale>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <fstream>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#endif
#include <memory>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <tuple>
#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

// ─── Константы ───────────────────────────────────────────────────────────────

const std::string DEFAULT_STRATZ_TOKEN =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJTdWJqZWN0IjoiNjhjODAzMjItMzQyYS00NWYwLWFlOWYtNjlhZjA3NzllMTMxIiwiU3RlYW1JZCI6IjEyNjE2NjAxMzUiLCJBUElVc2VyIjoidHJ1ZSIsIm5iZiI6MTc1MzEyNzU2NiwiZXhwIjoxNzg0NjYzNTY2LCJpYXQiOjE3NTMxMjc1NjYsImlzcyI6Imh0dHBzOi8vYXBpLnN0cmF0ei5jb20ifQ.UrC1HTdn6dDNf76buspQd3jSQNvhSqOtgAL-2kURGnE";

// ─── Типы данных ─────────────────────────────────────────────────────────────

// Статистика героя игрока (из OpenDota)
struct HeroStats {
    long long hero_id;
    long long games;
    long long wins;
};

// Запись справочника героев (из OpenDota /api/heroes)
struct HeroInfo {
    long long id;
    std::string name;           // внутреннее имя (npc_dota_hero_*)
    std::string localized_name; // отображаемое имя
};

// ─── RAII-обёртки ────────────────────────────────────────────────────────────

// curl_global_init / cleanup
class CurlGlobal {
public:
    CurlGlobal()  { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

// curl_slist для HTTP-заголовков
class CurlHeaders {
    struct curl_slist* headers;
public:
    CurlHeaders() : headers(nullptr) {}
    ~CurlHeaders() { if (headers) curl_slist_free_all(headers); }
    void append(const char* header) { headers = curl_slist_append(headers, header); }
    struct curl_slist* get() const { return headers; }
};

// curl_easy_init / cleanup
class CurlHandle {
    CURL* curl;
public:
    CurlHandle() : curl(curl_easy_init()) {
        if (!curl) throw std::runtime_error("Не удалось инициализировать CURL");
    }
    ~CurlHandle() { if (curl) curl_easy_cleanup(curl); }
    CURL* get() const { return curl; }
};

// sqlite3_open / close с настройкой PRAGMA
class SqliteDB {
    sqlite3* db;
public:
    SqliteDB(const std::string& filename) : db(nullptr) {
        int rc = sqlite3_open(filename.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::string err = sqlite3_errmsg(db);
            if (db) sqlite3_close(db);
            throw std::runtime_error("Не удалось открыть БД: " + err);
        }
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;",   nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA cache_size=-32768;",  nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA temp_store=MEMORY;",  nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA foreign_keys=ON;",    nullptr, nullptr, nullptr);
    }
    ~SqliteDB() { if (db) sqlite3_close(db); }
    sqlite3* get() const { return db; }
};

// BEGIN / COMMIT / ROLLBACK (автоматический откат в деструкторе)
class SqliteTransaction {
    sqlite3* db_;
    bool committed_ = false;
public:
    explicit SqliteTransaction(sqlite3* db) : db_(db) {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::string e = errMsg; sqlite3_free(errMsg);
            throw std::runtime_error("Ошибка начала транзакции: " + e);
        }
    }
    void commit() {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::string e = errMsg; sqlite3_free(errMsg);
            throw std::runtime_error("Ошибка COMMIT: " + e);
        }
        committed_ = true;
    }
    ~SqliteTransaction() {
        if (!committed_)
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    SqliteTransaction(const SqliteTransaction&) = delete;
    SqliteTransaction& operator=(const SqliteTransaction&) = delete;
};

// ─── Логирование ─────────────────────────────────────────────────────────────

enum class LogLevel { INFO, WARN, ERR };

extern std::mutex   g_logMutex;
extern std::mutex   g_dbWriteMutex;    // защита параллельных записей в SQLite
extern bool         g_ansiEnabled;
extern std::ofstream g_logFile;
extern FILE*        g_curlDebugFile;

// Вывод сообщения с таймстампом в консоль и logs/console.log
void logConsole(LogLevel level, const std::string& msg);
// Инициализация логов, ANSI-режима консоли, curl debug файла
void initConsole();
// Создание папки logs/
void ensureLogsDir();

#define LOG_INFO(msg) do { std::ostringstream _ss; _ss.imbue(std::locale::classic()); _ss << msg; logConsole(LogLevel::INFO, _ss.str()); } while(0)
#define LOG_WARN(msg) do { std::ostringstream _ss; _ss.imbue(std::locale::classic()); _ss << msg; logConsole(LogLevel::WARN, _ss.str()); } while(0)
#define LOG_ERR(msg)  do { std::ostringstream _ss; _ss.imbue(std::locale::classic()); _ss << msg; logConsole(LogLevel::ERR,  _ss.str()); } while(0)

// ─── HTTP ────────────────────────────────────────────────────────────────────

// GET-запрос с ретраями (3 попытки, пауза 10 сек). Бросает исключение при неудаче.
std::string httpGet (const std::string& url);
// POST-запрос с ретраями и Bearer-авторизацией. Бросает исключение при неудаче.
std::string httpPost(const std::string& url, const std::string& postData, const std::string& authToken);

// ─── Утилиты ─────────────────────────────────────────────────────────────────

// Замена невалидных UTF-8 последовательностей на U+FFFD
std::string sanitizeUtf8(const std::string& input);
