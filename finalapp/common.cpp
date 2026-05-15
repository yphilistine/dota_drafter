#include "common.h"

// ---------------------- Глобальные переменные ----------------------
std::mutex    g_logMutex;
std::mutex    g_dbWriteMutex;
bool          g_ansiEnabled  = false;
std::ofstream g_logFile;
FILE*         g_curlDebugFile = nullptr;

static std::atomic<int> g_logCounter{0};

// ---------------------- Утилиты ----------------------
std::string sanitizeUtf8(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        int seqLen = 0;
        if      (c < 0x80)            seqLen = 1;
        else if ((c & 0xE0) == 0xC0)  seqLen = 2;
        else if ((c & 0xF0) == 0xE0)  seqLen = 3;
        else if ((c & 0xF8) == 0xF0)  seqLen = 4;
        else { out += "\xEF\xBF\xBD"; ++i; continue; }
        bool valid = (i + seqLen <= input.size());
        for (int k = 1; valid && k < seqLen; ++k)
            if ((static_cast<unsigned char>(input[i+k]) & 0xC0) != 0x80) valid = false;
        if (valid) { out.append(input, i, seqLen); i += seqLen; }
        else       { out += "\xEF\xBF\xBD"; ++i; }
    }
    return out;
}

// ---------------------- Логирование ----------------------
void ensureLogsDir() {
#ifdef _WIN32
    CreateDirectoryA("logs", nullptr);
#else
    mkdir("logs", 0755);
#endif
}

static void clearLogsDir() {
#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA("logs\\*", &ffd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string path = std::string("logs\\") + ffd.cFileName;
            DeleteFileA(path.c_str());
        } while (FindNextFileA(hFind, &ffd));
        FindClose(hFind);
    }
#else
    DIR* dir = opendir("logs");
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string path = std::string("logs/") + ent->d_name;
            remove(path.c_str());
        }
        closedir(dir);
    }
#endif
}

static int curlDebugCallback(CURL*, curl_infotype type, char* data, size_t size, void*) {
    if (!g_curlDebugFile) return 0;
    const char* prefix = "";
    switch (type) {
        case CURLINFO_TEXT:       prefix = "* ";  break;
        case CURLINFO_HEADER_OUT: prefix = "-> "; break;
        case CURLINFO_HEADER_IN:  prefix = "<- "; break;
        case CURLINFO_DATA_OUT:   return 0;
        case CURLINFO_DATA_IN:    return 0;
        default:                  return 0;
    }
    fprintf(g_curlDebugFile, "%s%.*s", prefix, static_cast<int>(size), data);
    fflush(g_curlDebugFile);
    return 0;
}

void initConsole() {
    ensureLogsDir();
    clearLogsDir();
    g_logFile.open("logs/console.log", std::ios::out | std::ios::trunc);
    if (!g_logFile.is_open())
        std::cerr << "Не удалось открыть logs/console.log\n";
    g_curlDebugFile = fopen("logs/curl_debug.txt", "w");
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hOut, &mode)) {
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        g_ansiEnabled = (SetConsoleMode(hOut, mode) != 0);
    }
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD modeErr = 0;
    if (GetConsoleMode(hErr, &modeErr)) {
        modeErr |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hErr, modeErr);
    }
#else
    g_ansiEnabled = true;
#endif
}

void logConsole(LogLevel level, const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    char timebuf[32];
    {
        struct tm tmBuf{};
#ifdef _WIN32
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif
        char hms[10];
        std::strftime(hms, sizeof(hms), "%H:%M:%S", &tmBuf);
        std::snprintf(timebuf, sizeof(timebuf), "%s.%03d", hms, static_cast<int>(ms.count()));
    }
    const char* tag = "";
    const char* col = "";
    const char* rst = g_ansiEnabled ? "\033[0m" : "";
    std::ostream* out = &std::cout;
    switch (level) {
        case LogLevel::INFO: tag = "[INFO ]"; col = g_ansiEnabled ? "\033[32m" : ""; break;
        case LogLevel::WARN: tag = "[WARN ]"; col = g_ansiEnabled ? "\033[33m" : ""; out = &std::cerr; break;
        case LogLevel::ERR:  tag = "[ERROR]"; col = g_ansiEnabled ? "\033[31m" : ""; out = &std::cerr; break;
    }
    std::string fileLine = std::string(timebuf) + " " + tag + " " + msg + "\n";
    std::lock_guard<std::mutex> lock(g_logMutex);
    *out << timebuf << " " << col << tag << rst << " " << msg << std::endl;
    if (g_logFile.is_open()) {
        g_logFile.write(fileLine.data(), fileLine.size());
        g_logFile.flush();
    }
}

// ---------------------- HTTP ----------------------
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    output->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

static void dumpToFile(const std::string& filename, const std::string& data) {
#ifdef _WIN32
    HANDLE hFile = CreateFileA(filename.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(hFile);
    (void)written;
#else
    FILE* f = fopen(filename.c_str(), "wb");
    if (!f) return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
#endif
}

static std::string logTimestamp() {
    std::time_t t = std::time(nullptr);
    struct tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmBuf);
    return buf;
}

static void logRequest(const std::string& method, const std::string& url,
                       const std::string& body, int attempt)
{
    int n = ++g_logCounter;
    std::string fname = "logs/" + logTimestamp() + "_" + std::to_string(n)
                      + "_req_" + method + "_a" + std::to_string(attempt) + ".txt";
    std::ostringstream ss;
    ss << method << " " << url << "\nAttempt: " << attempt << "\n";
    if (!body.empty()) ss << "\nBODY:\n" << body;
    std::lock_guard<std::mutex> lock(g_logMutex);
    dumpToFile(fname, ss.str());
}

static void logResponse(const std::string& method, const std::string& url,
                        long http_code, const std::string& body, int attempt)
{
    int n = ++g_logCounter;
    std::string fname = "logs/" + logTimestamp() + "_" + std::to_string(n)
                      + "_res_" + method + "_" + std::to_string(http_code)
                      + "_a" + std::to_string(attempt) + ".txt";
    std::ostringstream ss;
    ss << method << " " << url << "\nHTTP " << http_code
       << " | Attempt: " << attempt << "\n\nBODY:\n" << body;
    std::lock_guard<std::mutex> lock(g_logMutex);
    dumpToFile(fname, ss.str());
}

static const int HTTP_RETRY_DELAY_SEC = 10;
static const int HTTP_MAX_RETRIES     = 3;

static void applyCurlNetworkOpts(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,    1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,    0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,    0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,    15L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE,         CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 60L);

    // Gzip-сжатие: OpenDota отдаёт ~6KB JSON в plain text,
    // с gzip это 800-1200 байт — скорость всегда выше порога LOW_SPEED_LIMIT
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING,   "gzip, deflate");

    // TCP keepalive: держит соединение живым при медленных ответах
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE,     1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE,      30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL,     10L);

    // Прерывать только если скорость < 100 байт/с в течение 90 сек
    // (было 30 — слишком мало для медленных ответов OpenDota)
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT,   100L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,    90L);

    if (g_curlDebugFile) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE,       1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curlDebugCallback);
        curl_easy_setopt(curl, CURLOPT_DEBUGDATA,     nullptr);
    }
}

std::string httpPost(const std::string& url, const std::string& postData,
                     const std::string& authToken) {
    ensureLogsDir();
    for (int attempt = 1; attempt <= HTTP_MAX_RETRIES; ++attempt) {
        logRequest("POST", url, postData, attempt);
        CurlHandle curlHandle;
        CURL* curl = curlHandle.get();
        std::string response;
        CurlHeaders headers;
        headers.append("Content-Type: application/json");
        headers.append("Accept: application/json, text/plain, */*");
        headers.append("Accept-Encoding: gzip, deflate");
        headers.append("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
        headers.append("Accept-Language: en-US,en;q=0.9");
        headers.append("Origin: https://stratz.com");
        headers.append("Referer: https://stratz.com/");
        headers.append("Sec-Fetch-Dest: empty");
        headers.append("Sec-Fetch-Mode: cors");
        headers.append("Sec-Fetch-Site: same-site");
        headers.append("Connection: keep-alive");
        if (!authToken.empty())
            headers.append(("Authorization: Bearer " + authToken).c_str());
        curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    postData.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers.get());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE,    "cookies.txt");
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR,     "cookies.txt");
        applyCurlNetworkOpts(curl);
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            logResponse("POST", url, 0, curl_easy_strerror(res), attempt);
            LOG_ERR("HTTP POST ошибка (попытка " << attempt << "/" << HTTP_MAX_RETRIES << "): " << curl_easy_strerror(res));
            if (attempt < HTTP_MAX_RETRIES) {
                LOG_WARN("Повтор через " << HTTP_RETRY_DELAY_SEC << " сек...");
                std::this_thread::sleep_for(std::chrono::seconds(HTTP_RETRY_DELAY_SEC));
                continue;
            }
            throw std::runtime_error(std::string("Ошибка HTTP POST: ") + curl_easy_strerror(res));
        }
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        logResponse("POST", url, http_code, response, attempt);
        if (http_code != 200) {
            LOG_ERR("HTTP POST код " << http_code << " (попытка " << attempt << "/" << HTTP_MAX_RETRIES << ") " << url);
            if (attempt < HTTP_MAX_RETRIES) {
                LOG_WARN("Повтор через " << HTTP_RETRY_DELAY_SEC << " сек...");
                std::this_thread::sleep_for(std::chrono::seconds(HTTP_RETRY_DELAY_SEC));
                continue;
            }
            throw std::runtime_error("HTTP ошибка POST: код " + std::to_string(http_code) + "\nОтвет: " + response);
        }
        return response;
    }
    throw std::runtime_error("HTTP POST: исчерпаны все попытки");
}

std::string httpGet(const std::string& url) {
    ensureLogsDir();
    for (int attempt = 1; attempt <= HTTP_MAX_RETRIES; ++attempt) {
        logRequest("GET", url, "", attempt);
        CurlHandle curlHandle;
        CURL* curl = curlHandle.get();
        std::string response;
        CurlHeaders headers;
        headers.append("Accept: application/json, text/plain, */*");
        headers.append("Accept-Encoding: gzip, deflate");
        headers.append("Accept-Language: en-US,en;q=0.9");
        headers.append("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
        headers.append("Connection: keep-alive");
        curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers.get());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
        applyCurlNetworkOpts(curl);
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            logResponse("GET", url, 0, curl_easy_strerror(res), attempt);
            LOG_ERR("HTTP GET ошибка (попытка " << attempt << "/" << HTTP_MAX_RETRIES << "): " << curl_easy_strerror(res));
            if (attempt < HTTP_MAX_RETRIES) {
                LOG_WARN("Повтор через " << HTTP_RETRY_DELAY_SEC << " сек...");
                std::this_thread::sleep_for(std::chrono::seconds(HTTP_RETRY_DELAY_SEC));
                continue;
            }
            throw std::runtime_error(std::string("Ошибка HTTP GET: ") + curl_easy_strerror(res));
        }
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        logResponse("GET", url, http_code, response, attempt);
        if (http_code != 200) {
            LOG_ERR("HTTP GET код " << http_code << " (попытка " << attempt << "/" << HTTP_MAX_RETRIES << ") " << url);
            if (attempt < HTTP_MAX_RETRIES) {
                LOG_WARN("Повтор через " << HTTP_RETRY_DELAY_SEC << " сек...");
                std::this_thread::sleep_for(std::chrono::seconds(HTTP_RETRY_DELAY_SEC));
                continue;
            }
            throw std::runtime_error("HTTP ошибка GET: код " + std::to_string(http_code) + "\nОтвет: " + response);
        }
        return response;
    }
    throw std::runtime_error("HTTP GET: исчерпаны все попытки");
}