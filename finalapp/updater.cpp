#include "updater.h"
#include "version.h"
#include "common.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

static const char* STAGING_DIR  = "staging";
static const char* SWAP_LOCK    = "staging\\swap.lock";

// Ключ манифеста, который не является путём относительно {app} — этот файл
// живёт в папке Dota 2 и обрабатывается отдельно, см. syncGsiConfig().
static const char* GSI_CONFIG_KEY = "gamestate_integration_dota2.cfg";
static bool isExternalManifestKey(const std::string& fname) {
    return fname == GSI_CONFIG_KEY;
}

// ─── Утилиты ─────────────────────────────────────────────────────────────────

// Создаёт все директории-предки файла (напр. "staging\assets\foo.png" ->
// "staging" затем "staging\assets"). Нужно, т.к. manifest.dataFiles может
// содержать вложенные ключи (assets/*.png), а не только файлы верхнего уровня.
static void ensureParentDir(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return;
    std::string dir = path.substr(0, pos);
    for (size_t i = 0; i < dir.size(); ++i) {
        if (dir[i] == '\\' || dir[i] == '/')
            CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
    }
    CreateDirectoryA(dir.c_str(), nullptr);
}

int compareVersions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<int> parts;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, '.')) {
            try { parts.push_back(std::stoi(tok)); }
            catch (...) { parts.push_back(0); }
        }
        return parts;
    };
    auto pa = split(a), pb = split(b);
    size_t n = (std::max)(pa.size(), pb.size());
    pa.resize(n, 0);
    pb.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return  1;
    }
    return 0;
}

// ─── SHA-256 через BCrypt ────────────────────────────────────────────────────

std::string fileSha256(const std::string& path) {
    BCRYPT_ALG_HANDLE  hAlg  = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) != 0)
        throw std::runtime_error("BCrypt: cannot open SHA256 provider");

    DWORD hashObjSize = 0, cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                      (PBYTE)&hashObjSize, sizeof(hashObjSize), &cbData, 0);
    std::vector<BYTE> hashObj(hashObjSize);

    DWORD hashLen = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                      (PBYTE)&hashLen, sizeof(hashLen), &cbData, 0);
    std::vector<BYTE> hashValue(hashLen);

    if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize,
                          nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error("BCrypt: cannot create hash");
    }

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error("Cannot open file for hashing: " + path);
    }

    char buf[65536];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        BCryptHashData(hHash, (PUCHAR)buf, (ULONG)f.gcount(), 0);
    }
    f.close();

    BCryptFinishHash(hHash, hashValue.data(), hashLen, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (DWORD i = 0; i < hashLen; ++i)
        hex << std::setw(2) << (int)hashValue[i];
    return hex.str();
}

// ─── fetchManifest ───────────────────────────────────────────────────────────

static size_t writeStringCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

bool fetchManifest(ManifestInfo& out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL,              kManifestUrl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    writeStringCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,   2L);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,      CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE,        CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,        "DotaDrafter-Updater/1.0");

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode != 200) return false;

    try {
        json j = json::parse(body);

        auto& app = j["app"];
        out.appVersion   = app.value("version",   std::string());
        out.appUrl       = app.value("url",       std::string());
        out.appSha256    = app.value("sha256",    std::string());
        out.appMandatory = app.value("mandatory", false);

        auto& data = j["data"];
        out.dataVersion = data.value("version", std::string());
        out.dataSchema  = data.value("schema",  0);
        if (data.contains("files") && data["files"].is_object()) {
            for (auto& [fname, fobj] : data["files"].items()) {
                FileEntry fe;
                fe.url    = fobj.value("url",    std::string());
                fe.sha256 = fobj.value("sha256", std::string());
                fe.size   = fobj.value("size",   (size_t)0);
                out.dataFiles[fname] = fe;
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}

// ─── checkForUpdates ─────────────────────────────────────────────────────────

UpdateAction checkForUpdates(const ManifestInfo& manifest) {
    // App: сравнение скомпилированной версии с манифестом
    if (compareVersions(manifest.appVersion, kAppVersion) > 0)
        return UpdateAction::APP_UPDATE;

    // Data: сравнение SHA-256 локальных файлов с манифестом
    for (auto& [fname, fe] : manifest.dataFiles) {
        if (fe.sha256.empty()) continue;
        if (isExternalManifestKey(fname)) continue; // не {app}-путь, см. syncGsiConfig()
        if (GetFileAttributesA(fname.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (manifest.dataSchema <= kSupportedSchema)
                return UpdateAction::DATA_UPDATE;
            return UpdateAction::SCHEMA_TOO_NEW;
        }
        try {
            std::string localHash = fileSha256(fname);
            if (localHash != fe.sha256) {
                if (manifest.dataSchema <= kSupportedSchema)
                    return UpdateAction::DATA_UPDATE;
                return UpdateAction::SCHEMA_TOO_NEW;
            }
        } catch (...) {
            if (manifest.dataSchema <= kSupportedSchema)
                return UpdateAction::DATA_UPDATE;
            return UpdateAction::SCHEMA_TOO_NEW;
        }
    }
    return UpdateAction::NONE;
}

// ─── downloadToStaging ───────────────────────────────────────────────────────

struct DownloadCtx {
    FILE*      fp = nullptr;
    ProgressCb progress;
};

static size_t writeFileCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* ctx = static_cast<DownloadCtx*>(ud);
    return fwrite(ptr, size, nmemb, ctx->fp);
}

static int progressCb(void* ud, curl_off_t dlTotal, curl_off_t dlNow,
                       curl_off_t, curl_off_t) {
    auto* ctx = static_cast<DownloadCtx*>(ud);
    if (ctx->progress)
        ctx->progress(static_cast<size_t>(dlNow), static_cast<size_t>(dlTotal));
    return 0;
}

bool downloadToStaging(const std::string& url,
                       const std::string& expectedSha256,
                       const std::string& stagingPath,
                       ProgressCb progress) {
    ensureParentDir(stagingPath);
    std::string partPath = stagingPath + ".part";

    CURL* curl = curl_easy_init();
    if (!curl) { LOG_ERR("downloadToStaging: curl_easy_init failed for " << url); return false; }

    DownloadCtx ctx;
    ctx.fp = fopen(partPath.c_str(), "wb");
    if (!ctx.fp) {
        LOG_ERR("downloadToStaging: cannot open " << partPath << " for writing");
        curl_easy_cleanup(curl);
        return false;
    }
    ctx.progress = progress;

    curl_easy_setopt(curl, CURLOPT_URL,               url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,      writeFileCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,          &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,     1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,     15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT,    100L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,     60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,     1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,     2L);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,        CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE,          CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,          "DotaDrafter-Updater/1.0");
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,         0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,   progressCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,       &ctx);

    CURLcode res = curl_easy_perform(curl);
    fclose(ctx.fp);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode != 200) {
        LOG_WARN("downloadToStaging: download failed for " << url
                 << ": curl=" << curl_easy_strerror(res) << " http=" << httpCode);
        DeleteFileA(partPath.c_str());
        return false;
    }

    if (!expectedSha256.empty()) {
        std::string actual = fileSha256(partPath);
        if (actual != expectedSha256) {
            LOG_ERR("SHA-256 mismatch for " << stagingPath
                    << ": expected " << expectedSha256
                    << ", got " << actual);
            DeleteFileA(partPath.c_str());
            return false;
        }
    }

    MoveFileExA(partPath.c_str(), stagingPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    return true;
}

// ─── Data channel ────────────────────────────────────────────────────────────

// Локальный файл уже совпадает с манифестом — перекачивать не нужно.
static bool localFileUpToDate(const std::string& fname, const std::string& expectedSha256) {
    if (expectedSha256.empty()) return false;
    if (GetFileAttributesA(fname.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    try {
        return fileSha256(fname) == expectedSha256;
    } catch (...) {
        return false;
    }
}

bool downloadAndStageData(const ManifestInfo& manifest,
                          std::vector<std::string>& stagedFiles,
                          ProgressCb progress) {
    stagedFiles.clear();

    // Сначала выбираем файлы, которые реально нужно скачать, и считаем их
    // суммарный размер (manifest.size) — чтобы progress шёл по всей пачке
    // сразу, а не скидывался на 0% на старте каждого отдельного файла.
    std::vector<std::pair<std::string, FileEntry>> toDownload;
    size_t totalBytes = 0;
    for (auto& [fname, fe] : manifest.dataFiles) {
        if (isExternalManifestKey(fname)) continue; // не {app}-путь, см. syncGsiConfig()
        if (localFileUpToDate(fname, fe.sha256)) continue; // уже актуален — не трогаем
        toDownload.emplace_back(fname, fe);
        totalBytes += fe.size;
    }

    size_t doneBytes = 0;
    for (auto& [fname, fe] : toDownload) {
        std::string stagingPath = std::string(STAGING_DIR) + "\\" + fname;

        ProgressCb wrapped = nullptr;
        if (progress) {
            wrapped = [&](size_t fileDone, size_t fileTotal) {
                if (totalBytes > 0)
                    progress(doneBytes + fileDone, totalBytes);
                else
                    progress(fileDone, fileTotal); // манифест без "size" — прогресс по текущему файлу
            };
        }

        if (!downloadToStaging(fe.url, fe.sha256, stagingPath, wrapped))
            return false;
        doneBytes += fe.size;
        stagedFiles.push_back(fname);
    }
    LOG_INFO("Data update: " << stagedFiles.size() << "/" << manifest.dataFiles.size()
             << " files need downloading (rest already up to date)");
    return true;
}

// Удаляет assets\*.png, которые не перечислены в manifest.dataFiles под
// ключом "assets/<name>" — иначе переименованные/убранные герои оставляют
// мусор в assets/ навсегда (сама маска assets\*.png в манифесте только
// добавляет/перезаписывает файлы, но никогда не убирает лишние).
static void cleanupObsoleteAssets(const ManifestInfo& manifest) {
    std::set<std::string> expected;
    for (auto& [fname, _] : manifest.dataFiles) {
        if (fname.rfind("assets\\", 0) == 0 || fname.rfind("assets/", 0) == 0) {
            size_t slash = fname.find_last_of("\\/");
            expected.insert(fname.substr(slash + 1));
        }
    }
    if (expected.empty()) return; // манифест не описывает ассеты — не трогаем локальную папку

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("assets\\*.png", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!expected.count(fd.cFileName)) {
            std::string path = std::string("assets\\") + fd.cFileName;
            LOG_INFO("Removing obsolete asset: " << path);
            DeleteFileA(path.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

bool swapDataFiles(const ManifestInfo& manifest, const std::vector<std::string>& stagedFiles) {
    if (stagedFiles.empty()) return true; // всё уже актуально — менять нечего

    // swap.lock для отслеживания прерванных операций
    {
        std::ofstream lk(SWAP_LOCK, std::ios::trunc);
        lk << "{\"status\":\"in_progress\"}";
    }

    // Бэкап текущих файлов (только тех, что реально заменяем)
    for (auto& fname : stagedFiles) {
        std::string bak = fname + ".bak";
        CopyFileA(fname.c_str(), bak.c_str(), FALSE);
    }

    // Замена: staging → live
    bool ok = true;
    for (auto& fname : stagedFiles) {
        std::string staged = std::string(STAGING_DIR) + "\\" + fname;
        ensureParentDir(fname);
        if (!MoveFileExA(staged.c_str(), fname.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            LOG_ERR("Failed to swap " << staged << " -> " << fname
                    << " err=" << GetLastError());
            ok = false;
            break;
        }
    }

    if (!ok) {
        rollbackDataFiles();
        DeleteFileA(SWAP_LOCK);
        return false;
    }

    // Удалить бэкапы и lock
    for (auto& fname : stagedFiles)
        DeleteFileA((fname + ".bak").c_str());
    DeleteFileA(SWAP_LOCK);

    // Ассеты (assets/*.png) копируются по маске в манифесте, поэтому файлы
    // героев, пропавших из манифеста (переименован/убран), сами по себе не
    // перезапишутся — удаляем их явно, иначе они остаются в assets/ навсегда.
    cleanupObsoleteAssets(manifest);

    return true;
}

// Восстанавливает файлы из *.bak после прерванного swap. Работает без
// ManifestInfo (вызывается из checkPendingSwap до fetchManifest), поэтому
// ищет *.bak по маске, а не по жёстко заданному списку файлов — это же
// нужно, чтобы работать с произвольным набором файлов манифеста (включая
// assets/*.png.bak), а не только с cbm/db.
static void restoreBakGlob(const std::string& pattern) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    size_t slash = pattern.find_last_of("\\/");
    std::string dirPrefix = (slash == std::string::npos) ? "" : pattern.substr(0, slash + 1);

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string bakPath = dirPrefix + fd.cFileName;
        std::string origPath = bakPath.substr(0, bakPath.size() - 4); // strip ".bak"
        MoveFileExA(bakPath.c_str(), origPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void rollbackDataFiles() {
    restoreBakGlob("*.bak");
    restoreBakGlob("assets\\*.bak");
}

void checkPendingSwap() {
    if (GetFileAttributesA(SWAP_LOCK) != INVALID_FILE_ATTRIBUTES) {
        LOG_WARN("Found incomplete data swap — rolling back");
        rollbackDataFiles();
        DeleteFileA(SWAP_LOCK);
    }
}

static void deletePartGlob(const std::string& pattern) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    size_t slash = pattern.find_last_of("\\/");
    std::string dirPrefix = (slash == std::string::npos) ? "" : pattern.substr(0, slash + 1);

    do {
        std::string path = dirPrefix + fd.cFileName;
        DeleteFileA(path.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void cleanupStaging() {
    deletePartGlob(std::string(STAGING_DIR) + "\\*.part");
    deletePartGlob(std::string(STAGING_DIR) + "\\assets\\*.part");
}

// ─── GSI config sync (файл вне {app}, живёт в папке Dota 2) ──────────────────

static bool regReadString(HKEY root, const char* subKey, const char* value, std::string& out) {
    HKEY hKey;
    if (RegOpenKeyExA(root, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    char buf[MAX_PATH] = {};
    DWORD size = sizeof(buf) - 1;
    DWORD type = 0;
    LONG rc = RegQueryValueExA(hKey, value, nullptr, &type, (LPBYTE)buf, &size);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    out = buf;
    return true;
}

static bool dirExistsA(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Ищет "...\dota 2 beta\game\dota\cfg" среди ВСЕХ библиотек Steam, а не только
// под путём установки самого Steam — Dota 2 (30+ ГБ) часто стоит в отдельной
// библиотеке на другом диске (steamapps\libraryfolders.vdf). Тот же алгоритм,
// что и в installer/dota_draft_setup.iss (FindDotaCfgFolder), но на C++, т.к.
// здесь он должен отрабатывать не один раз при установке, а при каждом запуске.
// Пустая строка = Dota 2 на этой машине не найдена.
static std::string findDotaCfgFolder() {
    std::string steamPath;
    if (!regReadString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath", steamPath) &&
        !regReadString(HKEY_CURRENT_USER,  "SOFTWARE\\Valve\\Steam", "SteamPath",   steamPath))
        return "";
    for (auto& c : steamPath) if (c == '/') c = '\\';

    std::string candidate = steamPath + "\\steamapps\\common\\dota 2 beta\\game\\dota\\cfg";
    if (dirExistsA(candidate)) return candidate;

    std::ifstream vdf(steamPath + "\\steamapps\\libraryfolders.vdf");
    if (!vdf.is_open()) return "";

    std::string line;
    while (std::getline(vdf, line)) {
        auto keyPos = line.find("\"path\"");
        if (keyPos == std::string::npos) continue;
        auto q1 = line.find('"', keyPos + 6);
        if (q1 == std::string::npos) continue;
        auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;

        std::string libPath = line.substr(q1 + 1, q2 - q1 - 1);
        std::string cleaned;
        cleaned.reserve(libPath.size());
        for (size_t i = 0; i < libPath.size(); ++i) {
            if (libPath[i] == '\\' && i + 1 < libPath.size() && libPath[i + 1] == '\\') {
                cleaned += '\\';
                ++i;
            } else {
                cleaned += libPath[i];
            }
        }

        candidate = cleaned + "\\steamapps\\common\\dota 2 beta\\game\\dota\\cfg";
        if (dirExistsA(candidate)) return candidate;
    }
    return "";
}

void syncGsiConfig(const ManifestInfo& manifest) {
    try {
        auto it = manifest.dataFiles.find(GSI_CONFIG_KEY);
        if (it == manifest.dataFiles.end() || it->second.sha256.empty()) return;

        std::string cfgFolder = findDotaCfgFolder();
        if (cfgFolder.empty()) {
            LOG_INFO("[GSI cfg] Dota 2 не найдена на этой машине — пропускаем синхронизацию");
            return;
        }

        std::string gsiDir  = cfgFolder + "\\gamestate_integration";
        std::string gsiPath = gsiDir + "\\" + GSI_CONFIG_KEY;

        if (localFileUpToDate(gsiPath, it->second.sha256)) return; // уже актуален

        CreateDirectoryA(gsiDir.c_str(), nullptr); // может не существовать (см. dota_draft_setup.iss)

        std::string tmpPath = gsiPath + ".part";
        DeleteFileA(tmpPath.c_str()); // подчистить обрывок от прошлой неудачной попытки

        if (!downloadToStaging(it->second.url, it->second.sha256, tmpPath, nullptr)) {
            LOG_ERR("[GSI cfg] Не удалось скачать обновлённый конфиг");
            return;
        }
        if (!MoveFileExA(tmpPath.c_str(), gsiPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            LOG_ERR("[GSI cfg] Не удалось заменить конфиг, err=" << GetLastError());
            DeleteFileA(tmpPath.c_str());
            return;
        }
        LOG_INFO("[GSI cfg] Конфиг обновлён: " << gsiPath);
    } catch (const std::exception& e) {
        LOG_ERR("[GSI cfg] Исключение при синхронизации: " << e.what());
    } catch (...) {
        LOG_ERR("[GSI cfg] Неизвестное исключение при синхронизации");
    }
}
