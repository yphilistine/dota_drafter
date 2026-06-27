#pragma once
#include <string>

struct LocalVersionInfo {
    std::string appVersion;
    std::string dataVersion;
    int         schema = 0;
};

struct DataMeta {
    int         schema = 0;
    std::string dataVersion;
};

LocalVersionInfo loadLocalVersion();
void             saveLocalVersion(const LocalVersionInfo& v);
DataMeta         readDataDbMeta(const std::string& dataDbPath);
