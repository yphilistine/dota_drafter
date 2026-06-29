#pragma once
#include <string>

struct DataMeta {
    int         schema = 0;
    std::string dataVersion;
};

DataMeta readDataDbMeta(const std::string& dataDbPath);
