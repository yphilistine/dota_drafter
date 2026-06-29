#pragma once
#include <string>
#include <functional>
#include <map>
#include "version_utils.h"

// ─── Типы ────────────────────────────────────────────────────────────────────

enum class UpdateAction {
    NONE,
    APP_UPDATE,
    DATA_UPDATE,
    SCHEMA_TOO_NEW
};

struct FileEntry {
    std::string url;
    std::string sha256;
};

struct ManifestInfo {
    std::string appVersion;
    std::string appUrl;
    std::string appSha256;
    bool        appMandatory = false;

    std::string dataVersion;
    int         dataSchema   = 0;
    std::map<std::string, FileEntry> dataFiles;
};

using ProgressCb = std::function<void(size_t done, size_t total)>;

// ─── API ─────────────────────────────────────────────────────────────────────

bool         fetchManifest(ManifestInfo& out);
UpdateAction checkForUpdates(const ManifestInfo& manifest);

bool downloadToStaging(const std::string& url,
                       const std::string& expectedSha256,
                       const std::string& stagingPath,
                       ProgressCb progress = nullptr);

std::string fileSha256(const std::string& path);

bool downloadAndStageData(const ManifestInfo& manifest, ProgressCb progress = nullptr);
bool swapDataFiles(const ManifestInfo& manifest);
void rollbackDataFiles();
void checkPendingSwap();
void cleanupStaging();

int  compareVersions(const std::string& a, const std::string& b);
