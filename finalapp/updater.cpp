#include "updater.h"
#include "version.h"
#include "common.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

static const char* STAGING_DIR  = "staging";
static const char* SWAP_LOCK    = "staging\\swap.lock";

// ─── Утилиты ─────────────────────────────────────────────────────────────────

static void ensureStagingDir() {
    CreateDirectoryA(STAGING_DIR, nullptr);
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
    ensureStagingDir();
    std::string partPath = stagingPath + ".part";

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    DownloadCtx ctx;
    ctx.fp = fopen(partPath.c_str(), "wb");
    if (!ctx.fp) { curl_easy_cleanup(curl); return false; }
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

bool downloadAndStageData(const ManifestInfo& manifest, ProgressCb progress) {
    for (auto& [fname, fe] : manifest.dataFiles) {
        std::string stagingPath = std::string(STAGING_DIR) + "\\" + fname;
        if (!downloadToStaging(fe.url, fe.sha256, stagingPath, progress))
            return false;
    }
    return true;
}

bool swapDataFiles(const ManifestInfo& manifest) {
    // swap.lock для отслеживания прерванных операций
    {
        std::ofstream lk(SWAP_LOCK, std::ios::trunc);
        lk << "{\"status\":\"in_progress\"}";
    }

    // Бэкап текущих файлов
    for (auto& [fname, _] : manifest.dataFiles) {
        std::string bak = fname + ".bak";
        CopyFileA(fname.c_str(), bak.c_str(), FALSE);
    }

    // Замена: staging → live
    bool ok = true;
    for (auto& [fname, _] : manifest.dataFiles) {
        std::string staged = std::string(STAGING_DIR) + "\\" + fname;
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
    for (auto& [fname, _] : manifest.dataFiles)
        DeleteFileA((fname + ".bak").c_str());
    DeleteFileA(SWAP_LOCK);

    return true;
}

void rollbackDataFiles() {
    const char* files[] = {
        "draft_helper_abstract.cbm",
        "draft_helper_abstract_data.db"
    };
    for (auto* f : files) {
        std::string bak = std::string(f) + ".bak";
        if (GetFileAttributesA(bak.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileExA(bak.c_str(), f, MOVEFILE_REPLACE_EXISTING);
    }
}

void checkPendingSwap() {
    if (GetFileAttributesA(SWAP_LOCK) != INVALID_FILE_ATTRIBUTES) {
        LOG_WARN("Found incomplete data swap — rolling back");
        rollbackDataFiles();
        DeleteFileA(SWAP_LOCK);
    }
}

void cleanupStaging() {
    WIN32_FIND_DATAA fd;
    std::string pattern = std::string(STAGING_DIR) + "\\*.part";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string path = std::string(STAGING_DIR) + "\\" + fd.cFileName;
            DeleteFileA(path.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}
