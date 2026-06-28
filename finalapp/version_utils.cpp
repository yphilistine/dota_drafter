#include "version_utils.h"
#include "version.h"
#include "common.h"
#include <fstream>
#include <windows.h>

static const char* VERSION_FILE = "version.json";
static const char* MODEL_PATH_  = "draft_helper_abstract";

// ─── loadLocalVersion ────────────────────────────────────────────────────────

LocalVersionInfo loadLocalVersion() {
    LocalVersionInfo v;
    v.appVersion = kAppVersion;

    try {
        std::ifstream f(VERSION_FILE);
        if (!f.is_open()) throw std::runtime_error("no file");
        json j = json::parse(f);
        v.appVersion  = j.value("app_version",  std::string(kAppVersion));
        v.dataVersion = j.value("data_version", std::string());
        v.schema      = j.value("schema",       0);
    } catch (...) {
        LOG_WARN("version.json missing or corrupt — reconstructing");
        v.appVersion = kAppVersion;
        try {
            auto dm = readDataDbMeta(std::string(MODEL_PATH_) + "_data.db");
            v.dataVersion = dm.dataVersion;
            v.schema      = dm.schema;
        } catch (...) {
            v.dataVersion.clear();
            v.schema = 0;
        }
        try { saveLocalVersion(v); } catch (...) {}
    }
    return v;
}

// ─── saveLocalVersion ────────────────────────────────────────────────────────

void saveLocalVersion(const LocalVersionInfo& v) {
    json j;
    j["app_version"]  = v.appVersion;
    j["data_version"] = v.dataVersion;
    j["schema"]       = v.schema;

    std::string tmp = std::string(VERSION_FILE) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) throw std::runtime_error("Cannot write " + tmp);
        f << j.dump(2);
    }
    MoveFileExA(tmp.c_str(), VERSION_FILE, MOVEFILE_REPLACE_EXISTING);
    SetFileAttributesA(VERSION_FILE, FILE_ATTRIBUTE_HIDDEN);
}

// ─── readDataDbMeta ──────────────────────────────────────────────────────────

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
