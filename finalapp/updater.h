#pragma once
#include <string>
#include <functional>
#include <map>
#include <vector>
#include "version_utils.h"

// --- Типы --------------------------------------------------------------------

enum class UpdateAction {
    NONE,
    APP_UPDATE,
    DATA_UPDATE,
    SCHEMA_TOO_NEW
};

struct FileEntry {
    std::string url;
    std::string sha256;
    size_t      size = 0; // байт, из манифеста (0 = неизвестно, старый манифест без поля)
};

struct ManifestInfo {
    std::string appVersion;
    std::string appUrl;
    std::string appSha256;

    std::string dataVersion;
    int         dataSchema   = 0;
    std::map<std::string, FileEntry> dataFiles;
};

using ProgressCb = std::function<void(size_t done, size_t total)>;

// --- API ---------------------------------------------------------------------

bool         fetchManifest(ManifestInfo& out);
UpdateAction checkForUpdates(const ManifestInfo& manifest);

bool downloadToStaging(const std::string& url,
                       const std::string& expectedSha256,
                       const std::string& stagingPath,
                       ProgressCb progress = nullptr);

std::string fileSha256(const std::string& path);

// stagedFiles получает ключи манифеста, которые реально были скачаны (т.е. локальный
// файл отсутствовал или не совпадал по SHA-256). Файлы, уже совпадающие с манифестом,
// не перекачиваются и не попадают в stagedFiles.
bool downloadAndStageData(const ManifestInfo& manifest,
                          std::vector<std::string>& stagedFiles,
                          ProgressCb progress = nullptr);

// Меняет местами (staging -> live) только файлы из stagedFiles — остальные
// (уже актуальные) не трогаются вовсе.
bool swapDataFiles(const ManifestInfo& manifest, const std::vector<std::string>& stagedFiles);
void rollbackDataFiles();
void checkPendingSwap();
void cleanupStaging();

int  compareVersions(const std::string& a, const std::string& b);

// Синхронизирует gamestate_integration_dota2.cfg с манифестом. Этот файл живёт
// вне {app} — в папке Dota 2 (путь определяется динамически: реестр Steam +
// steamapps\libraryfolders.vdf, т.к. Dota 2 часто стоит в отдельной библиотеке
// на другом диске), поэтому не проходит через общий dataFiles-конвейер
// (checkForUpdates/downloadAndStageData/swapDataFiles рассчитаны на пути
// относительно {app}). Best-effort: если Dota 2 не найдена или скачивание не
// удалось — тихо пропускает, ничего не бросает наружу и не блокирует запуск.
void syncGsiConfig(const ManifestInfo& manifest);
