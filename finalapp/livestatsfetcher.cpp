/*
 * livestatsfetcher.cpp — GSI HTTP-сервер (порт 62326, localhost).
 *
 * POST / — приём GSI-данных от Dota 2 → обновление GameInfo (phase, matchId, slot, lastUpdate).
 * GET /phase — JSON статус текущей фазы.
 * lastUpdate используется оркестратором для GSI-таймаута (сброс на IDLE при закрытии Dota 2).
 * logs/gsi.log — полная, недедуплицированная история принятых состояний за
 * сессию (каждый POST, а не только смена состояния, как в logs/console.log).
 */

#pragma comment(lib, "ws2_32.lib")

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include "shared_types.h"
#include "app_state.h"
#include "common.h"

#include <iostream>
#include <mutex>
#include <string>
#include <thread>

static const int PORT = 62326;
static GameInfo* g_gsiInfo = nullptr;
static std::string g_lastLoggedState;  // для дедупликации записей в лог (только смена состояния)

// ─── Полный GSI-лог сессии (logs/gsi.log) ─────────────────────────────────────
// Отдельный от logs/console.log файл: пишет каждое принятое GSI-состояние без
// дедупликации по смене state (в отличие от LOG_INFO ниже), плюс интервал с
// предыдущей записи — по нему видно реальные разрывы в потоке GSI-обновлений
// от Dota, не совпадающие с ожидаемым 5с heartbeat'ом.
static std::ofstream                         g_gsiLogFile;
static std::mutex                            g_gsiLogMutex;
static std::chrono::steady_clock::time_point g_gsiLogPrevTime;
static bool                                  g_gsiLogHasPrev = false;

static void openGsiLog() {
    ensureLogsDir();
    g_gsiLogFile.open("logs/gsi.log", std::ios::out | std::ios::trunc);
}

static void logGsiState(const std::string& gstate, const std::string& mid,
                         const std::string& team, int teamSlot,
                         const std::string& uid) {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    struct tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char hms[10];
    std::strftime(hms, sizeof(hms), "%H:%M:%S", &tmBuf);
    char timebuf[32];
    std::snprintf(timebuf, sizeof(timebuf), "%s.%03d", hms, static_cast<int>(ms.count()));

    auto steadyNow = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_gsiLogMutex);
    if (!g_gsiLogFile.is_open()) return;

    long long gapMs = -1;
    if (g_gsiLogHasPrev)
        gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            steadyNow - g_gsiLogPrevTime).count();
    g_gsiLogPrevTime = steadyNow;
    g_gsiLogHasPrev  = true;

    g_gsiLogFile << timebuf
                 << " gap="    << (gapMs < 0 ? std::string("-") : std::to_string(gapMs) + "ms")
                 << " state="  << (gstate.empty() ? "-" : gstate)
                 << " match="  << (mid.empty()    ? "-" : mid)
                 << " team="   << (team.empty()   ? "-" : team)
                 << " slot="   << teamSlot
                 << " player=" << (uid.empty()    ? "-" : uid)
                 << "\n";
    g_gsiLogFile.flush();
}

static GamePhase parsePhaseStr(const std::string& s) {
    if (s == "DOTA_GAMERULES_STATE_HERO_SELECTION" ||
        s == "DOTA_GAMERULES_STATE_STRATEGY_TIME"  ||
        s == "DOTA_GAMERULES_STATE_TEAM_SHOWCASE")
        return GamePhase::DRAFT;
    if (s == "DOTA_GAMERULES_STATE_WAIT_FOR_PLAYERS_TO_LOAD" ||
        s == "DOTA_GAMERULES_STATE_PRE_GAME" ||
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

// ─── HTTP handler ────────────────────────────────────────────────────────────

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

        // safeStoi вместо std::stoi: team_slot приходит из hand-rolled JSON-парсера
        // непроверенного GSI-payload'а, брошенное здесь исключение убило бы весь
        // процесс (см. комментарий у client_thread). Клэмп на случай не кидающего,
        // но всё равно некорректного значения (напр. "99"), которое иначе испортит
        // индексацию массива слотов 1-5 ниже по стеку.
        int team_slot = safeStoi(slot_str, 0);
        if (team_slot < 0 || team_slot > 4) team_slot = 0;

        // Полная запись в logs/gsi.log — на каждый POST, до дедупликации ниже.
        logGsiState(gstate, mid, team, team_slot, uid);

        if (g_gsiInfo) {
            bool stateChanged = false;
            {
                std::lock_guard<std::mutex> lk(g_gsiInfo->mtx);
                // lastUpdate — единственный сигнал для watchdog'а в orchestrator.cpp
                // (readGameStateWithTimeoutWatchdog, сброс фазы на IDLE после 15с
                // без обновлений этого поля): любой дошедший POST от Dota означает
                // живое GSI-соединение, независимо от того, удалось ли распарсить
                // matchid в конкретном payload'е.
                g_gsiInfo->lastUpdate = std::chrono::steady_clock::now();

                if (!mid.empty()) {
                    bool newId = (mid != g_gsiInfo->matchId);
                    g_gsiInfo->matchId = mid;
                    if (!gstate.empty()) g_gsiInfo->phase   = parsePhaseStr(gstate);
                    if (!team.empty())   g_gsiInfo->ourSide = (team == "radiant") ? 1 : 0;
                    g_gsiInfo->ourSlot = team_slot + 1;
                    if (newId) g_gsiInfo->newMatch = true;
                    // Логируем только первый контакт и смену состояния — иначе лог
                    // захлёбывается GSI-запросами, которые идут несколько раз в секунду.
                    stateChanged = newId || (gstate != g_lastLoggedState);
                    if (stateChanged) g_lastLoggedState = gstate;
                    g_gsiInfo->isHeroSelection =
                        (gstate == "DOTA_GAMERULES_STATE_HERO_SELECTION");
                    g_gsiInfo->isWaitingForPlayers =
                        (gstate == "DOTA_GAMERULES_STATE_WAIT_FOR_PLAYERS_TO_LOAD");
                }
            }

            if (stateChanged) {
                LOG_INFO("[GSI] match=" << mid << " state=" << gstate
                    << " team=" << team << " slot=" << team_slot << " player=" << uid);
                // Реальная смена фазы/матча — не heartbeat-повтор того же
                // состояния каждые 5с — стоит перерисовать статус-бар сразу,
                // не дожидаясь ближайшего тика оркестратора/таймаута лупа.
                requestRedraw();
            }
        }
        resp_body = "OK";

    } else if (is_get && path == "/phase") {
        if (g_gsiInfo) {
            std::lock_guard<std::mutex> lk(g_gsiInfo->mtx);
            resp_body = std::string("{\"phase\":\"")
                      + phaseName(g_gsiInfo->phase)
                      + "\",\"heroSel\":"
                      + (g_gsiInfo->isHeroSelection ? "true" : "false")
                      + ",\"match\":\"" + g_gsiInfo->matchId + "\"}";
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

// Запускается как std::thread(client_thread, client).detach() — без внешней
// сетки. Необработанное исключение в отсоединённом потоке = std::terminate() =
// падение всего процесса, поэтому всё тело обёрнуто здесь, а не полагается на
// installCrashHandlers() как на единственную защиту.
static void client_thread(SOCKET client) {
    try {
        std::string request;
        char buf[4096];

        // Читаем до конца заголовков (\r\n\r\n)
        while (true) {
            int received = recv(client, buf, sizeof(buf), 0);
            if (received <= 0) break;
            request.append(buf, received);
            if (request.find("\r\n\r\n") != std::string::npos) break;
        }

        // Определяем Content-Length и дочитываем тело
        auto hdrEnd = request.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) {
            int contentLen = 0;
            std::string hdr = request.substr(0, hdrEnd);
            auto pos = hdr.find("Content-Length:");
            if (pos == std::string::npos) pos = hdr.find("content-length:");
            if (pos != std::string::npos) {
                pos += 15; // strlen("Content-Length:")
                while (pos < hdr.size() && hdr[pos] == ' ') pos++;
                contentLen = std::atoi(hdr.c_str() + pos);
            }
            size_t bodyStart = hdrEnd + 4;
            size_t bodyHave  = request.size() - bodyStart;
            while ((int)bodyHave < contentLen) {
                int received = recv(client, buf, sizeof(buf), 0);
                if (received <= 0) break;
                request.append(buf, received);
                bodyHave += received;
            }
        }

        if (!request.empty()) {
            std::string resp = handle_request(request);
            send(client, resp.c_str(), (int)resp.size(), 0);
        }
    } catch (const std::exception& e) {
        LOG_ERR("[GSI] client_thread exception: " << e.what());
    } catch (...) {
        LOG_ERR("[GSI] client_thread unknown exception");
    }
    closesocket(client);
}

// ─────────────────────────────────────────────────────────────────────────────
// runGsiServer
// ─────────────────────────────────────────────────────────────────────────────

void runGsiServer(GameInfo& gameInfo) {
    g_gsiInfo = &gameInfo;
    openGsiLog();

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        LOG_ERR("[GSI] WSAStartup failed"); return;
    }
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        LOG_ERR("[GSI] socket() failed, WSA error " << WSAGetLastError()); return;
    }
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERR("[GSI] bind() на порту " << PORT << " не удался, WSA error "
            << WSAGetLastError() << " — порт уже занят другим процессом?");
        closesocket(server); return;
    }
    if (listen(server, SOMAXCONN) == SOCKET_ERROR) {
        LOG_ERR("[GSI] listen() не удался, WSA error " << WSAGetLastError());
        closesocket(server); return;
    }
    LOG_INFO("[GSI] Сервер запущен на порту " << PORT);
    LOG_INFO("[GSI] GET http://localhost:" << PORT << "/phase — статус");

    while (true) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        // std::thread(...) может бросить std::system_error на всплеске
        // одновременных подключений (нехватка потоковых ресурсов). Этот цикл
        // сам запущен как detach()-поток (orchestrator.cpp) без внешней сетки —
        // необработанное исключение здесь — это std::terminate() и падение
        // всего процесса, а не только GSI-сервера, поэтому ловим отдельно от
        // client_thread (у того своя сетка на тело запроса).
        try {
            std::thread(client_thread, client).detach();
        } catch (const std::exception& e) {
            LOG_ERR("[GSI] failed to spawn client thread: " << e.what());
            closesocket(client);
        } catch (...) {
            LOG_ERR("[GSI] failed to spawn client thread: unknown exception");
            closesocket(client);
        }
    }
    closesocket(server);
    WSACleanup();
}