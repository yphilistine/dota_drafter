/*
 * livestatsfetcher.cpp
 * GSI HTTP-сервер + Steam-поллер (только лог).
 * Обновляет GameInfo.phase и GameInfo.isHeroSelection.
 */

#pragma comment(lib, "ws2_32.lib")

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <curl/curl.h>
#include <sqlite3.h>

#include "shared_types.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <ctime>

static std::string g_steam_api_key;
static const char* DB_FILE  = "playerandlivestats.db";
static const int   PORT     = 3000;
static const int   POLL_SEC = 5;

struct LocalState {
    std::string match_id;
    std::string game_state;
    int         team_slot = 0;
    std::string team;
};
static LocalState            g_local;
static std::mutex            g_localMutex;
static std::map<int,std::string> g_heroes;
static GameInfo*             g_gameInfo = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Утилиты
// ─────────────────────────────────────────────────────────────────────────────

static std::string now_hms() {
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePhaseStr
// ─────────────────────────────────────────────────────────────────────────────

static GamePhase parsePhaseStr(const std::string& s) {
    if (s == "DOTA_GAMERULES_STATE_HERO_SELECTION" ||
        s == "DOTA_GAMERULES_STATE_STRATEGY_TIME"  ||
        s == "DOTA_GAMERULES_STATE_TEAM_SHOWCASE")
        return GamePhase::DRAFT;
    if (s == "DOTA_GAMERULES_STATE_PRE_GAME" ||
        s == "DOTA_GAMERULES_STATE_GAME_IN_PROGRESS")
        return GamePhase::INGAME;
    if (s == "DOTA_GAMERULES_STATE_POST_GAME")
        return GamePhase::POSTGAME;
    return GamePhase::IDLE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Минимальный JSON-парсер
// ─────────────────────────────────────────────────────────────────────────────

static std::string json_get(const std::string& src, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = src.find(needle);
    if (pos == std::string::npos) return "";
    pos = src.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < src.size() && (src[pos]==' '||src[pos]=='\t')) pos++;
    if (pos >= src.size()) return "";
    if (src[pos] == '"') {
        pos++;
        std::string val;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos]=='\\') pos++;
            if (pos < src.size()) val += src[pos++];
        }
        return val;
    } else {
        auto end = src.find_first_of(",}\n]", pos);
        std::string val = src.substr(pos, end==std::string::npos?std::string::npos:end-pos);
        while (!val.empty() && (val.back()==' '||val.back()=='\r')) val.pop_back();
        return val;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Загрузка героев
// ─────────────────────────────────────────────────────────────────────────────

static void load_heroes() {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(DB_FILE, &db,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        return;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, localized_name FROM heroes",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            const char* name = (const char*)sqlite3_column_text(stmt, 1);
            if (name) { g_heroes[id] = name; count++; }
        }
        sqlite3_finalize(stmt);
        std::printf("[GSI] Loaded %d heroes from local DB\n", count);
    }
    sqlite3_close(db);
}

// ─────────────────────────────────────────────────────────────────────────────
// curl GET
// ─────────────────────────────────────────────────────────────────────────────

static size_t curl_write_cb(void* ptr, size_t sz, size_t nmemb, std::string* s) {
    s->append((char*)ptr, sz*nmemb); return sz*nmemb;
}
static std::string curl_get(const std::string& url) {
    CURL* curl = curl_easy_init(); if (!curl) return "";
    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return resp;
}

// ─────────────────────────────────────────────────────────────────────────────
// Steam-поллер — только лог, в livepicks не пишет
// ─────────────────────────────────────────────────────────────────────────────

static void poll_loop() {
    while (true) {
        Sleep(POLL_SEC * 1000);
        std::string mid, gstate;
        {
            std::lock_guard<std::mutex> lk(g_localMutex);
            mid    = g_local.match_id;
            gstate = g_local.game_state;
        }
        if (mid.empty() || g_steam_api_key.empty()) continue;
        if (gstate != "DOTA_GAMERULES_STATE_HERO_SELECTION") continue;

        std::string url =
            "https://api.steampowered.com/IDOTA2Match_570/GetMatchDetails/v1/"
            "?key=" + g_steam_api_key + "&match_id=" + mid;
        std::string resp = curl_get(url);
        if (resp.empty()) continue;

        // Простой лог: сколько героев пикнуто
        int radiant_count = 0, dire_count = 0;
        size_t p = 0;
        while ((p = resp.find("\"hero_id\"", p)) != std::string::npos) {
            p += 9;
            size_t colon = resp.find(':', p);
            if (colon == std::string::npos) break;
            std::string hv = resp.substr(colon+1, 10);
            hv.erase(std::remove_if(hv.begin(), hv.end(),
                [](char c){ return c==' '||c==','||c=='}'||c==']'; }), hv.end());
            if (hv == "0" || hv.empty()) continue;
            // player_slot — ищем рядом
            size_t slotPos = resp.rfind("\"player_slot\"", colon);
            if (slotPos != std::string::npos && colon - slotPos < 200) {
                size_t sc = resp.find(':', slotPos);
                if (sc != std::string::npos) {
                    int slot = std::stoi(resp.substr(sc+1, 5));
                    if (slot < 128) radiant_count++; else dire_count++;
                }
            }
        }
        if (radiant_count || dire_count)
            std::printf("[%s] Steam match=%s  Radiant:%d  Dire:%d\n",
                now_hms().c_str(), mid.c_str(), radiant_count, dire_count);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP handler
// ─────────────────────────────────────────────────────────────────────────────

static std::string handle_request(const std::string& raw) {
    bool is_post = raw.size() >= 4 && raw.substr(0,4) == "POST";
    bool is_get  = raw.size() >= 3 && raw.substr(0,3) == "GET";

    std::string path;
    {
        auto sp = raw.find(' ');
        auto ep = raw.find(' ', sp+1);
        if (sp != std::string::npos && ep != std::string::npos)
            path = raw.substr(sp+1, ep-sp-1);
    }
    std::string body;
    auto sep = raw.find("\r\n\r\n");
    if (sep != std::string::npos) body = raw.substr(sep+4);

    std::string resp_body;
    std::string content_type = "text/plain";

    if (is_post && path == "/") {
        std::string mid      = json_get(body, "matchid");
        std::string uid      = json_get(body, "steamid");
        std::string gstate   = json_get(body, "game_state");
        std::string team     = json_get(body, "team_name");
        std::string slot_str = json_get(body, "team_slot");

        int team_slot = slot_str.empty() ? 0 : std::stoi(slot_str);

        if (!mid.empty()) {
            {
                std::lock_guard<std::mutex> lk(g_localMutex);
                g_local.match_id   = mid;
                g_local.game_state = gstate;
                g_local.team       = team;
                g_local.team_slot  = team_slot;
            }

            if (g_gameInfo) {
                std::lock_guard<std::mutex> lk(g_gameInfo->mtx);
                bool newId = (!mid.empty() && mid != g_gameInfo->matchId);
                if (!mid.empty())    g_gameInfo->matchId = mid;
                if (!gstate.empty()) g_gameInfo->phase   = parsePhaseStr(gstate);
                if (!team.empty())   g_gameInfo->ourSide = (team == "radiant") ? 1 : 0;
                g_gameInfo->ourSlot = team_slot + 1;
                if (newId) {
                    g_gameInfo->newMatch = true;
                    std::printf("[GSI] Новый матч: %s  player:%s  state:%s\n",
                        mid.c_str(), uid.c_str(), gstate.c_str());
                }
                // isHeroSelection — только при этом конкретном состоянии
                g_gameInfo->isHeroSelection =
                    (gstate == "DOTA_GAMERULES_STATE_HERO_SELECTION");
            }

            std::printf("[GSI] match=%s state=%s team=%s slot=%d\n",
                mid.c_str(), gstate.c_str(), team.c_str(), team_slot);
        }
        resp_body = "OK";

    } else if (is_get && path == "/phase") {
        if (g_gameInfo) {
            std::lock_guard<std::mutex> lk(g_gameInfo->mtx);
            resp_body = std::string("{\"phase\":\"")
                      + phaseName(g_gameInfo->phase)
                      + "\",\"heroSel\":"
                      + (g_gameInfo->isHeroSelection ? "true" : "false")
                      + ",\"match\":\"" + g_gameInfo->matchId + "\"}";
        } else {
            resp_body = "{\"phase\":\"unknown\"}";
        }
        content_type = "application/json";
    } else {
        resp_body = "Not Found";
    }

    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: "   + content_type + "\r\n"
           "Content-Length: " + std::to_string(resp_body.size()) + "\r\n"
           "Connection: close\r\n\r\n" + resp_body;
}

static void client_thread(SOCKET client) {
    std::string request;
    char buf[4096]; int received;
    do {
        received = recv(client, buf, sizeof(buf), 0);
        if (received > 0) request.append(buf, received);
    } while (received == (int)sizeof(buf));
    if (!request.empty()) {
        std::string resp = handle_request(request);
        send(client, resp.c_str(), (int)resp.size(), 0);
    }
    closesocket(client);
}

// ─────────────────────────────────────────────────────────────────────────────
// runGsiServer
// ─────────────────────────────────────────────────────────────────────────────

void runGsiServer(GameInfo& gameInfo, const std::string& steamApiKey) {
    g_gameInfo      = &gameInfo;
    g_steam_api_key = steamApiKey;
    load_heroes();
    std::thread(poll_loop).detach();

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        std::fprintf(stderr, "[GSI] WSAStartup failed\n"); return;
    }
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        std::fprintf(stderr, "[GSI] socket() failed\n"); return;
    }
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(server, SOMAXCONN) == SOCKET_ERROR) {
        std::fprintf(stderr, "[GSI] bind/listen error: %d\n", WSAGetLastError());
        closesocket(server); return;
    }
    std::printf("[GSI] Сервер запущен на порту %d\n", PORT);
    std::printf("[GSI] GET http://localhost:%d/phase — статус\n", PORT);

    while (true) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::thread(client_thread, client).detach();
    }
    closesocket(server);
    WSACleanup();
}