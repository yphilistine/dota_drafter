#include "version_utils.h"
#include <sqlite3.h>
#include <stdexcept>
#include <string>

DataMeta readDataDbMeta(const std::string& dataDbPath) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(dataDbPath.c_str(), &db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "open failed";
        if (db) sqlite3_close(db);
        throw std::runtime_error("Cannot open " + dataDbPath + ": " + err);
    }

    DataMeta meta;
    auto readKey = [&](const char* key) -> std::string {
        sqlite3_stmt* stmt = nullptr;
        rc = sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key=?", -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            if (stmt) sqlite3_finalize(stmt);
            throw std::runtime_error("meta table not found in " + dataDbPath);
        }
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        std::string result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* t = sqlite3_column_text(stmt, 0);
            if (t) result = reinterpret_cast<const char*>(t);
        }
        sqlite3_finalize(stmt);
        return result;
    };

    try {
        std::string sv = readKey("schema_version");
        meta.schema = sv.empty() ? 0 : std::stoi(sv);
        meta.dataVersion = readKey("data_version");
    } catch (...) {
        sqlite3_close(db);
        throw;
    }

    sqlite3_close(db);
    return meta;
}
