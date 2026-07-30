#pragma once
/*
 * common.h - общие утилиты: логирование, HTTP, RAII-обёртки для curl/SQLite.
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

// --- Константы ---------------------------------------------------------------

const std::string DEFAULT_STRATZ_TOKEN =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJTdWJqZWN0IjoiNjhjODAzMjItMzQyYS00NWYwLWFlOWYtNjlhZjA3NzllMTMxIiwiU3RlYW1JZCI6IjEyNjE2NjAxMzUiLCJBUElVc2VyIjoidHJ1ZSIsIm5iZiI6MTc4NTAyNDk3MywiZXhwIjoxODE2NTYwOTczLCJpYXQiOjE3ODUwMjQ5NzMsImlzcyI6Imh0dHBzOi8vYXBpLnN0cmF0ei5jb20ifQ.0Os8gKLGGO00cyKGct-gjo_b6ohnglC37z6gxsDFYXk";

// --- Типы данных -------------------------------------------------------------

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

// --- RAII-обёртки ------------------------------------------------------------

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

// sqlite3_open / close с настройкой PRAGMA. readOnly=true - только чтение
// (SQLITE_OPEN_READONLY, без write-PRAGMA), для коротко- и долгоживущих
// читателей вроде dota_picker.cpp. busy_timeout выставляется всегда -
// единая точка защиты от SQLITE_BUSY под конкурентными писателями.
class SqliteDB {
    sqlite3* db;
public:
    explicit SqliteDB(const std::string& filename, bool readOnly = false) : db(nullptr) {
        int rc = readOnly
            ? sqlite3_open_v2(filename.c_str(), &db,
                              SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr)
            : sqlite3_open(filename.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::string err = db ? sqlite3_errmsg(db) : "неизвестная ошибка";
            if (db) sqlite3_close(db);
            throw std::runtime_error("Не удалось открыть БД: " + err);
        }
        sqlite3_busy_timeout(db, 5000);
        if (!readOnly) {
            sqlite3_exec(db, "PRAGMA journal_mode=WAL;",   nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA cache_size=-32768;",  nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA temp_store=MEMORY;",  nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA foreign_keys=ON;",    nullptr, nullptr, nullptr);
        }
    }
    ~SqliteDB() { if (db) sqlite3_close(db); }
    sqlite3* get() const { return db; }
    SqliteDB(const SqliteDB&) = delete;
    SqliteDB& operator=(const SqliteDB&) = delete;
};

// sqlite3_stmt RAII: prepare/finalize + типизированные bind/column-хелперы.
class SqliteStmt {
    sqlite3_stmt* s_ = nullptr;
public:
    SqliteStmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("SQL prepare: ") + sqlite3_errmsg(db));
    }
    ~SqliteStmt() { if (s_) sqlite3_finalize(s_); }
    sqlite3_stmt* get() { return s_; }
    bool row()                       { return sqlite3_step(s_) == SQLITE_ROW; }
    void bind_int(int i, int v)      { sqlite3_bind_int(s_, i, v); }
    int         col_int(int i)       { return sqlite3_column_int(s_, i); }
    long long   col_int64(int i)     { return sqlite3_column_int64(s_, i); }
    std::string col_text(int i)      { auto* t = sqlite3_column_text(s_, i); return t ? (const char*)t : ""; }
    bool col_null(int i)             { return sqlite3_column_type(s_, i) == SQLITE_NULL; }
    double col_double(int i)         { return sqlite3_column_double(s_, i); }
    SqliteStmt(const SqliteStmt&) = delete;
    SqliteStmt& operator=(const SqliteStmt&) = delete;
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

// --- Логирование -------------------------------------------------------------

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

// --- HTTP --------------------------------------------------------------------

// GET-запрос с ретраями (3 попытки, пауза 10 сек). Бросает исключение при неудаче.
std::string httpGet (const std::string& url);
// POST-запрос с ретраями и Bearer-авторизацией. Бросает исключение при неудаче.
std::string httpPost(const std::string& url, const std::string& postData, const std::string& authToken);

// --- Утилиты -----------------------------------------------------------------

// Замена невалидных UTF-8 последовательностей на U+FFFD
std::string sanitizeUtf8(const std::string& input);

// Разбор int/long long без исключений - для непроверенных внешних данных
// (GSI-payload от Dota 2, STRATZ-ответы). fallback возвращается при пустой
// строке или ошибке разбора вместо проброса std::invalid_argument/out_of_range.
inline int safeStoi(const std::string& s, int fallback = 0) {
    try { return s.empty() ? fallback : std::stoi(s); } catch (...) { return fallback; }
}
inline long long safeStoll(const std::string& s, long long fallback = -1) {
    try { return s.empty() ? fallback : std::stoll(s); } catch (...) { return fallback; }
}

// STRATZ "POSITION_n" (n=1..5) -> int; 0, если формат не распознан.
// Используется и в пайплайне живой меты (hero_meta_stats.cpp), и в пайплайне
// истории матчей игрока (playerdatafetcher.cpp) - общий разбор одного и того
// же перечисления STRATZ.
inline int positionToInt(const std::string& pos) {
    if (pos == "POSITION_1") return 1;
    if (pos == "POSITION_2") return 2;
    if (pos == "POSITION_3") return 3;
    if (pos == "POSITION_4") return 4;
    if (pos == "POSITION_5") return 5;
    return 0;
}

// Устанавливает std::set_terminate + SetUnhandledExceptionFilter - сетка
// безопасности на уровне процесса: логирует необработанное исключение/SEH
// в logs/console.log перед завершением, вместо тихого краха без следа.
// Вызывать один раз при старте, сразу после initConsole().
void installCrashHandlers();
